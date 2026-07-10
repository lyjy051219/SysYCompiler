#include "ir/opt/ConstantHoist.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

BasicBlock *ConstantHoistPass::foldCommonDominator(
    const DominatorTree &DT,
    const std::vector<ConstantHoistUseSite> &uses) {
    if (uses.empty()) return nullptr;
    BasicBlock *acc = uses[0].block;
    if (!acc) return nullptr;
    std::unordered_set<BasicBlock*> accAncestors;
    for (BasicBlock *p = acc; p; p = DT.getIDom(p)) accAncestors.insert(p);

    for (size_t i = 1; i < uses.size(); ++i) {
        BasicBlock *B = uses[i].block;
        if (!B) return nullptr;
        BasicBlock *newAcc = nullptr;
        for (BasicBlock *p = B; p; p = DT.getIDom(p)) {
            if (accAncestors.count(p)) { newAcc = p; break; }
        }
        if (!newAcc) return nullptr;
        if (newAcc != acc) {
            acc = newAcc;
            accAncestors.clear();
            for (BasicBlock *p = acc; p; p = DT.getIDom(p)) accAncestors.insert(p);
        }
    }
    return acc;
}

BasicBlock *ConstantHoistPass::pickInsertionBlock(
    BasicBlock *common, BasicBlock *entry, const LoopInfo &LI) {
    if (!common) return entry;
    Loop *L = LI.getLoopFor(common);
    if (!L) return common;
    while (L->getParentLoop()) L = L->getParentLoop();
    BasicBlock *preHeader = nullptr, *latch = nullptr;
    if (L->getLoopSimplifyForm(preHeader, latch) && preHeader)
        return preHeader;
    return entry;
}

ConstantHoistCollectResult ConstantHoistPass::collectCandidates(
    Function &F, const LoopInfo &LI) {
    ConstantHoistCollectResult R;
    for (BasicBlock *BB : F) {
        unsigned depth = 0;
        if (Loop *L = LI.getLoopFor(BB)) depth = L->getDepth();
        for (Instruction *I : *BB) {
            // PHI 的 incoming 定义点必须在 incoming block 末尾，我们的物化
            // 指令插在非-PHI 位置无法满足这个规约
            if (dynamic_cast<PhiInst *>(I)) continue;
            for (size_t OperandIndex = 0;
                 OperandIndex < I->getNumOperands(); ++OperandIndex) {
                Use *u = I->getOperandUse(OperandIndex);
                if (!u) continue;
                Value *v = u->getValue();
                if (auto *ci = dynamic_cast<ConstInt *>(v)) {
                    int32_t val = (int32_t)ci->getVal();
                    if ((int64_t)std::abs(val) < ImmediateThreshold) continue;
                    auto &C = R.intBuckets[val];
                    C.value = val;
                    C.uses.push_back({u, I, BB, depth});
                    if (depth > C.maxDepth) C.maxDepth = depth;
                } else if (auto *cf = dynamic_cast<ConstFloat *>(v)) {
                    float fv = cf->getVal();
                    uint32_t bits;
                    std::memcpy(&bits, &fv, sizeof(bits));
                    auto &C = R.floatBuckets[bits];
                    C.value = (int32_t)bits;  // 复用字段存 bit pattern
                    C.uses.push_back({u, I, BB, depth});
                    if (depth > C.maxDepth) C.maxDepth = depth;
                }
            }
        }
    }
    return R;
}

void ConstantHoistPass::closeGroup(
    ConstantHoistGroup &cur, std::vector<ConstantHoistGroup> &groups) {
    if (cur.members.empty()) return;
    size_t totalUses = 0, bestIdx = 0, bestUses = 0;
    for (size_t j = 0; j < cur.members.size(); ++j) {
        totalUses += cur.members[j].uses.size();
        if (cur.members[j].uses.size() > bestUses) {
            bestUses = cur.members[j].uses.size();
            bestIdx = j;
        }
    }
    cur.baseIdx = bestIdx;
    if ((int)totalUses >= UseThreshold)
        groups.push_back(std::move(cur));
}

// 窗口条件 diff < IMM_THRESHOLD 保证组内 max-min <= 2047，无论 base 选
// 谁，|v - base| <= max-min 都落在 imm12 内
std::vector<ConstantHoistGroup> ConstantHoistPass::groupByBaseOffset(
    std::vector<ConstantHoistCandidate> &sorted) {
    std::vector<ConstantHoistGroup> groups;
    if (sorted.empty()) return groups;

    ConstantHoistGroup cur;
    cur.members.push_back(std::move(sorted[0]));
    int64_t windowMin = cur.members[0].value;

    for (size_t i = 1; i < sorted.size(); ++i) {
        int64_t diff = (int64_t)sorted[i].value - windowMin;
        if (diff < ImmediateThreshold) {
            cur.members.push_back(std::move(sorted[i]));
        } else {
            closeGroup(cur, groups);
            cur = ConstantHoistGroup{};
            cur.members.push_back(std::move(sorted[i]));
            windowMin = cur.members[0].value;
        }
    }
    closeGroup(cur, groups);
    return groups;
}

bool ConstantHoistPass::materializeGroup(
    ConstantHoistGroup &G, BasicBlock *insertBB) {
    if (G.members.empty()) return false;
    int32_t baseValue = G.members[G.baseIdx].value;

    auto *baseSSA = new BinaryInst(
        ConstInt::newConstInt(0), BinaryInst::ADD,
        ConstInt::newConstInt(baseValue));
    insertBB->insertAfterPhiNodes(baseSSA);

    bool anyChange = false;
    for (size_t i = 0; i < G.members.size(); ++i) {
        ConstantHoistCandidate &m = G.members[i];
        int64_t offset = (int64_t)m.value - (int64_t)baseValue;

        if (offset == 0) {
            for (ConstantHoistUseSite &us : m.uses) {
                us.u->replaceUseeWith(baseSSA);
                anyChange = true;
            }
            continue;
        }

        // 同 block 内同值多 use 聚合为一条 rebase
        std::unordered_map<BasicBlock*, std::vector<ConstantHoistUseSite*>> byBlock;
        for (ConstantHoistUseSite &us : m.uses) byBlock[us.block].push_back(&us);

        for (auto &kv : byBlock) {
            BasicBlock *B = kv.first;
            auto &useList = kv.second;
            if (useList.empty() || !B) continue;

            std::unordered_set<Instruction*> userSet;
            for (ConstantHoistUseSite *us : useList) userSet.insert(us->inst);

            Instruction *earliest = nullptr;
            for (Instruction *I : *B) {
                if (userSet.count(I)) { earliest = I; break; }
            }
            if (!earliest) continue;

            auto *matInst = new BinaryInst(
                baseSSA, BinaryInst::ADD,
                ConstInt::newConstInt((int32_t)offset));
            if (!earliest->insertBefore(matInst))
                B->insertBeforeTerminator(matInst);
            for (ConstantHoistUseSite *us : useList) us->u->replaceUseeWith(matInst);
            anyChange = true;
        }
    }
    return anyChange;
}

bool ConstantHoistPass::materializeFloatCandidate(
    ConstantHoistCandidate &C, BasicBlock *insertBB) {
    uint32_t bits = (uint32_t)C.value;
    float fv;
    std::memcpy(&fv, &bits, sizeof(fv));
    // IEEE 754 下 -0.0 + x = x 对所有 x（含 ±0.0）严格成立，+0.0 会把
    // -0.0 翻正。注：本 IR 的 ConstFloat 对 ±0.0 做了 interning 合并，
    // 真实落到的可能仍是 +0.0；但写成 -0.0 让意图显式、将来解除合并时直接正确
    auto *baseSSA = new BinaryInst(
        ConstFloat::newConstFloat(-0.0f), BinaryInst::ADD,
        ConstFloat::newConstFloat(fv));
    insertBB->insertAfterPhiNodes(baseSSA);
    for (ConstantHoistUseSite &us : C.uses) us.u->replaceUseeWith(baseSSA);
    return true;
}

PreservationStatus ConstantHoistPass::run(Function &F,
                                          FunctionAnalysisPassManager &FAM) {
    if (F.Size() == 0) return PreservationStatus::all();
    BasicBlock *entry = F.front();
    if (!entry) return PreservationStatus::all();

    auto &DT = FAM.getResult<DominantAnalysis>(F);
    auto &LI = FAM.getResult<LoopAnalysis>(F);

    ConstantHoistCollectResult R = collectCandidates(F, LI);

    std::vector<ConstantHoistCandidate> intSorted;
    intSorted.reserve(R.intBuckets.size());
    for (auto &kv : R.intBuckets) intSorted.push_back(std::move(kv.second));
    std::sort(intSorted.begin(), intSorted.end(),
              [](const ConstantHoistCandidate &a, const ConstantHoistCandidate &b) {
                  return a.value < b.value;
              });
    std::vector<ConstantHoistGroup> intGroups = groupByBaseOffset(intSorted);

    bool changed = false;
    for (ConstantHoistGroup &G : intGroups) {
        unsigned maxDepth = 0;
        size_t totalUses = 0;
        for (auto &m : G.members) {
            if (m.maxDepth > maxDepth) maxDepth = m.maxDepth;
            totalUses += m.uses.size();
        }
        // 冷代码下 hoist 净增一条 base add，不划算
        if (maxDepth == 0 &&
            (int)totalUses < StraightLineUseThreshold)
            continue;

        std::vector<ConstantHoistUseSite> allUses;
        for (auto &m : G.members)
            for (auto &u : m.uses) allUses.push_back(u);

        BasicBlock *common = foldCommonDominator(DT, allUses);
        BasicBlock *insertBB = pickInsertionBlock(common, entry, LI);
        if (!insertBB) insertBB = entry;

        changed |= materializeGroup(G, insertBB);
    }

    for (auto &kv : R.floatBuckets) {
        ConstantHoistCandidate &C = kv.second;
        if ((int)C.uses.size() < UseThreshold) continue;
        if (C.maxDepth == 0 &&
            (int)C.uses.size() < StraightLineUseThreshold)
            continue;
        BasicBlock *common = foldCommonDominator(DT, C.uses);
        BasicBlock *insertBB = pickInsertionBlock(common, entry, LI);
        if (!insertBB) insertBB = entry;
        changed |= materializeFloatCandidate(C, insertBB);
    }

    if (!changed) return PreservationStatus::all();
    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<LoopAnalysis>();
    return PA;
}
