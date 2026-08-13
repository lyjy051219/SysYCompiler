#include "ir/opt/ConstantHoist.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "ir/analysis/TargetCostAnalysis.hpp"
#include "lib/IRUtils.hpp"
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>
//步骤 3.1：沿支配树向上折叠全部使用块，得到能覆盖整组使用的公共支配块。
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
//步骤 3.2：公共块位于循环内时，把一次性物化移到最外层规范循环的预头；
//缺少规范预头时退回函数入口，确保生成的基值仍支配全部使用点。
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
//步骤 1.1：判定操作数是否必须保留字面量形态。GEP 索引、调用参数、
//select 操作数以及目标相关算术均不参与提升，避免遮蔽后续专用匹配。
bool ConstantHoistPass::isLiteralSensitiveUse(
    const Instruction *User, size_t OperandIndex) {
    if (!User) return true;
    //GEP 的首操作数是基地址；其余操作数是应由地址选择直接消费的索引。
    if (dynamic_cast<const GetElementPtrInst *>(User))
        return OperandIndex > 0;
    //调用约定和条件选择都可能依赖常量形态或路径局部性，统一保守排除。
    if (dynamic_cast<const CallInst *>(User)) return true;
    if (dynamic_cast<const SelectInst *>(User)) return true;
    if (auto *BI = dynamic_cast<const BinaryInst *>(User)) {
        BinaryInst::Operation Op = BI->getOp();
        if (BinaryInst::isCompareOp(Op)) return true;
        switch (Op) {
        case BinaryInst::DIV:
        case BinaryInst::UDIV:
        case BinaryInst::MOD:
        case BinaryInst::UREM:
            //保留常量除数，供除余强度削弱识别。
            return OperandIndex == 1;
        case BinaryInst::SHL:
        case BinaryInst::LSHR:
        case BinaryInst::ASHR:
            //移位量通常由目标立即数字段直接编码。
            return OperandIndex == 1;
        case BinaryInst::MUL:
        case BinaryInst::AND:
        case BinaryInst::OR:
        case BinaryInst::XOR:
            //乘常量及位掩码具有独立的后端匹配规则。
            return true;
        case BinaryInst::ADD:
        case BinaryInst::SUB:
            return false;
        default:
            return true;
        }
    }
    //当前仅明确允许 store 的值操作数；未知指令种类默认不改写。
    return User->instType != Instruction::STORE;
}
//步骤 1.2：遍历全部非 PHI 操作数，按值为可提升的大 i32 常量建桶，同时
//记录使用点和最大循环深度；这些信息分别服务支配性检查和动态收益估计。
ConstantHoistCollectResult ConstantHoistPass::collectCandidates(
    Function &F, const LoopInfo &LI,
    const TargetCostInfo &TargetCost) {
    ConstantHoistCollectResult R;
    forEachInstruction(
        F, [&](BasicBlock *BB, Instruction *I) {
        unsigned depth = 0;
        if (Loop *L = LI.getLoopFor(BB)) depth = L->getDepth();
        //PHI 的值定义在对应入边，不能使用普通块内插入点统一替换。
        if (dynamic_cast<PhiInst *>(I)) return;
        forEachOperand(
            I, [&](size_t OperandIndex, Use *u, Value *v) {
                if (!u) return;
                if (auto *ci = dynamic_cast<ConstInt *>(v)) {
                    if (isLiteralSensitiveUse(I, OperandIndex)) return;
                    //重组表达式固定使用i32；具体使用点能够直接吸收的立即数
                    //以及零寄存器常量都无需公共物化。
                    if (!IRType::hasIntegerBitWidth(
                            ci->getIRType(), 32))
                        return;
                    const int32_t val = static_cast<int32_t>(ci->getVal());
                    const int MaterializeCost =
                        TargetCost.getIntImmediateOperandCost(
                            I->instType,
                            static_cast<unsigned>(OperandIndex), val, 32,
                            TargetCostKind::CodeSize);
                    if (MaterializeCost <= 0 ||
                        MaterializeCost >= TargetCostInfo::InvalidCost)
                        return;
                    auto &C = R.intBuckets[val];
                    C.value = val;
                    C.uses.push_back({u, I, BB, depth});
                    if (depth > C.maxDepth) C.maxDepth = depth;
                }
            });
    });
    return R;
}
//步骤 2.2：完成一个窗口的收益筛选，并以使用次数最多的成员作为公共基值。
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
//步骤 2.1：按常量值排序后构造立即数可编码的连续窗口。窗口最大值与最小值
//之差可由目标ADD立即数字段吸收，因此任一成员到基值的偏移都可直接编码。
std::vector<ConstantHoistGroup> ConstantHoistPass::groupByBaseOffset(
    std::vector<ConstantHoistCandidate> &sorted,
    const TargetCostInfo &TargetCost) {
    std::vector<ConstantHoistGroup> groups;
    if (sorted.empty()) return groups;
    ConstantHoistGroup cur;
    cur.members.push_back(std::move(sorted[0]));
    int64_t windowMin = cur.members[0].value;
    for (size_t i = 1; i < sorted.size(); ++i) {
        int64_t diff = (int64_t)sorted[i].value - windowMin;
        const bool FitsWindow = TargetCost.isLegalIntImmediate(
            Instruction::ADD, 1, diff, 64);
        if (FitsWindow) {
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
//步骤 4：在公共支配块生成一次基值；基值成员直接复用，其余成员按使用块
//在最早使用前各生成一条 rebase，避免无谓扩大偏移值的活跃范围。
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
        //不同基本块分别物化偏移，块内的同值使用共享同一条加法。
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
//收集、分组并物化函数中可共享的大整数常量。
PreservationStatus ConstantHoistPass::run(Function &F,
                                          FunctionAnalysisPassManager &FAM) {
    if (F.size() == 0) return PreservationStatus::all();
    BasicBlock *entry = F.front();
    if (!entry) return PreservationStatus::all();
    auto &DT = FAM.getResult<DominantAnalysis>(F);
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    const auto &TargetCost = FAM.getResult<TargetCostAnalysis>(F);
    //1.发现候选：过滤字面量敏感位置，并按大整数值汇总使用点。
    ConstantHoistCollectResult R = collectCandidates(F, LI, TargetCost);
    //2.合并候选：排序后按可编码偏移分组，剔除总使用次数不足的窗口。
    std::vector<ConstantHoistCandidate> intSorted;
    intSorted.reserve(R.intBuckets.size());
    for (auto &kv : R.intBuckets) intSorted.push_back(std::move(kv.second));
    std::sort(intSorted.begin(), intSorted.end(),
              [](const ConstantHoistCandidate &a, const ConstantHoistCandidate &b) {
                  return a.value < b.value;
              });
    std::vector<ConstantHoistGroup> intGroups =
        groupByBaseOffset(intSorted, TargetCost);
    bool changed = false;
    for (ConstantHoistGroup &G : intGroups) {
        unsigned maxDepth = 0;
        size_t totalUses = 0;
        for (auto &m : G.members) {
            if (m.maxDepth > maxDepth) maxDepth = m.maxDepth;
            totalUses += m.uses.size();
        }
        //2.1 直线代码需要更多复用才抵消基值物化成本；循环内复用按动态
        //执行收益放宽到通用阈值。
        if (maxDepth == 0 &&
            (int)totalUses < StraightLineUseThreshold)
            continue;
        std::vector<ConstantHoistUseSite> allUses;
        for (auto &m : G.members)
            for (auto &u : m.uses) allUses.push_back(u);
        //3.合法性验证：选择支配所有use的插入块，并拒绝把常量物化
        //跨越计时等顺序屏障搬到区间外。
        BasicBlock *common = foldCommonDominator(DT, allUses);
        BasicBlock *insertBB = pickInsertionBlock(common, entry, LI);
        if (!insertBB) insertBB = entry;
        Instruction *StartAfter = nullptr;
        for (Instruction *I : *insertBB) {
            if (!dynamic_cast<PhiInst *>(I)) break;
            StartAfter = I;
        }
        bool CrossesBarrier = false;
        for (const ConstantHoistUseSite &Use : allUses) {
            if (!Use.inst || hasOrderingBarrierOnPath(
                    F, insertBB, StartAfter, Use.inst, FAM)) {
                CrossesBarrier = true;
                break;
            }
        }
        if (CrossesBarrier) continue;
        //4.IR 改写：生成公共基值、块内偏移值，并替换整组常量使用。
        changed |= materializeGroup(G, insertBB);
    }
    if (!changed) return PreservationStatus::all();
    //5.分析失效：仅改变 SSA 表达式，CFG、支配树和循环结构保持有效。
    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<LoopAnalysis>();
    return PA;
}
