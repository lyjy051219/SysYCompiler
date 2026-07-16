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

bool LoopFusionPass::expressionKeysEqual(const SCEVExpr *A,
                                         const SCEVExpr *B) {
    return A && B && !A->Key.empty() && A->Key == B->Key;
}

bool LoopFusionPass::hasFiniteLoopTree(Loop *L, SCEVResult &SE) {
    if (!L || !SE.isKnownFiniteLoop(L)) return false;
    for (Loop *SubLoop : L->getSubLoops()) {
        if (!hasFiniteLoopTree(SubLoop, SE)) return false;
    }
    return true;
}

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

bool LoopFusionPass::isSafeReorderedInstruction(Instruction *I) {
    if (!I) return false;
    // Loads remain in their original control domain and are covered by the
    // cross-loop memory-dependence proof.  The generic speculation predicate
    // flags every load because hoisting it onto a new path could fault, which
    // fusion does not do.
    if (dynamic_cast<LoadInst *>(I)) return true;
    if (!instructionMayFaultOrProducePoison(I)) return true;

    // SysY remainder-heavy kernels commonly use a compile-time non-zero
    // divisor.  This operation cannot trap; reject -1 as well so signed
    // INT_MIN division/remainder follows the same conservative rule.
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

bool LoopFusionPass::hasDistributionProvenance(const LoopShape &Shape) {
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

bool LoopFusionPass::isUnitStrideAccess(Value *Pointer,
                                        PhiInst *Induction) {
    auto *GEP = dynamic_cast<GetElementPtrInst *>(Pointer);
    if (!GEP || !Induction) return false;

    Value *Base = nullptr;
    std::vector<Value *> Indices;
    if (!GEP->getBaseAndIndices(Base, Indices) || !Base || Indices.empty())
        return false;

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

    auto PointerType = std::dynamic_pointer_cast<POINTERType>(GEP->getIRType());
    auto ElementType = PointerType ? PointerType->getPointerType() : nullptr;
    if (!ElementType || !ElementType->isScalar() ||
        ElementType->getSize() != 4) {
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
            } else if (auto *CurrentLoad = dynamic_cast<LoadInst *>(I)) {
                if (Load) return false;
                Load = CurrentLoad;
            }
        }
    }
    if (!Store ||
        !isUnitStrideAccess(Store->getPointerOperand(), Shape.Induction)) {
        return false;
    }
    if (isRepeatedByteConstant(Store->getValueOperand())) return !Load;
    return Load && Store->getValueOperand() == Load &&
           isUnitStrideAccess(Load->getPointerOperand(), Shape.Induction);
}

bool LoopFusionPass::isUniqueMemoryObject(Value *Root) {
    if (!Root || Root->isUndef()) return false;
    if (dynamic_cast<AllocaInst *>(Root)) return true;
    auto *Object = dynamic_cast<Variable *>(Root);
    return Object && Object->isGlobal();
}

bool LoopFusionPass::hasReductionControlShape(const LoopShape &Shape) {
    if (!Shape.Header || !Shape.BodyEntry || !Shape.Latch ||
        Shape.BodyEntry != Shape.Latch || Shape.Blocks.size() != 2 ||
        !Shape.HeaderBranch || !Shape.LatchBranch ||
        !Shape.HeaderBranch->isConditional() ||
        !BasicBlock::isUncondBr(Shape.LatchBranch) ||
        Shape.LatchBranch->getTrueSuccessor() != Shape.Header ||
        !Shape.BodyEntry->collectPhiNodes().empty()) {
        return false;
    }

    std::vector<PhiInst *> HeaderPhis = Shape.Header->collectPhiNodes();
    if (HeaderPhis.size() != 1 || HeaderPhis.front() != Shape.Induction)
        return false;
    for (Instruction *I : *Shape.Header) {
        if (I == Shape.Induction || I == Shape.ExitCompare ||
            I == Shape.HeaderBranch) {
            continue;
        }
        return false;
    }
    return true;
}

bool LoopFusionPass::valueDependsOn(Value *V, Value *Needle,
                                    unsigned Depth,
                                    ValueSet &Visited) {
    if (!V || !Needle) return true;
    if (V == Needle) return true;
    if (Depth >= MaxDependencyWalkDepth) return true;
    auto *I = dynamic_cast<Instruction *>(V);
    if (!I || !Visited.insert(V).second) return false;

    for (std::size_t Index = 0; Index < I->getNumOperands(); ++Index) {
        Value *Operand = I->getOperand(Index);
        if (dynamic_cast<BasicBlock *>(Operand)) continue;
        if (valueDependsOn(Operand, Needle, Depth + 1, Visited))
            return true;
    }
    return false;
}

bool LoopFusionPass::collectDependencySourceRoots(
    Value *V, Value *TargetRoot, unsigned Depth,
    ValueSet &Visited, std::unordered_set<Value *> &SourceRoots) {
    if (!V || !TargetRoot || Depth >= MaxDependencyWalkDepth)
        return false;
    auto *I = dynamic_cast<Instruction *>(V);
    if (!I || !Visited.insert(V).second) return true;

    if (auto *Load = dynamic_cast<LoadInst *>(I)) {
        Value *Root = Value::stripPointerCastsAndGEP(
            Load->getPointerOperand());
        if (!isUniqueMemoryObject(Root) || Root == TargetRoot)
            return false;
        SourceRoots.insert(Root);
    }
    for (std::size_t Index = 0; Index < I->getNumOperands(); ++Index) {
        Value *Operand = I->getOperand(Index);
        if (dynamic_cast<BasicBlock *>(Operand)) continue;
        if (!collectDependencySourceRoots(
                Operand, TargetRoot, Depth + 1, Visited, SourceRoots)) {
            return false;
        }
    }
    return true;
}

bool LoopFusionPass::valuesEquivalentUnderInductionMap(
    Value *First, PhiInst *FirstInduction,
    Value *Second, PhiInst *SecondInduction,
    unsigned Depth) {
    if (!First || !Second || !FirstInduction || !SecondInduction ||
        Depth >= MaxEquivalentValueDepth) {
        return false;
    }
    if (First == Second) return true;
    if (First == FirstInduction || Second == SecondInduction)
        return First == FirstInduction && Second == SecondInduction;
    if (First->isConst() || Second->isConst())
        return Value::areEqualConstants(First, Second);

    auto FirstType = First->getIRType();
    auto SecondType = Second->getIRType();
    if (!FirstType || !SecondType || !FirstType->hasSameShape(*SecondType))
        return false;

    auto *FirstGEP = dynamic_cast<GetElementPtrInst *>(First);
    auto *SecondGEP = dynamic_cast<GetElementPtrInst *>(Second);
    if (FirstGEP || SecondGEP) {
        if (!FirstGEP || !SecondGEP) return false;
        Value *FirstBase = nullptr;
        Value *SecondBase = nullptr;
        std::vector<Value *> FirstIndices;
        std::vector<Value *> SecondIndices;
        if (!FirstGEP->getBaseAndIndices(FirstBase, FirstIndices) ||
            !SecondGEP->getBaseAndIndices(SecondBase, SecondIndices) ||
            FirstIndices.size() != SecondIndices.size() ||
            !valuesEquivalentUnderInductionMap(
                FirstBase, FirstInduction, SecondBase, SecondInduction,
                Depth + 1)) {
            return false;
        }
        for (std::size_t Index = 0; Index < FirstIndices.size(); ++Index) {
            if (!valuesEquivalentUnderInductionMap(
                    FirstIndices[Index], FirstInduction,
                    SecondIndices[Index], SecondInduction, Depth + 1)) {
                return false;
            }
        }
        return true;
    }

    auto *FirstLoad = dynamic_cast<LoadInst *>(First);
    auto *SecondLoad = dynamic_cast<LoadInst *>(Second);
    if (FirstLoad || SecondLoad) {
        return FirstLoad && SecondLoad &&
               valuesEquivalentUnderInductionMap(
                   FirstLoad->getPointerOperand(), FirstInduction,
                   SecondLoad->getPointerOperand(), SecondInduction,
                   Depth + 1);
    }

    auto *FirstBinary = dynamic_cast<BinaryInst *>(First);
    auto *SecondBinary = dynamic_cast<BinaryInst *>(Second);
    if (FirstBinary || SecondBinary) {
        if (!FirstBinary || !SecondBinary ||
            FirstBinary->getOp() != SecondBinary->getOp()) {
            return false;
        }
        Value *FirstLHS = nullptr;
        Value *FirstRHS = nullptr;
        Value *SecondLHS = nullptr;
        Value *SecondRHS = nullptr;
        return FirstBinary->getOperands(FirstLHS, FirstRHS) &&
               SecondBinary->getOperands(SecondLHS, SecondRHS) &&
               valuesEquivalentUnderInductionMap(
                   FirstLHS, FirstInduction, SecondLHS, SecondInduction,
                   Depth + 1) &&
               valuesEquivalentUnderInductionMap(
                   FirstRHS, FirstInduction, SecondRHS, SecondInduction,
                   Depth + 1);
    }

    auto *FirstInstruction = dynamic_cast<Instruction *>(First);
    auto *SecondInstruction = dynamic_cast<Instruction *>(Second);
    if (!FirstInstruction || !SecondInstruction ||
        FirstInstruction->instType != SecondInstruction->instType) {
        return false;
    }
    switch (FirstInstruction->instType) {
        case Instruction::TRUNC:
        case Instruction::ZEXT:
        case Instruction::SEXT:
        case Instruction::BITCAST:
            return valuesEquivalentUnderInductionMap(
                FirstInstruction->getUnaryOperand(), FirstInduction,
                SecondInstruction->getUnaryOperand(), SecondInduction,
                Depth + 1);
        default:
            return false;
    }
}

bool LoopFusionPass::matchIntegerReductionUpdate(
    const LoopShape &Shape, IntegerReductionUpdate &Update) {
    if (!hasReductionControlShape(Shape)) return false;

    std::vector<LoadInst *> Loads;
    for (BasicBlock *BB : Shape.Blocks) {
        if (!BB) return false;
        for (Instruction *I : *BB) {
            if (auto *Store = dynamic_cast<StoreInst *>(I)) {
                if (Update.Store) return false;
                Update.Store = Store;
            } else if (auto *Load = dynamic_cast<LoadInst *>(I)) {
                Loads.push_back(Load);
            } else if (dynamic_cast<CallInst *>(I)) {
                return false;
            }
        }
    }
    if (!Update.Store) return false;

    auto *Add = dynamic_cast<BinaryInst *>(Update.Store->getValueOperand());
    auto AddType = Add ? Add->getIRType() : nullptr;
    if (!Add || Add->getOp() != BinaryInst::ADD || !AddType ||
        !AddType->isInt() || !Add->hasExactlyOneUse()) {
        return false;
    }

    Value *LHS = nullptr;
    Value *RHS = nullptr;
    if (!Add->getOperands(LHS, RHS)) return false;
    auto *LHSLoad = dynamic_cast<LoadInst *>(LHS);
    auto *RHSLoad = dynamic_cast<LoadInst *>(RHS);
    Value *StorePointer = Update.Store->getPointerOperand();
    const bool LHSIsTarget =
        LHSLoad && LHSLoad->getPointerOperand() == StorePointer;
    const bool RHSIsTarget =
        RHSLoad && RHSLoad->getPointerOperand() == StorePointer;
    if (LHSIsTarget == RHSIsTarget) return false;

    Update.TargetLoad = LHSIsTarget ? LHSLoad : RHSLoad;
    Update.Delta = LHSIsTarget ? RHS : LHS;
    Update.Pointer = StorePointer;
    if (!Update.TargetLoad || !Update.Delta ||
        !Update.TargetLoad->hasExactlyOneUse() ||
        !Update.TargetLoad->getIRType() ||
        !Update.TargetLoad->getIRType()->hasSameShape(*AddType) ||
        !Update.Delta->getIRType() ||
        !Update.Delta->getIRType()->hasSameShape(*AddType)) {
        return false;
    }

    Update.TargetRoot = Value::stripPointerCastsAndGEP(Update.Pointer);
    if (!isUniqueMemoryObject(Update.TargetRoot)) return false;
    for (LoadInst *Load : Loads) {
        Value *Root = Value::stripPointerCastsAndGEP(
            Load ? Load->getPointerOperand() : nullptr);
        if (!isUniqueMemoryObject(Root)) return false;
        if (Root == Update.TargetRoot) {
            if (Load != Update.TargetLoad) return false;
            continue;
        }
        Update.SourceRoots.insert(Root);
    }

    ValueSet Visited;
    if (valueDependsOn(Update.Delta, Update.TargetLoad, 0, Visited))
        return false;
    Visited.clear();
    return collectDependencySourceRoots(
        Update.Delta, Update.TargetRoot, 0, Visited,
        Update.SourceRoots);
}

bool LoopFusionPass::isCommutativeIntegerReductionFusion(
    const FusionCandidate &Candidate) {
    IntegerReductionUpdate First;
    IntegerReductionUpdate Second;
    if (!matchIntegerReductionUpdate(Candidate.First, First) ||
        !matchIntegerReductionUpdate(Candidate.Second, Second) ||
        First.TargetRoot != Second.TargetRoot) {
        return false;
    }

    return valuesEquivalentUnderInductionMap(
        First.Pointer, Candidate.First.Induction,
        Second.Pointer, Candidate.Second.Induction, 0);
}

bool LoopFusionPass::isProfitable(const FusionCandidate &Candidate) {
    const std::size_t Instructions = Candidate.First.InstructionCount +
                                     Candidate.Second.InstructionCount;
    // Independent loops do not provide locality reuse. Even with a real flow
    // edge, fusing a unit-stride single-store producer can hide a memset/memcpy
    // candidate from the later LoopIdiomRecognize pass. Preserve that producer
    // and keep every other fused hot body small enough to limit pressure.
    return (Candidate.CommutativeIntegerReduction ||
            Candidate.SameIterationFlowPairs != 0) &&
           !isSingleStoreIdiomCandidate(Candidate.First) &&
           Instructions <= MaxFlowFusionInstructions;
}

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

std::optional<LoopFusionPass::FusionCandidate>
LoopFusionPass::buildCandidate(
    Function &F, Loop *FirstLoop, LoopInfo &LI, SCEVResult &SE,
    const DominatorTree &DT, const PostDominatorTree &PDT,
    LoopAccessInfo &LA) {
    auto First = buildLoopShape(F, FirstLoop, SE, PDT);
    if (!First || !First->Exit) return std::nullopt;

    BasicBlock *Bridge = First->Exit;
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
    Candidate.CommutativeIntegerReduction =
        isCommutativeIntegerReductionFusion(Candidate);
    if ((!Access.Legal || Access.Unknown) &&
        !Candidate.CommutativeIntegerReduction) {
        return std::nullopt;
    }
    if (Access.Legal && !Access.Unknown) {
        Candidate.SameIterationFlowPairs =
            Access.SameIterationFlowPairs;
        Candidate.SameIterationConflictPairs =
            Access.SameIterationConflictPairs;
    }
    if (!isProfitable(Candidate)) return std::nullopt;

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

void LoopFusionPass::commitCandidate(FusionCandidate &Candidate) {
    for (Instruction *I : Candidate.HoistedBridgeInstructions) {
        assert(I && I->getParent() == Candidate.Bridge);
        I->eraseFromParent();
        const bool Inserted =
            Candidate.First.PreHeader->insertBeforeTerminator(I);
        assert(Inserted);
        (void)Inserted;
    }

    for (const PhiRewrite &Rewrite : Candidate.PhiRewrites) {
        assert(Rewrite.Phi && Rewrite.NewPredecessor &&
               Rewrite.Phi->getIncomingBlock(Rewrite.IncomingIndex) ==
                   Rewrite.OldPredecessor);
        const bool Rewritten = Rewrite.Phi->setIncomingBlock(
            Rewrite.IncomingIndex, Rewrite.NewPredecessor);
        assert(Rewritten);
        (void)Rewritten;
    }

    for (PhiInst *Phi : Candidate.MovedSecondPhis) {
        assert(Phi && Phi->getParent() == Candidate.Second.Header);
        Phi->eraseFromParent();
        const bool Inserted = Candidate.First.Header->insertAfterPhiNodes(Phi);
        assert(Inserted);
        (void)Inserted;
    }

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

    // Clear both removed blocks before deleting either one.  The bridge branch
    // still names the second header, while the second IV still names the bridge
    // as an incoming predecessor; clearing only one and immediately deleting
    // its block would leave the other block with a dangling Value use.
    Candidate.Second.Header->clear();
    Candidate.Bridge->clear();
    assert(!Candidate.Second.Header->hasUses());
    assert(!Candidate.Bridge->hasUses());
    delete Candidate.Second.Header;
    delete Candidate.Bridge;

    BasicBlock *ProvenanceBlock = Candidate.First.BodyEntry;
    Function *F = ProvenanceBlock ? ProvenanceBlock->getParent() : nullptr;
    assert(F && "fusion payload must remain attached to its function");
    ProvenanceBlock->setIdent(F->makeUniqueBlockIdent(
        ProvenanceBlock->getIdent() + ".fuse"));
}

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
