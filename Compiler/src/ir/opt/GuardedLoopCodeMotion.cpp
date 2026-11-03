// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#include "ir/opt/GuardedLoopCodeMotion.hpp"

#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "ir/analysis/TargetCostAnalysis.hpp"
#include "ir/opt/DCE.hpp"
#include "ir/opt/LCSSA.hpp"
#include "lib/IRUtils.hpp"

#include <algorithm>

//判断一条指令是否可以从无用户路径中安全删除，并能按 SSA 映射复制
bool GuardedLoopCodeMotionPass::isSafeScalarInstruction(Instruction *I) {
    if (!I || !I->getParent() || !I->getIRType() ||
        !I->getIRType()->isScalar() || dceInstructionMustBePreserved(I) ||
        instructionMayFaultOrProducePoison(I) ||
        valueMayDependOnUndef(I, 256)) {
        return false;
    }
    //首版只接受当前 CFG/BaseManager 已完整支持 clone 的标量值节点；
    //不移动 load、GEP、调用和任何内存状态，避免借助越界或别名假设。
    return dynamic_cast<BinaryInst *>(I) ||
        dynamic_cast<SelectInst *>(I) ||
        dynamic_cast<TruncInst *>(I) ||
        dynamic_cast<ZextInst *>(I) ||
        dynamic_cast<SextInst *>(I) ||
        dynamic_cast<BitCastInst *>(I) ||
        dynamic_cast<SITOFPInst *>(I);
}

//验证循环外定义在入口条件克隆点之前可用。
bool GuardedLoopCodeMotionPass::isAvailableAtPreheader(
    Value *V, BasicBlock *PreHeader, const DominatorTree &DT) {
    if (!V || !PreHeader || !PreHeader->getTerminator()) return false;
    if (dynamic_cast<ConstantSystem *>(V) || V->isParam() ||
        V->isGlobal() || dynamic_cast<Function *>(V) ||
        dynamic_cast<BuildInFunction *>(V)) {
        return true;
    }
    auto *Definition = dynamic_cast<Instruction *>(V);
    if (!Definition || !Definition->getParent()) return false;
    BasicBlock *DefinitionBlock = Definition->getParent();
    if (DefinitionBlock != PreHeader)
        return DT.dominates(DefinitionBlock, PreHeader);
    for (Instruction *Current : *PreHeader) {
        if (Current == Definition) return true;
        if (Current == PreHeader->getTerminator()) return false;
    }
    return false;
}

//沿预头内部 def-use 反向闭包保护全部依赖，避免只保留顶层定义而把其
//操作数下沉到守卫真边，造成入口条件或零次出口 use 不再受定义支配。
bool GuardedLoopCodeMotionPass::protectPreheaderDependency(
    Value *Root, BasicBlock *PreHeader,
    std::unordered_set<Instruction *> &Protected,
    std::size_t &Visited) {
    if (!Root || !PreHeader || Visited > MaxCandidateCount * 4U)
        return false;
    auto *Definition = dynamic_cast<Instruction *>(Root);
    if (!Definition || Definition->getParent() != PreHeader) return true;
    if (!Protected.insert(Definition).second) return true;
    ++Visited;
    for (std::size_t Index = 0; Index < Definition->getNumOperands(); ++Index)
        if (!protectPreheaderDependency(
                Definition->getOperand(Index), PreHeader,
                Protected, Visited)) {
            return false;
        }
    return true;
}

//收集所有 Header 普通计算，并标记其在预头中的依赖，防止下沉后入口克隆失去操作数。
bool GuardedLoopCodeMotionPass::collectHeaderCloneOrder(
    BasicBlock *Header, BasicBlock *PreHeader,
    BasicBlock *ExitBlock, const DominatorTree &DT,
    std::vector<Instruction *> &Order,
    std::unordered_set<Instruction *> &ProtectedPreheaderValues) {
    Order.clear();
    ProtectedPreheaderValues.clear();
    if (!Header || !PreHeader || !ExitBlock) return false;
    auto *HeaderBranch = dynamic_cast<BrInst *>(Header->getTerminator());
    if (!HeaderBranch || !HeaderBranch->isConditional()) return false;

    std::unordered_set<Instruction *> HeaderValues;
    std::size_t ProtectedVisited = 0;
    //所有 Header PHI 的首次值都可能参与条件克隆或零次出口映射；统一保护
    //其预头依赖闭包，避免后续候选筛选遗漏间接依赖。
    for (PhiInst *Phi : Header->collectPhiNodes()) {
        unsigned InitialCount = 0;
        Value *InitialValue = nullptr;
        for (unsigned Index = 0; Index < Phi->getNumIncomingValues(); ++Index) {
            if (Phi->getIncomingBlock(Index) != PreHeader) continue;
            InitialValue = Phi->getIncomingValue(Index);
            ++InitialCount;
        }
        if (InitialCount != 1 || !InitialValue ||
            !protectPreheaderDependency(
                InitialValue, PreHeader, ProtectedPreheaderValues,
                ProtectedVisited)) {
            return false;
        }
    }
    for (Instruction *I : *Header) {
        if (dynamic_cast<PhiInst *>(I) || I == HeaderBranch) continue;
        if (!isSafeScalarInstruction(I) ||
            Order.size() >= MaxHeaderCloneCount) {
            return false;
        }
        Order.push_back(I);
        HeaderValues.insert(I);
    }
    if (Order.empty() || HeaderValues.count(
            dynamic_cast<Instruction *>(HeaderBranch->getCondition())) == 0) {
        return false;
    }

    //Header 指令必须按块内拓扑顺序仅依赖 PHI、较早 Header 值或循环外值。
    std::unordered_set<Instruction *> SeenHeaderValues;
    for (Instruction *I : Order) {
        for (std::size_t Index = 0; Index < I->getNumOperands(); ++Index) {
            Value *Operand = I->getOperand(Index);
            auto *OperandInstruction =
                dynamic_cast<Instruction *>(Operand);
            if (auto *Phi = dynamic_cast<PhiInst *>(OperandInstruction)) {
                if (Phi->getParent() == Header) continue;
            }
            if (OperandInstruction &&
                OperandInstruction->getParent() == Header) {
                if (SeenHeaderValues.count(OperandInstruction) == 0)
                    return false;
                continue;
            }
            if (!isAvailableAtPreheader(Operand, PreHeader, DT))
                return false;
            if (!protectPreheaderDependency(
                    Operand, PreHeader, ProtectedPreheaderValues,
                    ProtectedVisited))
                return false;
        }
        SeenHeaderValues.insert(I);
    }

    //零次旁路会成为 Exit 的新前驱；其每个 PHI 的 Header 入值必须能由
    //初始 PHI 或上面已克隆的 Header 计算精确重建。
    for (PhiInst *ExitPhi : ExitBlock->collectPhiNodes()) {
        bool HasHeaderIncoming = false;
        for (unsigned Index = 0;
             Index < ExitPhi->getNumIncomingValues(); ++Index) {
            if (ExitPhi->getIncomingBlock(Index) != Header) continue;
            if (HasHeaderIncoming) return false;
            HasHeaderIncoming = true;
            Value *Incoming = ExitPhi->getIncomingValue(Index);
            auto *IncomingInstruction =
                dynamic_cast<Instruction *>(Incoming);
            if (IncomingInstruction &&
                IncomingInstruction->getParent() == Header &&
                !dynamic_cast<PhiInst *>(IncomingInstruction) &&
                HeaderValues.count(IncomingInstruction) == 0) {
                return false;
            }
            if (IncomingInstruction &&
                IncomingInstruction->getParent() != Header &&
                !isAvailableAtPreheader(Incoming, PreHeader, DT)) {
                return false;
            }
            if (!protectPreheaderDependency(
                    Incoming, PreHeader, ProtectedPreheaderValues,
                    ProtectedVisited))
                return false;
        }
        if (!HasHeaderIncoming) return false;
    }
    return true;
}

//识别“条件前驱 -> 预头或另一分支”的标准入口守卫，避免 LoopSimplify
//插入专用出口后因出口块身份变化而在固定点中重复包裹。
bool GuardedLoopCodeMotionPass::hasExistingEntryGuard(
    BasicBlock *PreHeader, BasicBlock *ExitBlock) {
    (void)ExitBlock;
    if (!PreHeader || !ExitBlock) return false;
    std::vector<BasicBlock *> Predecessors = PreHeader->getPredecessors();
    if (Predecessors.size() != 1 || !Predecessors.front()) return false;
    auto *Branch = dynamic_cast<BrInst *>(
        Predecessors.front()->getTerminator());
    if (!Branch || !Branch->isConditional()) return false;
    BasicBlock *TrueSuccessor = Branch->getTrueSuccessor();
    BasicBlock *FalseSuccessor = Branch->getFalseSuccessor();
    return TrueSuccessor && FalseSuccessor &&
        TrueSuccessor != FalseSuccessor &&
        (TrueSuccessor == PreHeader || FalseSuccessor == PreHeader);
}

//从预头中找出所有用户都在循环内的纯 SSA 闭包，并用目标吞吐成本判断收益。
bool GuardedLoopCodeMotionPass::collectCandidates(
    Function &F, FunctionAnalysisPassManager &FAM,
    const Loop &L, BasicBlock *PreHeader,
    const std::unordered_set<Instruction *> &ProtectedPreheaderValues,
    const TargetCostInfo &TargetCost,
    const std::vector<Instruction *> &HeaderCloneOrder,
    std::vector<Instruction *> &Candidates) {
    Candidates.clear();
    if (!PreHeader) return false;
    std::unordered_set<Instruction *> CandidateSet;
    for (Instruction *I : *PreHeader) {
        if (dynamic_cast<PhiInst *>(I) || dynamic_cast<BrInst *>(I) ||
            ProtectedPreheaderValues.count(I) != 0 ||
            !isSafeScalarInstruction(I)) {
            continue;
        }
        //下沉不得跨越同一预头中位于候选之后的计时、IO 或其他顺序屏障。
        if (hasOrderingBarrierOnPath(
                F, I, PreHeader->getTerminator(), FAM))
            continue;
        if (Candidates.size() >= MaxCandidateCount) return false;
        Candidates.push_back(I);
        CandidateSet.insert(I);
    }

    //若候选的任一结果在循环外可见，就不能让零次路径跳过其定义。
    bool Changed = true;
    while (Changed) {
        Changed = false;
        for (auto It = Candidates.begin(); It != Candidates.end();) {
            Instruction *I = *It;
            bool Valid = I && I->hasUses();
            if (Valid) {
                for (Use *U : I->getUses()) {
                    auto *UserInstruction = U
                        ? dynamic_cast<Instruction *>(U->getUser())
                        : nullptr;
                    if (!UserInstruction || !UserInstruction->getParent()) {
                        Valid = false;
                        break;
                    }
                    if (CandidateSet.count(UserInstruction) != 0) continue;
                    if (!L.contains(UserInstruction->getParent())) {
                        Valid = false;
                        break;
                    }
                }
            }
            if (Valid) {
                ++It;
                continue;
            }
            CandidateSet.erase(I);
            It = Candidates.erase(It);
            Changed = true;
        }
    }
    if (Candidates.empty()) return false;

    int MovedCost = 0;
    for (Instruction *I : Candidates) {
        int Cost = TargetCost.getInstructionCost(
            I, TargetCostKind::Throughput);
        if (Cost <= 0 || Cost >= TargetCostInfo::InvalidCost) return false;
        MovedCost = std::min(TargetCostInfo::InvalidCost,
                             MovedCost + Cost);
    }
    int GuardCost = 1;
    for (Instruction *I : HeaderCloneOrder) {
        int Cost = TargetCost.getInstructionCost(
            I, TargetCostKind::Throughput);
        if (Cost <= 0 || Cost >= TargetCostInfo::InvalidCost) return false;
        GuardCost = std::min(TargetCostInfo::InvalidCost,
                             GuardCost + Cost);
    }
    return MovedCost >= MinimumMovedCost && MovedCost >= GuardCost * 2;
}

//检查规范头测循环、唯一出口、可克隆入口条件与可获益下沉闭包。
bool GuardedLoopCodeMotionPass::buildPlan(
    Function &F, FunctionAnalysisPassManager &FAM,
    Loop &L, const DominatorTree &DT,
    const TargetCostInfo &TargetCost, GuardPlan &Plan) {
    Plan = GuardPlan{};
    BasicBlock *PreHeader = nullptr;
    BasicBlock *Latch = nullptr;
    if (!L.getLoopSimplifyForm(PreHeader, Latch) ||
        !PreHeader || !Latch || !L.getHeader() ||
        !PreHeader->getTerminator() || !L.isInnermost() ||
        !LCSSAPass::isLCSSAForm(&L)) {
        return false;
    }
    auto *PreHeaderBranch = dynamic_cast<BrInst *>(
        PreHeader->getTerminator());
    if (!PreHeaderBranch || PreHeaderBranch->isConditional() ||
        PreHeaderBranch->getTrueSuccessor() != L.getHeader()) {
        return false;
    }
    BasicBlock *Header = L.getHeader();
    auto *HeaderBranch = dynamic_cast<BrInst *>(Header->getTerminator());
    if (!HeaderBranch || !HeaderBranch->isConditional()) return false;
    BasicBlock *TrueSuccessor = HeaderBranch->getTrueSuccessor();
    BasicBlock *FalseSuccessor = HeaderBranch->getFalseSuccessor();
    const bool TrueInside = TrueSuccessor && L.contains(TrueSuccessor);
    const bool FalseInside = FalseSuccessor && L.contains(FalseSuccessor);
    if (TrueInside == FalseInside) return false;
    BasicBlock *EnterBlock = TrueInside ? TrueSuccessor : FalseSuccessor;
    BasicBlock *ExitBlock = TrueInside ? FalseSuccessor : TrueSuccessor;
    if (!EnterBlock || !ExitBlock || L.contains(ExitBlock) ||
        hasExistingEntryGuard(PreHeader, ExitBlock)) {
        return false;
    }

    //初版要求 Header 是唯一退出点，保证新增零次边只需修复一个出口。
    std::vector<BasicBlock *> ExitBlocks = L.getExitBlocks();
    if (ExitBlocks.size() != 1 || ExitBlocks.front() != ExitBlock)
        return false;
    for (BasicBlock *Block : L.getBlocksIncludingSubLoops()) {
        if (Block != Header && L.isExitingBlock(Block)) return false;
    }

    std::unordered_set<Instruction *> ProtectedPreheaderValues;
    std::vector<Instruction *> HeaderCloneOrder;
    if (!collectHeaderCloneOrder(
            Header, PreHeader, ExitBlock, DT, HeaderCloneOrder,
            ProtectedPreheaderValues)) {
        return false;
    }
    std::vector<Instruction *> Candidates;
    if (!collectCandidates(
            F, FAM, L, PreHeader, ProtectedPreheaderValues, TargetCost,
            HeaderCloneOrder, Candidates)) {
        return false;
    }

    Plan.TargetLoop = &L;
    Plan.PreHeader = PreHeader;
    Plan.Header = Header;
    Plan.Latch = Latch;
    Plan.EnterBlock = EnterBlock;
    Plan.ExitBlock = ExitBlock;
    Plan.HeaderBranch = HeaderBranch;
    Plan.EnterOnTrue = TrueInside;
    Plan.HeaderCloneOrder = std::move(HeaderCloneOrder);
    Plan.Candidates = std::move(Candidates);
    return true;
}

//提交已预检计划；所有可能失败的 clone 与零次值映射均在改 CFG 前完成。
bool GuardedLoopCodeMotionPass::applyPlan(
    Function &F, const GuardPlan &Plan) {
    if (!Plan.TargetLoop || !Plan.PreHeader || !Plan.Header ||
        !Plan.Latch || !Plan.EnterBlock || !Plan.ExitBlock ||
        !Plan.HeaderBranch || Plan.Candidates.empty()) {
        return false;
    }

    std::unordered_map<Value *, Value *> ValueMap;
    for (PhiInst *Phi : Plan.Header->collectPhiNodes()) {
        Value *InitialValue = nullptr;
        unsigned InitialCount = 0;
        for (unsigned Index = 0; Index < Phi->getNumIncomingValues(); ++Index) {
            if (Phi->getIncomingBlock(Index) == Plan.PreHeader) {
                InitialValue = Phi->getIncomingValue(Index);
                ++InitialCount;
            }
        }
        if (InitialCount != 1 || !InitialValue) return false;
        ValueMap[Phi] = InitialValue;
    }

    std::vector<Instruction *> Clones;
    for (Instruction *Original : Plan.HeaderCloneOrder) {
        Instruction *Clone = Original ? Original->clone(ValueMap) : nullptr;
        if (!Clone) {
            for (auto It = Clones.rbegin(); It != Clones.rend(); ++It)
                delete *It;
            return false;
        }
        Clones.push_back(Clone);
        ValueMap[Original] = Clone;
    }
    auto ConditionIt = ValueMap.find(Plan.HeaderBranch->getCondition());
    Value *GuardCondition = ConditionIt == ValueMap.end()
        ? Plan.HeaderBranch->getCondition() : ConditionIt->second;
    if (!GuardCondition || !GuardCondition->getIRType() ||
        !GuardCondition->getIRType()->isBool()) {
        for (auto It = Clones.rbegin(); It != Clones.rend(); ++It)
            delete *It;
        return false;
    }

    std::vector<std::pair<PhiInst *, Value *>> ZeroTripIncoming;
    for (PhiInst *ExitPhi : Plan.ExitBlock->collectPhiNodes()) {
        Value *HeaderValue = nullptr;
        for (unsigned Index = 0;
             Index < ExitPhi->getNumIncomingValues(); ++Index) {
            if (ExitPhi->getIncomingBlock(Index) == Plan.Header) {
                if (HeaderValue) {
                    for (auto It = Clones.rbegin(); It != Clones.rend(); ++It)
                        delete *It;
                    return false;
                }
                HeaderValue = ExitPhi->getIncomingValue(Index);
            }
        }
        if (!HeaderValue) {
            for (auto It = Clones.rbegin(); It != Clones.rend(); ++It)
                delete *It;
            return false;
        }
        auto Mapped = ValueMap.find(HeaderValue);
        Value *ZeroValue = Mapped == ValueMap.end()
            ? HeaderValue : Mapped->second;
        auto *ZeroInstruction = dynamic_cast<Instruction *>(ZeroValue);
        if (ZeroInstruction && ZeroInstruction->getParent() &&
            ZeroInstruction->getParent() != Plan.PreHeader) {
            for (auto It = Clones.rbegin(); It != Clones.rend(); ++It)
                delete *It;
            return false;
        }
        ZeroTripIncoming.push_back({ExitPhi, ZeroValue});
    }

    //至此所有失败条件已排除，以下仅调用 CFG/BaseManager 的确定性接口。
    for (Instruction *Clone : Clones)
        Plan.PreHeader->insertBeforeTerminator(Clone);
    auto *GuardBlock = new BasicBlock();
    GuardBlock->setIdent(F.makeUniqueBlockIdent(
        Plan.Header->getIdent() + ".guarded.preheader"));
    F.pushBlock(GuardBlock);
    for (Instruction *Candidate : Plan.Candidates) {
        Candidate->eraseFromParent();
        GuardBlock->pushBack(Candidate);
    }
    GuardBlock->genUnCondInst(Plan.Header);

    auto *NewBranch = Plan.EnterOnTrue
        ? new BrInst(GuardCondition, GuardBlock, Plan.ExitBlock)
        : new BrInst(GuardCondition, Plan.ExitBlock, GuardBlock);
    if (!Plan.PreHeader->replaceTerminator(NewBranch)) return false;
    for (PhiInst *Phi : Plan.Header->collectPhiNodes()) {
        for (unsigned Index = 0; Index < Phi->getNumIncomingValues(); ++Index) {
            if (Phi->getIncomingBlock(Index) == Plan.PreHeader)
                Phi->setIncomingBlock(Index, GuardBlock);
        }
    }
    for (const auto &Incoming : ZeroTripIncoming)
        Incoming.first->addIncoming(Incoming.second, Plan.PreHeader);
    return true;
}

//同一 SSA 定义天然相等；常量则复用 Value 的类型敏感比较，避免把不同
//浮点位模式、不同整数位宽或 undef 错当成相同状态。
bool GuardedLoopCodeMotionPass::sameScalarValue(
    Value *Left, Value *Right) {
    if (!Left || !Right || Value::isUndef(Left) || Value::isUndef(Right))
        return false;
    if (Left == Right) return true;
    return Left->isConst() && Right->isConst() &&
        Value::areEqualConstants(Left, Right);
}

//父状态必须沿“父头 -> 子预头 -> 子头 -> 父锁存”形成一一对应的 PHI
//通道；子循环零次时原样透传，非零退出时回传子 PHI 的最终值。
bool GuardedLoopCodeMotionPass::matchSaturatedStatePhi(
    PhiInst *ParentPhi, BasicBlock *ParentPreHeader,
    BasicBlock *ParentHeader, BasicBlock *ParentLatch,
    BasicBlock *ChildPreHeader, BasicBlock *ChildHeader,
    BasicBlock *ChildLatch, PhiInst *&ChildPhi,
    PhiInst *&MergePhi) {
    ChildPhi = nullptr;
    MergePhi = nullptr;
    if (!ParentPhi || !ParentPreHeader || !ParentHeader ||
        !ParentLatch || !ChildPreHeader || !ChildHeader || !ChildLatch ||
        ParentPhi->getParent() != ParentHeader ||
        ParentPhi->getNumIncomingValues() != 2 ||
        ParentPhi->countIncomingFromPred(ParentPreHeader) != 1 ||
        ParentPhi->countIncomingFromPred(ParentLatch) != 1) {
        return false;
    }
    MergePhi = dynamic_cast<PhiInst *>(
        ParentPhi->getIncomingFromPred(ParentLatch));
    if (!MergePhi || MergePhi->getParent() != ParentLatch ||
        MergePhi->getNumIncomingValues() != 2 ||
        MergePhi->countIncomingFromPred(ParentHeader) != 1 ||
        MergePhi->countIncomingFromPred(ChildHeader) != 1 ||
        MergePhi->getIncomingFromPred(ParentHeader) != ParentPhi) {
        return false;
    }
    ChildPhi = dynamic_cast<PhiInst *>(
        MergePhi->getIncomingFromPred(ChildHeader));
    if (!ChildPhi || ChildPhi->getParent() != ChildHeader ||
        ChildPhi->getNumIncomingValues() != 2 ||
        ChildPhi->countIncomingFromPred(ChildPreHeader) != 1 ||
        ChildPhi->countIncomingFromPred(ChildLatch) != 1 ||
        ChildPhi->getIncomingFromPred(ChildPreHeader) != ParentPhi) {
        ChildPhi = nullptr;
        return false;
    }
    return true;
}

//把子循环退出比较中的子 PHI 映射回对应父状态 PHI；若两条有效继续
//谓词完全一致，则子循环从退出边回传后，下一次父迭代必定仍选择零次旁路。
bool GuardedLoopCodeMotionPass::matchSaturationCondition(
    BrInst *ParentHeaderBranch, BasicBlock *ChildPreHeader,
    BrInst *ChildHeaderBranch, const Loop &Child,
    const std::unordered_map<PhiInst *, PhiInst *> &StateMap) {
    if (!ParentHeaderBranch || !ParentHeaderBranch->isConditional() ||
        !ChildPreHeader || !ChildHeaderBranch ||
        !ChildHeaderBranch->isConditional() || StateMap.empty()) {
        return false;
    }
    const bool ParentContinueOnTrue =
        ParentHeaderBranch->getTrueSuccessor() == ChildPreHeader;
    if (!ParentContinueOnTrue &&
        ParentHeaderBranch->getFalseSuccessor() != ChildPreHeader) {
        return false;
    }
    const bool ChildContinueOnTrue = Child.contains(
        ChildHeaderBranch->getTrueSuccessor());
    const bool ChildContinueOnFalse = Child.contains(
        ChildHeaderBranch->getFalseSuccessor());
    if (ChildContinueOnTrue == ChildContinueOnFalse) return false;
    auto *ParentCompare = dynamic_cast<BinaryInst *>(
        ParentHeaderBranch->getCondition());
    auto *ChildCompare = dynamic_cast<BinaryInst *>(
        ChildHeaderBranch->getCondition());
    if (!ParentCompare || !ChildCompare ||
        !BinaryInst::isCompareOp(ParentCompare->getOp()) ||
        !BinaryInst::isCompareOp(ChildCompare->getOp()) ||
        !ParentCompare->getOperand(0) || !ParentCompare->getOperand(1) ||
        !ChildCompare->getOperand(0) || !ChildCompare->getOperand(1) ||
        !ParentCompare->getOperand(0)->getIRType() ||
        !ParentCompare->getOperand(1)->getIRType() ||
        !ChildCompare->getOperand(0)->getIRType() ||
        !ChildCompare->getOperand(1)->getIRType() ||
        !ParentCompare->getOperand(0)->getIRType()->isInt() ||
        !ParentCompare->getOperand(1)->getIRType()->isInt() ||
        !ChildCompare->getOperand(0)->getIRType()->isInt() ||
        !ChildCompare->getOperand(1)->getIRType()->isInt() ||
        valueMayDependOnUndef(ParentCompare, 256) ||
        valueMayDependOnUndef(ChildCompare, 256)) {
        return false;
    }
    //浮点有序比较在 NaN 上不满足“假边谓词等于反谓词”，饱和证明只处理
    //整数关系，避免把 unordered 路径错误折叠成一次执行。
    BinaryInst::Operation ParentPredicate = ParentContinueOnTrue
        ? ParentCompare->getOp()
        : BinaryInst::negatePredicate(ParentCompare->getOp());
    BinaryInst::Operation ChildPredicate = ChildContinueOnTrue
        ? ChildCompare->getOp()
        : BinaryInst::negatePredicate(ChildCompare->getOp());

    std::unordered_map<PhiInst *, PhiInst *> ChildToParent;
    for (const auto &Entry : StateMap) {
        if (!Entry.first || !Entry.second ||
            !ChildToParent.emplace(Entry.second, Entry.first).second) {
            return false;
        }
    }
    bool UsesSaturatedState = false;
    auto MapChildOperand = [&](Value *Operand) -> Value * {
        auto *Phi = dynamic_cast<PhiInst *>(Operand);
        auto It = Phi ? ChildToParent.find(Phi) : ChildToParent.end();
        if (It == ChildToParent.end()) return Operand;
        UsesSaturatedState = true;
        return It->second;
    };
    Value *MappedChildLeft = MapChildOperand(ChildCompare->getOperand(0));
    Value *MappedChildRight = MapChildOperand(ChildCompare->getOperand(1));
    Value *ParentLeft = ParentCompare->getOperand(0);
    Value *ParentRight = ParentCompare->getOperand(1);
    const bool SameOrientation = ParentPredicate == ChildPredicate &&
        sameScalarValue(ParentLeft, MappedChildLeft) &&
        sameScalarValue(ParentRight, MappedChildRight);
    const bool ReverseOrientation =
        ParentPredicate == BinaryInst::reversePredicate(ChildPredicate) &&
        sameScalarValue(ParentLeft, MappedChildRight) &&
        sameScalarValue(ParentRight, MappedChildLeft);
    return UsesSaturatedState && (SameOrientation || ReverseOrientation);
}

//父归纳变量的最终值会因折叠而改变，因此仅当其 SSA 图严格局限于更新、
//退出比较和自身回边时才允许改写；任何 exit PHI 或普通计算使用都会拒绝。
bool GuardedLoopCodeMotionPass::hasOnlyParentControlUses(
    const InductionDescriptor &Induction,
    const LoopTripCountInfo &Trip,
    BrInst *ParentLatchBranch) {
    auto *Update = dynamic_cast<Instruction *>(Induction.UpdateValue);
    if (!Induction.Valid || !Induction.Phi || !Update || !Trip.ExitCmp ||
        !ParentLatchBranch || Update->getParent() != Trip.Latch ||
        Trip.ExitingBlock != Trip.Latch ||
        ParentLatchBranch->getCondition() != Trip.ExitCmp) {
        return false;
    }
    for (Use *U : Induction.Phi->getUses()) {
        auto *User = U ? dynamic_cast<Instruction *>(U->getUser()) : nullptr;
        if (User != Update && User != Trip.ExitCmp) return false;
    }
    for (Use *U : Update->getUses()) {
        auto *User = U ? dynamic_cast<Instruction *>(U->getUser()) : nullptr;
        if (User != Induction.Phi && User != Trip.ExitCmp) return false;
    }
    return Trip.ExitCmp->hasExactlyOneUse() &&
        Trip.ExitCmp->getUses().front()->getUser() == ParentLatchBranch;
}

//识别“父循环重复驱动一个会把控制状态推进到退出边界的子循环”。证明只
//依赖有限循环、精确 CFG 边和 SSA PHI 通道，不依赖源码名称或运行期输入。
bool GuardedLoopCodeMotionPass::buildSaturatedParentPlan(
    Loop &Parent, SCEVResult &SE, RangeInfo &RI,
    DominatorTree &DT,
    SaturatedParentPlan &Plan) {
    Plan = SaturatedParentPlan{};
    if (Parent.getSubLoops().size() != 1) {
        return false;
    }
    Loop *Child = Parent.getSubLoops().front();
    if (!Child || !Child->isInnermost()) {
        return false;
    }
    BasicBlock *ParentPreHeader = nullptr;
    BasicBlock *ParentLatch = nullptr;
    BasicBlock *ChildPreHeader = nullptr;
    BasicBlock *ChildLatch = nullptr;
    if (!Parent.getLoopSimplifyForm(ParentPreHeader, ParentLatch) ||
        !ParentPreHeader || !ParentLatch) {
        return false;
    }
    //子循环出口同时是父层零次旁路的汇合块，按定义不会是 dedicated exit；
    //这里只直接验证唯一外部预头、唯一内部回边和预头单后继。
    BasicBlock *ChildHeader = Child->getHeader();
    if (!ChildHeader) return false;
    for (BasicBlock *Predecessor : ChildHeader->getPredecessors()) {
        if (!Predecessor) return false;
        if (Child->contains(Predecessor)) {
            if (ChildLatch) return false;
            ChildLatch = Predecessor;
        } else {
            if (ChildPreHeader) return false;
            ChildPreHeader = Predecessor;
        }
    }
    if (!ChildPreHeader || !ChildLatch ||
        ChildPreHeader->getSuccessors().size() != 1 ||
        ChildPreHeader->getSuccessors().front() != ChildHeader) {
        return false;
    }
    BasicBlock *ParentHeader = Parent.getHeader();
    auto ParentExits = Parent.getExitBlocks();
    auto ChildExits = Child->getExitBlocks();
    if (!ParentHeader || !ChildHeader || ParentExits.size() != 1 ||
        ChildExits.size() != 1 || ChildExits.front() != ParentLatch) {
        return false;
    }
    BasicBlock *ParentExit = ParentExits.front();
    auto *ParentHeaderBranch = ParentHeader->getBranchTerminator();
    auto *ChildPreHeaderBranch = ChildPreHeader->getBranchTerminator();
    auto *ChildHeaderBranch = ChildHeader->getBranchTerminator();
    auto *ParentLatchBranch = ParentLatch->getBranchTerminator();
    if (!BasicBlock::isCondBr(ParentHeaderBranch) ||
        !BasicBlock::isUncondBr(ChildPreHeaderBranch) ||
        ChildPreHeaderBranch->getTrueSuccessor() != ChildHeader ||
        !BasicBlock::isCondBr(ChildHeaderBranch) ||
        !BasicBlock::isCondBr(ParentLatchBranch)) {
        return false;
    }
    const bool ParentHeaderShape =
        (ParentHeaderBranch->getTrueSuccessor() == ChildPreHeader &&
         ParentHeaderBranch->getFalseSuccessor() == ParentLatch) ||
        (ParentHeaderBranch->getFalseSuccessor() == ChildPreHeader &&
         ParentHeaderBranch->getTrueSuccessor() == ParentLatch);
    const bool ParentLatchShape =
        (ParentLatchBranch->getTrueSuccessor() == ParentHeader &&
         ParentLatchBranch->getFalseSuccessor() == ParentExit) ||
        (ParentLatchBranch->getFalseSuccessor() == ParentHeader &&
         ParentLatchBranch->getTrueSuccessor() == ParentExit);
    if (!ParentHeaderShape || !ParentLatchShape ||
        ParentHeader->countEdgesToSucc(ParentLatch) != 1 ||
        ChildHeader->countEdgesToSucc(ParentLatch) != 1 ||
        ChildPreHeader->getPredecessors().size() != 1 ||
        ChildPreHeader->getPredecessors().front() != ParentHeader) {
        return false;
    }
    std::unordered_set<BasicBlock *> ExpectedDirectBlocks{
        ParentHeader, ParentLatch, ChildPreHeader};
    if (Parent.getBlocks().size() != ExpectedDirectBlocks.size())
        return false;
    for (BasicBlock *Block : Parent.getBlocks())
        if (!Block || ExpectedDirectBlocks.count(Block) == 0) return false;

    auto ParentTrip = SE.analyzeLoopTripCount(&Parent);
    auto ParentInduction = SE.getCanonicalInduction(&Parent);
    if (!ParentTrip || !ParentTrip->Valid ||
        !SE.isKnownFiniteLoop(&Parent, RI, DT) ||
        ParentTrip->ExitingBlock != ParentLatch ||
        ParentTrip->ExitBlock != ParentExit || !ParentInduction ||
        !hasOnlyParentControlUses(
            *ParentInduction, *ParentTrip, ParentLatchBranch)) {
        return false;
    }
    for (BasicBlock *Block : Child->getBlocksIncludingSubLoops()) {
        if (Block != ChildHeader && Child->isExitingBlock(Block))
            return false;
    }
    if (!ParentHeaderBranch->getCondition()->hasExactlyOneUse() ||
        ParentHeaderBranch->getCondition()->getUses().front()->getUser() !=
            ParentHeaderBranch ||
        !ChildHeaderBranch->getCondition()->hasExactlyOneUse() ||
        ChildHeaderBranch->getCondition()->getUses().front()->getUser() !=
            ChildHeaderBranch) {
        return false;
    }

    std::unordered_map<PhiInst *, PhiInst *> StateMap;
    std::unordered_set<PhiInst *> MatchedMergePhis;
    std::vector<PhiInst *> ParentHeaderPhis =
        ParentHeader->collectPhiNodes();
    if (ParentHeaderPhis.size() < 2) return false;
    for (PhiInst *ParentPhi : ParentHeaderPhis) {
        if (!ParentPhi) return false;
        //没有 LCSSA 时父头 PHI 可能被出口直接观察；这种值在保留一次后
        //不一定仍代表原循环最终状态，因此要求其所有用户仍位于父循环内。
        for (Use *U : ParentPhi->getUses()) {
            auto *User = U ? dynamic_cast<Instruction *>(U->getUser())
                           : nullptr;
            if (!User || !User->getParent() ||
                !Parent.contains(User->getParent())) {
                return false;
            }
        }
        if (ParentPhi == ParentInduction->Phi) {
            if (ParentPhi->countIncomingFromPred(ParentPreHeader) != 1 ||
                ParentPhi->countIncomingFromPred(ParentLatch) != 1)
                return false;
            continue;
        }
        PhiInst *ChildPhi = nullptr;
        PhiInst *MergePhi = nullptr;
        if (!matchSaturatedStatePhi(
                ParentPhi, ParentPreHeader, ParentHeader, ParentLatch,
                ChildPreHeader, ChildHeader, ChildLatch, ChildPhi,
                MergePhi) ||
            !StateMap.emplace(ParentPhi, ChildPhi).second ||
            !MatchedMergePhis.insert(MergePhi).second) {
            return false;
        }
    }
    std::vector<PhiInst *> ParentLatchPhis =
        ParentLatch->collectPhiNodes();
    if (ParentLatchPhis.size() != MatchedMergePhis.size()) return false;
    for (PhiInst *Phi : ParentLatchPhis)
        if (!Phi || MatchedMergePhis.count(Phi) == 0) return false;
    //不动点结论要求每次观察到的是同一个确定状态。沿三层 PHI 通道反向
    //拒绝任何 undef 传递依赖；有界检查预算耗尽同样按不安全处理。
    for (const auto &Entry : StateMap) {
        PhiInst *ParentPhi = Entry.first;
        PhiInst *ChildPhi = Entry.second;
        PhiInst *MergePhi = ParentPhi
            ? dynamic_cast<PhiInst *>(
                  ParentPhi->getIncomingFromPred(ParentLatch))
            : nullptr;
        if (!ParentPhi || !ChildPhi || !MergePhi ||
            valueMayDependOnUndef(ParentPhi, 256) ||
            valueMayDependOnUndef(ChildPhi, 256) ||
            valueMayDependOnUndef(MergePhi, 256)) {
            return false;
        }
    }
    if (valueMayDependOnUndef(ParentTrip->ExitCmp, 256) ||
        valueMayDependOnUndef(ParentInduction->Phi, 256) ||
        valueMayDependOnUndef(ParentInduction->UpdateValue, 256)) {
        return false;
    }
    if (!matchSaturationCondition(
            ParentHeaderBranch, ChildPreHeader, ChildHeaderBranch,
            *Child, StateMap)) {
        return false;
    }

    //父层直接块不得在重复空迭代中隐藏访存、调用或额外状态更新。
    std::unordered_set<Instruction *> AllowedParentInstructions{
        ParentHeaderBranch,
        dynamic_cast<Instruction *>(ParentHeaderBranch->getCondition()),
        ParentLatchBranch, ParentTrip->ExitCmp,
        dynamic_cast<Instruction *>(ParentInduction->UpdateValue)};
    for (PhiInst *Phi : ParentHeaderPhis) AllowedParentInstructions.insert(Phi);
    for (PhiInst *Phi : ParentLatchPhis)
        AllowedParentInstructions.insert(Phi);
    if (AllowedParentInstructions.count(nullptr)) return false;
    for (BasicBlock *Block : Parent.getBlocks()) {
        if (!Block) return false;
        for (Instruction *I : *Block) {
            if (AllowedParentInstructions.count(I)) continue;
            if (Block == ChildPreHeader &&
                (I == ChildPreHeaderBranch ||
                 isSafeScalarInstruction(I))) {
                continue;
            }
            return false;
        }
    }

    Plan.ParentLoop = &Parent;
    Plan.ChildLoop = Child;
    Plan.ParentHeader = ParentHeader;
    Plan.ParentLatch = ParentLatch;
    Plan.ParentExit = ParentExit;
    Plan.ParentLatchBranch = ParentLatchBranch;
    Plan.ParentHeaderPhis = std::move(ParentHeaderPhis);
    return true;
}

//把父锁存回边改为直达原出口，再删除各父头 PHI 的唯一回边输入。所有
//输入数量已在计划阶段验证，后续 CFGSimplify 会自然清除退化控制与死指令。
bool GuardedLoopCodeMotionPass::applySaturatedParentPlan(
    const SaturatedParentPlan &Plan) {
    if (!Plan.ParentLoop || !Plan.ChildLoop || !Plan.ParentHeader ||
        !Plan.ParentLatch || !Plan.ParentExit ||
        !Plan.ParentLatchBranch || Plan.ParentHeaderPhis.empty()) {
        return false;
    }
    //提交前统一复核所有回边输入，保证后续 CFG 改写不会在中途失败而留下
    //部分更新的 SSA。计划建立后若 IR 已变化则整项保守放弃。
    for (PhiInst *Phi : Plan.ParentHeaderPhis) {
        if (!Phi || Phi->getParent() != Plan.ParentHeader ||
            Phi->countIncomingFromPred(Plan.ParentLatch) != 1) {
            return false;
        }
    }
    if (!Plan.ParentLatch->replaceTerminator(
            new BrInst(Plan.ParentExit))) {
        return false;
    }
    for (PhiInst *Phi : Plan.ParentHeaderPhis) {
        if (!Phi || Phi->countIncomingFromPred(Plan.ParentLatch) != 1 ||
            !PhiInst::removeIncomingByPred(Phi, Plan.ParentLatch, 1)) {
            return false;
        }
    }
    return true;
}

//从内向外寻找首个合法循环，提交后让后续统一重建 CFG、Loop 和 SCEV。
PreservationStatus GuardedLoopCodeMotionPass::run(
    Function &F, FunctionAnalysisPassManager &FAM) {
    if (!F.front()) return PreservationStatus::all();
    auto &DT = FAM.getResult<DominantAnalysis>(F);
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &RI = FAM.getResult<RangeAnalysis>(F);
    auto &SE = FAM.getResult<SCEVAnalysis>(F);
    const auto &TargetCost = FAM.getResult<TargetCostAnalysis>(F);
    //入口守卫由上一轮建立后，优先消费其暴露出的父循环不动点机会。
    for (Loop *Current : LI.getLoopsInPostOrder(true)) {
        if (!Current || Current->getSubLoops().empty()) continue;
        SaturatedParentPlan Plan;
        if (!buildSaturatedParentPlan(*Current, SE, RI, DT, Plan))
            continue;
        if (applySaturatedParentPlan(Plan))
            return PreservationStatus::none();
    }
    for (Loop *Top : LI.getTopLevelLoops()) {
        if (!Top) continue;
        for (Loop *Current : Top->getLoopsInPostOrder()) {
            if (!Current) continue;
            GuardPlan Plan;
            if (!buildPlan(F, FAM, *Current, DT, TargetCost, Plan)) continue;
            if (applyPlan(F, Plan)) return PreservationStatus::none();
        }
    }
    return PreservationStatus::all();
}
