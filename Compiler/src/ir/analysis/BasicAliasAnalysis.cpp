#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/SCEV.hpp"
#include "lib/TraverseTools.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>

MemoryLocation::MemoryLocation() = default;

MemoryLocation::MemoryLocation(Value *P, std::optional<std::size_t> S)
    : Pointer(P), Size(S) {}

BasicAAResult::BasicAAResult() = default;

BasicAAResult::BasicAAResult(SCEVResult *SCEV) : SE(SCEV) {}

BasicAAResult::BasicAAResult(BasicAAResult &&) = default;

BasicAAResult &BasicAAResult::operator=(BasicAAResult &&) = default;

std::size_t BasicAAResult::ValuePairHash::operator()(
    const std::pair<Value *, Value *> &P) const {
    auto H1 = static_cast<std::size_t>(
        reinterpret_cast<std::uintptr_t>(P.first));
    auto H2 = static_cast<std::size_t>(
        reinterpret_cast<std::uintptr_t>(P.second));
    return H1 ^ (H2 + 0x9e3779b97f4a7c15ULL + (H1 << 6) + (H1 >> 2));
}

bool BasicAAResult::checkedAdd(long long A, long long B, long long &Result) {
    if ((B > 0 && A > std::numeric_limits<long long>::max() - B) ||
        (B < 0 && A < std::numeric_limits<long long>::min() - B))
        return false;
    Result = A + B;
    return true;
}

bool BasicAAResult::checkedSub(long long A, long long B, long long &Result) {
    if (B == std::numeric_limits<long long>::min()) {
        if (A >= 0) return false;
        Result = A - B;
        return true;
    }
    return checkedAdd(A, -B, Result);
}

bool BasicAAResult::checkedMul(long long A, long long B, long long &Result) {
    if (A == 0 || B == 0) {
        Result = 0;
        return true;
    }
    if ((A == -1 && B == std::numeric_limits<long long>::min()) ||
        (B == -1 && A == std::numeric_limits<long long>::min()))
        return false;
    if (A > 0) {
        if (B > 0) {
            if (A > std::numeric_limits<long long>::max() / B) return false;
        } else if (B < std::numeric_limits<long long>::min() / A) {
            return false;
        }
    } else {
        if (B > 0) {
            if (A < std::numeric_limits<long long>::min() / B) return false;
        } else if (A < std::numeric_limits<long long>::max() / B) {
            return false;
        }
    }
    Result = A * B;
    return true;
}

bool BasicAAResult::hasUndefPointerOrigin(
    Value *V, std::unordered_set<Value *> &Active, unsigned Depth) {
    if (!V || Value::isUndef(V)) return true;
    if (Depth > 32) return true;

    auto *I = dynamic_cast<Instruction *>(V);
    if (!I) return false;
    const bool TransparentPointerProducer =
        dynamic_cast<GetElementPtrInst *>(I) ||
        dynamic_cast<BitCastInst *>(I) ||
        dynamic_cast<PhiInst *>(I) ||
        dynamic_cast<SelectInst *>(I);
    if (!TransparentPointerProducer) return false;
    ScopedActiveMark ActiveMark(Active, V);
    if (!ActiveMark.inserted()) return false;

    bool HasUndefOrigin = false;
    forEachOperand(
        I, [&](size_t, Use *, Value *Operand) {
            if (dynamic_cast<BasicBlock *>(Operand)) return true;
            if (!hasUndefPointerOrigin(Operand, Active, Depth + 1))
                return true;
            HasUndefOrigin = true;
            return false;
        });
    return HasUndefOrigin;
}

MemoryLocation MemoryLocation::get(const LoadInst *Load) {
    if (!Load) return {};
    std::shared_ptr<IRType> Ty = Load->getIRType();
    return {Load->getPointerOperand(),
            Ty ? std::optional<std::size_t>(Ty->getSize()) : std::nullopt};
}

MemoryLocation MemoryLocation::get(const StoreInst *Store) {
    if (!Store) return {};
    Value *Stored = Store->getValueOperand();
    std::shared_ptr<IRType> Ty = Stored ? Stored->getIRType() : nullptr;
    return {Store->getPointerOperand(),
            Ty ? std::optional<std::size_t>(Ty->getSize()) : std::nullopt};
}

bool BasicAAResult::invalidate(Function &F, const PreservationStatus &PA) {
    if (PA.isKept(BasicAliasAnalysis::ID()) &&
        (!SE || PA.isKept(SCEVAnalysis::ID()))) {
        return false;
    }
    (void)F;
    AliasCache.clear();
    return true;
}

void BasicAAResult::addObjectNoAliasFact(Value* V1, Value* V2) {
    if (!V1 || !V2 || V1 == V2) {
        return;
    }
    ObjectNoAlias.insert(makeCanonicalPair(V1, V2));
}

bool BasicAAResult::hasObjectNoAlias(Value* V1, Value* V2) const {
    if (!V1 || !V2 || V1 == V2) {
        return false;
    }
    return ObjectNoAlias.find(makeCanonicalPair(V1, V2)) != ObjectNoAlias.end();
}

bool BasicAAResult::callMayAliasPointer(CallInst *CI, Value *P,
                                        const LoopInfo &LI) {
    if (!CI || !P) return false;
    (void)LI;
    for (unsigned I = 0; I < CI->getArgCount(); ++I) {
        Value *Arg = CI->getArg(I);
        if (!Arg || !Arg->isPointerValue()) continue;
        // A call parameter describes an object-range access, not merely one
        // pointee-sized access at the argument's immediate address.  Distinct
        // GEP offsets into one array therefore remain potentially aliasing.
        if (!haveKnownDistinctUnderlyingObjects(P, Arg)) {
            return true;
        }
    }
    return false;
}

std::size_t BasicAAResult::getPointeeAccessSize(Value* V) const {
    std::shared_ptr<IRType> Pointee =
        V && V->getIRType() ? V->getIRType()->getPointeeType() : nullptr;
    return Pointee ? Pointee->getSize() : 0;
}

Value* BasicAAResult::stripBaseInst(Value* V) const {
    std::unordered_set<Value*> Visited;
    while (V && Visited.insert(V).second) {
        if (!V->isPointerValue()) {
            return V;
        }

        if (auto *GEP = dynamic_cast<GetElementPtrInst*>(V)) {
            bool AllZero = true;
            for (Value *Idx : GEP->getIndices()) {
                if (!Idx || !Idx->isConstIntZero()) {
                    AllZero = false;
                    break;
                }
            }
            if (!AllZero) {
                return V;
            }
            V = GEP->getBasePointer();
            continue;
        }

        // Keep bitcasts in the query value.  Although getBaseAddress() and
        // fillLinearGEP() may look through a cast to identify the allocation,
        // aliasGEP must still see the cast result's pointee width.  Stripping
        // it here can turn a wide access into the source's narrow access and
        // manufacture NoAlias for overlapping byte ranges.
        if (dynamic_cast<BitCastInst*>(V)) return V;

        if (auto *Phi = dynamic_cast<PhiInst*>(V)) {
            if (Phi->getNumIncomingValues() == 1) {
                V = Phi->getIncomingValue(0);
                continue;
            }
        }

        return V;
    }
    return V;
}

Value* BasicAAResult::getBaseAddress(Value* V) const {
    std::unordered_set<Value*> Visited;
    while (V && Visited.insert(V).second) {
        if (auto *GEP = dynamic_cast<GetElementPtrInst*>(V)) {
            V = GEP->getBasePointer();
            continue;
        }

        if (auto *BC = dynamic_cast<BitCastInst*>(V)) {
            V = BC->getUnaryOperand();
            continue;
        }

        if (auto *Phi = dynamic_cast<PhiInst*>(V)) {
            if (Phi->getNumIncomingValues() == 1) {
                V = Phi->getIncomingValue(0);
                continue;
            }
        }

        return V;
    }
    return V;
}

bool BasicAAResult::haveKnownDistinctUnderlyingObjects(Value* V1,
                                                        Value* V2) const {
    Value *O1 = getBaseAddress(V1);
    Value *O2 = getBaseAddress(V2);
    if (!O1 || !O2 || O1 == O2 || O1->isUndef() || O2->isUndef())
        return false;

    auto *Var1 = dynamic_cast<Variable*>(O1);
    auto *Var2 = dynamic_cast<Variable*>(O2);
    bool IsAlloca1 = dynamic_cast<AllocaInst*>(O1) != nullptr;
    bool IsAlloca2 = dynamic_cast<AllocaInst*>(O2) != nullptr;
    bool IsParam1 = Var1 && Var1->vartag == Variable::Param;
    bool IsParam2 = Var2 && Var2->vartag == Variable::Param;
    bool IsGlobal1 = Var1 && Var1->vartag == Variable::GlobalVariable;
    bool IsGlobal2 = Var2 && Var2->vartag == Variable::GlobalVariable;
    bool IsConst1 = Var1 && Var1->vartag == Variable::Constant;
    bool IsConst2 = Var2 && Var2->vartag == Variable::Constant;

    if (IsAlloca1 && IsAlloca2)
        return true;
    if ((IsGlobal1 || IsConst1) && (IsGlobal2 || IsConst2))
        return true;
    if ((IsParam1 && IsAlloca2) || (IsParam2 && IsAlloca1))
        return true;
    if ((IsAlloca1 && (IsGlobal2 || IsConst2)) ||
        (IsAlloca2 && (IsGlobal1 || IsConst1)))
        return true;

    return hasObjectNoAlias(O1, O2);
}

bool BasicAAResult::isEqualAlias(Value* V1, Value* V2, const LoopInfo& LI) const {
    (void)LI;
    // Alias queries describe two SSA pointer values in the same execution.
    // A loop-carried dependence question is a different analysis problem:
    // weakening `alias(P, P)` merely because P is defined in a loop loses the
    // fundamental reflexivity property and prevents safe local forwarding.
    return V1 == V2;
}

AliasInfo BasicAAResult::mergePhiAlias(AliasInfo A, AliasInfo B) const {
    if (A == B) {
        return A;
    }
    if (A == AliasInfo::MayAlias || B == AliasInfo::MayAlias) {
        return AliasInfo::MayAlias;
    }
    return AliasInfo::MayAlias;
}

std::pair<Value*, Value*> BasicAAResult::makeCanonicalPair(Value* A, Value* B) const {
    auto AddrA = reinterpret_cast<std::uintptr_t>(A);
    auto AddrB = reinterpret_cast<std::uintptr_t>(B);
    return AddrA <= AddrB ? std::make_pair(A, B) : std::make_pair(B, A);
}

bool BasicAAResult::accumulateGEPIndices(GetElementPtrInst* GEP, LinearGEP& Expr) const {
    Value* BasePtr = GEP ? GEP->getBasePointer() : nullptr;
    if (!BasePtr || !BasePtr->getIRType()) {
        return false;
    }

    std::shared_ptr<IRType> CurType = BasePtr->getIRType();
    for (size_t i = 0; i < GEP->getNumIndices(); ++i) {
        CurType = CurType ? CurType->getIndexableElementType() : nullptr;
        if (!CurType) return false;
        std::size_t Stride = CurType->getSize();

        Value* Idx = GEP->getIndex(i);
        if (auto *CI = dynamic_cast<ConstInt*>(Idx)) {
            if (Stride > static_cast<std::size_t>(
                             std::numeric_limits<long long>::max()))
                return false;
            long long Scaled = 0;
            long long Updated = 0;
            if (!checkedMul(static_cast<long long>(CI->getVal()),
                            static_cast<long long>(Stride), Scaled) ||
                !checkedAdd(Expr.ConstantOffset, Scaled, Updated))
                return false;
            Expr.ConstantOffset = Updated;
        } else if (Idx) {
            if (Stride > static_cast<std::size_t>(
                             std::numeric_limits<long long>::max()))
                return false;
            long long Updated = 0;
            if (!checkedAdd(Expr.VariableOffsets[Idx],
                            static_cast<long long>(Stride), Updated))
                return false;
            Expr.VariableOffsets[Idx] = Updated;
        } else {
            return false;
        }
    }

    return true;
}

BasicAAResult::LinearGEP BasicAAResult::fillLinearGEP(Value* V) const {
    LinearGEP Expr;
    std::unordered_set<Value*> Visited;

    while (V && Visited.insert(V).second) {
        if (auto *GEP = dynamic_cast<GetElementPtrInst*>(V)) {
            if (!accumulateGEPIndices(GEP, Expr)) {
                Expr.Valid = false;
                return Expr;
            }
            V = GEP->getBasePointer();
            continue;
        }

        if (auto *BC = dynamic_cast<BitCastInst*>(V)) {
            V = BC->getUnaryOperand();
            continue;
        }

        if (auto *Phi = dynamic_cast<PhiInst*>(V)) {
            if (Phi->getNumIncomingValues() == 1) {
                V = Phi->getIncomingValue(0);
                continue;
            }
        }

        Expr.Base = V;
        return Expr;
    }

    Expr.Valid = false;
    return Expr;
}

BasicAAResult::AdditiveDecomposition
BasicAAResult::decomposeAdditive(Value* V) const {
    AdditiveDecomposition Result;
    Result.Base = V;
    std::unordered_set<Value*> Visited;

    while (Result.Base && Visited.insert(Result.Base).second) {
        auto *BI = dynamic_cast<BinaryInst*>(Result.Base);
        if (!BI) {
            return Result;
        }

        Value* LHS = BI->getOperand(0);
        Value* RHS = BI->getOperand(1);
        auto *LC = dynamic_cast<ConstInt*>(LHS);
        auto *RC = dynamic_cast<ConstInt*>(RHS);

        if (BI->getOp() == BinaryInst::ADD) {
            if (RC) {
                long long Updated = 0;
                if (!checkedAdd(Result.Offset,
                                static_cast<long long>(RC->getVal()), Updated)) {
                    Result.Valid = false;
                    return Result;
                }
                Result.Offset = Updated;
                Result.Base = LHS;
                continue;
            }
            if (LC) {
                long long Updated = 0;
                if (!checkedAdd(Result.Offset,
                                static_cast<long long>(LC->getVal()), Updated)) {
                    Result.Valid = false;
                    return Result;
                }
                Result.Offset = Updated;
                Result.Base = RHS;
                continue;
            }
        } else if (BI->getOp() == BinaryInst::SUB && RC) {
            long long Updated = 0;
            if (!checkedSub(Result.Offset,
                            static_cast<long long>(RC->getVal()), Updated)) {
                Result.Valid = false;
                return Result;
            }
            Result.Offset = Updated;
            Result.Base = LHS;
            continue;
        }

        return Result;
    }

    Result.Valid = false;
    return Result;
}

std::optional<long long>
BasicAAResult::getAdditiveOffsetFrom(Value* V, Value* Base) const {
    if (!V || !Base) {
        return std::nullopt;
    }
    if (V == Base) {
        return 0;
    }

    auto Dec = decomposeAdditive(V);
    if (!Dec.Valid) {
        return std::nullopt;
    }
    if (Dec.Base == Base) {
        return Dec.Offset;
    }
    return std::nullopt;
}

bool BasicAAResult::isLoopInvariant(Value* V, const Loop* L) const {
    if (!V || !L) {
        return true;
    }
    if (V->isConst()) {
        return true;
    }
    if (auto *Var = dynamic_cast<Variable*>(V)) {
        return Var->isParam() || Var->isGlobal();
    }
    auto *I = dynamic_cast<Instruction*>(V);
    if (!I) {
        return true;
    }
    BasicBlock* BB = I->getParent();
    return !BB || !L->contains(BB);
}

std::optional<BasicAAResult::PositivePhiRecurrence>
BasicAAResult::getPositivePhiRecurrence(PhiInst* Phi,
                                        const LoopInfo& LI) const {
    if (!Phi || !Phi->getParent()) {
        return std::nullopt;
    }

    Loop* L = LI.getLoopFor(Phi->getParent());
    if (!L || L->getHeader() != Phi->getParent()) {
        return std::nullopt;
    }

    if (!SE) return std::nullopt;
    auto Induction = SE->getCanonicalInduction(L);
    if (!Induction || !Induction->Valid || Induction->Phi != Phi ||
        !Induction->StartValue || !Induction->UpdateValue ||
        !Induction->ConstantStep || *Induction->ConstantStep <= 0 ||
        !SE->isKnownNoWrapInductionValue(L, Induction->UpdateValue)) {
        return std::nullopt;
    }
    return PositivePhiRecurrence{Induction->StartValue,
                                 Induction->UpdateValue,
                                 *Induction->ConstantStep, L};
}

std::optional<long long>
BasicAAResult::lowerBoundDiff(Value* Greater, Value* Lesser,
                              const LoopInfo& LI) const {
    if (!Greater || !Lesser) {
        return std::nullopt;
    }

    if (Greater == Lesser) return 0;

    auto Dec = decomposeAdditive(Greater);
    if (!Dec.Valid) {
        return std::nullopt;
    }

    auto *Phi = dynamic_cast<PhiInst*>(Dec.Base);
    auto Rec = getPositivePhiRecurrence(Phi, LI);
    if (!Rec) {
        return std::nullopt;
    }

    if (!isLoopInvariant(Lesser, Rec->L)) {
        return std::nullopt;
    }

    auto StartOff = getAdditiveOffsetFrom(Rec->Start, Lesser);
    if (!StartOff || *StartOff != 0) {
        return std::nullopt;
    }

    // A positive syntactic offset is not a signed lower bound when i32 ADD
    // may wrap.  Consume only the exact canonical update covered by SCEV's
    // finite/no-wrap proof; arbitrary `phi + C` remains unknown.
    if (Dec.Offset != 0 &&
        (Greater != Rec->Update || Dec.Offset != Rec->Step)) {
        return std::nullopt;
    }

    long long LB = 0;
    if (!checkedAdd(*StartOff, Dec.Offset, LB)) return std::nullopt;
    if (LB < 0) {
        return std::nullopt;
    }
    return LB;
}

AliasInfo BasicAAResult::aliasGEPByIndexDistance(
    const std::unordered_map<Value*, long long>& DiffVars,
    long long ConstDiff, std::size_t Threshold, const LoopInfo& LI) const {
    // First handle an affine relation encoded directly in SSA, such as
    //   j = i + 1;  A[j] versus A[i].
    // This proof does not assume no-wrap.  For signed i32 ADD/SUB, `j - i`
    // has at most two mathematical representatives: C and C +/- 2^32.  If
    // both byte distances are outside the touched range, the accesses cannot
    // overlap even on the wrapping execution.
    if (DiffVars.size() == 2 && Threshold > 0 &&
        Threshold <= static_cast<std::size_t>(
                         std::numeric_limits<long long>::max())) {
        auto It = DiffVars.begin();
        Value *A = It->first;
        long long Scale = It->second;
        ++It;
        Value *B = It->first;
        long long OtherScale = It->second;
        if (Scale != 0 &&
            Scale != std::numeric_limits<long long>::min() &&
            OtherScale == -Scale) {
            if (Scale < 0) {
                Scale = -Scale;
                std::swap(A, B);
            }

            std::optional<long long> Offset = getAdditiveOffsetFrom(A, B);
            if (!Offset) {
                if (auto Reverse = getAdditiveOffsetFrom(B, A);
                    Reverse &&
                    *Reverse != std::numeric_limits<long long>::min())
                    Offset = -*Reverse;
            }

            auto Ty = A ? A->getIRType() : nullptr;
            auto BTy = B ? B->getIRType() : nullptr;
            if (Offset && IRType::hasIntegerBitWidth(Ty, 32) &&
                IRType::hasIntegerBitWidth(BTy, 32)) {
                // A chain of i32 additions can accumulate more than one full
                // modulus in the mathematical decomposition.  Normalize it
                // first; the signed difference of the two final i32 values
                // is then either R or R-2^32 (only zero when R is zero).
                long long Residue = *Offset % I32Modulus;
                if (Residue < 0) Residue += I32Modulus;
                std::vector<long long> Representatives{Residue};
                if (Residue != 0)
                    Representatives.push_back(Residue - I32Modulus);

                bool AllDisjoint = true;
                for (long long IndexDiff : Representatives) {
                    long long Scaled = 0;
                    long long ByteDiff = 0;
                    if (!checkedMul(Scale, IndexDiff, Scaled) ||
                        !checkedAdd(ConstDiff, Scaled, ByteDiff) ||
                        ByteDiff == std::numeric_limits<long long>::min() ||
                        std::llabs(ByteDiff) <
                            static_cast<long long>(Threshold)) {
                        AllDisjoint = false;
                        break;
                    }
                }
                if (AllDisjoint) return AliasInfo::NoAlias;
            }

            // When SCEV proves a finite canonical recurrence, its exact
            // update has a mathematical lower bound relative to the invariant
            // start.  This covers variable distances such as j=i+1 without
            // treating every positive-looking i32 ADD as non-wrapping.
            if (auto Lower = lowerBoundDiff(A, B, LI)) {
                long long Scaled = 0;
                long long MinByteDiff = 0;
                if (checkedMul(Scale, *Lower, Scaled) &&
                    checkedAdd(ConstDiff, Scaled, MinByteDiff) &&
                    MinByteDiff >= static_cast<long long>(Threshold)) {
                    return AliasInfo::NoAlias;
                }
            }
            if (auto ReverseLower = lowerBoundDiff(B, A, LI)) {
                long long Scaled = 0;
                long long MaxByteDiff = 0;
                if (checkedMul(Scale, *ReverseLower, Scaled) &&
                    checkedSub(ConstDiff, Scaled, MaxByteDiff) &&
                    MaxByteDiff <= -static_cast<long long>(Threshold)) {
                    return AliasInfo::NoAlias;
                }
            }
        }
    }

    // Integer ADD/SUB and PHI recurrences in this IR carry no no-wrap flag.
    // Recurrences not covered by the explicit SCEV proof remain MayAlias.
    return AliasInfo::MayAlias;
}

AliasInfo BasicAAResult::aliasGEP(
    Value* GEP1, Value* GEP2, const LoopInfo& LI, bool UseIndexRange,
    std::optional<std::size_t> AccessSize1,
    std::optional<std::size_t> AccessSize2) {
    auto E1 = fillLinearGEP(GEP1);
    auto E2 = fillLinearGEP(GEP2);

    if (!E1.Valid || !E2.Valid) {
        return AliasInfo::MayAlias;
    }
    if (E1.Base != E2.Base) {
        return AliasInfo::MayAlias;
    }

    std::unordered_map<Value*, long long> DiffVars = E1.VariableOffsets;
    for (auto &KV : E2.VariableOffsets) {
        long long Updated = 0;
        if (!checkedSub(DiffVars[KV.first], KV.second, Updated))
            return AliasInfo::MayAlias;
        DiffVars[KV.first] = Updated;
    }
    for (auto It = DiffVars.begin(); It != DiffVars.end();) {
        if (It->second == 0) {
            It = DiffVars.erase(It);
        } else {
            ++It;
        }
    }

    long long ConstDiff = 0;
    if (!checkedSub(E1.ConstantOffset, E2.ConstantOffset, ConstDiff))
        return AliasInfo::MayAlias;

    std::size_t Access1 = AccessSize1.value_or(getPointeeAccessSize(GEP1));
    std::size_t Access2 = AccessSize2.value_or(getPointeeAccessSize(GEP2));
    // A zero size here means that the pointer's pointee width is unknown to
    // this IR, not that every access through the pointer touches zero bytes.
    // Equal offsets are still MustAlias, but distance cannot prove NoAlias.
    if (Access1 == 0 || Access2 == 0)
        return ConstDiff == 0 && DiffVars.empty() ? AliasInfo::MustAlias
                                                  : AliasInfo::MayAlias;
    std::size_t Threshold = std::max<std::size_t>(1, std::max(Access1, Access2));
    if (Threshold > static_cast<std::size_t>(
                        std::numeric_limits<long long>::max()))
        return AliasInfo::MayAlias;

    if (!DiffVars.empty()) {
        if (UseIndexRange) {
            return aliasGEPByIndexDistance(DiffVars, ConstDiff, Threshold, LI);
        }
        return AliasInfo::MayAlias;
    }

    if (ConstDiff == 0) {
        return AliasInfo::MustAlias;
    }

    if (ConstDiff == std::numeric_limits<long long>::min())
        return AliasInfo::MayAlias;
    // D = address(GEP1) - address(GEP2).  For unequal access widths the two
    // separating distances are asymmetric: a positive D is compared against
    // Access2, while a negative D is compared against Access1.
    if ((ConstDiff > 0 &&
         ConstDiff >= static_cast<long long>(Access2)) ||
        (ConstDiff < 0 &&
         ConstDiff != std::numeric_limits<long long>::min() &&
         -ConstDiff >= static_cast<long long>(Access1))) {
        return AliasInfo::NoAlias;
    }
    return AliasInfo::MayAlias;
}

AliasInfo BasicAAResult::aliasPHI(PhiInst* Phi, Value* Other, const LoopInfo& LI,
                                  bool UseIndexRange) {
    if (!Phi) {
        return AliasInfo::MayAlias;
    }
    unsigned N = Phi->getNumIncomingValues();
    if (N == 0) {
        return AliasInfo::MayAlias;
    }

    if (auto *OtherPhi = dynamic_cast<PhiInst*>(Other)) {
        if (OtherPhi->getParent() == Phi->getParent()) {
            unsigned M = OtherPhi->getNumIncomingValues();
            if (M == 0 || M != N) {
                return AliasInfo::MayAlias;
            }

            // PHI incoming order is not semantic.  Pair values by predecessor
            // block; matching by raw index can compare values from different
            // control-flow edges and manufacture NoAlias/MustAlias.
            std::unordered_map<BasicBlock *, Value *> OtherByPred;
            if (!forEachPhiIncoming(
                    OtherPhi,
                    [&](unsigned, Value *Incoming, BasicBlock *Pred) {
                        return Pred &&
                               OtherByPred.emplace(Pred, Incoming).second;
                    })) {
                return AliasInfo::MayAlias;
            }

            std::unordered_set<BasicBlock *> SeenPreds;
            AliasInfo Acc = AliasInfo::MayAlias;
            bool First = true;
            bool InvalidIncoming = false;
            forEachPhiIncoming(
                Phi, [&](unsigned, Value *Incoming, BasicBlock *Pred) {
                    if (!Pred || !SeenPreds.insert(Pred).second) {
                        InvalidIncoming = true;
                        return false;
                    }
                    auto OtherIt = OtherByPred.find(Pred);
                    if (OtherIt == OtherByPred.end()) {
                        InvalidIncoming = true;
                        return false;
                    }

                    AliasInfo Cur = aliasImpl(Incoming, OtherIt->second, LI,
                                              UseIndexRange);
                    if (First) {
                        Acc = Cur;
                        First = false;
                        return true;
                    }
                    Acc = mergePhiAlias(Acc, Cur);
                    return Acc != AliasInfo::MayAlias;
                });
            if (InvalidIncoming) return AliasInfo::MayAlias;
            return Acc;
        }
    }

    AliasInfo Acc = aliasImpl(Phi->getIncomingValue(0), Other, LI, UseIndexRange);
    for (unsigned i = 1; i < N; ++i) {
        AliasInfo Cur = aliasImpl(Phi->getIncomingValue(i), Other, LI, UseIndexRange);
        Acc = mergePhiAlias(Acc, Cur);
        if (Acc == AliasInfo::MayAlias) {
            break;
        }
    }
    return Acc;
}

AliasInfo BasicAAResult::aliasSelect(SelectInst *Select, Value *Other,
                                     const LoopInfo &LI,
                                     bool UseIndexRange) {
    if (!Select || !Other) return AliasInfo::MayAlias;

    auto Key = makeCanonicalPair(Select, Other);
    ScopedActiveMark ActiveMark(ActiveSelectQueries, Key);
    if (!ActiveMark.inserted())
        return AliasInfo::MayAlias;

    Value *TrueValue = Select->getOperand(1);
    Value *FalseValue = Select->getOperand(2);
    if (!TrueValue || !FalseValue)
        return AliasInfo::MayAlias;

    // Correlated selects with the same condition choose corresponding arms in
    // every execution.  Pairing all four combinations would lose useful
    // NoAlias/MustAlias facts and is unnecessarily pessimistic.
    if (auto *OtherSelect = dynamic_cast<SelectInst *>(Other)) {
        if (Select->getOperand(0) == OtherSelect->getOperand(0)) {
            AliasInfo OnTrue = aliasImpl(TrueValue,
                                         OtherSelect->getOperand(1), LI,
                                         UseIndexRange);
            AliasInfo OnFalse = aliasImpl(FalseValue,
                                          OtherSelect->getOperand(2), LI,
                                          UseIndexRange);
            return mergePhiAlias(OnTrue, OnFalse);
        }
    }

    AliasInfo OnTrue = aliasImpl(TrueValue, Other, LI, UseIndexRange);
    AliasInfo OnFalse = aliasImpl(FalseValue, Other, LI, UseIndexRange);
    return mergePhiAlias(OnTrue, OnFalse);
}

AliasInfo BasicAAResult::alias(Value* V1, Value* V2, const LoopInfo& LI) {
    return aliasImpl(V1, V2, LI, false);
}

AliasInfo BasicAAResult::aliasWithIndexRange(Value* V1, Value* V2,
                                             const LoopInfo& LI) {
    return aliasImpl(V1, V2, LI, true);
}

AliasInfo BasicAAResult::aliasLocationImpl(
    const MemoryLocation &L1, const MemoryLocation &L2,
    const LoopInfo &LI, bool UseIndexRange, unsigned Depth,
    std::unordered_set<std::pair<Value *, Value *>, ValuePairHash> &Active) {
    auto NormalizeControlPointer = [](Value *P,
                                      const std::optional<std::size_t> &Size) {
        // Once an explicit byte width is present, bitcasts and zero-offset
        // GEPs no longer contribute access-width information.  Peel them so a
        // wrapped select/phi is still expanded with that explicit width.
        if (!Size) return P;
        std::unordered_set<Value *> Seen;
        while (P && Seen.insert(P).second) {
            if (auto *BC = dynamic_cast<BitCastInst *>(P)) {
                P = BC->getUnaryOperand();
                continue;
            }
            auto *GEP = dynamic_cast<GetElementPtrInst *>(P);
            if (!GEP || !GEP->hasOnlyConstantIndices()) break;
            bool AllZero = true;
            for (Value *Index : GEP->getIndices()) {
                if (!Index || !Index->isConstIntZero()) {
                    AllZero = false;
                    break;
                }
            }
            if (!AllZero) break;
            P = GEP->getBasePointer();
        }
        return P;
    };

    Value *P1 = NormalizeControlPointer(L1.Pointer, L1.Size);
    Value *P2 = NormalizeControlPointer(L2.Pointer, L2.Size);
    if (!P1 || !P2 || Depth > 32) return AliasInfo::MayAlias;

    // Equal SSA pointers start at the same byte even when their access widths
    // differ.  Let aliasImpl retain its undef-origin safeguards.
    if (P1 == P2) return aliasImpl(P1, P2, LI, UseIndexRange);

    auto Key = makeCanonicalPair(P1, P2);
    ScopedActiveMark ActiveMark(Active, Key);
    if (!ActiveMark.inserted()) return AliasInfo::MayAlias;
    auto Merge = [&](AliasInfo A, AliasInfo B) {
        return mergePhiAlias(A, B);
    };

    auto *S1 = dynamic_cast<SelectInst *>(P1);
    auto *S2 = dynamic_cast<SelectInst *>(P2);
    if (S1 && S2 && S1->getOperand(0) == S2->getOperand(0)) {
        AliasInfo OnTrue = aliasLocationImpl(
            {S1->getOperand(1), L1.Size}, {S2->getOperand(1), L2.Size},
            LI, UseIndexRange, Depth + 1, Active);
        AliasInfo OnFalse = aliasLocationImpl(
            {S1->getOperand(2), L1.Size}, {S2->getOperand(2), L2.Size},
            LI, UseIndexRange, Depth + 1, Active);
        return Merge(OnTrue, OnFalse);
    }
    if (S1) {
        AliasInfo OnTrue = aliasLocationImpl(
            {S1->getOperand(1), L1.Size}, L2, LI, UseIndexRange,
            Depth + 1, Active);
        AliasInfo OnFalse = aliasLocationImpl(
            {S1->getOperand(2), L1.Size}, L2, LI, UseIndexRange,
            Depth + 1, Active);
        return Merge(OnTrue, OnFalse);
    }
    if (S2) {
        AliasInfo OnTrue = aliasLocationImpl(
            L1, {S2->getOperand(1), L2.Size}, LI, UseIndexRange,
            Depth + 1, Active);
        AliasInfo OnFalse = aliasLocationImpl(
            L1, {S2->getOperand(2), L2.Size}, LI, UseIndexRange,
            Depth + 1, Active);
        return Merge(OnTrue, OnFalse);
    }

    auto *Phi1 = dynamic_cast<PhiInst *>(P1);
    auto *Phi2 = dynamic_cast<PhiInst *>(P2);
    if (Phi1 && Phi2 && Phi1->getParent() == Phi2->getParent() &&
        Phi1->getNumIncomingValues() == Phi2->getNumIncomingValues() &&
        Phi1->getNumIncomingValues() != 0) {
        std::unordered_map<BasicBlock *, Value *> Incoming2;
        if (!forEachPhiIncoming(
                Phi2, [&](unsigned, Value *Incoming, BasicBlock *Pred) {
                    return Pred && Incoming2.emplace(Pred, Incoming).second;
                })) {
            return AliasInfo::MayAlias;
        }
        bool First = true;
        AliasInfo Result = AliasInfo::MayAlias;
        std::unordered_set<BasicBlock *> Seen;
        bool InvalidIncoming = false;
        forEachPhiIncoming(
            Phi1, [&](unsigned, Value *Incoming, BasicBlock *Pred) {
                auto It = Incoming2.find(Pred);
                if (!Pred || !Seen.insert(Pred).second ||
                    It == Incoming2.end()) {
                    InvalidIncoming = true;
                    return false;
                }
                AliasInfo Current = aliasLocationImpl(
                    {Incoming, L1.Size}, {It->second, L2.Size}, LI,
                    UseIndexRange, Depth + 1, Active);
                Result = First ? Current : Merge(Result, Current);
                First = false;
                return Result != AliasInfo::MayAlias;
            });
        if (InvalidIncoming) return AliasInfo::MayAlias;
        return Result;
    }

    auto ExpandPhi = [&](PhiInst *Phi, const MemoryLocation &Other,
                         bool PhiIsFirst) {
        if (!Phi || Phi->getNumIncomingValues() == 0)
            return AliasInfo::MayAlias;
        bool First = true;
        AliasInfo Result = AliasInfo::MayAlias;
        forEachPhiIncoming(
            Phi, [&](unsigned, Value *IncomingValue, BasicBlock *) {
                MemoryLocation Incoming(
                    IncomingValue, PhiIsFirst ? L1.Size : L2.Size);
                AliasInfo Current = PhiIsFirst
                    ? aliasLocationImpl(Incoming, Other, LI, UseIndexRange,
                                        Depth + 1, Active)
                    : aliasLocationImpl(Other, Incoming, LI, UseIndexRange,
                                        Depth + 1, Active);
                Result = First ? Current : Merge(Result, Current);
                First = false;
                return Result != AliasInfo::MayAlias;
            });
        return Result;
    };
    if (Phi1) return ExpandPhi(Phi1, L2, true);
    if (Phi2) return ExpandPhi(Phi2, L1, false);

    AliasInfo Result = aliasImpl(P1, P2, LI, UseIndexRange);
    if (L1.Size && L2.Size) {
        Value *V1 = stripBaseInst(P1);
        Value *V2 = stripBaseInst(P2);
        // An explicit access width is more authoritative than the pointer's
        // nominal pointee width.  Re-run same-object GEP queries even if the
        // pointer-only query already returned NoAlias.
        if (V1 && V2 && getBaseAddress(V1) == getBaseAddress(V2) &&
            (dynamic_cast<GetElementPtrInst *>(V1) ||
             dynamic_cast<GetElementPtrInst *>(V2))) {
            Result = aliasGEP(V1, V2, LI, UseIndexRange,
                              L1.Size, L2.Size);
        }
    }
    return Result;
}

AliasInfo BasicAAResult::alias(const MemoryLocation &L1,
                               const MemoryLocation &L2,
                               const LoopInfo &LI) {
    std::unordered_set<std::pair<Value *, Value *>, ValuePairHash> Active;
    return aliasLocationImpl(L1, L2, LI, false, 0, Active);
}

AliasInfo BasicAAResult::aliasWithIndexRange(
    const MemoryLocation &L1, const MemoryLocation &L2,
    const LoopInfo &LI) {
    std::unordered_set<std::pair<Value *, Value *>, ValuePairHash> Active;
    return aliasLocationImpl(L1, L2, LI, true, 0, Active);
}

AliasInfo BasicAAResult::aliasImpl(Value* V1, Value* V2, const LoopInfo& LI,
                                   bool UseIndexRange) {
    //1.进行初步剥离
    V1 = stripBaseInst(V1);
    V2 = stripBaseInst(V2);

    if (!V1 || !V2 || !V1->isPointerValue() || !V2->isPointerValue()) {
        return AliasInfo::MayAlias;
    }

    //2. An undef pointer is not a distinct allocation.  Treating it as
    // NoAlias lets a client move/delete real memory operations across it.
    if (V1->isUndef() || V2->isUndef()) {
        return AliasInfo::MayAlias;
    }
    std::unordered_set<Value *> UndefActive;
    if (hasUndefPointerOrigin(V1, UndefActive))
        return AliasInfo::MayAlias;
    UndefActive.clear();
    if (hasUndefPointerOrigin(V2, UndefActive))
        return AliasInfo::MayAlias;
    //3. Obtain the underlying roots before reflexivity.  A GEP/bitcast whose
    // root is undef is not a stable object identity, even when the same
    // derived SSA value appears on both sides of the query.
    Value* O1 = getBaseAddress(V1);
    Value* O2 = getBaseAddress(V2);
    if (!O1 || !O2 || O1->isUndef() || O2->isUndef())
        return AliasInfo::MayAlias;

    //4.判断相等性
    if (isEqualAlias(V1, V2, LI)) {
        return AliasInfo::MustAlias;
    }

    // A select is a path-sensitive pointer union.  Resolve its arms before
    // comparing underlying roots: the select instruction itself is not an
    // allocation object and therefore cannot participate in the usual root
    // distinction rule.
    if (auto *S1 = dynamic_cast<SelectInst *>(V1))
        return aliasSelect(S1, V2, LI, UseIndexRange);
    if (auto *S2 = dynamic_cast<SelectInst *>(V2))
        return aliasSelect(S2, V1, LI, UseIndexRange);

    //5.如果底层对象不同
    if (O1 != O2) {
        if (haveKnownDistinctUnderlyingObjects(O1, O2)) {
            return AliasInfo::NoAlias;
        }

        return AliasInfo::MayAlias;
    }

    std::pair<Value*, Value*> Key;
    if (!UseIndexRange) {
        Key = makeCanonicalPair(V1, V2);
        auto It = AliasCache.find(Key);
        if (It != AliasCache.end()) {
            return It->second;
        }
        AliasCache[Key] = AliasInfo::MayAlias;
    }

    AliasInfo Result = AliasInfo::MayAlias;
    auto *G1 = dynamic_cast<GetElementPtrInst*>(V1);
    auto *G2 = dynamic_cast<GetElementPtrInst*>(V2);
    if (G1 || G2) {
        // fillLinearGEP also accepts a plain pointer as a zero-offset
        // expression, which lets BasicAA distinguish an object root from a
        // constant non-zero GEP into that same object.
        Result = aliasGEP(V1, V2, LI, UseIndexRange);
    } else if (auto *P1 = dynamic_cast<PhiInst*>(V1)) {
        Result = aliasPHI(P1, V2, LI, UseIndexRange);
    } else if (auto *P2 = dynamic_cast<PhiInst*>(V2)) {
        Result = aliasPHI(P2, V1, LI, UseIndexRange);
    } else if (V1 == V2) {
        Result = AliasInfo::MayAlias;
    }

    if (!UseIndexRange) {
        AliasCache[Key] = Result;
    }
    return Result;
}

BasicAAResult BasicAliasAnalysis::run(Function &F, FunctionAnalysisPassManager &FAM) const {
    auto &SE = FAM.getResult<SCEVAnalysis>(F);
    BasicAAResult Result(&SE);

    std::vector<unsigned> PointerParams;
    PointerParams.reserve(F.getParamCount());
    for (unsigned I = 0; I < F.getParamCount(); ++I) {
        Value *Param = F.getParam(I);
        if (Param && Param->isPointerValue()) {
            PointerParams.push_back(I);
        }
    }
    if (PointerParams.empty()) {
        return Result;
    }

    std::vector<CallInst*> Calls = F.getDirectCallSites();
    if (!F.hasOnlyDirectCallUses() || Calls.empty()) {
        return Result;
    }

    BasicAAResult SeedAA;
    for (unsigned I = 0; I < PointerParams.size(); ++I) {
        for (unsigned J = I + 1; J < PointerParams.size(); ++J) {
            unsigned PI = PointerParams[I];
            unsigned PJ = PointerParams[J];
            bool AllNoAlias = true;
            for (CallInst* CI : Calls) {
                Value* ArgI = CI->getArg(PI);
                Value* ArgJ = CI->getArg(PJ);
                // A NoAlias answer for the immediate access ranges is not an
                // allocation-object fact.  In particular, &a[0] and &a[1]
                // are disjoint i32 accesses but p + 1 may still equal q in
                // the callee.  Promote only structurally distinct objects.
                if (!ArgI || !ArgJ ||
                    !SeedAA.haveKnownDistinctUnderlyingObjects(ArgI, ArgJ)) {
                    AllNoAlias = false;
                    break;
                }
            }
            if (AllNoAlias) {
                Result.addObjectNoAliasFact(F.getParam(PI), F.getParam(PJ));
            }
        }
    }

    Module* M = Module::getActiveModule();
    if (!M) {
        return Result;
    }
    for (auto &GV : M->getGlobalVar()) {
        Value* Global = GV.get();
        if (!(Global && Global->getIRType() && Global->getIRType()->isPointer())) {
            continue;
        }
        for (unsigned PI : PointerParams) {
            bool AllNoAlias = true;
            for (CallInst* CI : Calls) {
                Value* Arg = CI->getArg(PI);
                if (!Arg ||
                    !SeedAA.haveKnownDistinctUnderlyingObjects(Arg, Global)) {
                    AllNoAlias = false;
                    break;
                }
            }
            if (AllNoAlias) {
                Result.addObjectNoAliasFact(F.getParam(PI), Global);
            }
        }
    }
    return Result;
}
