#include "ir/opt/LoopIdiomRecognize.hpp"

#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "lib/TraverseTools.hpp"

#include <cstring>
#include <limits>
#include <unordered_set>

bool LoopIdiomRecognizePass::matchLinearAccess(
    GetElementPtrInst *GEP, PhiInst *IndVar, Loop *L,
    LinearAccess &Access) {
    Access = {};
    if (!GEP || !IndVar || !L) return false;

    Value *Base = nullptr;
    std::vector<Value *> Indices;
    if (!GEP->getBaseAndIndices(Base, Indices) || !Base || Indices.empty())
        return false;
    if (auto *BaseI = dynamic_cast<Instruction *>(Base)) {
        BasicBlock *BaseBB = BaseI->getParent();
        if (BaseBB && L->contains(BaseBB)) return false;
    }

    size_t DynamicIndex = 0;
    unsigned DynamicCount = 0;
    for (size_t I = 0; I < Indices.size(); ++I) {
        if (Indices[I] == IndVar) {
            DynamicIndex = I;
            ++DynamicCount;
            continue;
        }
        int ConstantIndex = 0;
        if (!Value::tryGetIntIndex(Indices[I], ConstantIndex)) return false;
    }
    if (DynamicCount != 1) return false;

    auto PtrTy = std::dynamic_pointer_cast<POINTERType>(GEP->getIRType());
    auto ElementTy = PtrTy ? PtrTy->getPointerType() : nullptr;
    // The current RISC-V lowering emits aligned word stores for small memset
    // calls.  Restrict the idiom to SysY's naturally aligned i32/f32 elements.
    if (!ElementTy || !ElementTy->isScalar() || ElementTy->getSize() != 4)
        return false;

    std::vector<int64_t> Strides;
    int64_t FinalStride = 0;
    if (!GetElementPtrInst::getIndexStrides(Base, Indices, Strides,
                                            FinalStride) ||
        DynamicIndex >= Strides.size() ||
        Strides[DynamicIndex] != static_cast<int64_t>(ElementTy->getSize()) ||
        FinalStride != static_cast<int64_t>(ElementTy->getSize()))
        return false;

    Access.GEP = GEP;
    Access.Base = Base;
    Access.Indices = std::move(Indices);
    Access.InductionIndex = DynamicIndex;
    Access.ElementTy = ElementTy;
    return true;
}

Value *LoopIdiomRecognizePass::getDisjointObjectRoot(Value *V) {
    std::unordered_set<Value *> Seen;
    while (V && Seen.insert(V).second) {
        if (auto *GEP = dynamic_cast<GetElementPtrInst *>(V)) {
            V = GEP->getBasePointer();
            continue;
        }
        if (auto *BC = dynamic_cast<BitCastInst *>(V)) {
            V = BC->getUnaryOperand();
            continue;
        }
        break;
    }
    if (auto *Var = dynamic_cast<Variable *>(V)) {
        return (Var->vartag == Variable::GlobalVariable ||
                Var->vartag == Variable::Constant)
                   ? V
                   : nullptr;
    }
    return dynamic_cast<AllocaInst *>(V) ? V : nullptr;
}

bool LoopIdiomRecognizePass::areProvablyDisjoint(
    const LinearAccess &A, const LinearAccess &B) {
    Value *ARoot = getDisjointObjectRoot(A.Base);
    Value *BRoot = getDisjointObjectRoot(B.Base);
    return ARoot && BRoot && ARoot != BRoot;
}

bool LoopIdiomRecognizePass::getRepeatedByte(Value *V, uint8_t &Byte) {
    uint32_t Bits = 0;
    if (auto *CI = dynamic_cast<ConstInt *>(V)) {
        Bits = static_cast<uint32_t>(CI->getVal());
    } else if (auto *CF = dynamic_cast<ConstFloat *>(V)) {
        float F = CF->getVal();
        static_assert(sizeof(Bits) == sizeof(F), "unexpected float width");
        std::memcpy(&Bits, &F, sizeof(Bits));
    } else {
        return false;
    }

    uint8_t B0 = static_cast<uint8_t>(Bits & 0xffu);
    if (static_cast<uint8_t>((Bits >> 8) & 0xffu) != B0 ||
        static_cast<uint8_t>((Bits >> 16) & 0xffu) != B0 ||
        static_cast<uint8_t>((Bits >> 24) & 0xffu) != B0)
        return false;
    Byte = B0;
    return true;
}

bool LoopIdiomRecognizePass::hasOnlyAllowedLoopInstructions(
    Loop *L, const LoopTripCountInfo &TCI,
    const InductionDescriptor &IV, StoreInst *Store,
    GetElementPtrInst *DestGEP, LoadInst *SourceLoad,
    GetElementPtrInst *SourceGEP, DominatorTree &DT) {
    if (!L || !Store || !DestGEP || !IV.Phi || !IV.UpdateValue ||
        !TCI.ExitCmp || !TCI.Latch || !TCI.ExitingBlock)
        return false;
    if (!DT.dominates(Store->getParent(), TCI.Latch)) return false;

    std::unordered_set<Instruction *> Allowed{
        IV.Phi, dynamic_cast<Instruction *>(IV.UpdateValue), TCI.ExitCmp,
        DestGEP, Store};
    if (SourceLoad) Allowed.insert(SourceLoad);
    if (SourceGEP) Allowed.insert(SourceGEP);
    if (Allowed.count(nullptr)) Allowed.erase(nullptr);

    unsigned ConditionalBranches = 0;
    auto LoopBlocks = L->getBlocksIncludingSubLoops();
    for (BasicBlock *BB : LoopBlocks)
        if (!BB) return false;
    if (!forEachInstructionInBlocks(
            LoopBlocks, [&](Instruction *I) {
        if (auto *Br = dynamic_cast<BrInst *>(I)) {
            if (Br->isConditional()) {
                ++ConditionalBranches;
                if (Br->getCondition() != TCI.ExitCmp) return false;
            }
            return true;
        }
        return Allowed.count(I) != 0;
    })) return false;
    if (ConditionalBranches != 1) return false;

    // No value computed by the loop may be observed after it.  The memory
    // effect is the only live-out and is replaced by the intrinsic call.
    for (Instruction *I : Allowed) {
        if (!I) continue;
        for (Use *U : I->getUses()) {
            auto *UserI = U ? dynamic_cast<Instruction *>(U->getUser()) : nullptr;
            if (!UserI || !UserI->getParent() ||
                !L->contains(UserI->getParent()))
                return false;
        }
    }
    return true;
}

GetElementPtrInst *LoopIdiomRecognizePass::materializeStartPointer(
    const LinearAccess &Access, PhiInst *IndVar, int64_t Start,
    BasicBlock *PreHeader) {
    if (!Access.Base || !IndVar || !PreHeader ||
        Access.InductionIndex >= Access.Indices.size() ||
        Start < std::numeric_limits<int32_t>::min() ||
        Start > std::numeric_limits<int32_t>::max())
        return nullptr;

    std::vector<Value *> Indices = Access.Indices;
    Value *StartValue = Value::makeIntegerLike(
        Indices[Access.InductionIndex], static_cast<int32_t>(Start));
    if (!StartValue) return nullptr;
    Indices[Access.InductionIndex] = StartValue;

    auto *StartGEP = new GetElementPtrInst(Access.Base, Indices);
    if (!PreHeader->insertBeforeTerminator(StartGEP)) {
        delete StartGEP;
        return nullptr;
    }
    return StartGEP;
}

bool LoopIdiomRecognizePass::matchSimpleCopyLoop(
    Loop *L, SCEVResult &SE, DominatorTree &DT,
    CopyLoopMatch &Match) {
    Match = {};
    if (!L || !L->isInnermost()) return false;

    auto TCI = SE.analyzeLoopTripCount(L);
    auto IV = SE.getCanonicalInduction(L);
    if (!TCI || !TCI->Valid || !IV || !IV->Valid ||
        !IV->ConstantStart || !IV->ConstantStep ||
        *IV->ConstantStep != 1 || TCI->IndVar != IV->Phi)
        return false;

    Match.L = L;
    Match.PreHeader = TCI->PreHeader;
    Match.Header = TCI->Header;
    Match.ExitBlock = TCI->ExitBlock;
    Match.IndVar = IV->Phi;
    Match.Start = *IV->ConstantStart;
    if (!Match.PreHeader || !Match.Header || !Match.ExitBlock)
        return false;

    if (TCI->ConstantTripCount) {
        if (!TCI->ProvenNoWrap || *TCI->ConstantTripCount <= 0)
            return false;
        Match.TripCount = *TCI->ConstantTripCount;
    } else {
        if (TCI->ExitingBlock != TCI->Header || Match.Start < 0)
            return false;
        BinaryInst::Operation Pred = TCI->ContinueOnTrue
                                         ? TCI->ExitCmp->getOp()
                                         : BinaryInst::negatePredicate(
                                               TCI->ExitCmp->getOp());
        Value *Bound = nullptr;
        if (TCI->ExitCmp->getOperand(0) == IV->Phi) {
            Bound = TCI->ExitCmp->getOperand(1);
        } else if (TCI->ExitCmp->getOperand(1) == IV->Phi) {
            Bound = TCI->ExitCmp->getOperand(0);
            Pred = BinaryInst::reversePredicate(Pred);
        }
        if (!Bound || Pred != BinaryInst::L || !Bound->getIRType() ||
            !Bound->getIRType()->isInt() ||
            Bound->getIRType()->getIntegerBitWidth(32) != 32)
            return false;
        Match.IsRuntime = true;
        Match.RuntimeBound = Bound;
    }

    std::vector<LoadInst *> Loads;
    auto LoopBlocks = L->getBlocksIncludingSubLoops();
    if (!forEachInstructionInBlocks(
            LoopBlocks, [&](Instruction *I) {
        if (auto *Store = dynamic_cast<StoreInst *>(I)) {
            if (Match.Store) return false;
            Match.Store = Store;
        } else if (auto *Load = dynamic_cast<LoadInst *>(I)) {
            Loads.push_back(Load);
        } else if (dynamic_cast<CallInst *>(I)) {
            return false;
        }
        return true;
    })) return false;
    if (!Match.Store || Loads.size() != 1 ||
        Match.Store->getValueOperand() != Loads.front())
        return false;
    Match.Load = Loads.front();

    // In a header-tested loop the header itself is reached before the exit
    // predicate can reject a zero-trip execution.  A memory access in that
    // block therefore also runs on the final failed test (and on a zero-trip
    // entry), which a count-sized memmove would not reproduce.
    if (TCI->ExitingBlock == TCI->Header &&
        (Match.Store->getParent() == TCI->Header ||
         Match.Load->getParent() == TCI->Header))
        return false;

    auto *DestGEP = dynamic_cast<GetElementPtrInst *>(
        Match.Store->getPointerOperand());
    auto *SourceGEP = dynamic_cast<GetElementPtrInst *>(
        Match.Load->getPointerOperand());
    if (!matchLinearAccess(DestGEP, IV->Phi, L, Match.Destination) ||
        !matchLinearAccess(SourceGEP, IV->Phi, L, Match.Source) ||
        !Match.Destination.ElementTy->hasSameStorageType(
            *Match.Source.ElementTy) ||
        !hasOnlyAllowedLoopInstructions(
            L, *TCI, *IV, Match.Store, DestGEP, Match.Load,
            SourceGEP, DT))
        return false;
    return true;
}

bool LoopIdiomRecognizePass::transformTemporaryCopyPair(
    Function &F, const CopyLoopMatch &First,
    const CopyLoopMatch &Second, DominatorTree &DT,
    bool &CFGChanged) {
    if (!First.L || !Second.L || First.L == Second.L ||
        First.ExitBlock != Second.PreHeader ||
        First.Start != Second.Start ||
        First.IsRuntime != Second.IsRuntime)
        return false;
    if (First.IsRuntime) {
        if (!First.RuntimeBound ||
            First.RuntimeBound != Second.RuntimeBound)
            return false;
    } else if (First.TripCount != Second.TripCount ||
               First.TripCount >
                   std::numeric_limits<int32_t>::max() / 4) {
        return false;
    }

    // The bridge is both the first loop's exit and the second loop's
    // preheader.  Any computation there could observe the temporary or be
    // reordered by bypassing both loops, so require a bare unconditional edge.
    BasicBlock *Bridge = First.ExitBlock;
    if (!Bridge || !Bridge->getBranchTerminator() ||
        Bridge->getBranchTerminator()->isConditional() ||
        Bridge->getSuccessors().size() != 1 ||
        Bridge->getSuccessors().front() != Second.Header)
        return false;
    for (Instruction *I : *Bridge)
        if (I != Bridge->getBranchTerminator()) return false;

    auto *Temporary = dynamic_cast<AllocaInst *>(First.Destination.Base);
    if (!Temporary || Second.Source.Base != Temporary ||
        !AllocaInst::isArrayAllocation(Temporary) ||
        !First.Source.ElementTy->hasSameStorageType(
            *Second.Destination.ElementTy))
        return false;

    GetElementPtrInst *TempWrite = First.Destination.GEP;
    GetElementPtrInst *TempRead = Second.Source.GEP;
    if (!TempWrite || !TempRead || TempWrite == TempRead ||
        TempWrite->getBasePointer() != Temporary ||
        TempRead->getBasePointer() != Temporary)
        return false;

    // The two loops must address precisely the same temporary element for
    // every IV value.  Merely sharing the alloca is insufficient for nested
    // arrays (for example, tmp[0][i] and tmp[1][i]).
    if (First.Destination.Indices.size() !=
            Second.Source.Indices.size() ||
        First.Destination.InductionIndex !=
            Second.Source.InductionIndex)
        return false;
    for (size_t I = 0; I < First.Destination.Indices.size(); ++I) {
        if (I == First.Destination.InductionIndex) continue;
        int WriteIndex = 0;
        int ReadIndex = 0;
        if (!Value::tryGetIntIndex(First.Destination.Indices[I],
                                   WriteIndex) ||
            !Value::tryGetIntIndex(Second.Source.Indices[I], ReadIndex) ||
            WriteIndex != ReadIndex)
            return false;
    }

    // Whole-object escape proof for the temporary.  Exactly the first-loop
    // store and second-loop load may use its two derived element pointers.
    std::unordered_set<Value *> TempPointerUses;
    for (Use *U : Temporary->getUses()) {
        if (!U || U->getValue() != Temporary) continue;
        Value *User = dynamic_cast<Value *>(U->getUser());
        if (User != TempWrite && User != TempRead) return false;
        TempPointerUses.insert(User);
    }
    if (TempPointerUses.size() != 2) return false;
    for (auto Pair : {std::make_pair(static_cast<Value *>(TempWrite),
                                    static_cast<Instruction *>(First.Store)),
                      std::make_pair(static_cast<Value *>(TempRead),
                                    static_cast<Instruction *>(Second.Load))}) {
        unsigned Uses = 0;
        for (Use *U : Pair.first->getUses()) {
            if (!U || U->getValue() != Pair.first) continue;
            if (U->getUser() != Pair.second) return false;
            ++Uses;
        }
        if (Uses != 1) return false;
    }

    auto AvailableAt = [&](Value *V, BasicBlock *Where) {
        auto *I = dynamic_cast<Instruction *>(V);
        if (!I) return true;
        BasicBlock *DefBB = I->getParent();
        if (!DefBB || !DT.dominates(DefBB, Where)) return false;
        return DefBB != Where ||
               Instruction::comesBefore(I, Where->back());
    };
    if (!AvailableAt(First.Source.Base, First.PreHeader) ||
        !AvailableAt(Second.Destination.Base, First.PreHeader))
        return false;
    if (First.IsRuntime &&
        !AvailableAt(First.RuntimeBound, First.PreHeader))
        return false;

    GetElementPtrInst *SourceStart = materializeStartPointer(
        First.Source, First.IndVar, First.Start, First.PreHeader);
    if (!SourceStart) return false;
    GetElementPtrInst *DestStart = materializeStartPointer(
        Second.Destination, Second.IndVar, Second.Start,
        First.PreHeader);
    if (!DestStart) {
        delete SourceStart;
        return false;
    }

    Value *ByteCount = nullptr;
    Value *RuntimeGuard = nullptr;
    BasicBlock *FastBlock = nullptr;
    std::vector<Instruction *> RuntimeValues;
    if (!First.IsRuntime) {
        ByteCount = ConstInt::newConstInt(
            static_cast<int32_t>(First.TripCount * 4));
    } else {
        BrInst *OldBranch = First.PreHeader->getBranchTerminator();
        if (!OldBranch || OldBranch->isConditional() ||
            First.PreHeader->getSuccessors().size() != 1 ||
            First.PreHeader->getSuccessors().front() != First.Header) {
            delete DestStart;
            delete SourceStart;
            return false;
        }
        for (Instruction *I : *Second.ExitBlock) {
            if (dynamic_cast<PhiInst *>(I)) {
                delete DestStart;
                delete SourceStart;
                return false;
            }
            break;
        }

        Value *Bound = First.RuntimeBound;
        Value *Start = Value::makeIntegerLike(Bound,
                                               static_cast<int32_t>(First.Start));
        Value *Zero = Value::makeIntegerLike(Bound, 0);
        Value *Four = Value::makeIntegerLike(Bound, 4);
        Value *MinCount = Value::makeIntegerLike(Bound, 8);
        Value *MaxCount = Value::makeIntegerLike(
            Bound, std::numeric_limits<int32_t>::max() / 4);
        if (!Start || !Zero || !Four || !MinCount || !MaxCount) {
            delete DestStart;
            delete SourceStart;
            return false;
        }
        auto Insert = [&](Instruction *I) {
            if (!I || !First.PreHeader->insertBeforeTerminator(I)) {
                delete I;
                return false;
            }
            RuntimeValues.push_back(I);
            return true;
        };
        auto *Positive = new BinaryInst(Bound, BinaryInst::G, Start);
        auto *Difference = new BinaryInst(Bound, BinaryInst::SUB, Start);
        if (!Insert(Positive) || !Insert(Difference)) goto PairFailure;
        {
            auto *Count = new SelectInst(Positive, Difference, Zero);
            if (!Insert(Count)) goto PairFailure;
            auto *LargeEnough = new BinaryInst(Count, BinaryInst::GE,
                                                MinCount);
            auto *Fits = new BinaryInst(Count, BinaryInst::LE, MaxCount);
            auto *Guard = new BinaryInst(LargeEnough, BinaryInst::AND, Fits);
            auto *Bytes = new BinaryInst(Count, BinaryInst::MUL, Four);
            if (!Insert(LargeEnough) || !Insert(Fits) || !Insert(Guard) ||
                !Insert(Bytes))
                goto PairFailure;
            RuntimeGuard = Guard;
            ByteCount = Bytes;
        }
        FastBlock = new BasicBlock();
        FastBlock->setIdent(F.makeUniqueBlockIdent(
            First.PreHeader->getIdent() + ".memmove"));
        F.pushBlock(FastBlock);
    }

    {
        std::vector<Value *> Args{
            DestStart, SourceStart, ByteCount,
            ConstBool::newConstBool(false)};
        auto *Call = new CallInst(BuildInFunction::genBuildInFunction(
                                      BuildInFunction::getDefaultMemmoveName()),
                                  Args);
        if (FastBlock) FastBlock->pushBack(Call);
        else if (!First.PreHeader->insertBeforeTerminator(Call)) {
            delete Call;
            goto PairFailure;
        }
        if (Module *M = Module::getActiveModule())
            M->registerBuiltin(BuildInFunction::getDefaultMemmoveName());
    }

    if (FastBlock) {
        delete First.PreHeader->getBranchTerminator();
        First.PreHeader->genCondInst(RuntimeGuard, FastBlock, First.Header);
        FastBlock->genUnCondInst(Second.ExitBlock);
        CFGChanged = true;
    } else {
        for (Instruction *I : {static_cast<Instruction *>(First.Store),
                               static_cast<Instruction *>(Second.Store)})
            delete I;
        for (Instruction *I : {static_cast<Instruction *>(First.Load),
                               static_cast<Instruction *>(Second.Load)})
            if (!I->hasUses()) delete I;
        for (Instruction *I : {static_cast<Instruction *>(First.Source.GEP),
                               static_cast<Instruction *>(TempWrite),
                               static_cast<Instruction *>(TempRead),
                               static_cast<Instruction *>(Second.Destination.GEP)})
            if (!I->hasUses()) delete I;
        if (!Temporary->hasUses()) delete Temporary;
    }
    return true;

PairFailure:
    if (FastBlock) {
        FastBlock->eraseFromParent();
        delete FastBlock;
    }
    for (auto It = RuntimeValues.rbegin(); It != RuntimeValues.rend(); ++It)
        delete *It;
    delete DestStart;
    delete SourceStart;
    return false;
}

bool LoopIdiomRecognizePass::transformLoop(
    Function &F, Loop *L, SCEVResult &SE, DominatorTree &DT,
    bool &CFGChanged) {
    if (!L || !L->isInnermost()) return false;
    BasicBlock *PreHeader = nullptr;
    BasicBlock *Latch = nullptr;
    if (!L->getLoopSimplifyForm(PreHeader, Latch) || !PreHeader ||
        !PreHeader->hasTerminator())
        return false;

    auto TCI = SE.analyzeLoopTripCount(L);
    auto IV = SE.getCanonicalInduction(L);
    if (!TCI || !TCI->Valid || !IV || !IV->Valid ||
        !IV->ConstantStart || !IV->ConstantStep ||
        *IV->ConstantStep != 1 || TCI->IndVar != IV->Phi)
        return false;

    const bool IsConstant = TCI->ConstantTripCount.has_value();
    if (IsConstant && (!TCI->ProvenNoWrap ||
                       *TCI->ConstantTripCount <= 0))
        return false;

    StoreInst *Store = nullptr;
    std::vector<LoadInst *> Loads;
    auto LoopBlocks = L->getBlocksIncludingSubLoops();
    if (!forEachInstructionInBlocks(
            LoopBlocks, [&](Instruction *I) {
        if (auto *SI = dynamic_cast<StoreInst *>(I)) {
            if (Store) return false;
            Store = SI;
        } else if (auto *LI = dynamic_cast<LoadInst *>(I)) {
            Loads.push_back(LI);
        } else if (dynamic_cast<CallInst *>(I)) {
            return false;
        }
        return true;
    })) return false;
    if (!Store) return false;

    auto *DestGEP = dynamic_cast<GetElementPtrInst *>(
        Store->getPointerOperand());
    LinearAccess DestAccess;
    if (!matchLinearAccess(DestGEP, IV->Phi, L, DestAccess)) return false;

    // In a header-tested loop, an operation placed in the exiting header runs
    // once for the final failed test as well (and even on a zero-trip entry).
    // SCEV's trip count describes successful body iterations, so replacing
    // such a store with exactly TripCount elements would drop one observable
    // write.  Require the memory effect to be control-dependent on the
    // continue edge; the canonical body/latch forms satisfy this naturally.
    if (TCI->ExitingBlock == TCI->Header &&
        Store->getParent() == TCI->Header)
        return false;

    int64_t TripCount = 0;
    int32_t ConstantByteCount = 0;
    if (IsConstant) {
        TripCount = *TCI->ConstantTripCount;
        if (TripCount > std::numeric_limits<int32_t>::max() / 4)
            return false;
        ConstantByteCount = static_cast<int32_t>(TripCount * 4);
    }

    LoadInst *SourceLoad = nullptr;
    GetElementPtrInst *SourceGEP = nullptr;
    LinearAccess SourceAccess;
    uint8_t FillByte = 0;
    bool IsMemset = getRepeatedByte(Store->getValueOperand(), FillByte);
    if (IsMemset) {
        if (!Loads.empty()) return false;
    } else {
        SourceLoad = dynamic_cast<LoadInst *>(Store->getValueOperand());
        if (!SourceLoad || Loads.size() != 1 || Loads.front() != SourceLoad)
            return false;
        SourceGEP = dynamic_cast<GetElementPtrInst *>(
            SourceLoad->getPointerOperand());
        if (!matchLinearAccess(SourceGEP, IV->Phi, L, SourceAccess) ||
            !DestAccess.ElementTy->hasSameStorageType(*SourceAccess.ElementTy) ||
            !areProvablyDisjoint(DestAccess, SourceAccess))
            return false;
    }

    // A source load in an exiting header executes for the final failed test,
    // even though the body store does not.  Replacing it with a count-sized
    // memcpy would silently remove that potentially faulting extra access.
    if (TCI->ExitingBlock == TCI->Header && SourceLoad &&
        SourceLoad->getParent() == TCI->Header)
        return false;

    if (!hasOnlyAllowedLoopInstructions(
            L, *TCI, *IV, Store, DestGEP, SourceLoad, SourceGEP, DT))
        return false;

    auto *DestStart = materializeStartPointer(
        DestAccess, IV->Phi, *IV->ConstantStart, PreHeader);
    if (!DestStart) return false;

    GetElementPtrInst *SourceStart = nullptr;
    if (!IsMemset) {
        SourceStart = materializeStartPointer(
            SourceAccess, IV->Phi, *IV->ConstantStart, PreHeader);
        if (!SourceStart) {
            delete DestStart;
            return false;
        }
    }

    Value *ByteCount = nullptr;
    BasicBlock *CallBlock = PreHeader;
    BasicBlock *FastBlock = nullptr;
    Value *RuntimeGuard = nullptr;
    std::vector<Instruction *> RuntimeValues;

    if (IsConstant) {
        ByteCount = ConstInt::newConstInt(ConstantByteCount);
    } else {
        // Runtime recognition is deliberately limited to a header-tested
        // signed `i < bound` recurrence with constant non-negative start and
        // unit step.  It cannot wrap: when bound > start, the final update is
        // exactly bound (an i32 value); otherwise the loop is zero-trip.
        if (TCI->ExitingBlock != TCI->Header ||
            *IV->ConstantStart < 0 ||
            *IV->ConstantStart > std::numeric_limits<int32_t>::max()) {
            delete SourceStart;
            delete DestStart;
            return false;
        }

        BinaryInst::Operation Pred = TCI->ContinueOnTrue
                                         ? TCI->ExitCmp->getOp()
                                         : BinaryInst::negatePredicate(
                                               TCI->ExitCmp->getOp());
        Value *Bound = nullptr;
        if (TCI->ExitCmp->getOperand(0) == IV->Phi) {
            Bound = TCI->ExitCmp->getOperand(1);
        } else if (TCI->ExitCmp->getOperand(1) == IV->Phi) {
            Bound = TCI->ExitCmp->getOperand(0);
            Pred = BinaryInst::reversePredicate(Pred);
        }
        if (!Bound || Pred != BinaryInst::L || !Bound->getIRType() ||
            !Bound->getIRType()->isInt() ||
            Bound->getIRType()->getIntegerBitWidth(32) != 32) {
            delete SourceStart;
            delete DestStart;
            return false;
        }
        if (auto *BoundI = dynamic_cast<Instruction *>(Bound)) {
            BasicBlock *BoundBB = BoundI->getParent();
            if (!BoundBB || L->contains(BoundBB) ||
                !DT.dominates(BoundBB, PreHeader) ||
                (BoundBB == PreHeader &&
                 !Instruction::comesBefore(BoundI,
                                            PreHeader->back()))) {
                delete SourceStart;
                delete DestStart;
                return false;
            }
        }

        // A versioned fast path avoids changing short-loop cost and keeps a
        // scalar fallback for counts whose byte size would overflow i32.
        // The pass only handles loops without live-outs, so the fast block can
        // branch directly to the unique exit without synthesizing SSA values.
        if (!PreHeader->getBranchTerminator() ||
            PreHeader->getBranchTerminator()->isConditional() ||
            PreHeader->getSuccessors().size() != 1 ||
            PreHeader->getSuccessors().front() != TCI->Header) {
            delete SourceStart;
            delete DestStart;
            return false;
        }
        for (Instruction *ExitI : *TCI->ExitBlock) {
            if (dynamic_cast<PhiInst *>(ExitI)) {
                delete SourceStart;
                delete DestStart;
                return false;
            }
            break;
        }

        Value *StartValue = Value::makeIntegerLike(
            Bound, static_cast<int32_t>(*IV->ConstantStart));
        Value *ZeroValue = Value::makeIntegerLike(Bound, 0);
        Value *FourValue = Value::makeIntegerLike(Bound, 4);
        Value *MinFastCount = Value::makeIntegerLike(Bound, 8);
        Value *MaxFastCount = Value::makeIntegerLike(
            Bound, std::numeric_limits<int32_t>::max() / 4);
        if (!StartValue || !ZeroValue || !FourValue || !MinFastCount ||
            !MaxFastCount) {
            delete SourceStart;
            delete DestStart;
            return false;
        }

        auto InsertRuntime = [&](Instruction *I) -> bool {
            if (!I || !PreHeader->insertBeforeTerminator(I)) {
                delete I;
                return false;
            }
            RuntimeValues.push_back(I);
            return true;
        };

        auto *HasIterations = new BinaryInst(Bound, BinaryInst::G,
                                             StartValue);
        auto *Difference = new BinaryInst(Bound, BinaryInst::SUB,
                                          StartValue);
        if (!InsertRuntime(HasIterations) || !InsertRuntime(Difference)) {
            for (auto It = RuntimeValues.rbegin();
                 It != RuntimeValues.rend(); ++It)
                delete *It;
            delete SourceStart;
            delete DestStart;
            return false;
        }
        auto *Count = new SelectInst(HasIterations, Difference, ZeroValue);
        if (!InsertRuntime(Count)) {
            for (auto It = RuntimeValues.rbegin();
                 It != RuntimeValues.rend(); ++It)
                delete *It;
            delete SourceStart;
            delete DestStart;
            return false;
        }
        auto *EnoughWork = new BinaryInst(Count, BinaryInst::GE,
                                          MinFastCount);
        auto *FitsByteCount = new BinaryInst(Count, BinaryInst::LE,
                                             MaxFastCount);
        auto *UseFastPath = new BinaryInst(EnoughWork, BinaryInst::AND,
                                           FitsByteCount);
        auto *Bytes = new BinaryInst(Count, BinaryInst::MUL, FourValue);
        if (!InsertRuntime(EnoughWork) || !InsertRuntime(FitsByteCount) ||
            !InsertRuntime(UseFastPath) || !InsertRuntime(Bytes)) {
            for (auto It = RuntimeValues.rbegin();
                 It != RuntimeValues.rend(); ++It)
                delete *It;
            delete SourceStart;
            delete DestStart;
            return false;
        }
        RuntimeGuard = UseFastPath;
        ByteCount = Bytes;

        FastBlock = new BasicBlock();
        FastBlock->setIdent(F.makeUniqueBlockIdent(
            PreHeader->getIdent() + ".memidiom"));
        F.pushBlock(FastBlock);
        CallBlock = FastBlock;
    }

    std::vector<Value *> Args;
    const char *IntrinsicName = nullptr;
    if (IsMemset) {
        IntrinsicName = BuildInFunction::getDefaultMemsetName();
        Args = {DestStart, ConstInt::newConstInt(FillByte, 1),
                ByteCount,
                ConstBool::newConstBool(false)};
    } else {
        IntrinsicName = BuildInFunction::getDefaultMemcpyName();
        Args = {DestStart, SourceStart, ByteCount,
                ConstBool::newConstBool(false)};
    }
    auto *Call = new CallInst(
        BuildInFunction::genBuildInFunction(IntrinsicName), Args);
    bool InsertedCall = FastBlock ? (FastBlock->pushBack(Call), true)
                                  : PreHeader->insertBeforeTerminator(Call);
    if (!InsertedCall) {
        delete Call;
        if (FastBlock) {
            FastBlock->eraseFromParent();
            delete FastBlock;
        }
        for (auto It = RuntimeValues.rbegin();
             It != RuntimeValues.rend(); ++It)
            delete *It;
        delete SourceStart;
        delete DestStart;
        return false;
    }
    if (Module *M = Module::getActiveModule()) M->registerBuiltin(IntrinsicName);

    if (FastBlock) {
        BrInst *OldBranch = PreHeader->getBranchTerminator();
        delete OldBranch;
        PreHeader->genCondInst(RuntimeGuard, FastBlock, TCI->Header);
        FastBlock->genUnCondInst(TCI->ExitBlock);
        CFGChanged = true;
    } else {
        delete Store;
        if (SourceLoad && !SourceLoad->hasUses()) delete SourceLoad;
        if (SourceGEP && !SourceGEP->hasUses()) delete SourceGEP;
        if (DestGEP && !DestGEP->hasUses()) delete DestGEP;
    }
    return true;
}

PreservationStatus LoopIdiomRecognizePass::run(
    Function &F, FunctionAnalysisPassManager &FAM) {
    auto &DT = FAM.getResult<DominantAnalysis>(F);
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &SE = FAM.getResult<SCEVAnalysis>(F);

    bool Changed = false;
    bool CFGChanged = false;
    std::vector<Loop *> Loops = LI.getLoopsInPostOrder(true);

    // A direct memmove replacement is legal only for a two-loop transaction
    // through a private temporary: tmp[i] = src[i], followed immediately by
    // dst[i] = tmp[i].  Ordinary one-way copies are intentionally excluded.
    std::vector<CopyLoopMatch> Copies;
    Copies.reserve(Loops.size());
    for (Loop *L : Loops) {
        CopyLoopMatch Match;
        if (matchSimpleCopyLoop(L, SE, DT, Match))
            Copies.push_back(Match);
    }
    for (const CopyLoopMatch &First : Copies) {
        for (const CopyLoopMatch &Second : Copies) {
            if (transformTemporaryCopyPair(
                    F, First, Second, DT, CFGChanged)) {
                // Even a constant-size pair deletes memory instructions from
                // two loops.  Rebuild MemorySSA/SCEV/LoopInfo before looking
                // for another pair or scalar idiom.
                return PreservationStatus::none();
            }
        }
    }

    for (Loop *L : Loops) {
        Changed |= transformLoop(F, L, SE, DT, CFGChanged);
        // A versioned loop changes predecessor sets and invalidates the loop
        // forest.  Let the surrounding fixed-point pipeline rebuild analyses
        // before considering another runtime idiom.
        if (CFGChanged) break;
    }
    if (!Changed) return PreservationStatus::all();

    if (CFGChanged) return PreservationStatus::none();

    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<LoopAnalysis>();
    PA.discard<BasicAliasAnalysis>();
    PA.discard<RangeAnalysis>();
    PA.discard<SCEVAnalysis>();
    PA.discard<SideEffectAnalysis>();
    return PA;
}
