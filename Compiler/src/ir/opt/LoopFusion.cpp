#include "ir/opt/LoopFusion.hpp"

#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopAccessAnalysis.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/opt/DCE.hpp"
#include "ir/opt/LCSSA.hpp"
#include "lib/CFG.hpp"

#include <cassert>
#include <cstring>
#include <utility>

// 步骤 2 的迭代域比较只接受非空且完全相同的 SCEV 结构键，不以指针身份或
// 局部形状猜测数学等价。
bool LoopFusionPass::expressionKeysEqual(const SCEVExpr *A,
                                         const SCEVExpr *B) {
    return A && B && !A->Key.empty() && A->Key == B->Key;
}

// 步骤 1.1：待融合循环及其全部子循环都必须由 SCEV 证明有限，防止融合改变
// 原本串行的无限执行行为。
bool LoopFusionPass::hasFiniteLoopTree(Loop *L, SCEVResult &SE) {
    if (!L || !SE.isKnownFiniteLoop(L)) return false;
    for (Loop *SubLoop : L->getSubLoops()) {
        if (!hasFiniteLoopTree(SubLoop, SE)) return false;
    }
    return true;
}

// 步骤 2.1：循环内任一指令结果都不得在循环块集之外使用；控制结构被删除
// 后不会留下需要跨循环边界保留的值。
bool LoopFusionPass::hasNoLiveOut(const LoopShape &Shape) {
    if (!Shape.L || Shape.BlockLookup.empty()) return false;
    for (BasicBlock *BB : Shape.Blocks) {
        if (!BB) return false;
        for (Instruction *I : *BB) {
            if (!I) return false;
            for (Use *U : I->getUses()) {
                auto *UserI = dynamic_cast<Instruction *>(
                    U ? U->getUser() : nullptr);
                if (!UserI || !UserI->getParent() ||
                    !Shape.BlockLookup.count(UserI->getParent())) {
                    return false;
                }
            }
        }
    }
    return true;
}

// 步骤 2.2：判断把第二循环指令移入第一循环迭代顺序是否可安全执行。load
// 留在融合后的循环执行域，由内存依赖分析另证次序；其他指令不得 fault/poison。
bool LoopFusionPass::isSafeReorderedInstruction(Instruction *I) {
    if (!I) return false;
    if (dynamic_cast<LoadInst *>(I)) return true;
    if (!instructionMayFaultOrProducePoison(I)) return true;

    // 整数除余仅在常量除数排除零，且有符号操作额外排除 -1 时可重排。
    auto *Binary = dynamic_cast<BinaryInst *>(I);
    if (!Binary) return false;
    BinaryInst::Operation Op = Binary->getOp();
    if (Op != BinaryInst::DIV && Op != BinaryInst::UDIV &&
        Op != BinaryInst::MOD && Op != BinaryInst::UREM) {
        return false;
    }
    auto *Divisor = dynamic_cast<ConstInt *>(Binary->getOperand(1));
    if (!Divisor || Divisor->getVal() == 0) return false;
    return (Op == BinaryInst::UDIV || Op == BinaryInst::UREM) ||
           Divisor->getVal() != -1;
}

// 步骤 2.3：调用、alloca、ret 或任何未通过重排安全判定的循环指令都使结构
// 候选失效；store 的内存次序在后续 LoopAccess 统一验证。
bool LoopFusionPass::hasUnsupportedLoopInstruction(
    const LoopShape &Shape) {
    for (BasicBlock *BB : Shape.Blocks) {
        if (!BB) return true;
        for (Instruction *I : *BB) {
            if (!I || dynamic_cast<CallInst *>(I) ||
                dynamic_cast<AllocaInst *>(I) ||
                dynamic_cast<RetInst *>(I) ||
                !isSafeReorderedInstruction(I)) {
                return true;
            }
        }
    }
    return false;
}

// 已由 LoopDistribution 产生的循环不立即再融合，避免两个相反结构 pass 在
// 外层不动点中来回振荡；来源标记可位于头、预头或任一循环块。
bool LoopFusionPass::hasDistributionProvenance(
    const LoopShape &Shape) {
    if (!Shape.Header || !Shape.PreHeader) return true;
    if (Shape.Header->getIdent().find(".dist") != std::string::npos ||
        Shape.PreHeader->getIdent().find(".dist") != std::string::npos) {
        return true;
    }
    for (BasicBlock *BB : Shape.Blocks) {
        if (!BB || BB->getIdent().find(".dist") != std::string::npos)
            return true;
    }
    return false;
}

// 步骤 2.4：桥接块计算会被移到第一循环预头，只接受不会访问内存、产生
// 副作用、fault 或 poison 的有限纯地址/整数转换指令。
bool LoopFusionPass::isSafeBridgeInstruction(Instruction *I) {
    if (!I || dynamic_cast<PhiInst *>(I) || dynamic_cast<LoadInst *>(I) ||
        dynamic_cast<StoreInst *>(I) || dynamic_cast<CallInst *>(I) ||
        dynamic_cast<AllocaInst *>(I) || dynamic_cast<BrInst *>(I) ||
        dynamic_cast<RetInst *>(I) || dceInstructionMustBePreserved(I) ||
        instructionMayFaultOrProducePoison(I)) {
        return false;
    }

    return dynamic_cast<GetElementPtrInst *>(I) ||
           dynamic_cast<BinaryInst *>(I) ||
           dynamic_cast<BitCastInst *>(I) ||
           dynamic_cast<ZextInst *>(I) ||
           dynamic_cast<SextInst *>(I) ||
           dynamic_cast<TruncInst *>(I);
}

// 桥指令操作数必须在新位置可用：非指令值天然可用，桥内定义须已按原顺序
// 加入外提集，其他指令定义须支配第一循环预头终结点。
bool LoopFusionPass::valueDominatesPreheader(
    Value *V, BasicBlock *PreHeader, BasicBlock *Bridge,
    const InstructionSet &EarlierBridgeInstructions,
    const DominatorTree &DT) {
    if (!V || !PreHeader || !Bridge) return false;
    auto *Def = dynamic_cast<Instruction *>(V);
    if (!Def) return true;
    if (Def->getParent() == Bridge)
        return EarlierBridgeInstructions.count(Def) != 0;

    Instruction *PreHeaderTerminator = PreHeader->getTerminator();
    return PreHeaderTerminator &&
           DT.dominatesValue(Def, PreHeaderTerminator);
}

// 步骤 2.5：按程序序验证并收集整座桥的非终结指令；桥必须唯一无条件跳到
// 第二头部，且指令数受预算限制。
bool LoopFusionPass::collectBridgeInstructions(
    FusionCandidate &Candidate, const DominatorTree &DT) {
    BasicBlock *Bridge = Candidate.Bridge;
    if (!Bridge || !Candidate.First.PreHeader) return false;
    BrInst *Terminator = Bridge->getBranchTerminator();
    if (!BasicBlock::isUncondBr(Terminator) ||
        Terminator->getTrueSuccessor() != Candidate.Second.Header) {
        return false;
    }

    InstructionSet Earlier;
    for (Instruction *I : *Bridge) {
        if (I == Terminator) continue;
        if (!isSafeBridgeInstruction(I) ||
            Candidate.HoistedBridgeInstructions.size() >=
                MaxBridgeInstructions) {
            return false;
        }
        for (std::size_t OperandIndex = 0;
             OperandIndex < I->getNumOperands(); ++OperandIndex) {
            Value *Operand = I->getOperand(OperandIndex);
            if (dynamic_cast<BasicBlock *>(Operand)) continue;
            if (!valueDominatesPreheader(
                    Operand, Candidate.First.PreHeader, Bridge, Earlier, DT)) {
                return false;
            }
        }
        Candidate.HoistedBridgeInstructions.push_back(I);
        Earlier.insert(I);
    }
    return true;
}

// 步骤 2.6：预计算融合后的 PHI 前驱修复。第一头部回边将来自第二 latch；
// 第二头部除归纳变量外的 PHI 移到第一头部，初值边改接第一预头。
bool LoopFusionPass::collectHeaderPhiRewrites(
    FusionCandidate &Candidate, const DominatorTree &DT) {
    const LoopShape &First = Candidate.First;
    const LoopShape &Second = Candidate.Second;
    if (!First.Header || !First.Latch || !First.PreHeader ||
        !Second.Header || !Second.Latch || !Second.Induction ||
        !Candidate.Bridge) {
        return false;
    }

    for (PhiInst *Phi : First.Header->collectPhiNodes()) {
        if (!Phi || Phi->getNumIncomingValues() != 2 ||
            Phi->countIncomingFromPred(First.PreHeader) != 1 ||
            Phi->countIncomingFromPred(First.Latch) != 1) {
            return false;
        }
        for (unsigned Index = 0; Index < Phi->getNumIncomingValues(); ++Index) {
            if (Phi->getIncomingBlock(Index) != First.Latch) continue;
            Candidate.PhiRewrites.push_back(
                {Phi, Index, First.Latch, Second.Latch});
        }
    }

    InstructionSet Hoisted(
        Candidate.HoistedBridgeInstructions.begin(),
        Candidate.HoistedBridgeInstructions.end());
    bool FoundInduction = false;
    for (PhiInst *Phi : Second.Header->collectPhiNodes()) {
        if (!Phi || Phi->getNumIncomingValues() != 2 ||
            Phi->countIncomingFromPred(Candidate.Bridge) != 1 ||
            Phi->countIncomingFromPred(Second.Latch) != 1) {
            return false;
        }
        if (Phi == Second.Induction) {
            FoundInduction = true;
            continue;
        }
        if (Candidate.MovedSecondPhis.size() >= MaxMovedHeaderPhis)
            return false;

        for (unsigned Index = 0; Index < Phi->getNumIncomingValues(); ++Index) {
            if (Phi->getIncomingBlock(Index) != Candidate.Bridge) continue;
            Value *Start = Phi->getIncomingValue(Index);
            if (!valueDominatesPreheader(Start, First.PreHeader,
                                         Candidate.Bridge, Hoisted, DT)) {
                return false;
            }
            Candidate.PhiRewrites.push_back(
                {Phi, Index, Candidate.Bridge, First.PreHeader});
        }
        Candidate.MovedSecondPhis.push_back(Phi);
    }
    return FoundInduction;
}

// 步骤 2.7：第二头部只能保留 PHI、退出比较和以该比较为唯一条件 use 的
// 分支；删除该头部时不会丢失第二循环的有效载荷计算。
bool LoopFusionPass::secondHeaderContainsOnlyControl(
    const FusionCandidate &Candidate) {
    const LoopShape &Second = Candidate.Second;
    if (!Second.Header || !Second.HeaderBranch || !Second.ExitCompare ||
        Second.HeaderBranch->getCondition() != Second.ExitCompare) {
        return false;
    }

    for (Instruction *I : *Second.Header) {
        if (dynamic_cast<PhiInst *>(I) || I == Second.ExitCompare ||
            I == Second.HeaderBranch) {
            continue;
        }
        return false;
    }
    for (Use *U : Second.ExitCompare->getUses()) {
        if (!U || U->getUser() != Second.HeaderBranch) return false;
    }
    return true;
}

// 步骤 2.8：两循环必须有不同但同类型的 canonical IV，相同非零常量步长、
// 零迭代属性、SCEV 起点、步长表达式和 trip count。
bool LoopFusionPass::sameIterationDomain(const LoopShape &First,
                                         const LoopShape &Second) {
    if (!First.Induction || !Second.Induction ||
        First.Induction == Second.Induction || !First.ConstantStep ||
        !Second.ConstantStep || *First.ConstantStep == 0 ||
        *First.ConstantStep != *Second.ConstantStep ||
        First.MayBeZeroTrip != Second.MayBeZeroTrip ||
        !expressionKeysEqual(First.Start, Second.Start) ||
        !expressionKeysEqual(First.Step, Second.Step) ||
        !expressionKeysEqual(First.TripCount, Second.TripCount)) {
        return false;
    }

    auto FirstType = First.Induction->getIRType();
    auto SecondType = Second.Induction->getIRType();
    return FirstType && SecondType && FirstType->hasSameShape(*SecondType);
}

// 步骤 3 的 idiom 收益保护辅助：识别可由 memset 表达的重复字节常量。
bool LoopFusionPass::isRepeatedByteConstant(Value *V) {
    std::uint32_t Bits = 0;
    if (auto *Integer = dynamic_cast<ConstInt *>(V)) {
        Bits = static_cast<std::uint32_t>(Integer->getVal());
    } else if (auto *Floating = dynamic_cast<ConstFloat *>(V)) {
        const float Number = Floating->getVal();
        static_assert(sizeof(Bits) == sizeof(Number),
                      "unexpected float width");
        std::memcpy(&Bits, &Number, sizeof(Bits));
    } else {
        return false;
    }
    const std::uint32_t Byte = Bits & 0xffU;
    return Bits == Byte * 0x01010101U;
}

// 判断指针是否是以 canonical IV 为唯一动态维度、其余维度全为常量，且该
// 维度步长恰为末级 i32/float 元素大小的连续访问。
bool LoopFusionPass::isUnitStrideAccess(Value *Pointer,
                                        PhiInst *Induction) {
    auto *GEP = dynamic_cast<GetElementPtrInst *>(Pointer);
    if (!GEP || !Induction) return false;

    Value *Base = nullptr;
    std::vector<Value *> Indices;
    if (!GEP->getBaseAndIndices(Base, Indices) ||
        !Base || Indices.empty()) {
        return false;
    }

    std::size_t InductionIndex = 0;
    unsigned InductionCount = 0;
    for (std::size_t Index = 0; Index < Indices.size(); ++Index) {
        if (Indices[Index] == Induction) {
            InductionIndex = Index;
            ++InductionCount;
            continue;
        }
        int ConstantIndex = 0;
        if (!Value::tryGetIntIndex(Indices[Index], ConstantIndex))
            return false;
    }
    if (InductionCount != 1) return false;

    auto ElementType = IRType::getPointeeType(GEP->getIRType());
    if (!ElementType ||
        (!ElementType->hasIntegerBitWidth(32) &&
         !ElementType->isFloat())) {
        return false;
    }

    std::vector<std::int64_t> Strides;
    std::int64_t FinalStride = 0;
    return GetElementPtrInst::getIndexStrides(
               Base, Indices, Strides, FinalStride) &&
           InductionIndex < Strides.size() &&
           Strides[InductionIndex] ==
               static_cast<std::int64_t>(ElementType->getSize()) &&
           FinalStride ==
               static_cast<std::int64_t>(ElementType->getSize());
}

// 步骤 3.1：识别单 store 的 memset 或单 load-to-store 的 memcpy 循环；这类
// 循环留给 LoopIdiomRecognize，不用融合破坏更专门的收益机会。
bool LoopFusionPass::isSingleStoreIdiomCandidate(
    const LoopShape &Shape) {
    StoreInst *Store = nullptr;
    LoadInst *Load = nullptr;
    for (BasicBlock *BB : Shape.Blocks) {
        if (!BB) return false;
        for (Instruction *I : *BB) {
            if (auto *CurrentStore = dynamic_cast<StoreInst *>(I)) {
                if (Store) return false;
                Store = CurrentStore;
            } else if (auto *CurrentLoad =
                           dynamic_cast<LoadInst *>(I)) {
                if (Load) return false;
                Load = CurrentLoad;
            }
        }
    }
    if (!Store ||
        !isUnitStrideAccess(
            Store->getPointerOperand(), Shape.Induction)) {
        return false;
    }
    if (isRepeatedByteConstant(Store->getValueOperand()))
        return !Load;
    return Load && Store->getValueOperand() == Load &&
           isUnitStrideAccess(
               Load->getPointerOperand(), Shape.Induction);
}

// 步骤 3.2：融合收益必须来自 LoopAccess 报告的同迭代真依赖，同时限制总
// 指令数，并排除首循环的单 store idiom。
bool LoopFusionPass::isProfitable(const FusionCandidate &Candidate) {
    const std::size_t Instructions = Candidate.First.InstructionCount +
                                     Candidate.Second.InstructionCount;
    return Candidate.SameIterationFlowPairs != 0 &&
           !isSingleStoreIdiomCandidate(Candidate.First) &&
           Instructions <= MaxFlowFusionInstructions;
}

// 步骤 2.9：验证两循环同级、均为 LCSSA 且由唯一桥严格串接；第二体入口和
// 最终出口不得含 PHI，避免重定向后还需无法预先描述的数据流修复。
bool LoopFusionPass::isLCSSAAndStructurallyAdjacent(
    const FusionCandidate &Candidate, const LoopInfo &LI) {
    const LoopShape &First = Candidate.First;
    const LoopShape &Second = Candidate.Second;
    if (!First.L || !Second.L || First.L == Second.L ||
        First.L->getParentLoop() != Second.L->getParentLoop() ||
        !LCSSAPass::isLCSSAForm(First.L) ||
        !LCSSAPass::isLCSSAForm(Second.L) ||
        First.Exit != Candidate.Bridge ||
        Second.PreHeader != Candidate.Bridge ||
        LI.getLoopFor(First.Header) != First.L ||
        LI.getLoopFor(Second.Header) != Second.L) {
        return false;
    }

    const auto BridgePreds = Candidate.Bridge->getPredecessors();
    const auto BridgeSuccs = Candidate.Bridge->getSuccessors();
    const auto SecondHeaderPreds = Second.Header->getPredecessors();
    const auto SecondBodyPreds = Second.BodyEntry->getPredecessors();
    if (BridgePreds.size() != 1 || BridgePreds.front() != First.Header ||
        BridgeSuccs.size() != 1 || BridgeSuccs.front() != Second.Header ||
        SecondHeaderPreds.size() != 2 ||
        Second.Header->countEdgesToSucc(Second.BodyEntry) != 1 ||
        SecondBodyPreds.size() != 1 ||
        SecondBodyPreds.front() != Second.Header ||
        !Second.BodyEntry->collectPhiNodes().empty() ||
        !Second.Exit->collectPhiNodes().empty()) {
        return false;
    }

    bool HasBridgePred = false;
    bool HasLatchPred = false;
    for (BasicBlock *Pred : SecondHeaderPreds) {
        HasBridgePred |= Pred == Candidate.Bridge;
        HasLatchPred |= Pred == Second.Latch;
    }
    return HasBridgePred && HasLatchPred;
}

// 步骤 4 的边计划只接受分支中恰好一次出现的目标，拒绝重复 CFG 边导致的
// 后继索引歧义。
std::optional<unsigned> LoopFusionPass::findUniqueSuccessorIndex(
    BrInst *Branch, BasicBlock *Successor) {
    if (!Branch || !Successor) return std::nullopt;
    std::optional<unsigned> Result;
    for (unsigned Index = 0; Index < Branch->getSuccessorCount(); ++Index) {
        if (Branch->getSuccessor(Index) != Successor) continue;
        if (Result) return std::nullopt;
        Result = Index;
    }
    return Result;
}

// 步骤 1：为单个循环构建可融合结构摘要。要求规范 preheader/header/latch、
// canonical IV 与头部退出 trip 信息、唯一退出边，以及所有体路径到达 latch。
std::optional<LoopFusionPass::LoopShape> LoopFusionPass::buildLoopShape(
    Function &F, Loop *L, SCEVResult &SE,
    const PostDominatorTree &PDT) {
    if (!L || !L->getHeader() || L->getHeader()->getParent() != &F ||
        !hasFiniteLoopTree(L, SE)) {
        return std::nullopt;
    }

    LoopShape Shape;
    Shape.L = L;
    Shape.Header = L->getHeader();
    if (!L->getLoopSimplifyForm(Shape.PreHeader, Shape.Latch) ||
        !Shape.PreHeader || !Shape.Latch || Shape.Header == Shape.Latch) {
        return std::nullopt;
    }

    auto Induction = SE.getCanonicalInduction(L);
    auto Trip = SE.analyzeLoopTripCount(L);
    // 1.2 退出判定必须位于 header，使融合后可完整复用第一循环的预检语义。
    if (!Induction || !Induction->Valid || !Induction->Phi ||
        !Induction->UpdateValue || !Induction->Start || !Induction->Step ||
        !Induction->ConstantStep || !Trip || !Trip->Valid ||
        Trip->Header != Shape.Header || Trip->ExitingBlock != Shape.Header ||
        !Trip->ExitBlock || !Trip->ExitCmp || !Trip->TripCount) {
        return std::nullopt;
    }

    Shape.Induction = Induction->Phi;
    Shape.InductionUpdate =
        dynamic_cast<Instruction *>(Induction->UpdateValue);
    Shape.ExitCompare = Trip->ExitCmp;
    Shape.Exit = Trip->ExitBlock;
    Shape.Start = Induction->Start;
    Shape.Step = Induction->Step;
    Shape.TripCount = Trip->TripCount;
    Shape.ConstantStep = *Induction->ConstantStep;
    Shape.MayBeZeroTrip = Trip->MayBeZeroTrip;
    Shape.HeaderBranch = Shape.Header->getBranchTerminator();
    Shape.LatchBranch = Shape.Latch->getBranchTerminator();
    if (!Shape.InductionUpdate || !Shape.HeaderBranch ||
        !Shape.HeaderBranch->isConditional() ||
        !BasicBlock::isUncondBr(Shape.LatchBranch) ||
        Shape.LatchBranch->getTrueSuccessor() != Shape.Header) {
        return std::nullopt;
    }

    unsigned InsideSuccessors = 0;
    unsigned ExitSuccessors = 0;
    for (unsigned Index = 0;
         Index < Shape.HeaderBranch->getSuccessorCount(); ++Index) {
        BasicBlock *Successor = Shape.HeaderBranch->getSuccessor(Index);
        if (!Successor) return std::nullopt;
        if (L->contains(Successor)) {
            Shape.BodyEntry = Successor;
            ++InsideSuccessors;
        } else if (Successor == Shape.Exit) {
            ++ExitSuccessors;
        } else {
            return std::nullopt;
        }
    }
    if (InsideSuccessors != 1 || ExitSuccessors != 1 ||
        !Shape.BodyEntry || Shape.BodyEntry == Shape.Header) {
        return std::nullopt;
    }

    Shape.Blocks = L->getBlocksInFunctionOrder();
    // 1.3 全循环只能由 header 向摘要中的 Exit 退出一次，body entry 必须最终
    // 到达 latch；块数和指令数同时在此汇总供后续代码量预算使用。
    if (Shape.Blocks.empty()) return std::nullopt;
    Shape.BlockLookup.insert(Shape.Blocks.begin(), Shape.Blocks.end());
    unsigned ExitEdgeCount = 0;
    BasicBlock *ExitingBlock = nullptr;
    for (BasicBlock *BB : Shape.Blocks) {
        if (!BB || BB->getParent() != &F) return std::nullopt;
        Shape.InstructionCount += static_cast<std::size_t>(BB->size());
        for (BasicBlock *Successor : BB->getSuccessors()) {
            if (!Successor) return std::nullopt;
            if (Shape.BlockLookup.count(Successor)) continue;
            ++ExitEdgeCount;
            ExitingBlock = BB;
            if (Successor != Shape.Exit) return std::nullopt;
        }
    }
    if (ExitEdgeCount != 1 || ExitingBlock != Shape.Header ||
        !PDT.isReachableToExit(Shape.Header) ||
        !PDT.isReachableToExit(Shape.BodyEntry) ||
        !PDT.postDominates(Shape.Latch, Shape.BodyEntry)) {
        return std::nullopt;
    }
    return Shape;
}

// 候选构建把全部可能失败的结构、语义、依赖、收益和改写位置检查集中在提交
// 之前，成功返回的计划可由 commitCandidate 无条件执行。
std::optional<LoopFusionPass::FusionCandidate>
LoopFusionPass::buildCandidate(
    Function &F, Loop *FirstLoop, LoopInfo &LI, SCEVResult &SE,
    const DominatorTree &DT, const PostDominatorTree &PDT,
    LoopAccessInfo &LA) {
    auto First = buildLoopShape(F, FirstLoop, SE, PDT);
    if (!First || !First->Exit) return std::nullopt;

    BasicBlock *Bridge = First->Exit;
    // 1.4 第一退出必须是无条件通向另一循环头部的桥，由此唯一发现第二循环。
    BrInst *BridgeBranch = Bridge->getBranchTerminator();
    if (!BasicBlock::isUncondBr(BridgeBranch)) return std::nullopt;
    BasicBlock *SecondHeader = BridgeBranch->getTrueSuccessor();
    Loop *SecondLoop = LI.getLoopFor(SecondHeader);
    if (!SecondLoop || SecondLoop->getHeader() != SecondHeader)
        return std::nullopt;
    auto Second = buildLoopShape(F, SecondLoop, SE, PDT);
    if (!Second) return std::nullopt;

    FusionCandidate Candidate;
    Candidate.First = std::move(*First);
    Candidate.Second = std::move(*Second);
    Candidate.Bridge = Bridge;
    // 2. 依次核验迭代域、相邻 LCSSA、live-out、重排安全、来源、第二头部、
    // 桥外提和 PHI 移动；任一证据不足都放弃整对循环。
    if (!sameIterationDomain(Candidate.First, Candidate.Second) ||
        !isLCSSAAndStructurallyAdjacent(Candidate, LI) ||
        !hasNoLiveOut(Candidate.First) ||
        !hasNoLiveOut(Candidate.Second) ||
        hasUnsupportedLoopInstruction(Candidate.First) ||
        hasUnsupportedLoopInstruction(Candidate.Second) ||
        hasDistributionProvenance(Candidate.First) ||
        hasDistributionProvenance(Candidate.Second) ||
        !secondHeaderContainsOnlyControl(Candidate) ||
        !collectBridgeInstructions(Candidate, DT) ||
        !collectHeaderPhiRewrites(Candidate, DT)) {
        return std::nullopt;
    }

    const std::size_t CombinedBlocks = Candidate.First.Blocks.size() +
                                       Candidate.Second.Blocks.size();
    const std::size_t CombinedInstructions =
        Candidate.First.InstructionCount +
        Candidate.Second.InstructionCount;
    if (CombinedBlocks > MaxFusionBlocks ||
        CombinedInstructions > MaxFusionInstructions) {
        return std::nullopt;
    }

    LoopFusionAccessResult Access =
        LA.analyzeFusion(Candidate.First.L, Candidate.Second.L);
    // 3. 跨循环访存次序必须直接由统一 LoopAccess 证明；Illegal 和 Unknown
    // 都不能通过特定 opcode 或 def-use 形状旁路。
    if (!Access.Legal || Access.Unknown) {
        return std::nullopt;
    }
    Candidate.SameIterationFlowPairs =
        Access.SameIterationFlowPairs;
    if (!isProfitable(Candidate)) return std::nullopt;

    // 4. 最后解析三条待重接边的唯一后继槽位并记录计划：第一头部退出改到
    // 第二出口，第一 latch 进入第二 body，第二 latch 回到第一 header。
    auto FirstExitIndex = findUniqueSuccessorIndex(
        Candidate.First.HeaderBranch, Candidate.Bridge);
    auto FirstLatchIndex = findUniqueSuccessorIndex(
        Candidate.First.LatchBranch, Candidate.First.Header);
    auto SecondLatchIndex = findUniqueSuccessorIndex(
        Candidate.Second.LatchBranch, Candidate.Second.Header);
    if (!FirstExitIndex || !FirstLatchIndex || !SecondLatchIndex ||
        Candidate.Second.Exit == Candidate.Bridge ||
        Candidate.Second.BodyEntry == Candidate.First.Header) {
        return std::nullopt;
    }
    Candidate.EdgeRewrites.push_back(
        {Candidate.First.HeaderBranch, *FirstExitIndex, Candidate.Bridge,
         Candidate.Second.Exit});
    Candidate.EdgeRewrites.push_back(
        {Candidate.First.LatchBranch, *FirstLatchIndex,
         Candidate.First.Header, Candidate.Second.BodyEntry});
    Candidate.EdgeRewrites.push_back(
        {Candidate.Second.LatchBranch, *SecondLatchIndex,
         Candidate.Second.Header, Candidate.First.Header});
    return Candidate;
}

// 步骤 4 提交：先移动仍需保留的定义和 PHI，再替换 IV、重接 CFG，最后删除
// 已完全脱离的第二头部与桥；顺序保证每个中间状态都不产生悬空 Value use。
void LoopFusionPass::commitCandidate(FusionCandidate &Candidate) {
    // 4.1 将桥中纯计算按原顺序外提到第一预头。
    for (Instruction *I : Candidate.HoistedBridgeInstructions) {
        assert(I && I->getParent() == Candidate.Bridge);
        I->eraseFromParent();
        const bool Inserted =
            Candidate.First.PreHeader->insertBeforeTerminator(I);
        assert(Inserted);
        (void)Inserted;
    }

    // 4.2 按预先记录的 incoming 槽位修复第一回边和被移动第二 PHI 的初值边。
    for (const PhiRewrite &Rewrite : Candidate.PhiRewrites) {
        assert(Rewrite.Phi && Rewrite.NewPredecessor &&
               Rewrite.Phi->getIncomingBlock(Rewrite.IncomingIndex) ==
                   Rewrite.OldPredecessor);
        const bool Rewritten = Rewrite.Phi->setIncomingBlock(
            Rewrite.IncomingIndex, Rewrite.NewPredecessor);
        assert(Rewritten);
        (void)Rewritten;
    }

    // 4.3 把第二头部的非 IV PHI 移到第一头部 PHI 序列之后。
    for (PhiInst *Phi : Candidate.MovedSecondPhis) {
        assert(Phi && Phi->getParent() == Candidate.Second.Header);
        Phi->eraseFromParent();
        const bool Inserted = Candidate.First.Header->insertAfterPhiNodes(Phi);
        assert(Inserted);
        (void)Inserted;
    }

    // 4.4 两循环迭代域已严格相等，第二 IV 的所有使用可直接换成第一 IV。
    Candidate.Second.Induction->replaceAllUsesWith(
        Candidate.First.Induction);

    for (const EdgeRewrite &Rewrite : Candidate.EdgeRewrites) {
        assert(Rewrite.Branch && Rewrite.NewSuccessor &&
               Rewrite.Branch->getSuccessor(Rewrite.SuccessorIndex) ==
                   Rewrite.OldSuccessor);
        const bool Rewritten = Rewrite.Branch->setSuccessor(
            Rewrite.SuccessorIndex, Rewrite.NewSuccessor);
        assert(Rewritten);
        (void)Rewritten;
    }

    // 4.5 两个待删块仍可能互相引用，先同时 clear 断开操作数，再断言无 use
    // 并删除，避免逐块删除留下悬空边或 PHI 前驱。
    Candidate.Second.Header->clear();
    Candidate.Bridge->clear();
    assert(!Candidate.Second.Header->hasUses());
    assert(!Candidate.Bridge->hasUses());
    delete Candidate.Second.Header;
    delete Candidate.Bridge;

    // 4.6 给保留的融合载荷块添加来源标记，供结构 pass 防止反向振荡。
    BasicBlock *ProvenanceBlock = Candidate.First.BodyEntry;
    Function *F = ProvenanceBlock ? ProvenanceBlock->getParent() : nullptr;
    assert(F && "fusion payload must remain attached to its function");
    ProvenanceBlock->setIdent(F->makeUniqueBlockIdent(
        ProvenanceBlock->getIdent() + ".fuse"));
}

// Pass 主流程按循环后序寻找首个完整候选；同一 pass 对象按函数累计总融合
// 预算，每次提交后立即返回并使全部分析失效，让下一轮使用新 CFG 快照。
PassResult LoopFusionPass::run(
    Function &F, FunctionAnalysisPassManager &FAM) {
    unsigned &Count = TransformCounts[&F];
    if (Count >= MaxFusionsPerFunction)
        return PassResult::unchanged();

    auto &DT = FAM.getResult<DominantAnalysis>(F);
    auto &PDT = FAM.getResult<PostDominantAnalysis>(F);
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &SE = FAM.getResult<SCEVAnalysis>(F);
    auto &LA = FAM.getResult<LoopAccessAnalysis>(F);

    for (Loop *First : LI.getLoopsInPostOrder()) {
        auto Candidate = buildCandidate(F, First, LI, SE, DT, PDT, LA);
        if (!Candidate) continue;
        commitCandidate(*Candidate);
        ++Count;
        return PassResult::changed(PreservationStatus::none());
    }
    return PassResult::unchanged();
}
