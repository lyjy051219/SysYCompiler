#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/LoopInfo.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

bool BasicAAResult::invalidate(Function &F, const PreservationStatus &PA) {
    if (PA.isKept(BasicAliasAnalysis::ID())) {
        return false;
    }
    (void)F;
    AliasCache.clear();
    return true;
}

Value* BasicAAResult::stripBaseInst(Value* V) const {
    std::unordered_set<Value*> Visited;
    while (V && Visited.insert(V).second) {
        if (!V || !V->getIRType() || V->getIRType()->getEnum() != IR_POINTER) {
            return V;
        }

        if (auto *GEP = dynamic_cast<GetElementPtrInst*>(V)) {
            bool AllZero = true;
            for (unsigned i = 1; i < GEP->getUserList().size(); ++i) {
                auto &Ops = GEP->getUserList();
                Value* Idx = (i < Ops.size() && Ops[i]) ? Ops[i]->getValue() : nullptr;
                auto *CI = dynamic_cast<ConstInt*>(Idx);
                if (!CI || CI->getVal() != 0) {
                    AllZero = false;
                    break;
                }
            }
            if (!AllZero) {
                return V;
            }
            auto &Ops = GEP->getUserList();
            V = (!Ops.empty() && Ops[0]) ? Ops[0]->getValue() : nullptr;
            continue;
        }

        if (auto *BC = dynamic_cast<BitCastInst*>(V)) {
            auto &Ops = BC->getUserList();
            V = (!Ops.empty() && Ops[0]) ? Ops[0]->getValue() : nullptr;
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
            auto &Ops = GEP->getUserList();
            V = (!Ops.empty() && Ops[0]) ? Ops[0]->getValue() : nullptr;
            continue;
        }

        if (auto *BC = dynamic_cast<BitCastInst*>(V)) {
            auto &Ops = BC->getUserList();
            V = (!Ops.empty() && Ops[0]) ? Ops[0]->getValue() : nullptr;
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
    auto &Ops = GEP->getUserList();
    Value* BasePtr = (!Ops.empty() && Ops[0]) ? Ops[0]->getValue() : nullptr;
    if (!BasePtr || !BasePtr->getIRType()) {
        return false;
    }

    std::shared_ptr<IRType> CurType = BasePtr->getIRType();
    for (unsigned i = 1; i < Ops.size(); ++i) {
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

        Value* Idx = Ops[i] ? Ops[i]->getValue() : nullptr;
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
            auto &Ops = GEP->getUserList();
            V = (!Ops.empty() && Ops[0]) ? Ops[0]->getValue() : nullptr;
            continue;
        }

        if (auto *BC = dynamic_cast<BitCastInst*>(V)) {
            auto &Ops = BC->getUserList();
            V = (!Ops.empty() && Ops[0]) ? Ops[0]->getValue() : nullptr;
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

AliasInfo BasicAAResult::aliasGEP(GetElementPtrInst* GEP1, GetElementPtrInst* GEP2) {
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

    if (!DiffVars.empty()) {
        return AliasInfo::MayAlias;
    }

    if (ConstDiff == 0) {
        return AliasInfo::MustAlias;
    }

    std::size_t Access1 = 0;
    if (GEP1 && GEP1->getIRType() && GEP1->getIRType()->getEnum() == IR_POINTER) {
        auto PtrV1 = std::dynamic_pointer_cast<POINTERType>(GEP1->getIRType());
        if (PtrV1 && PtrV1->getPointerType()) {
            Access1 = PtrV1->getPointerType()->getSize();
        }
    }
    std::size_t Access2 = 0;
    if (GEP2 && GEP2->getIRType() && GEP2->getIRType()->getEnum() == IR_POINTER) {
        auto PtrV2 = std::dynamic_pointer_cast<POINTERType>(GEP2->getIRType());
        if (PtrV2 && PtrV2->getPointerType()) {
            Access2 = PtrV2->getPointerType()->getSize();
        }
    }
    std::size_t Threshold = std::max<std::size_t>(1, std::max(Access1, Access2));
    long long Distance = std::llabs(ConstDiff);
    if (Distance >= static_cast<long long>(Threshold)) {
        return AliasInfo::NoAlias;
    }
    return AliasInfo::MayAlias;
}

AliasInfo BasicAAResult::aliasPHI(PhiInst* Phi, Value* Other, const LoopInfo& LI) {
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

            AliasInfo Acc = alias(Phi->getIncomingValue(0), OtherPhi->getIncomingValue(0), LI);
            for (unsigned i = 1; i < N; ++i) {
                AliasInfo Cur = alias(Phi->getIncomingValue(i), OtherPhi->getIncomingValue(i), LI);
                Acc = mergePhiAlias(Acc, Cur);
                if (Acc == AliasInfo::MayAlias) {
                    break;
                }
            }
            return Acc;
        }
    }

    AliasInfo Acc = alias(Phi->getIncomingValue(0), Other, LI);
    for (unsigned i = 1; i < N; ++i) {
        AliasInfo Cur = alias(Phi->getIncomingValue(i), Other, LI);
        Acc = mergePhiAlias(Acc, Cur);
        if (Acc == AliasInfo::MayAlias) {
            break;
        }
    }
    return Acc;
}

AliasInfo BasicAAResult::alias(Value* V1, Value* V2, const LoopInfo& LI) {
    //1.如果任一者访问的内存大小为0,直接判定为NoAlias
    std::size_t Access1 = 0;
    if (V1->getIRType() && V1->getIRType()->getEnum() == IR_POINTER) {
        auto PtrV1 = std::dynamic_pointer_cast<POINTERType>(V1->getIRType());
        if (PtrV1 && PtrV1->getPointerType()) {
            Access1 = PtrV1->getPointerType()->getSize();
        }
    }
    std::size_t Access2 = 0;
    if (V2->getIRType() && V2->getIRType()->getEnum() == IR_POINTER) {
        auto PtrV2 = std::dynamic_pointer_cast<POINTERType>(V2->getIRType());
        if (PtrV2 && PtrV2->getPointerType()) {
            Access2 = PtrV2->getPointerType()->getSize();
        }
    }

    if (Access1 == 0 || Access2 == 0) {
        return AliasInfo::NoAlias;
    }

    //2.进行初步剥离
    V1 = stripBaseInst(V1);
    V2 = stripBaseInst(V2);

    if (!V1 || !V2 || !V1->getIRType() || !V2->getIRType() ||
        V1->getIRType()->getEnum() != IR_POINTER ||
        V2->getIRType()->getEnum() != IR_POINTER) {
        return AliasInfo::NoAlias;
    }

    //3.如果有UndefValue参与，直接判定为NoAlias
    if (dynamic_cast<UndefValue*>(V1) || dynamic_cast<UndefValue*>(V2)) {
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
        if (IsGlobal1 && IsGlobal2) {
            return AliasInfo::NoAlias;
        }
        if ((IsParam1 && IsAlloca2) || (IsParam2 && IsAlloca1)) {
            return AliasInfo::NoAlias;
        }
        if ((IsAlloca1 && (IsGlobal2 || IsConst2)) ||
            (IsAlloca2 && (IsGlobal1 || IsConst1))) {
            return AliasInfo::NoAlias;
        }

        return AliasInfo::MayAlias;
    }

    auto Key = makeCanonicalPair(V1, V2);
    auto It = AliasCache.find(Key);
    if (It != AliasCache.end()) {
        return It->second;
    }
    AliasCache[Key] = AliasInfo::MayAlias;

    AliasInfo Result = AliasInfo::MayAlias;
    auto *G1 = dynamic_cast<GetElementPtrInst*>(V1);
    auto *G2 = dynamic_cast<GetElementPtrInst*>(V2);
    if (G1 && G2) {
        Result = aliasGEP(G1, G2);
    } else if (auto *P1 = dynamic_cast<PhiInst*>(V1)) {
        Result = aliasPHI(P1, V2, LI);
    } else if (auto *P2 = dynamic_cast<PhiInst*>(V2)) {
        Result = aliasPHI(P2, V1, LI);
    } else if (V1 == V2) {
        Result = AliasInfo::MayAlias;
    }

    AliasCache[Key] = Result;
    return Result;
}

PassID BasicAliasAnalysis::Key;

BasicAAResult BasicAliasAnalysis::run(Function &F, FunctionAnalysisPassManager &FAM) const {
    BasicAAResult Result;
    (void)FAM;
    return Result;
}