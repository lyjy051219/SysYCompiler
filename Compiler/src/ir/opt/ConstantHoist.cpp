#include "ir/opt/ConstantHoist.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "lib/IRUtils.hpp"

#include <algorithm>
#include <cstdint>
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

bool ConstantHoistPass::isLiteralSensitiveUse(
    const Instruction *User, size_t OperandIndex) {
    if (!User) return true;

    // GEP 的第 0 个 operand 是 base pointer，后续 operand 是 index。
    // 后端会把字面 index 直接合并进地址偏移，不能改成 SSA。
    if (dynamic_cast<const GetElementPtrInst *>(User))
        return OperandIndex > 0;

    // 调用约定中可能有必须保持 literal 的参数。例如 RISC-V
    // memcpy lowering 要求 size 是非负常量。普通调用也保守处理，
    // 避免在 ABI lowering 前隐藏参数常量。
    if (dynamic_cast<const CallInst *>(User)) return true;

    // select 常量臂会影响后端分支/条件物化决策；并且提前
    // hoist 会把原本只在某一条路径需要的常量变成无条件物化。
    if (dynamic_cast<const SelectInst *>(User)) return true;

    if (auto *BI = dynamic_cast<const BinaryInst *>(User)) {
        BinaryInst::Operation Op = BI->getOp();
        if (BinaryInst::isCompareOp(Op)) return true;

        switch (Op) {
        case BinaryInst::DIV:
        case BinaryInst::UDIV:
        case BinaryInst::MOD:
        case BinaryInst::UREM:
            // 保留 divisor，让 div/rem-by-constant 强度削弱可见。
            return OperandIndex == 1;
        case BinaryInst::SHL:
        case BinaryInst::LSHR:
        case BinaryInst::ASHR:
            // RISC-V 立即数 shift 必须看到字面 shift amount。
            return OperandIndex == 1;
        case BinaryInst::MUL:
        case BinaryInst::AND:
        case BinaryInst::OR:
        case BinaryInst::XOR:
            // 常量乘法和位掩码都有 literal-specific 后端匹配。
            return true;
        case BinaryInst::ADD:
        case BinaryInst::SUB:
            return false;
        default:
            return true;
        }
    }

    // 常量 cast/branch 等应在这个 late pass 之前折叠。对未显式证明
    // 可安全的指令保守保留 literal，也使将来增加 switch 指令时
    // case value 不会被无意改写。只有 store 的大整数 value 在这里
    // 明确视为可共享；ret 保留字面量，避免把冷返回路径的物化
    // 提到公共支配块。
    return User->instType != Instruction::STORE;
}

ConstantHoistCollectResult ConstantHoistPass::collectCandidates(
    Function &F, const LoopInfo &LI) {
    ConstantHoistCollectResult R;
    forEachInstruction(
        F, [&](BasicBlock *BB, Instruction *I) {
        unsigned depth = 0;
        if (Loop *L = LI.getLoopFor(BB)) depth = L->getDepth();
        // PHI 的 incoming 定义点必须在 incoming block 末尾，我们的物化
        // 指令插在非-PHI 位置无法满足这个规约
        if (dynamic_cast<PhiInst *>(I)) return;
        forEachOperand(
            I, [&](size_t OperandIndex, Use *u, Value *v) {
                if (!u) return;
                if (auto *ci = dynamic_cast<ConstInt *>(v)) {
                    if (isLiteralSensitiveUse(I, OperandIndex)) return;
                    // 当前重组成本模型和候选键均按 i32 设计；其他位宽
                    // 保持原常量，避免生成类型不匹配的辅助加法。
                    if (!IRType::hasIntegerBitWidth(
                            ci->getIRType(), 32))
                        return;
                    const int32_t val = static_cast<int32_t>(ci->getVal());
                    // 先拓宽再取绝对值，避免 abs(INT_MIN) 的 C++ UB。
                    const int64_t wideVal = static_cast<int64_t>(val);
                    const int64_t magnitude = wideVal < 0 ? -wideVal : wideVal;
                    if (magnitude < ImmediateThreshold) return;
                    auto &C = R.intBuckets[val];
                    C.value = val;
                    C.uses.push_back({u, I, BB, depth});
                    if (depth > C.maxDepth) C.maxDepth = depth;
                }
                // 严格浮点下不用 fadd wrapper 物化 ConstFloat。
            });
    });
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
            forEachInstruction(*B, [&](Instruction *I) {
                if (!userSet.count(I)) return true;
                earliest = I;
                return false;
            });
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

PreservationStatus ConstantHoistPass::run(Function &F,
                                          FunctionAnalysisPassManager &FAM) {
    if (F.size() == 0) return PreservationStatus::all();
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

    if (!changed) return PreservationStatus::all();
    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<LoopAnalysis>();
    return PA;
}
