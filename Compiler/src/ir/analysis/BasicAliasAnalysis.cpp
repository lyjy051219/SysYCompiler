#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/LoopInfo.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

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

bool BasicAAResult::isEqualAlias(Value* V1, Value* V2, const LoopInfo& LI) const {
    if (V1 != V2) {
        return false;
    }

    auto *I = dynamic_cast<Instruction*>(V1);
    if (!I) {
        return true;
    }
    BasicBlock* Parent = I->getParent();
    if (!Parent) {
        return true;
    }
    return LI.getLoopFor(Parent) == nullptr;
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
            Expr.ConstantOffset += static_cast<long long>(CI->getVal()) * static_cast<long long>(Stride);
        } else if (Idx) {
            Expr.VariableOffsets[Idx] += static_cast<long long>(Stride);
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
                Result.Offset += RC->getVal();
                Result.Base = LHS;
                continue;
            }
            if (LC) {
                Result.Offset += LC->getVal();
                Result.Base = RHS;
                continue;
            }
        } else if (BI->getOp() == BinaryInst::SUB && RC) {
            Result.Offset -= RC->getVal();
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

    long long LB = *StartOff + Dec.Offset;
    if (LB < 0) {
        return std::nullopt;
    }
    return LB;
}

AliasInfo BasicAAResult::aliasGEPByIndexDistance(
    const std::unordered_map<Value*, long long>& DiffVars,
    long long ConstDiff, std::size_t Threshold, const LoopInfo& LI) const {
    if (DiffVars.size() != 2) {
        return AliasInfo::MayAlias;
    }

    auto It = DiffVars.begin();
    Value* A = It->first;
    long long Scale = It->second;
    ++It;
    Value* B = It->first;
    long long OtherScale = It->second;

    if (Scale == 0 || Scale != -OtherScale) {
        return AliasInfo::MayAlias;
    }
    if (Scale < 0) {
        Scale = -Scale;
        std::swap(A, B);
    }

    Threshold = std::max<std::size_t>(1, Threshold);
    if (auto LB = lowerBoundDiff(A, B, LI)) {
        long long MinDiff = ConstDiff + Scale * (*LB);
        if (MinDiff >= static_cast<long long>(Threshold)) {
            return AliasInfo::NoAlias;
        }
    }
    if (auto LB = lowerBoundDiff(B, A, LI)) {
        long long MaxDiff = ConstDiff - Scale * (*LB);
        if (MaxDiff <= -static_cast<long long>(Threshold)) {
            return AliasInfo::NoAlias;
        }
    }

    return AliasInfo::MayAlias;
}

AliasInfo BasicAAResult::aliasGEP(GetElementPtrInst* GEP1, GetElementPtrInst* GEP2,
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
        DiffVars[KV.first] -= KV.second;
    }
    for (auto It = DiffVars.begin(); It != DiffVars.end();) {
        if (It->second == 0) {
            It = DiffVars.erase(It);
        } else {
            ++It;
        }
    }

    const long long ConstDiff = E1.ConstantOffset - E2.ConstantOffset;

    std::size_t Access1 = getPointeeAccessSize(GEP1);
    std::size_t Access2 = getPointeeAccessSize(GEP2);
    std::size_t Threshold = std::max<std::size_t>(1, std::max(Access1, Access2));

    if (!DiffVars.empty()) {
        if (UseIndexRange) {
            return aliasGEPByIndexDistance(DiffVars, ConstDiff, Threshold, LI);
        }
        return AliasInfo::MayAlias;
    }

    if (ConstDiff == 0) {
        return AliasInfo::MustAlias;
    }

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
        return AliasInfo::NoAlias;
    }

    if (auto *OtherPhi = dynamic_cast<PhiInst*>(Other)) {
        if (OtherPhi->getParent() == Phi->getParent()) {
            unsigned M = OtherPhi->getNumIncomingValues();
            if (M == 0 || M != N) {
                return AliasInfo::MayAlias;
            }

            AliasInfo Acc = aliasImpl(Phi->getIncomingValue(0), OtherPhi->getIncomingValue(0),
                                      LI, UseIndexRange);
            for (unsigned i = 1; i < N; ++i) {
                AliasInfo Cur = aliasImpl(Phi->getIncomingValue(i), OtherPhi->getIncomingValue(i),
                                          LI, UseIndexRange);
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
    //1.如果任一者访问的内存大小为0,直接判定为NoAlias
    std::size_t Access1 = getPointeeAccessSize(V1);
    std::size_t Access2 = getPointeeAccessSize(V2);

    if (Access1 == 0 || Access2 == 0) {
        return AliasInfo::NoAlias;
    }

    //2.进行初步剥离
    V1 = stripBaseInst(V1);
    V2 = stripBaseInst(V2);

    if (!V1 || !V2 || !V1->isPointerValue() || !V2->isPointerValue()) {
        return AliasInfo::NoAlias;
    }

    //3.如果有UndefValue参与，直接判定为NoAlias
    if (V1->isUndef() || V2->isUndef()) {
        return AliasInfo::NoAlias;
    }
    //4.判断相等性及拦截跨迭代相等性的情况
    if (isEqualAlias(V1, V2, LI)) {
        return AliasInfo::MustAlias;
    }

    //5.获取基地址
    Value* O1 = getBaseAddress(V1);
    Value* O2 = getBaseAddress(V2);

    //6.如果底层对象不同
    if (O1 != O2) {
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

        if (IsAlloca1 && IsAlloca2) {
            return AliasInfo::NoAlias;
        }
        if ((IsGlobal1 || IsConst1) && (IsGlobal2 || IsConst2)) {
            return AliasInfo::NoAlias;
        }
        if ((IsParam1 && IsAlloca2) || (IsParam2 && IsAlloca1)) {
            return AliasInfo::NoAlias;
        }
        if ((IsAlloca1 && (IsGlobal2 || IsConst2)) ||
            (IsAlloca2 && (IsGlobal1 || IsConst1))) {
            return AliasInfo::NoAlias;
        }
        if (hasObjectNoAlias(O1, O2)) {
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
    if (G1 && G2) {
        Result = aliasGEP(G1, G2, LI, UseIndexRange);
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
    auto &LI = FAM.getResult<LoopAnalysis>(F);

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
                if (!ArgI || !ArgJ || SeedAA.alias(ArgI, ArgJ, LI) != AliasInfo::NoAlias) {
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
                if (!Arg || SeedAA.alias(Arg, Global, LI) != AliasInfo::NoAlias) {
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
