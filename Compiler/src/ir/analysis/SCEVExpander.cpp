#include "ir/analysis/SCEVExpander.hpp"
#include "lib/IRUtils.hpp"

#include <limits>

SCEVExpander::SCEVExpander(const Loop *Scope, BasicBlock *InsertBlock,
                           const DominatorTree &DT, unsigned Budget)
    : Scope(Scope), InsertBlock(InsertBlock), DT(DT), Budget(Budget) {}

const std::vector<Instruction *> &SCEVExpander::createdInstructions() const {
    return Created;
}

bool SCEVExpander::isAvailable(Value *V) const {
    if (!V || Value::isUndef(V) || !InsertBlock) return false;
    if (V->isConst()) return true;
    if (auto *Var = dynamic_cast<Variable *>(V))
        return Var->isParam() || Var->isGlobal();
    auto *I = dynamic_cast<Instruction *>(V);
    return I && I->getParent() && DT.dominates(I->getParent(), InsertBlock);
}

int SCEVExpander::estimateCostImpl(
    const SCEVExpr *E,
    std::unordered_set<const SCEVExpr *> &Active) const {
    if (!E || (Scope && E->mentionsLoop(Scope))) return -1;
    ScopedActiveMark ActiveMark(Active, E);
    if (!ActiveMark.inserted()) return -1;

    int Cost = 0;
    switch (E->Kind) {
    case SCEVExprKind::Constant:
        if (E->ConstValue < std::numeric_limits<int>::min() ||
            E->ConstValue > std::numeric_limits<int>::max())
            Cost = -1;
        break;
    case SCEVExprKind::Unknown:
        Cost = isAvailable(E->RawValue) ? 0 : -1;
        break;
    case SCEVExprKind::Cast: {
        if (!E->Ty || E->Operands.size() != 1) {
            Cost = -1;
            break;
        }
        int OpCost = estimateCostImpl(E->Operands[0], Active);
        Cost = OpCost < 0 ? -1 : OpCost + 1;
        break;
    }
    case SCEVExprKind::Add:
    case SCEVExprKind::Sub:
    case SCEVExprKind::Mul:
    case SCEVExprKind::Min:
    case SCEVExprKind::Max:
    case SCEVExprKind::Compare: {
        if (E->Operands.size() != 2) {
            Cost = -1;
            break;
        }
        int L = estimateCostImpl(E->Operands[0], Active);
        int R = estimateCostImpl(E->Operands[1], Active);
        if (L < 0 || R < 0) Cost = -1;
        else Cost = L + R +
            ((E->Kind == SCEVExprKind::Min ||
              E->Kind == SCEVExprKind::Max) ? 2 : 1);
        break;
    }
    case SCEVExprKind::Div:
    case SCEVExprKind::Mod:
        // The current SCEV node does not encode signedness or the precise
        // source division poison conditions.  Keep expansion disabled until
        // those semantics are represented explicitly.
        Cost = -1;
        break;
    case SCEVExprKind::AddRec:
    case SCEVExprKind::MulRec:
    case SCEVExprKind::DivRec:
        Cost = -1;
        break;
    }

    return Cost;
}

int SCEVExpander::estimateCost(const SCEVExpr *E) const {
    std::unordered_set<const SCEVExpr *> Active;
    return estimateCostImpl(E, Active);
}

bool SCEVExpander::canExpand(const SCEVExpr *E) const {
    int Cost = estimateCost(E);
    return InsertBlock && InsertBlock->hasTerminator() && Cost >= 0 &&
           Inserted <= Budget &&
           static_cast<unsigned>(Cost) <= Budget - Inserted;
}

Value *SCEVExpander::makeConstant(const SCEVExpr *E) const {
    if (!E || E->Kind != SCEVExprKind::Constant) {
        return nullptr;
    }
    auto IntegerType =
        E->Ty ? std::dynamic_pointer_cast<INTType>(E->Ty) : nullptr;
    if (IntegerType && IntegerType->getIsBool()) {
        return ConstBool::newConstBool(E->ConstValue != 0);
    }
    if (!IntegerType)
        IntegerType = INTType::getInt32Ty();
    if (IntegerType->getBitWidth() > 64)
        return nullptr;
    return ConstInt::newConstIntForType(E->ConstValue, IntegerType);
}

Instruction *SCEVExpander::insert(Instruction *I) {
    if (!I) return nullptr;
    if (!InsertBlock || Inserted >= Budget) {
        delete I;
        return nullptr;
    }
    if (!BasicBlock::insertBeforeTerminator(InsertBlock, I)) {
        delete I;
        return nullptr;
    }
    ++Inserted;
    Created.push_back(I);
    return I;
}

Value *SCEVExpander::expandImpl(const SCEVExpr *E) {
    auto It = Cache.find(E);
    if (It != Cache.end()) return It->second;

    Value *Result = nullptr;
    if (E->Kind == SCEVExprKind::Constant) {
        Result = makeConstant(E);
    } else if (E->Kind == SCEVExprKind::Unknown) {
        Result = E->RawValue;
    } else if (E->Kind == SCEVExprKind::Cast) {
        Value *Operand = expandImpl(E->Operands[0]);
        if (!Operand) return nullptr;
        Instruction *Cast = nullptr;
        auto IntegerType =
            std::dynamic_pointer_cast<INTType>(E->Ty);
        switch (E->CastKind) {
        case SCEVCastKind::Trunc:
            if (!IntegerType) return nullptr;
            Cast = new TruncInst(Operand, IntegerType);
            break;
        case SCEVCastKind::ZExt:
            if (!IntegerType) return nullptr;
            Cast = new ZextInst(Operand, IntegerType);
            break;
        case SCEVCastKind::SExt:
            if (!IntegerType) return nullptr;
            Cast = new SextInst(Operand, IntegerType);
            break;
        case SCEVCastKind::BitCast:
            Cast = new BitCastInst(Operand, E->Ty);
            break;
        }
        Result = insert(Cast);
    } else {
        Value *LHS = expandImpl(E->Operands[0]);
        Value *RHS = expandImpl(E->Operands[1]);
        if (!LHS || !RHS) return nullptr;

        if (E->Kind == SCEVExprKind::Min ||
            E->Kind == SCEVExprKind::Max) {
            auto Pred = E->Kind == SCEVExprKind::Min
                ? BinaryInst::L : BinaryInst::G;
            auto *Cmp = dynamic_cast<BinaryInst *>(
                insert(new BinaryInst(LHS, Pred, RHS)));
            if (!Cmp) return nullptr;
            Result = insert(new SelectInst(Cmp, LHS, RHS));
        } else {
            BinaryInst::Operation Op;
            switch (E->Kind) {
            case SCEVExprKind::Add: Op = BinaryInst::ADD; break;
            case SCEVExprKind::Sub: Op = BinaryInst::SUB; break;
            case SCEVExprKind::Mul: Op = BinaryInst::MUL; break;
            case SCEVExprKind::Compare: Op = E->Predicate; break;
            default: return nullptr;
            }
            Result = insert(new BinaryInst(LHS, Op, RHS));
        }
    }

    if (Result) Cache[E] = Result;
    return Result;
}

Value *SCEVExpander::expand(const SCEVExpr *E) {
    if (!canExpand(E)) return nullptr;
    const unsigned OldInserted = Inserted;
    const std::size_t OldCreatedSize = Created.size();
    auto OldCache = Cache;
    Value *Result = expandImpl(E);
    if (Result) return Result;

    // Preflight makes failure unusual, but keep the utility transactional if
    // the insertion block changes shape or a future instruction constructor
    // rejects an expression after some children were emitted.
    for (std::size_t I = Created.size(); I > OldCreatedSize; --I)
        delete Created[I - 1];
    Created.resize(OldCreatedSize);
    Inserted = OldInserted;
    Cache = std::move(OldCache);
    return nullptr;
}
