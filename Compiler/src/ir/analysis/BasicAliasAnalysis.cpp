#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/LoopInfo.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace {

bool checkedAdd(long long A, long long B, long long &Result) {
    if ((B > 0 && A > std::numeric_limits<long long>::max() - B) ||
        (B < 0 && A < std::numeric_limits<long long>::min() - B))
        return false;
    Result = A + B;
    return true;
}

bool checkedSub(long long A, long long B, long long &Result) {
    if (B == std::numeric_limits<long long>::min()) {
        if (A >= 0) return false;
        Result = A - B;
        return true;
    }
    return checkedAdd(A, -B, Result);
}

bool checkedMul(long long A, long long B, long long &Result) {
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

bool hasUndefPointerOrigin(Value *V, std::unordered_set<Value *> &Active,
                           unsigned Depth = 0) {
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
    if (!Active.insert(V).second) return false;

    for (size_t Op = 0; Op < I->getNumOperands(); ++Op) {
        Value *Operand = I->getOperand(Op);
        if (dynamic_cast<BasicBlock *>(Operand)) continue;
        if (hasUndefPointerOrigin(Operand, Active, Depth + 1)) {
            Active.erase(V);
            return true;
        }
    }
    Active.erase(V);
    return false;
}

} // namespace

bool BasicAAResult::invalidate(Function &F, const PreservationStatus &PA) {
    if (PA.isKept(BasicAliasAnalysis::ID())) {
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
    for (unsigned I = 0; I < CI->getArgCount(); ++I) {
        Value *Arg = CI->getArg(I);
        if (!Arg || !Arg->isPointerValue()) continue;
        if (alias(P, Arg, LI) != AliasInfo::NoAlias) {
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
        std::size_t Stride = 0;
        if (auto PtrTy = std::dynamic_pointer_cast<POINTERType>(CurType)) {
            CurType = PtrTy->getPointerType();
            if (!CurType) {
                return false;
            }
            Stride = CurType->getSize();
        } else if (auto ArrTy = std::dynamic_pointer_cast<ARRAYType>(CurType)) {
            CurType = ArrTy->getElementType();
            if (!CurType) {
                return false;
            }
            Stride = CurType->getSize();
        } else {
            return false;
        }

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

    Value* Start = nullptr;
    Value* Update = nullptr;
    for (unsigned I = 0; I < Phi->getNumIncomingValues(); ++I) {
        Value* V = Phi->getIncomingValue(I);
        BasicBlock* Pred = Phi->getIncomingBlock(I);
        if (!V || !Pred) {
            return std::nullopt;
        }
        if (L->contains(Pred)) {
            if (Update && Update != V) {
                return std::nullopt;
            }
            Update = V;
        } else {
            if (Start && Start != V) {
                return std::nullopt;
            }
            Start = V;
        }
    }

    auto Step = getAdditiveOffsetFrom(Update, Phi);
    if (!Start || !Step || *Step <= 0) {
        return std::nullopt;
    }
    return PositivePhiRecurrence{Start, *Step, L};
}

std::optional<long long>
BasicAAResult::lowerBoundDiff(Value* Greater, Value* Lesser,
                              const LoopInfo& LI) const {
    if (!Greater || !Lesser) {
        return std::nullopt;
    }

    if (auto Direct = getAdditiveOffsetFrom(Greater, Lesser)) {
        if (*Direct >= 0) {
            return *Direct;
        }
    }

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
    if (!StartOff) {
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
    (void)DiffVars;
    (void)ConstDiff;
    (void)Threshold;
    (void)LI;

    // Integer ADD/SUB and PHI recurrences in this IR carry no no-wrap flag.
    // A syntactically positive step can wrap and invalidate every mathematical
    // lower-bound argument below.  Until SCEV can attach an explicit finite
    // no-wrap proof, variable-distance GEPs must remain MayAlias.
    return AliasInfo::MayAlias;

#if 0
    if (DiffVars.size() != 2) {
        return AliasInfo::MayAlias;
    }

    auto It = DiffVars.begin();
    Value* A = It->first;
    long long Scale = It->second;
    ++It;
    Value* B = It->first;
    long long OtherScale = It->second;

    if (Scale == 0 ||
        Scale == std::numeric_limits<long long>::min() ||
        OtherScale == std::numeric_limits<long long>::min() ||
        Scale != -OtherScale) {
        return AliasInfo::MayAlias;
    }
    if (Scale < 0) {
        Scale = -Scale;
        std::swap(A, B);
    }

    Threshold = std::max<std::size_t>(1, Threshold);
    if (Threshold > static_cast<std::size_t>(
                        std::numeric_limits<long long>::max()))
        return AliasInfo::MayAlias;
    if (auto LB = lowerBoundDiff(A, B, LI)) {
        long long ScaledLB = 0;
        long long MinDiff = 0;
        if (!checkedMul(Scale, *LB, ScaledLB) ||
            !checkedAdd(ConstDiff, ScaledLB, MinDiff))
            return AliasInfo::MayAlias;
        if (MinDiff >= static_cast<long long>(Threshold)) {
            return AliasInfo::NoAlias;
        }
    }
    if (auto LB = lowerBoundDiff(B, A, LI)) {
        long long ScaledLB = 0;
        long long MaxDiff = 0;
        if (!checkedMul(Scale, *LB, ScaledLB) ||
            !checkedSub(ConstDiff, ScaledLB, MaxDiff))
            return AliasInfo::MayAlias;
        if (MaxDiff <= -static_cast<long long>(Threshold)) {
            return AliasInfo::NoAlias;
        }
    }

    return AliasInfo::MayAlias;
#endif
}

AliasInfo BasicAAResult::aliasGEP(Value* GEP1, Value* GEP2,
                                  const LoopInfo& LI, bool UseIndexRange) {
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

    std::size_t Access1 = getPointeeAccessSize(GEP1);
    std::size_t Access2 = getPointeeAccessSize(GEP2);
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
    long long Distance = std::llabs(ConstDiff);
    if (Distance >= static_cast<long long>(Threshold)) {
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
            for (unsigned i = 0; i < M; ++i) {
                BasicBlock *Pred = OtherPhi->getIncomingBlock(i);
                if (!Pred || !OtherByPred.emplace(
                                 Pred, OtherPhi->getIncomingValue(i)).second)
                    return AliasInfo::MayAlias;
            }

            std::unordered_set<BasicBlock *> SeenPreds;
            AliasInfo Acc = AliasInfo::MayAlias;
            bool First = true;
            for (unsigned i = 0; i < N; ++i) {
                BasicBlock *Pred = Phi->getIncomingBlock(i);
                if (!Pred || !SeenPreds.insert(Pred).second)
                    return AliasInfo::MayAlias;
                auto OtherIt = OtherByPred.find(Pred);
                if (OtherIt == OtherByPred.end())
                    return AliasInfo::MayAlias;

                AliasInfo Cur = aliasImpl(Phi->getIncomingValue(i),
                                          OtherIt->second, LI,
                                          UseIndexRange);
                if (First) {
                    Acc = Cur;
                    First = false;
                    continue;
                }
                Acc = mergePhiAlias(Acc, Cur);
                if (Acc == AliasInfo::MayAlias) {
                    break;
                }
            }
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

AliasInfo BasicAAResult::alias(Value* V1, Value* V2, const LoopInfo& LI) {
    return aliasImpl(V1, V2, LI, false);
}

AliasInfo BasicAAResult::aliasWithIndexRange(Value* V1, Value* V2,
                                             const LoopInfo& LI) {
    return aliasImpl(V1, V2, LI, true);
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

PassID BasicAliasAnalysis::Key;

BasicAAResult BasicAliasAnalysis::run(Function &F, FunctionAnalysisPassManager &FAM) const {
    BasicAAResult Result;
    (void)FAM;

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
