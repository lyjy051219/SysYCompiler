#include "ir/opt/LoopIdiomRecognize.hpp"

#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/analysis/SideEffect.hpp"

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
    for (BasicBlock *BB : L->getBlocksIncludingSubLoops()) {
        if (!BB) return false;
        for (Instruction *I : *BB) {
            if (!I) return false;
            if (auto *Br = dynamic_cast<BrInst *>(I)) {
                if (Br->isConditional()) {
                    ++ConditionalBranches;
                    if (Br->getCondition() != TCI.ExitCmp) return false;
                }
                continue;
            }
            if (!Allowed.count(I)) return false;
        }
    }
    if (ConditionalBranches != 1) return false;

    // No value computed by the loop may be observed after it.  The memory
    // effect is the only live-out and is replaced by the intrinsic call.
    for (Instruction *I : Allowed) {
        if (!I) continue;
        for (auto It = I->getUseList().begin();
             It != I->getUseList().end(); ++It) {
            Use *U = *It;
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

bool LoopIdiomRecognizePass::transformLoop(
    Loop *L, SCEVResult &SE, DominatorTree &DT) {
    if (!L || !L->isInnermost()) return false;
    BasicBlock *PreHeader = nullptr;
    BasicBlock *Latch = nullptr;
    if (!L->getLoopSimplifyForm(PreHeader, Latch) || !PreHeader ||
        !PreHeader->hasTerminator())
        return false;

    auto TCI = SE.analyzeLoopTripCount(L);
    auto IV = SE.getCanonicalInduction(L);
    if (!TCI || !TCI->Valid || !TCI->ProvenNoWrap ||
        !TCI->ConstantTripCount ||
        *TCI->ConstantTripCount <= 0 || !IV || !IV->Valid ||
        !IV->ConstantStart || !IV->ConstantStep ||
        *IV->ConstantStep != 1 || TCI->IndVar != IV->Phi)
        return false;

    StoreInst *Store = nullptr;
    std::vector<LoadInst *> Loads;
    for (BasicBlock *BB : L->getBlocksIncludingSubLoops()) {
        for (Instruction *I : *BB) {
            if (auto *SI = dynamic_cast<StoreInst *>(I)) {
                if (Store) return false;
                Store = SI;
            } else if (auto *LI = dynamic_cast<LoadInst *>(I)) {
                Loads.push_back(LI);
            } else if (dynamic_cast<CallInst *>(I)) {
                return false;
            }
        }
    }
    if (!Store) return false;

    auto *DestGEP = dynamic_cast<GetElementPtrInst *>(
        Store->getPointerOperand());
    LinearAccess DestAccess;
    if (!matchLinearAccess(DestGEP, IV->Phi, L, DestAccess)) return false;

    const int64_t TripCount = *TCI->ConstantTripCount;
    if (TripCount > std::numeric_limits<int32_t>::max() / 4)
        return false;
    const int32_t ByteCount = static_cast<int32_t>(TripCount * 4);

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

    std::vector<Value *> Args;
    const char *IntrinsicName = nullptr;
    if (IsMemset) {
        IntrinsicName = BuildInFunction::getDefaultMemsetName();
        Args = {DestStart, ConstInt::newConstInt(FillByte, 1),
                ConstInt::newConstInt(ByteCount),
                ConstBool::newConstBool(false)};
    } else {
        IntrinsicName = BuildInFunction::getDefaultMemcpyName();
        Args = {DestStart, SourceStart, ConstInt::newConstInt(ByteCount),
                ConstBool::newConstBool(false)};
    }
    auto *Call = new CallInst(
        BuildInFunction::genBuildInFunction(IntrinsicName), Args);
    if (!PreHeader->insertBeforeTerminator(Call)) {
        delete Call;
        delete SourceStart;
        delete DestStart;
        return false;
    }
    if (Module *M = Module::getActiveModule()) M->registerBuiltin(IntrinsicName);

    delete Store;
    if (SourceLoad && SourceLoad->getUseList().is_empty()) delete SourceLoad;
    if (SourceGEP && SourceGEP->getUseList().is_empty()) delete SourceGEP;
    if (DestGEP && DestGEP->getUseList().is_empty()) delete DestGEP;
    return true;
}

PreservationStatus LoopIdiomRecognizePass::run(
    Function &F, FunctionAnalysisPassManager &FAM) {
    auto &DT = FAM.getResult<DominantAnalysis>(F);
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &SE = FAM.getResult<SCEVAnalysis>(F);

    bool Changed = false;
    for (Loop *L : LI.getLoopsInPostOrder(true))
        Changed |= transformLoop(L, SE, DT);
    if (!Changed) return PreservationStatus::all();

    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<LoopAnalysis>();
    PA.discard<BasicAliasAnalysis>();
    PA.discard<RangeAnalysis>();
    PA.discard<SCEVAnalysis>();
    PA.discard<SideEffectAnalysis>();
    return PA;
}
