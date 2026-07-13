#include "ir/analysis/SCEV.hpp"
#include "lib/IRUtils.hpp"
#include <algorithm>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <unordered_set>

PassID SCEVAnalysis::Key;

namespace {

// Resolve only SSA forwarding that is constant by construction.  This is not
// mathematical constant folding: PHI/select forwarding cannot hide an
// overflowing arithmetic operation, so the returned value is the exact IR
// value on every incoming path.
std::optional<int64_t> getTrivialExactIntConstant(
    Value *V, std::unordered_set<Value *> &Active, unsigned Depth = 0) {
    if (!V || Value::isUndef(V) || Depth > 32) return std::nullopt;
    if (auto *CI = dynamic_cast<ConstInt *>(V)) return CI->getVal();
    if (!Active.insert(V).second) return std::nullopt;

    std::optional<int64_t> Result;
    if (auto *Phi = dynamic_cast<PhiInst *>(V)) {
        for (unsigned I = 0; I < Phi->getNumIncomingValues(); ++I) {
            Value *Incoming = Phi->getIncomingValue(I);
            if (Incoming == V) continue;
            auto C = getTrivialExactIntConstant(Incoming, Active, Depth + 1);
            if (!C || (Result && *Result != *C)) {
                Active.erase(V);
                return std::nullopt;
            }
            Result = *C;
        }
    } else if (auto *Sel = dynamic_cast<SelectInst *>(V)) {
        if (Sel->getNumOperands() >= 3) {
            auto T = getTrivialExactIntConstant(
                Sel->getOperand(1), Active, Depth + 1);
            auto F = getTrivialExactIntConstant(
                Sel->getOperand(2), Active, Depth + 1);
            if (T && F && *T == *F) Result = *T;
        }
    }
    Active.erase(V);
    return Result;
}

} // namespace

bool SCEVExpr::isRecurrence() const {
    SCEVExprKind K = Kind;
    return K == SCEVExprKind::AddRec ||
           K == SCEVExprKind::MulRec ||
           K == SCEVExprKind::DivRec;
}

bool SCEVExpr::mentionsLoop(const Loop *L) const {
    if (!L) return false;
    if (isRecurrence() && RecLoop == L) return true;
    for (const SCEVExpr *Op : Operands)
        if (Op && Op->mentionsLoop(L)) return true;
    return false;
}

const char *SCEVExpr::kindName(SCEVExprKind K) {
    switch (K) {
    case SCEVExprKind::Constant: return "const";
    case SCEVExprKind::Unknown: return "unknown";
    case SCEVExprKind::Add: return "add";
    case SCEVExprKind::Sub: return "sub";
    case SCEVExprKind::Mul: return "mul";
    case SCEVExprKind::Div: return "div";
    case SCEVExprKind::Mod: return "mod";
    case SCEVExprKind::Min: return "min";
    case SCEVExprKind::Max: return "max";
    case SCEVExprKind::Cast: return "cast";
    case SCEVExprKind::Compare: return "cmp";
    case SCEVExprKind::AddRec: return "addrec";
    case SCEVExprKind::MulRec: return "mulrec";
    case SCEVExprKind::DivRec: return "divrec";
    }
    return "bad";
}

const char *SCEVExpr::castName(SCEVCastKind K) {
    switch (K) {
    case SCEVCastKind::Trunc: return "trunc";
    case SCEVCastKind::ZExt: return "zext";
    case SCEVCastKind::SExt: return "sext";
    case SCEVCastKind::BitCast: return "bitcast";
    }
    return "badcast";
}

const char *SCEVExpr::predicateName(BinaryInst::Operation Op) {
    switch (Op) {
    case BinaryInst::E: return "eq";
    case BinaryInst::NE: return "ne";
    case BinaryInst::GE: return "ge";
    case BinaryInst::L: return "lt";
    case BinaryInst::LE: return "le";
    case BinaryInst::G: return "gt";
    default: return "badpred";
    }
}

SCEVResult::SCEVResult(Function *F, LoopInfo *LoopInfoPtr, DominatorTree *DTPtr)
    : Func(F), LI(LoopInfoPtr), DT(DTPtr) {}

bool SCEVResult::invalidate(Function &, const PreservationStatus &PA) {
    return !PA.isKept(SCEVAnalysis::ID());
}

const SCEVExpr *SCEVResult::intern(std::unique_ptr<SCEVExpr> E) {
    auto It = Interned.find(E->Key);
    if (It != Interned.end()) return It->second;
    const SCEVExpr *Raw = E.get();
    ExprStorage.push_back(std::move(E));
    Interned[Raw->Key] = Raw;
    return Raw;
}

std::string SCEVResult::typeKey(std::shared_ptr<IRType> Ty) const {
    return "ty" + IRKey::type(Ty);
}

const SCEVExpr *SCEVResult::makeConstant(int64_t V, std::shared_ptr<IRType> Ty) {
    if (!Ty) Ty = INTType::NewInt(4);
    auto E = std::make_unique<SCEVExpr>();
    E->Kind = SCEVExprKind::Constant;
    E->ConstValue = V;
    E->Ty = Ty;
    E->Key = std::string("c:") + typeKey(Ty) + ":" + std::to_string(V);
    return intern(std::move(E));
}

const SCEVExpr *SCEVResult::makeUnknown(Value *V) {
    auto E = std::make_unique<SCEVExpr>();
    E->Kind = SCEVExprKind::Unknown;
    E->RawValue = V;
    E->Ty = V ? V->getIRType() : nullptr;
    E->Key = std::string("u:") + IRKey::pointer(V);
    return intern(std::move(E));
}

const SCEVExpr *SCEVResult::makeBinary(SCEVExprKind K, const SCEVExpr *L,
                                       const SCEVExpr *R,
                                       std::shared_ptr<IRType> Ty) {
    if (!L || !R) return nullptr;
    if ((K == SCEVExprKind::Add || K == SCEVExprKind::Mul ||
         K == SCEVExprKind::Min || K == SCEVExprKind::Max) &&
        L->Key > R->Key) {
        std::swap(L, R);
    }
    if ((K == SCEVExprKind::Min || K == SCEVExprKind::Max) && sameExpr(L, R))
        return L;
    if ((K == SCEVExprKind::Min || K == SCEVExprKind::Max)) {
        if (auto LC = asConstant(L)) {
            if (auto RC = asConstant(R)) {
                int64_t V = K == SCEVExprKind::Min ? std::min(*LC, *RC)
                                                   : std::max(*LC, *RC);
                return makeConstant(V, Ty ? Ty : (L->Ty ? L->Ty : R->Ty));
            }
        }
    }
    auto E = std::make_unique<SCEVExpr>();
    E->Kind = K;
    E->Ty = Ty ? Ty : (L->Ty ? L->Ty : R->Ty);
    E->Operands = {L, R};
    E->Key = std::string(SCEVExpr::kindName(K)) + ":" + typeKey(E->Ty) + "(" +
             L->Key + "," + R->Key + ")";
    return intern(std::move(E));
}

const SCEVExpr *SCEVResult::makeCast(SCEVCastKind K, const SCEVExpr *Op,
                                     std::shared_ptr<IRType> Ty) {
    if (!Op) return nullptr;

    if (K == SCEVCastKind::BitCast && Op->Ty == Ty) return Op;

    if (Op->Kind == SCEVExprKind::Constant) {
        int64_t V = Op->ConstValue;
        if (K == SCEVCastKind::Trunc && Ty && Ty == INTType::NewBool())
            V = (V != 0);
        return makeConstant(V, Ty);
    }

    if (Op->Kind == SCEVExprKind::Cast) {
        const SCEVExpr *Inner = Op->Operands.empty() ? nullptr : Op->Operands[0];
        if (Inner && K == SCEVCastKind::Trunc && Inner->Ty == Ty &&
            (Op->CastKind == SCEVCastKind::ZExt ||
             Op->CastKind == SCEVCastKind::SExt ||
             Op->CastKind == SCEVCastKind::BitCast)) {
            return Inner;
        }
        if (Op->CastKind == K) {
            return makeCast(K, Inner, Ty);
        }
    }

    auto E = std::make_unique<SCEVExpr>();
    E->Kind = SCEVExprKind::Cast;
    E->CastKind = K;
    E->Ty = Ty;
    E->Operands = {Op};
    E->Key = std::string("cast:") + SCEVExpr::castName(K) + ":" + typeKey(Ty) +
             "(" + Op->Key + ")";
    return intern(std::move(E));
}

const SCEVExpr *SCEVResult::makeCompare(BinaryInst::Operation Pred,
                                        const SCEVExpr *L,
                                        const SCEVExpr *R) {
    if (!L || !R) return nullptr;
    if (auto LC = asConstant(L)) {
        if (auto RC = asConstant(R)) {
            bool Res = false;
            switch (Pred) {
            case BinaryInst::E:  Res = *LC == *RC; break;
            case BinaryInst::NE: Res = *LC != *RC; break;
            case BinaryInst::GE: Res = *LC >= *RC; break;
            case BinaryInst::L:  Res = *LC <  *RC; break;
            case BinaryInst::LE: Res = *LC <= *RC; break;
            case BinaryInst::G:  Res = *LC >  *RC; break;
            default: break;
            }
            return makeConstant(Res ? 1 : 0, INTType::NewBool());
        }
    }
    auto E = std::make_unique<SCEVExpr>();
    E->Kind = SCEVExprKind::Compare;
    E->Predicate = Pred;
    E->Ty = INTType::NewBool();
    E->Operands = {L, R};
    E->Key = std::string("cmp:") + SCEVExpr::predicateName(Pred) + "(" + L->Key + "," +
             R->Key + ")";
    return intern(std::move(E));
}

const SCEVExpr *SCEVResult::makeRec(SCEVExprKind K, const Loop *L,
                                    const SCEVExpr *Base,
                                    const SCEVExpr *Step,
                                    std::shared_ptr<IRType> Ty) {
    if (!L || !Base || !Step) return nullptr;
    if (K == SCEVExprKind::AddRec && Step->isConstant(0)) return Base;
    if ((K == SCEVExprKind::MulRec || K == SCEVExprKind::DivRec) &&
        Step->isConstant(1)) {
        return Base;
    }
    auto E = std::make_unique<SCEVExpr>();
    E->Kind = K;
    E->RecLoop = L;
    E->Ty = Ty ? Ty : Base->Ty;
    E->Operands = {Base, Step};
    E->Key = std::string(SCEVExpr::kindName(K)) + "@" + IRKey::pointer(L) + ":" +
             typeKey(E->Ty) + "(" + Base->Key + "," + Step->Key + ")";
    return intern(std::move(E));
}

std::optional<int64_t> SCEVResult::asConstant(const SCEVExpr *E) const {
    if (!E || E->Kind != SCEVExprKind::Constant) return std::nullopt;
    return E->ConstValue;
}

bool SCEVResult::sameExpr(const SCEVExpr *A, const SCEVExpr *B) const {
    if (A == B) return true;
    if (!A || !B) return false;
    return A->Key == B->Key;
}

const SCEVExpr *SCEVResult::neg(const SCEVExpr *E) {
    return mul(makeConstant(-1, E ? E->Ty : nullptr), E);
}

const SCEVExpr *SCEVResult::add(const SCEVExpr *L, const SCEVExpr *R) {
    if (!L || !R) return nullptr;
    if (L->isConstant(0)) return R;
    if (R->isConstant(0)) return L;
    if (auto LC = asConstant(L)) {
        if (auto RC = asConstant(R))
            return makeConstant(*LC + *RC, L->Ty ? L->Ty : R->Ty);
    }

    if (L->Kind == SCEVExprKind::AddRec && R->Kind == SCEVExprKind::AddRec &&
        L->RecLoop == R->RecLoop) {
        return makeRec(SCEVExprKind::AddRec, L->RecLoop,
                       add(L->Operands[0], R->Operands[0]),
                       add(L->Operands[1], R->Operands[1]), L->Ty);
    }
    if (L->Kind == SCEVExprKind::AddRec && !R->isRecurrence()) {
        return makeRec(SCEVExprKind::AddRec, L->RecLoop,
                       add(L->Operands[0], R), L->Operands[1], L->Ty);
    }
    if (R->Kind == SCEVExprKind::AddRec && !L->isRecurrence()) {
        return makeRec(SCEVExprKind::AddRec, R->RecLoop,
                       add(R->Operands[0], L), R->Operands[1], R->Ty);
    }

    return makeBinary(SCEVExprKind::Add, L, R);
}

const SCEVExpr *SCEVResult::sub(const SCEVExpr *L, const SCEVExpr *R) {
    if (!L || !R) return nullptr;
    if (sameExpr(L, R)) return makeConstant(0, L->Ty);
    if (R->isConstant(0)) return L;
    if (auto LC = asConstant(L)) {
        if (auto RC = asConstant(R))
            return makeConstant(*LC - *RC, L->Ty ? L->Ty : R->Ty);
    }
    if (R->Kind == SCEVExprKind::AddRec)
        return add(L, neg(R));
    if (L->Kind == SCEVExprKind::AddRec && !R->isRecurrence()) {
        return makeRec(SCEVExprKind::AddRec, L->RecLoop,
                       sub(L->Operands[0], R), L->Operands[1], L->Ty);
    }
    return makeBinary(SCEVExprKind::Sub, L, R);
}

const SCEVExpr *SCEVResult::mul(const SCEVExpr *L, const SCEVExpr *R) {
    if (!L || !R) return nullptr;
    if (L->isConstant(0) || R->isConstant(0))
        return makeConstant(0, L->Ty ? L->Ty : R->Ty);
    if (L->isConstant(1)) return R;
    if (R->isConstant(1)) return L;
    if (auto LC = asConstant(L)) {
        if (auto RC = asConstant(R))
            return makeConstant(*LC * *RC, L->Ty ? L->Ty : R->Ty);
    }

    if (L->Kind == SCEVExprKind::AddRec && asConstant(R)) {
        return makeRec(SCEVExprKind::AddRec, L->RecLoop,
                       mul(L->Operands[0], R), mul(L->Operands[1], R), L->Ty);
    }
    if (R->Kind == SCEVExprKind::AddRec && asConstant(L)) {
        return makeRec(SCEVExprKind::AddRec, R->RecLoop,
                       mul(R->Operands[0], L), mul(R->Operands[1], L), R->Ty);
    }

    if (L->Key > R->Key) std::swap(L, R);
    return makeBinary(SCEVExprKind::Mul, L, R);
}

const SCEVExpr *SCEVResult::div(const SCEVExpr *L, const SCEVExpr *R) {
    if (!L || !R) return nullptr;
    if (sameExpr(L, R)) return makeConstant(1, L->Ty);
    if (L->isConstant(0)) return L;
    if (R->isConstant(1)) return L;
    if (auto LC = asConstant(L)) {
        if (auto RC = asConstant(R)) {
            if (*RC != 0) return makeConstant(*LC / *RC, L->Ty ? L->Ty : R->Ty);
        }
    }
    return makeBinary(SCEVExprKind::Div, L, R);
}

const SCEVExpr *SCEVResult::mod(const SCEVExpr *L, const SCEVExpr *R) {
    if (!L || !R) return nullptr;
    if (R->isConstant(0)) return nullptr;
    if (L->isConstant(0)) return makeConstant(0, L->Ty ? L->Ty : R->Ty);
    if (auto LC = asConstant(L)) {
        if (auto RC = asConstant(R))
            return makeConstant(*LC % *RC, L->Ty ? L->Ty : R->Ty);
    }
    return makeBinary(SCEVExprKind::Mod, L, R);
}

const SCEVExpr *SCEVResult::getSCEV(Value *V) {
    if (!V) return nullptr;
    auto It = ValueCache.find(V);
    if (It != ValueCache.end()) return It->second;
    if (ActiveValues[V]) return makeUnknown(V);
    ActiveValues[V] = true;
    const SCEVExpr *E = create(V);
    ActiveValues[V] = false;
    if (!E) E = makeUnknown(V);
    ValueCache[V] = E;
    return E;
}

const SCEVExpr *SCEVResult::create(Value *V) {
    if (auto *CI = dynamic_cast<ConstInt *>(V))
        return makeConstant(CI->getVal(), V->getIRType());
    if (auto *CB = dynamic_cast<ConstBool *>(V))
        return makeConstant(CB->getVal() ? 1 : 0, V->getIRType());

    auto *I = dynamic_cast<Instruction *>(V);
    if (!I) return makeUnknown(V);

    if (auto *BI = dynamic_cast<BinaryInst *>(I))
        return createFromBinary(BI);
    if (auto *Phi = dynamic_cast<PhiInst *>(I))
        return createFromPhi(Phi);
    if (auto *Sel = dynamic_cast<SelectInst *>(I))
        return createFromSelect(Sel);
    if (auto *GEP = dynamic_cast<GetElementPtrInst *>(I))
        return createFromGEP(GEP);
    if (dynamic_cast<ZextInst *>(I))
        return createFromCast(I, SCEVCastKind::ZExt);
    if (dynamic_cast<SextInst *>(I))
        return createFromCast(I, SCEVCastKind::SExt);
    if (dynamic_cast<TruncInst *>(I))
        return createFromCast(I, SCEVCastKind::Trunc);
    if (dynamic_cast<BitCastInst *>(I))
        return createFromCast(I, SCEVCastKind::BitCast);

    return makeUnknown(V);
}

const SCEVExpr *SCEVResult::createFromBinary(BinaryInst *BI) {
    Value *LHS = BI->getOperand(0);
    Value *RHS = BI->getOperand(1);
    if (!LHS || !RHS) return makeUnknown(BI);

    BinaryInst::Operation Op = BI->getOp();
    if (BinaryInst::isCompareOp(Op)) return makeCompare(Op, getSCEV(LHS), getSCEV(RHS));

    if (Op == BinaryInst::SHL) {
        if (auto *CI = dynamic_cast<ConstInt *>(RHS)) {
            int Shift = CI->getVal();
            if (Shift >= 0 && Shift < 31)
                return mul(getSCEV(LHS), makeConstant(1LL << Shift, LHS->getIRType()));
        }
        return makeUnknown(BI);
    }

    switch (Op) {
    case BinaryInst::ADD: return add(getSCEV(LHS), getSCEV(RHS));
    case BinaryInst::SUB: return sub(getSCEV(LHS), getSCEV(RHS));
    case BinaryInst::MUL: return mul(getSCEV(LHS), getSCEV(RHS));
    case BinaryInst::DIV:
    case BinaryInst::UDIV: return div(getSCEV(LHS), getSCEV(RHS));
    case BinaryInst::MOD:
    case BinaryInst::UREM: return mod(getSCEV(LHS), getSCEV(RHS));
    default: break;
    }
    return makeUnknown(BI);
}

const SCEVExpr *SCEVResult::createFromCast(Instruction *I, SCEVCastKind CK) {
    Value *Op = I->getOperand(0);
    if (!Op) return makeUnknown(I);
    return makeCast(CK, getSCEV(Op), I->getIRType());
}

const SCEVExpr *SCEVResult::createFromPhi(PhiInst *Phi) {
    if (!Phi) return nullptr;

    BasicBlock *BB = Phi->getParent();
    Loop *L = LI ? LI->getLoopFor(BB) : nullptr;
    if (L && L->getHeader() == BB) {
        if (const SCEVExpr *R = tryHeaderPhi(Phi, L))
            return R;
    }

    if (Phi->getNumIncomingValues() == 1)
        return getSCEV(Phi->getIncomingValue(0));

    const SCEVExpr *Common = nullptr;
    for (unsigned I = 0; I < Phi->getNumIncomingValues(); ++I) {
        Value *In = Phi->getIncomingValue(I);
        if (!In || In == Phi) continue;
        const SCEVExpr *Cur = getSCEV(In);
        if (!Common) {
            Common = Cur;
            continue;
        }
        if (!sameExpr(Common, Cur)) return makeUnknown(Phi);
    }
    return Common ? Common : makeUnknown(Phi);
}

const SCEVExpr *SCEVResult::tryHeaderPhi(PhiInst *Phi, const Loop *L) {
    Value *Start = nullptr;
    Value *Update = nullptr;
    for (unsigned I = 0; I < Phi->getNumIncomingValues(); ++I) {
        Value *V = Phi->getIncomingValue(I);
        BasicBlock *Pred = Phi->getIncomingBlock(I);
        if (!V || !Pred) return nullptr;
        if (L->contains(Pred)) {
            if (Update && Update != V) return nullptr;
            Update = V;
        } else {
            if (Start && Start != V) return nullptr;
            Start = V;
        }
    }
    if (!Start || !Update) return nullptr;

    if (auto Delta = findAdditiveDelta(Phi, Update, L)) {
        if (Delta->SeenSelf && Delta->Delta) {
            return makeRec(SCEVExprKind::AddRec, L, getSCEV(Start),
                           Delta->Delta, Phi->getIRType());
        }
    }

    return trySimpleMulDivRec(Phi, Update, Start, L);
}

std::optional<SCEVResult::UpdateDelta>
SCEVResult::findAdditiveDelta(PhiInst *Self, Value *V, const Loop *L) {
    if (!V) return std::nullopt;
    if (V == Self) return UpdateDelta{true, makeConstant(0, Self->getIRType())};

    if (isLoopInvariant(V, L))
        return UpdateDelta{false, getSCEV(V)};

    auto *BI = dynamic_cast<BinaryInst *>(V);
    if (!BI) return std::nullopt;

    Value *A = BI->getOperand(0);
    Value *B = BI->getOperand(1);
    if (!A || !B) return std::nullopt;

    BinaryInst::Operation Op = BI->getOp();
    auto DA = findAdditiveDelta(Self, A, L);
    auto DB = findAdditiveDelta(Self, B, L);

    if (!DA || !DB) return std::nullopt;

    if (Op == BinaryInst::ADD) {
        if (DA->SeenSelf && DB->SeenSelf) return std::nullopt;
        return UpdateDelta{DA->SeenSelf || DB->SeenSelf,
                           add(DA->Delta, DB->Delta)};
    }

    if (Op == BinaryInst::SUB) {
        if (DA->SeenSelf && DB->SeenSelf) return std::nullopt;
        if (DB->SeenSelf) return std::nullopt;
        return UpdateDelta{DA->SeenSelf, sub(DA->Delta, DB->Delta)};
    }

    if (!DA->SeenSelf && !DB->SeenSelf) {
        if (Op == BinaryInst::MUL) return UpdateDelta{false, mul(DA->Delta, DB->Delta)};
        if (Op == BinaryInst::DIV || Op == BinaryInst::UDIV)
            return UpdateDelta{false, div(DA->Delta, DB->Delta)};
        if (Op == BinaryInst::SHL) {
            if (auto *CI = dynamic_cast<ConstInt *>(B)) {
                int Shift = CI->getVal();
                if (Shift >= 0 && Shift < 31)
                    return UpdateDelta{false, mul(DA->Delta, makeConstant(1LL << Shift))};
            }
        }
    }

    return std::nullopt;
}

const SCEVExpr *SCEVResult::trySimpleMulDivRec(PhiInst *Self, Value *Update,
                                               Value *Start, const Loop *L) {
    auto *BI = dynamic_cast<BinaryInst *>(Update);
    if (!BI) return nullptr;
    Value *A = BI->getOperand(0);
    Value *B = BI->getOperand(1);
    if (!A || !B) return nullptr;

    if (BI->getOp() == BinaryInst::MUL) {
        if (A == Self && isLoopInvariant(B, L))
            return makeRec(SCEVExprKind::MulRec, L, getSCEV(Start), getSCEV(B),
                           Self->getIRType());
        if (B == Self && isLoopInvariant(A, L))
            return makeRec(SCEVExprKind::MulRec, L, getSCEV(Start), getSCEV(A),
                           Self->getIRType());
    }
    if ((BI->getOp() == BinaryInst::DIV || BI->getOp() == BinaryInst::UDIV) &&
        A == Self && isLoopInvariant(B, L)) {
        return makeRec(SCEVExprKind::DivRec, L, getSCEV(Start), getSCEV(B),
                       Self->getIRType());
    }
    return nullptr;
}

bool SCEVResult::isLoopInvariant(Value *V, const Loop *L) const {
    if (!V || !L) return true;
    if (V->isConst()) return true;
    if (auto *Var = dynamic_cast<Variable *>(V))
        if (Var->isParam() || Var->isGlobal()) return true;
    auto *I = dynamic_cast<Instruction *>(V);
    if (!I) return true;
    BasicBlock *BB = I->getParent();
    return !BB || !L->contains(BB);
}

bool SCEVResult::isLoopInvariantExpr(const SCEVExpr *E,
                                     const Loop *L) const {
    if (!E || !L) return true;
    if (E->isRecurrence() && E->RecLoop == L) return false;
    if (E->Kind == SCEVExprKind::Unknown)
        return isLoopInvariant(E->RawValue, L);
    for (const SCEVExpr *Op : E->Operands)
        if (!isLoopInvariantExpr(Op, L)) return false;
    return true;
}

bool SCEVResult::isDirectLoopBlock(const Loop *L, BasicBlock *BB) const {
    if (!L || !BB) return false;
    if (LI) return LI->getLoopFor(BB) == L;
    const auto &Blocks = L->getBlocks();
    return std::find(Blocks.begin(), Blocks.end(), BB) != Blocks.end();
}

const SCEVExpr *SCEVResult::createFromSelect(SelectInst *Sel) {
    Value *Cond = Sel->getCondition();
    Value *TrueV = Sel->getTrueValue();
    Value *FalseV = Sel->getFalseValue();
    if (!Cond || !TrueV || !FalseV) return makeUnknown(Sel);

    bool CondValue = false;
    if (Cond->tryGetBoolConst(CondValue))
        return getSCEV(CondValue ? TrueV : FalseV);

    const SCEVExpr *TE = getSCEV(TrueV);
    const SCEVExpr *FE = getSCEV(FalseV);
    if (sameExpr(TE, FE)) return TE;

    auto *Cmp = dynamic_cast<BinaryInst *>(Cond);
    if (!Cmp || !BinaryInst::isCompareOp(Cmp->getOp())) return makeUnknown(Sel);

    Value *A = nullptr;
    Value *B = nullptr;
    if (!Cmp->getOperands(A, B)) return makeUnknown(Sel);

    const bool TrueIsA = sameExpr(TE, getSCEV(A));
    const bool TrueIsB = sameExpr(TE, getSCEV(B));
    const bool FalseIsA = sameExpr(FE, getSCEV(A));
    const bool FalseIsB = sameExpr(FE, getSCEV(B));

    switch (Cmp->getOp()) {
    case BinaryInst::G:
    case BinaryInst::GE:
        if (TrueIsA && FalseIsB)
            return makeBinary(SCEVExprKind::Max, getSCEV(A), getSCEV(B), Sel->getIRType());
        if (TrueIsB && FalseIsA)
            return makeBinary(SCEVExprKind::Min, getSCEV(A), getSCEV(B), Sel->getIRType());
        break;
    case BinaryInst::L:
    case BinaryInst::LE:
        if (TrueIsA && FalseIsB)
            return makeBinary(SCEVExprKind::Min, getSCEV(A), getSCEV(B), Sel->getIRType());
        if (TrueIsB && FalseIsA)
            return makeBinary(SCEVExprKind::Max, getSCEV(A), getSCEV(B), Sel->getIRType());
        break;
    default:
        break;
    }
    return makeUnknown(Sel);
}

const SCEVExpr *SCEVResult::createFromGEP(GetElementPtrInst *GEP) {
    Value *Base = GEP ? GEP->getBasePointer() : nullptr;
    if (!Base || !Base->getIRType()) return makeUnknown(GEP);

    const SCEVExpr *Result = getSCEV(Base);
    std::shared_ptr<IRType> CurType = Base->getIRType();
    int64_t ConstantOffset = 0;

    for (size_t I = 0; I < GEP->getNumIndices(); ++I) {
        std::size_t Stride = 0;
        if (auto PtrTy = std::dynamic_pointer_cast<POINTERType>(CurType)) {
            CurType = PtrTy->getPointerType();
            if (!CurType) return makeUnknown(GEP);
            Stride = CurType->getSize();
        } else if (auto ArrTy = std::dynamic_pointer_cast<ARRAYType>(CurType)) {
            CurType = ArrTy->getElementType();
            if (!CurType) return makeUnknown(GEP);
            Stride = CurType->getSize();
        } else {
            return makeUnknown(GEP);
        }

        Value *Idx = GEP->getIndex(I);
        if (!Idx) return makeUnknown(GEP);
        if (auto *CI = dynamic_cast<ConstInt *>(Idx)) {
            ConstantOffset += static_cast<int64_t>(CI->getVal()) *
                              static_cast<int64_t>(Stride);
        } else {
            const SCEVExpr *Scaled = mul(getSCEV(Idx),
                                         makeConstant(static_cast<int64_t>(Stride),
                                                      Idx->getIRType()));
            Result = add(Result, Scaled);
        }
    }

    if (ConstantOffset != 0)
        Result = add(Result, makeConstant(ConstantOffset, INTType::NewInt(4)));
    return Result ? Result : makeUnknown(GEP);
}

std::optional<SCEVAffineInfo> SCEVResult::getAffine(Value *V) {
    return getAffine(getSCEV(V));
}

std::optional<SCEVAffineInfo> SCEVResult::getAffine(const SCEVExpr *E) const {
    if (!E || E->Kind != SCEVExprKind::AddRec || E->Operands.size() != 2)
        return std::nullopt;
    SCEVAffineInfo Info;
    Info.RecLoop = E->RecLoop;
    Info.Start = E->Operands[0];
    Info.Step = E->Operands[1];
    Info.ConstantStart = asConstant(Info.Start);
    Info.ConstantStep = asConstant(Info.Step);
    return Info;
}

std::vector<BasicBlock *>
SCEVResult::collectDirectLoopBlocks(const Loop *L) const {
    std::vector<BasicBlock *> Blocks;
    if (!L) return Blocks;

    for (BasicBlock *BB : L->getBlocks()) {
        if (!BB) continue;
        if (LI && LI->getLoopFor(BB) != L) continue;
        Blocks.push_back(BB);
    }

    return Blocks;
}

const SCEVExpr *SCEVResult::getTripCount(const Loop *L) {
    if (!L) return nullptr;
    auto It = TripCountCache.find(L);
    if (It != TripCountCache.end()) return It->second;
    auto Info = analyzeLoopTripCount(L);
    if (!Info || !Info->TripCount) return nullptr;
    TripCountCache[L] = Info->TripCount;
    if (Info->BackedgeTakenCount)
        BackedgeCountCache[L] = Info->BackedgeTakenCount;
    return Info->TripCount;
}

const SCEVExpr *SCEVResult::getBackedgeTakenCount(const Loop *L) {
    if (!L) return nullptr;
    auto It = BackedgeCountCache.find(L);
    if (It != BackedgeCountCache.end()) return It->second;

    auto Info = analyzeLoopTripCount(L);
    if (!Info || !Info->BackedgeTakenCount) return nullptr;
    if (Info->TripCount) TripCountCache[L] = Info->TripCount;
    BackedgeCountCache[L] = Info->BackedgeTakenCount;
    return Info->BackedgeTakenCount;
}

const SCEVExpr *SCEVResult::computeTripCount(const Loop *L) {
    auto Info = analyzeLoopTripCount(L);
    return Info ? Info->TripCount : nullptr;
}

const SCEVExpr *SCEVResult::tripFromComparison(const Loop *L, BinaryInst *Cmp,
                                               bool ContinueOnTrue) {
    if (!L || !Cmp) return nullptr;
    LoopTripCountInfo Info;
    Info.L = L;
    Info.Header = L->getHeader();
    Info.ExitingBlock = Cmp->getParent();
    L->getLoopSimplifyForm(Info.PreHeader, Info.Latch);
    Info.ExitCmp = Cmp;
    Info.ContinueOnTrue = ContinueOnTrue;
    if (!fillTripCountFromComparison(Info)) return nullptr;
    return Info.TripCount;
}

std::optional<LoopTripCountInfo>
SCEVResult::analyzeLoopTripCount(const Loop *L) {
    if (!L) return std::nullopt;

    BasicBlock *PreHeader = nullptr;
    BasicBlock *Latch = nullptr;
    if (!L->getLoopSimplifyForm(PreHeader, Latch)) return std::nullopt;
    BasicBlock *Header = L->getHeader();
    if (!PreHeader || !Header || !Latch) return std::nullopt;

    LoopTripCountInfo Info;
    Info.L = L;
    Info.PreHeader = PreHeader;
    Info.Header = Header;
    Info.Latch = Latch;

    for (BasicBlock *BB : collectDirectLoopBlocks(L)) {
        if (!BB) continue;

        bool HasOutsideSucc = false;
        for (BasicBlock *Succ : BB->getSuccessors()) {
            if (Succ && !L->contains(Succ)) {
                HasOutsideSucc = true;
                break;
            }
        }
        if (!HasOutsideSucc) continue;

        if (Info.ExitingBlock && Info.ExitingBlock != BB)
            return std::nullopt;

        auto *Br = BB->getBranchTerminator();
        if (!BasicBlock::isCondBr(Br)) return std::nullopt;
        BasicBlock *TrueBB = Br->getTrueSuccessor();
        BasicBlock *FalseBB = Br->getFalseSuccessor();
        bool TrueIn = TrueBB && L->contains(TrueBB);
        bool FalseIn = FalseBB && L->contains(FalseBB);
        if (TrueIn == FalseIn) return std::nullopt;

        BasicBlock *ThisExit = TrueIn ? FalseBB : TrueBB;
        if (!ThisExit || L->contains(ThisExit)) return std::nullopt;
        if (Info.ExitBlock && Info.ExitBlock != ThisExit)
            return std::nullopt;

        Info.ExitingBlock = BB;
        Info.ExitBlock = ThisExit;
        Info.ContinueOnTrue = TrueIn;
    }

    if (!Info.ExitingBlock) return std::nullopt;
    if (DT && !DT->dominates(Info.ExitingBlock, Latch)) return std::nullopt;

    auto *Br = Info.ExitingBlock->getBranchTerminator();
    Value *Cond = Br->getOperand(0);
    auto *Cmp = dynamic_cast<BinaryInst *>(Cond);
    if (!Cmp || !BinaryInst::isCompareOp(Cmp->getOp())) return std::nullopt;

    Info.ExitCmp = Cmp;
    if (!fillTripCountFromComparison(Info)) return std::nullopt;
    Info.Valid = true;
    return Info;
}

bool SCEVResult::fillTripCountFromComparison(LoopTripCountInfo &Info) {
    const Loop *L = Info.L;
    BinaryInst *Cmp = Info.ExitCmp;
    if (!L || !Cmp) return false;

    BinaryInst::Operation Pred = Info.ContinueOnTrue
                                     ? Cmp->getOp()
                                     : BinaryInst::negatePredicate(Cmp->getOp());
    Value *LHSVal = Cmp->getOperand(0);
    Value *RHSVal = Cmp->getOperand(1);
    if (!LHSVal || !RHSVal) return false;

    const SCEVExpr *LHS = getSCEV(LHSVal);
    const SCEVExpr *RHS = getSCEV(RHSVal);
    auto Affine = getAffine(LHS);

    if (!Affine || Affine->RecLoop != L) {
        if (!isLoopInvariantExpr(LHS, L)) return false;
        auto RHSAffine = getAffine(RHS);
        if (RHSAffine && RHSAffine->RecLoop == L) {
            std::swap(LHS, RHS);
            std::swap(LHSVal, RHSVal);
            Pred = BinaryInst::reversePredicate(Pred);
            Affine = RHSAffine;
        } else {
            return false;
        }
    }

    if (!Affine || Affine->RecLoop != L) return false;
    if (!isLoopInvariantExpr(RHS, L)) return false;

    auto StepC = Affine->ConstantStep;
    if (!StepC || *StepC == 0) return false;
    if (*StepC == std::numeric_limits<int64_t>::min()) return false;

    const SCEVExpr *Start = Affine->Start;
    const SCEVExpr *Bound = RHS;
    const SCEVExpr *Distance = nullptr;
    int64_t Step = *StepC;
    int64_t AbsStep = Step < 0 ? -Step : Step;

    switch (Pred) {
    case BinaryInst::L:
        if (Step <= 0) return false;
        Distance = sub(Bound, Start);
        break;
    case BinaryInst::LE:
        if (Step <= 0) return false;
        Distance = sub(add(Bound, makeConstant(1, Bound->Ty)), Start);
        break;
    case BinaryInst::G:
        if (Step >= 0) return false;
        Distance = sub(Start, Bound);
        break;
    case BinaryInst::GE:
        if (Step >= 0) return false;
        Distance = sub(Start, sub(Bound, makeConstant(1, Bound->Ty)));
        break;
    case BinaryInst::NE:
        if (Step > 0) Distance = sub(Bound, Start);
        else Distance = sub(Start, Bound);
        break;
    default:
        return false;
    }

    if (!Distance) return false;

    const SCEVExpr *TC = nullptr;
    if (auto D = asConstant(Distance)) {
        if (Pred == BinaryInst::NE) {
            if (*D < 0 || (*D % AbsStep) != 0) return false;
            TC = makeConstant(*D / AbsStep, Distance->Ty);
        } else {
            TC = makeConstant(IRMath::ceilDivPositive(*D, AbsStep), Distance->Ty);
        }
    } else {
        // For !=, a runtime distance is only safe if exact reachability can be
        // proven.  This compact SCEV does not do symbolic divisibility proofs.
        if (Pred == BinaryInst::NE) return false;

        const SCEVExpr *Raw = nullptr;
        if (AbsStep == 1) {
            Raw = Distance;
        } else {
            Raw = div(add(Distance, makeConstant(AbsStep - 1, Distance->Ty)),
                      makeConstant(AbsStep, Distance->Ty));
        }
        if (!Raw) return false;
        TC = makeBinary(SCEVExprKind::Max, makeConstant(0, Raw->Ty), Raw, Raw->Ty);
    }

    if (!TC) return false;

    // Classify the compared recurrence by SSA identity, not by the block that
    // happens to contain the branch.  In particular, a one-block do-while has
    // Header == Latch, so the old `ExitingBlock != Header` test treated a
    // comparison of the backedge value as a zero-trip header test and was off
    // by one.
    bool ComparesExactHeaderPhi = false;
    bool ComparesExactBackedgeUpdate = false;
    if (auto *ComparedPhi = dynamic_cast<PhiInst *>(LHSVal)) {
        auto ComparedAffine = getAffine(ComparedPhi);
        if (ComparedPhi->getParent() == Info.Header && ComparedAffine &&
            ComparedAffine->RecLoop == L &&
            sameExpr(ComparedAffine->Start, Affine->Start) &&
            sameExpr(ComparedAffine->Step, Affine->Step)) {
            Info.IndVar = ComparedPhi;
            ComparesExactHeaderPhi = true;
        }
    }

    // A canonical latch test also commonly compares the exact backedge update
    // (`next = phi +/- C`).  Associate it only when that very instruction is
    // the unique in-loop incoming value of the candidate header PHI.  Merely
    // having an equal SCEV step (for example `i + C` used only by the compare)
    // is insufficient.
    if (!Info.IndVar) {
        auto *Update = dynamic_cast<BinaryInst *>(LHSVal);
        PhiInst *Candidate = nullptr;
        if (Update) {
            Value *A = Update->getOperand(0);
            Value *B = Update->getOperand(1);
            auto *PA = dynamic_cast<PhiInst *>(A);
            auto *PB = dynamic_cast<PhiInst *>(B);
            auto *CA = dynamic_cast<ConstInt *>(A);
            auto *CB = dynamic_cast<ConstInt *>(B);
            if (Update->getOp() == BinaryInst::ADD) {
                if (PA && CB) Candidate = PA;
                else if (PB && CA) Candidate = PB;
            } else if (Update->getOp() == BinaryInst::SUB && PA && CB) {
                Candidate = PA;
            }
        }
        if (Candidate && Candidate->getParent() == Info.Header) {
            bool UpdateIsBackedge = false;
            bool BadIncoming = false;
            for (unsigned I = 0; I < Candidate->getNumIncomingValues(); ++I) {
                BasicBlock *PredBB = Candidate->getIncomingBlock(I);
                Value *Incoming = Candidate->getIncomingValue(I);
                if (!PredBB || !Incoming) {
                    BadIncoming = true;
                    break;
                }
                if (L->contains(PredBB)) {
                    if (UpdateIsBackedge || Incoming != Update ||
                        (Info.Latch && PredBB != Info.Latch)) {
                        BadIncoming = true;
                        break;
                    }
                    UpdateIsBackedge = true;
                }
            }
            auto CandidateAffine = getAffine(Candidate);
            if (!BadIncoming && UpdateIsBackedge && CandidateAffine &&
                CandidateAffine->RecLoop == L &&
                sameExpr(CandidateAffine->Step, Affine->Step)) {
                Info.IndVar = Candidate;
                ComparesExactBackedgeUpdate = true;
            }
        }
    }

    // The distance derived from an exact backedge update counts successful
    // continuations after the first iteration has already executed, so the
    // first iteration must always be included -- even when Header == Latch.
    // An exact header PHI needs the same adjustment only for a latch test; a
    // normal header test can execute zero iterations.  Unknown affine aliases
    // are rejected rather than assigned a guessed iteration phase.
    if (ComparesExactBackedgeUpdate ||
        (ComparesExactHeaderPhi && Info.ExitingBlock == Info.Latch)) {
        TC = add(TC, makeConstant(1, TC->Ty));
        if (!TC) return false;
    } else if (!ComparesExactHeaderPhi) {
        return false;
    }

    Info.Start = Start;
    Info.Step = Affine->Step;
    Info.Bound = Bound;
    Info.TripCount = TC;
    Info.ConstantTripCount = asConstant(TC);
    Info.IsRuntime = !Info.ConstantTripCount.has_value();
    Info.MayBeZeroTrip = !Info.ConstantTripCount || *Info.ConstantTripCount == 0;

    // Constant arithmetic above is mathematical arithmetic.  The source IR
    // recurrence is signed i32 and may wrap, in which case that mathematical
    // count is neither a termination proof nor a legal range fact.  For now we
    // publish a no-wrap proof only when the comparison was tied to an exact
    // header PHI/backedge update and the value at the modeled exit also fits
    // i32.  Monotonicity then implies every intermediate recurrence value fits
    // as well.
    if ((Info.ExitingBlock == Info.Header ||
         Info.ExitingBlock == Info.Latch) && Info.IndVar &&
        Info.ConstantTripCount && *Info.ConstantTripCount >= 0) {
        auto StepConst = asConstant(Affine->Step);
        auto Ty = Info.IndVar->getIRType();
        std::optional<int64_t> ExactStart;
        Value *DirectUpdate = nullptr;
        for (unsigned I = 0; I < Info.IndVar->getNumIncomingValues(); ++I) {
            BasicBlock *PredBB = Info.IndVar->getIncomingBlock(I);
            Value *Incoming = Info.IndVar->getIncomingValue(I);
            if (!PredBB || !Incoming) {
                ExactStart.reset();
                DirectUpdate = nullptr;
                break;
            }
            if (L->contains(PredBB)) {
                if (DirectUpdate && DirectUpdate != Incoming) {
                    DirectUpdate = nullptr;
                    break;
                }
                DirectUpdate = Incoming;
            } else {
                std::unordered_set<Value *> Active;
                auto C = getTrivialExactIntConstant(Incoming, Active);
                if (!C || (ExactStart && *ExactStart != *C)) {
                    ExactStart.reset();
                    break;
                }
                ExactStart = *C;
            }
        }

        // Confirm that the backedge is exactly `phi +/- ConstInt`; otherwise
        // a mathematically folded invariant step could already have wrapped in
        // the source IR before entering this recurrence.
        std::optional<int64_t> DirectStep;
        if (auto *Update = dynamic_cast<BinaryInst *>(DirectUpdate)) {
            Value *A = Update->getOperand(0);
            Value *B = Update->getOperand(1);
            auto *CA = dynamic_cast<ConstInt *>(A);
            auto *CB = dynamic_cast<ConstInt *>(B);
            if (Update->getOp() == BinaryInst::ADD) {
                if (A == Info.IndVar && CB) DirectStep = CB->getVal();
                else if (B == Info.IndVar && CA) DirectStep = CA->getVal();
            } else if (Update->getOp() == BinaryInst::SUB &&
                       A == Info.IndVar && CB &&
                       CB->getVal() != std::numeric_limits<int32_t>::min()) {
                DirectStep = -static_cast<int64_t>(CB->getVal());
            }
        }

        auto *DirectBound = dynamic_cast<ConstInt *>(RHSVal);
        if (ExactStart && DirectBound && DirectStep && StepConst &&
            *DirectStep == *StepConst && Ty && Ty->getEnum() == IR_INT &&
            Ty->getIntegerBitWidth(32) == 32) {
            const int64_t StartValue = *ExactStart;
            const __int128 ExitValue = static_cast<__int128>(StartValue) +
                static_cast<__int128>(*StepConst) *
                    static_cast<__int128>(*Info.ConstantTripCount);
            Info.ProvenNoWrap =
                StartValue >= IntRange::I32_MIN &&
                StartValue <= IntRange::I32_MAX &&
                ExitValue >= IntRange::I32_MIN &&
                ExitValue <= IntRange::I32_MAX;
        }
    }
    if (Info.ConstantTripCount) {
        int64_t BE = *Info.ConstantTripCount <= 0 ? 0 : *Info.ConstantTripCount - 1;
        Info.BackedgeTakenCount = makeConstant(BE, TC->Ty);
        Info.ConstantBackedgeTakenCount = BE;
    } else {
        Info.BackedgeTakenCount =
            makeBinary(SCEVExprKind::Max, makeConstant(0, TC->Ty),
                       sub(TC, makeConstant(1, TC->Ty)), TC->Ty);
        Info.ConstantBackedgeTakenCount = asConstant(Info.BackedgeTakenCount);
    }

    return Info.BackedgeTakenCount != nullptr;
}

PhiInst *SCEVResult::findPhiForAddRec(const Loop *L,
                                      const SCEVExpr *AddRec) {
    if (!L || !AddRec || AddRec->Kind != SCEVExprKind::AddRec)
        return nullptr;
    BasicBlock *Header = L->getHeader();
    if (!Header) return nullptr;

    PhiInst *Found = nullptr;
    for (Instruction *I : *Header) {
        auto *Phi = dynamic_cast<PhiInst *>(I);
        if (!Phi) break;
        const SCEVExpr *PhiSCEV = getSCEV(Phi);
        if (!sameExpr(PhiSCEV, AddRec)) continue;
        if (Found && Found != Phi) return nullptr;
        Found = Phi;
    }
    return Found;
}

PhiInst *SCEVResult::findPhiForAffineRec(const Loop *L,
                                         const SCEVAffineInfo &Affine) {
    if (!L) return nullptr;

    BasicBlock *Header = L->getHeader();
    if (!Header) return nullptr;

    PhiInst *Found = nullptr;
    for (Instruction *I : *Header) {
        auto *Phi = dynamic_cast<PhiInst *>(I);
        if (!Phi) break;

        auto PhiAff = getAffine(Phi);
        if (!PhiAff || PhiAff->RecLoop != L) continue;

        // An equal step is not enough: with two recurrences, or a comparison
        // of `i + c`, it can associate the exit with the wrong header PHI.
        // Consumers that rewrite CFG/iterations require the exact recurrence.
        if (!sameExpr(PhiAff->Start, Affine.Start) ||
            !sameExpr(PhiAff->Step, Affine.Step))
            continue;
        if (Found && Found != Phi) return nullptr;
        Found = Phi;
    }

    return Found;
}

std::optional<int64_t>
SCEVResult::getSmallConstantTripCount(const Loop *L) {
    auto Info = analyzeLoopTripCount(L);
    if (!Info || !Info->ProvenNoWrap) return std::nullopt;
    return Info->ConstantTripCount;
}

std::optional<int64_t>
SCEVResult::getSmallConstantBackedgeTakenCount(const Loop *L) {
    auto Info = analyzeLoopTripCount(L);
    if (!Info || !Info->ProvenNoWrap) return std::nullopt;
    return Info->ConstantBackedgeTakenCount;
}

std::optional<int64_t>
SCEVResult::getSmallConstantTripMultiple(const Loop *L) {
    auto Info = analyzeLoopTripCount(L);
    if (!Info || !Info->ProvenNoWrap) return std::nullopt;
    if (!Info->ConstantTripCount) return 1;

    int64_t C = *Info->ConstantTripCount;
    if (C == 0) return 1;
    if (C == std::numeric_limits<int64_t>::min()) return std::nullopt;
    return C < 0 ? -C : C;
}

bool SCEVResult::isTripCountMultipleOf(const Loop *L, int64_t Factor) {
    if (Factor <= 0) return false;
    auto Info = analyzeLoopTripCount(L);
    if (!Info || !Info->ProvenNoWrap || !Info->ConstantTripCount) return false;
    return (*Info->ConstantTripCount % Factor) == 0;
}

std::optional<LoopRemainderInfo>
SCEVResult::getRemainderInfo(const Loop *L, int64_t Factor) {
    if (!L || Factor <= 1) return std::nullopt;
    auto TCInfo = analyzeLoopTripCount(L);
    if (!TCInfo || !TCInfo->TripCount)
        return std::nullopt;
    // A constant mathematical count must never be consumed when the i32
    // recurrence can wrap.  Runtime consumers may add a contextual proof at
    // the exact expansion point, which SCEV itself cannot see.
    if (TCInfo->ConstantTripCount && !TCInfo->ProvenNoWrap)
        return std::nullopt;

    LoopRemainderInfo Info;
    Info.L = L;
    Info.Factor = Factor;
    Info.TripCount = TCInfo->TripCount;
    Info.ConstantTripCount = TCInfo->ConstantTripCount;

    if (Info.ConstantTripCount) {
        int64_t Rem = *Info.ConstantTripCount % Factor;
        int64_t Main = *Info.ConstantTripCount - Rem;
        Info.RemainderCount = makeConstant(Rem, Info.TripCount->Ty);
        Info.MainTripCount = makeConstant(Main, Info.TripCount->Ty);
        Info.ConstantRemainderCount = Rem;
        Info.ConstantMainTripCount = Main;
        Info.NeedRemainderLoop = Rem != 0;
        Info.IsRuntimeRemainder = false;
        Info.Valid = Info.RemainderCount && Info.MainTripCount;
        return Info.Valid ? std::optional<LoopRemainderInfo>(Info) : std::nullopt;
    }

    const SCEVExpr *FactorExpr = makeConstant(Factor, Info.TripCount->Ty);
    Info.RemainderCount = mod(Info.TripCount, FactorExpr);
    Info.MainTripCount = sub(Info.TripCount, Info.RemainderCount);
    if (!Info.RemainderCount || !Info.MainTripCount) return std::nullopt;

    Info.NeedRemainderLoop = true;
    Info.IsRuntimeRemainder = true;
    Info.Valid = true;
    return Info;
}

std::optional<InductionDescriptor>
SCEVResult::getCanonicalInduction(const Loop *L) {
    auto TCInfo = analyzeLoopTripCount(L);
    if (!TCInfo || !TCInfo->IndVar) return std::nullopt;

    PhiInst *Phi = TCInfo->IndVar;
    Value *StartValue = nullptr;
    Value *UpdateValue = nullptr;
    for (unsigned I = 0; I < Phi->getNumIncomingValues(); ++I) {
        Value *V = Phi->getIncomingValue(I);
        BasicBlock *Pred = Phi->getIncomingBlock(I);
        if (!V || !Pred) return std::nullopt;
        if (L->contains(Pred)) {
            if (UpdateValue && UpdateValue != V) return std::nullopt;
            UpdateValue = V;
        } else {
            if (StartValue && StartValue != V) return std::nullopt;
            StartValue = V;
        }
    }

    if (!StartValue || !UpdateValue) return std::nullopt;

    auto PhiAff = getAffine(Phi);
    if (!PhiAff || PhiAff->RecLoop != L) return std::nullopt;

    InductionDescriptor Desc;
    Desc.Phi = Phi;
    Desc.L = L;
    Desc.StartValue = StartValue;
    Desc.UpdateValue = UpdateValue;
    Desc.Start = PhiAff->Start;
    Desc.Step = PhiAff->Step;
    Desc.ConstantStart = PhiAff->ConstantStart;
    Desc.ConstantStep = PhiAff->ConstantStep;
    Desc.Valid = Desc.Start && Desc.Step;
    return Desc.Valid ? std::optional<InductionDescriptor>(Desc) : std::nullopt;
}

const SCEVExpr *SCEVResult::getSCEVAtIteration(const SCEVExpr *E,
                                               const SCEVExpr *Iter,
                                               const Loop *L) {
    if (!E) return nullptr;

    switch (E->Kind) {
    case SCEVExprKind::Constant:
    case SCEVExprKind::Unknown:
        return E;
    case SCEVExprKind::Add:
    case SCEVExprKind::Sub:
    case SCEVExprKind::Mul:
    case SCEVExprKind::Div:
    case SCEVExprKind::Mod:
    case SCEVExprKind::Min:
    case SCEVExprKind::Max: {
        if (E->Operands.size() != 2) return nullptr;
        const SCEVExpr *LHS = getSCEVAtIteration(E->Operands[0], Iter, L);
        const SCEVExpr *RHS = getSCEVAtIteration(E->Operands[1], Iter, L);
        if (!LHS || !RHS) return nullptr;
        switch (E->Kind) {
        case SCEVExprKind::Add: return add(LHS, RHS);
        case SCEVExprKind::Sub: return sub(LHS, RHS);
        case SCEVExprKind::Mul: return mul(LHS, RHS);
        case SCEVExprKind::Div: return div(LHS, RHS);
        case SCEVExprKind::Mod: return mod(LHS, RHS);
        case SCEVExprKind::Min:
        case SCEVExprKind::Max:
            return makeBinary(E->Kind, LHS, RHS, E->Ty);
        default:
            return nullptr;
        }
    }
    case SCEVExprKind::Cast:
        if (E->Operands.size() != 1) return nullptr;
        return makeCast(E->CastKind,
                        getSCEVAtIteration(E->Operands[0], Iter, L), E->Ty);
    case SCEVExprKind::Compare:
        if (E->Operands.size() != 2) return nullptr;
        return makeCompare(E->Predicate,
                           getSCEVAtIteration(E->Operands[0], Iter, L),
                           getSCEVAtIteration(E->Operands[1], Iter, L));
    case SCEVExprKind::AddRec:
        if (E->Operands.size() != 2) return nullptr;
        if (E->RecLoop != L) return E;
        if (!Iter) return nullptr;
        return add(getSCEVAtIteration(E->Operands[0], Iter, L),
                   mul(Iter, getSCEVAtIteration(E->Operands[1], Iter, L)));
    case SCEVExprKind::MulRec:
    case SCEVExprKind::DivRec:
        return E;
    }
    return nullptr;
}

void SCEVResult::forgetValue(Value *) {
    clear();
}

void SCEVResult::forgetLoop(const Loop *) {
    clear();
}

void SCEVResult::forgetAllLoops() {
    clear();
}

void SCEVResult::clear() {
    // IR rewrites invalidate every previously returned SCEVExpr pointer.
    // Keep the first version simple and flush the whole interned expression
    // arena instead of trying to maintain fine-grained dependencies.
    ExprStorage.clear();
    Interned.clear();

    ValueCache.clear();
    ActiveValues.clear();
    TripCountCache.clear();
    BackedgeCountCache.clear();
    LoopInductions.clear();
    LoopInductionsReady = false;
}

const std::unordered_map<Value *, LoopInductionInfo> &
SCEVResult::getLoopInductions() {
    if (LoopInductionsReady) return LoopInductions;
    LoopInductionsReady = true;
    LoopInductions.clear();

    if (!Func) return LoopInductions;
    for (BasicBlock *BB : *Func) {
        if (!BB) continue;
        for (Instruction *I : *BB) {
            auto *Phi = dynamic_cast<PhiInst *>(I);
            if (!Phi) break;
            auto Aff = getAffine(Phi);
            if (!Aff || !Aff->ConstantStart || !Aff->ConstantStep) continue;

            LoopInductionInfo Info;
            Info.RecLoop = Aff->RecLoop;
            Info.initial = *Aff->ConstantStart;
            Info.step = *Aff->ConstantStep;

            if (auto TCI = analyzeLoopTripCount(Aff->RecLoop)) {
                if (TCI->ProvenNoWrap && TCI->ConstantTripCount) {
                    const int64_t C = *TCI->ConstantTripCount;
                    if (C > 0) {
                        const __int128 Last = static_cast<__int128>(Info.initial) +
                            static_cast<__int128>(Info.step) *
                                static_cast<__int128>(C - 1);
                        if (Last < IntRange::I32_MIN ||
                            Last > IntRange::I32_MAX)
                            continue;
                        Info.hasUpperBound = true;
                        Info.noWrap = true;
                        Info.upperBound = static_cast<int64_t>(Last);
                    }
                }
            }
            LoopInductions[Phi] = Info;
        }
    }
    return LoopInductions;
}

std::string SCEVResult::format(const SCEVExpr *E) const {
    if (!E) return "<null>";
    switch (E->Kind) {
    case SCEVExprKind::Constant:
        return std::to_string(E->ConstValue);
    case SCEVExprKind::Unknown:
        return E->RawValue ? E->RawValue->getIdent() : "<unknown>";
    case SCEVExprKind::Add:
        return "(" + format(E->Operands[0]) + " + " + format(E->Operands[1]) + ")";
    case SCEVExprKind::Sub:
        return "(" + format(E->Operands[0]) + " - " + format(E->Operands[1]) + ")";
    case SCEVExprKind::Mul:
        return "(" + format(E->Operands[0]) + " * " + format(E->Operands[1]) + ")";
    case SCEVExprKind::Div:
        return "(" + format(E->Operands[0]) + " / " + format(E->Operands[1]) + ")";
    case SCEVExprKind::Mod:
        return "(" + format(E->Operands[0]) + " % " + format(E->Operands[1]) + ")";
    case SCEVExprKind::Min:
        return "min(" + format(E->Operands[0]) + ", " + format(E->Operands[1]) + ")";
    case SCEVExprKind::Max:
        return "max(" + format(E->Operands[0]) + ", " + format(E->Operands[1]) + ")";
    case SCEVExprKind::Cast:
        return std::string(SCEVExpr::castName(E->CastKind)) + "(" +
               format(E->Operands[0]) + ")";
    case SCEVExprKind::Compare:
        return "(" + format(E->Operands[0]) + " " +
               SCEVExpr::predicateName(E->Predicate) +
               " " + format(E->Operands[1]) + ")";
    case SCEVExprKind::AddRec:
        return "{" + format(E->Operands[0]) + ",+, " + format(E->Operands[1]) +
               "}<" + (E->RecLoop && E->RecLoop->getHeader()
                            ? E->RecLoop->getHeader()->getIdent()
                            : "?") +
               ">";
    case SCEVExprKind::MulRec:
        return "{" + format(E->Operands[0]) + ",*, " + format(E->Operands[1]) +
               "}<" + (E->RecLoop && E->RecLoop->getHeader()
                            ? E->RecLoop->getHeader()->getIdent()
                            : "?") +
               ">";
    case SCEVExprKind::DivRec:
        return "{" + format(E->Operands[0]) + ",/, " + format(E->Operands[1]) +
               "}<" + (E->RecLoop && E->RecLoop->getHeader()
                            ? E->RecLoop->getHeader()->getIdent()
                            : "?") +
               ">";
    }
    return "<bad-scev>";
}

SCEVAnalysis::Result
SCEVAnalysis::run(Function &F, FunctionAnalysisPassManager &FAM) const {
    auto &Loops = FAM.getResult<LoopAnalysis>(F);
    auto &Dom = FAM.getResult<DominantAnalysis>(F);
    return SCEVResult(&F, &Loops, &Dom);
}
