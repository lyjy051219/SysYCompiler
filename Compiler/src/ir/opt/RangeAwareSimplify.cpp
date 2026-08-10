#include "ir/opt/RangeAwareSimplify.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/opt/DCE.hpp"
#include "lib/CFG.hpp"
#include "lib/IRUtils.hpp"
#include <vector>
#include <cmath>
#include <limits>
#include <unordered_set>
//判断二元操作是否支持局部整数区间递归求值
bool RangeAwareSimplifyPass::isPureIntExprOp(BinaryInst::Operation op) {
    switch (op) {
        case BinaryInst::ADD:
        case BinaryInst::SUB:
        case BinaryInst::MUL:
        case BinaryInst::DIV:
        case BinaryInst::MOD:
        case BinaryInst::SHL:
        case BinaryInst::LSHR:
        case BinaryInst::ASHR:
        case BinaryInst::AND:
        case BinaryInst::OR:
            return true;
        default:
            return false;
    }
}
//在深度和环检测预算内递归求解同块整数表达式区间
IntRange RangeAwareSimplifyPass::queryIntExprImpl(
    Value *V, BasicBlock *BB, RangeInfo &RI, DominatorTree &DT,
    int depth, std::unordered_set<Value *> &visiting) {
    //1.1 先取 RangeAnalysis 的程序点结论，再在同一基本块内有界展开纯整数表达式；
    //递归环、跨块定义或预算耗尽时保留基础区间，不凭局部表达式猜测。
    if (auto *CI = dynamic_cast<ConstInt *>(V))
        return IntRange(CI->getVal());
    if (!V || !V->getIRType() || !V->getIRType()->isInt())
        return IntRange();
    IntRange base = RI.query(V, BB, DT);
    if (depth <= 0 || visiting.count(V))
        return base;
    auto *BI = dynamic_cast<BinaryInst *>(V);
    if (!BI || !isPureIntExprOp(BI->getOp()))
        return base;
    BasicBlock *DefBB = BI->getParent();
    if (DefBB != BB)
        return base;
    Value *lhs = nullptr;
    Value *rhs = nullptr;
    if (!BI->getOperands(lhs, rhs)) return base;
    if (!lhs || !rhs) return base;
    if (!lhs->getIRType() || !lhs->getIRType()->isInt()) return base;
    if (!rhs->getIRType() || !rhs->getIRType()->isInt()) return base;
    ScopedActiveMark VisitingMark(visiting, V);
    IntRange l = queryIntExprImpl(lhs, BB, RI, DT, depth - 1, visiting);
    IntRange r = queryIntExprImpl(rhs, BB, RI, DT, depth - 1, visiting);
    IntRange expr;
    switch (BI->getOp()) {
        case BinaryInst::ADD:  expr = l.add(r);  break;
        case BinaryInst::SUB:  expr = l.sub(r);  break;
        case BinaryInst::MUL:  expr = l.mul(r);  break;
        case BinaryInst::DIV:  expr = l.sdiv(r); break;
        case BinaryInst::MOD:  expr = l.srem(r); break;
        case BinaryInst::SHL:  expr = l.shl(r);  break;
        case BinaryInst::LSHR: expr = l.lshr(r); break;
        case BinaryInst::ASHR: expr = l.ashr(r); break;
        case BinaryInst::AND:  expr = l.band(r); break;
        case BinaryInst::OR:   expr = l.bor(r);  break;
        default: return base;
    }
    //将局部传递得到的区间与全局分析结论取交，既利用表达式结构又不丢支配路径约束。
    return base.intersectWith(expr);
}
//查询并补强给定程序点的整数表达式区间
IntRange RangeAwareSimplifyPass::queryIntExpr(
    Value *V, BasicBlock *BB, RangeInfo &RI, DominatorTree &DT) {
    std::unordered_set<Value *> visiting;
    return queryIntExprImpl(V, BB, RI, DT, 3, visiting);
}
//根据区间和已知位判断整数值是否必定非负
bool RangeAwareSimplifyPass::isKnownNonNegative(
    Value *V, BasicBlock *BB, RangeInfo &RI, DominatorTree &DT) {
    //1.2 先查表达式区间，再用已知位的符号位及其反推区间补强非负证明。
    if (!V || !BB) return false;
    IntRange R = queryIntExpr(V, BB, RI, DT);
    if (!R.isBottom() && R.isNonNegative()) return true;
    KnownBits KB = RI.queryBits(V, BB, DT);
    if (KB.knownNonNegative()) return true;
    IntRange FromBits = KB.toIntRange();
    return !FromBits.isBottom() && FromBits.isNonNegative();
}
//判断整数条件在给定程序点是否恒假、恒真或未知
int RangeAwareSimplifyPass::evalTruth(
    Value *V, BasicBlock *BB, RangeInfo &RI, DominatorTree &DT) {
    bool ConstantTruth = false;
    if (V && V->tryGetBoolConst(ConstantTruth))
        return ConstantTruth ? 1 : 0;
    if (!V || !V->getIRType() || !V->getIRType()->isInt())
        return -1;
    IntRange R = queryIntExpr(V, BB, RI, DT);
    if (!R.isBottom() && !R.isTop()) {
        if (R.max == 0) return 0;
        if (R.min > 0 || R.max < 0) return 1;
    }
    KnownBits KB = RI.queryBits(V, BB, DT);
    if (KB.isFullyKnown())
        return KB.getConstant() != 0 ? 1 : 0;
    return -1;
}
//根据两个整数区间判断比较恒真、恒假或未知
int RangeAwareSimplifyPass::evalIntCmp(
    BinaryInst::Operation op, const IntRange &l, const IntRange &r) {
    if (l.isTop() || r.isTop() || l.isBottom() || r.isBottom()) return -1;
    switch (op) {
        case BinaryInst::L:
            if (l.max <  r.min) return 1;
            if (l.min >= r.max) return 0;
            break;
        case BinaryInst::LE:
            if (l.max <= r.min) return 1;
            if (l.min >  r.max) return 0;
            break;
        case BinaryInst::G:
            if (l.min >  r.max) return 1;
            if (l.max <= r.min) return 0;
            break;
        case BinaryInst::GE:
            if (l.min >= r.max) return 1;
            if (l.max <  r.min) return 0;
            break;
        case BinaryInst::E:
            if (l.max < r.min || l.min > r.max) return 0;
            if (l.isSingleValue() && r.isSingleValue() && l.min == r.min) return 1;
            break;
        case BinaryInst::NE:
            if (l.max < r.min || l.min > r.max) return 1;
            if (l.isSingleValue() && r.isSingleValue() && l.min == r.min) return 0;
            break;
        default: break;
    }
    return -1;
}
//判断二元操作是否为支持的整数比较谓词
bool RangeAwareSimplifyPass::isIntCmpOp(BinaryInst::Operation op) {
    return op == BinaryInst::L || op == BinaryInst::LE ||
           op == BinaryInst::G || op == BinaryInst::GE ||
           op == BinaryInst::E || op == BinaryInst::NE;
}
//折叠被区间或已知位证明为单值的纯计算
bool RangeAwareSimplifyPass::foldConstants(
    Function &F, RangeInfo &RI, DominatorTree &DT) {
    //2. 首轮只收集能被区间或已知位锁定为单值的纯计算指令；调用、内存操作和
    //尚在数据流收敛中的 PHI 不因单次范围结果而删除。
    bool changed = false;
    std::vector<Instruction *> intToFold;
    std::vector<Instruction *> floatToFold;
    for (Instruction *I : collectInstructions(F)) {
        if (!I->getIRType()) continue;
        if (dynamic_cast<CallInst *>(I)) continue;
        if (dynamic_cast<LoadInst *>(I)) continue;
        if (dynamic_cast<StoreInst *>(I)) continue;
        if (dynamic_cast<PhiInst *>(I)) continue;
        BasicBlock *block = I->getParent();
        if (!block) continue;
        auto Ty = I->getIRType();
        if (Ty->isInt()) {
            if (dynamic_cast<ConstInt *>(I)) continue;
            IntRange range = RI.query(I, block, DT);
            bool singleInt = range.isSingleValue();
            if (!singleInt) {
                KnownBits kb = RI.queryBits(I, block, DT);
                if (kb.isFullyKnown()) {
                    intToFold.push_back(I);
                    continue;
                }
            }
            if (singleInt) {
                intToFold.push_back(I);
            }
        } else if (Ty->isFloat()) {
            if (dynamic_cast<ConstFloat *>(I)) continue;
            FloatRange range = RI.queryFloat(I, block, DT);
            if (range.isSingleValue()) {
                floatToFold.push_back(I);
            }
        }
    }
    //2.1 第二轮重新查询候选并生成与原类型一致的整数/布尔常量，防止收集后状态陈旧。
    for (Instruction *I : intToFold) {
        BasicBlock *block = I->getParent();
        if (!block) continue;
        IntRange range = RI.query(I, block, DT);
        int64_t cval;
        if (range.isSingleValue()) {
            cval = range.min;
        } else {
            KnownBits kb = RI.queryBits(I, block, DT);
            if (!kb.isFullyKnown()) continue;
            cval = kb.getConstant();
        }
        auto Ty = I->getIRType();
        if (!Ty || !Ty->isInt()) continue;
        Value *Folded = Ty->isBool()
            ? static_cast<Value *>(ConstBool::newConstBool((cval & 1) != 0))
            : static_cast<Value *>(ConstInt::newConstIntForType(
                  cval, std::dynamic_pointer_cast<INTType>(Ty)));
        I->replaceAllUsesWith(Folded);
        delete I;
        changed = true;
    }
    //2.2 浮点单值折叠保留正零/负零信息，避免数值相等却改变可观察符号位。
    for (Instruction *I : floatToFold) {
        BasicBlock *block = I->getParent();
        if (!block) continue;
        FloatRange range = RI.queryFloat(I, block, DT);
        if (!range.isSingleValue()) continue;
        float Folded = static_cast<float>(range.min);
        if (range.min == 0.0 && range.mayNegativeZero &&
            !range.mayPositiveZero)
            Folded = -0.0f;
        I->replaceAllUsesWith(ConstFloat::newConstFloat(Folded));
        delete I;
        changed = true;
    }
    return changed;
}
//折叠被整数或浮点范围证明恒真的比较
bool RangeAwareSimplifyPass::foldComparisons(
    Function &F, RangeInfo &RI, DominatorTree &DT) {
    //3. 快照六类比较，根据操作数类型分别使用整数或浮点范围证明恒真/恒假。
    bool changed = false;
    std::vector<BinaryInst *> cmps;
    for (BinaryInst *BI : collectInstructions<BinaryInst>(F)) {
        auto op = BI->getOp();
        if (op == BinaryInst::L || op == BinaryInst::LE ||
            op == BinaryInst::G || op == BinaryInst::GE ||
            op == BinaryInst::E || op == BinaryInst::NE)
            cmps.push_back(BI);
    }
    for (BinaryInst *cmp : cmps) {
        Value *lhs = nullptr;
        Value *rhs = nullptr;
        if (!cmp->getOperands(lhs, rhs)) continue;
        if (!lhs->getIRType() || !rhs->getIRType()) continue;
        BasicBlock *BB = cmp->getParent();
        if (!BB) continue;
        auto OpTy = lhs->getIRType();
        //3.1 整数等值比较先用已知位冲突证明必不相等，再用不相交/单值区间
        //处理全部谓词；Top 和 Bottom 都不是可用于折叠的事实。
        if (OpTy->isInt() && rhs->getIRType()->isInt()) {
            IntRange lRange = queryIntExpr(lhs, BB, RI, DT);
            IntRange rRange = queryIntExpr(rhs, BB, RI, DT);
            if (lRange.isBottom() || rRange.isBottom()) continue;
            if (cmp->getOp() == BinaryInst::E || cmp->getOp() == BinaryInst::NE) {
                KnownBits lKB = RI.queryBits(lhs, BB, DT);
                KnownBits rKB = RI.queryBits(rhs, BB, DT);
                uint32_t conflict = (lKB.zero & rKB.one) | (lKB.one & rKB.zero);
                if (conflict != 0) {
                    bool isEq = (cmp->getOp() == BinaryInst::E);
                    cmp->replaceAllUsesWith(ConstBool::newConstBool(isEq ? false : true));
                    delete cmp;
                    changed = true;
                    continue;
                }
            }
            if (lRange.isTop() || rRange.isTop())
                continue;
            int result = -1;
            switch (cmp->getOp()) {
                case BinaryInst::L:
                    if (lRange.max < rRange.min) result = 1;
                    else if (lRange.min >= rRange.max) result = 0;
                    break;
                case BinaryInst::LE:
                    if (lRange.max <= rRange.min) result = 1;
                    else if (lRange.min > rRange.max) result = 0;
                    break;
                case BinaryInst::G:
                    if (lRange.min > rRange.max) result = 1;
                    else if (lRange.max <= rRange.min) result = 0;
                    break;
                case BinaryInst::GE:
                    if (lRange.min >= rRange.max) result = 1;
                    else if (lRange.max < rRange.min) result = 0;
                    break;
                case BinaryInst::E:
                    if (lRange.max < rRange.min || lRange.min > rRange.max) result = 0;
                    else if (lRange.isSingleValue() && rRange.isSingleValue() &&
                             lRange.min == rRange.min) result = 1;
                    break;
                case BinaryInst::NE:
                    if (lRange.max < rRange.min || lRange.min > rRange.max) result = 1;
                    else if (lRange.isSingleValue() && rRange.isSingleValue() &&
                             lRange.min == rRange.min) result = 0;
                    break;
                default: break;
            }
            if (result >= 0) {
                cmp->replaceAllUsesWith(ConstBool::newConstBool(result == 1));
                delete cmp;
                changed = true;
            }
            continue;
        }
        //3.2 浮点端点推理仅在两侧都有数值且明确排除 NaN 时成立，否则 ordered
        //比较和 != 的 NaN 语义会使整数式端点规则失效。
        if (OpTy->isFloat() && rhs->getIRType()->isFloat()) {
            FloatRange lRange = RI.queryFloat(lhs, BB, DT);
            FloatRange rRange = RI.queryFloat(rhs, BB, DT);
            if (!lRange.hasNumericValues() || !rRange.hasNumericValues() ||
                lRange.mayContainNaN() || rRange.mayContainNaN())
                continue;
            int result = -1;
            switch (cmp->getOp()) {
                case BinaryInst::L:
                    if (lRange.max < rRange.min) result = 1;
                    else if (lRange.min >= rRange.max) result = 0;
                    break;
                case BinaryInst::LE:
                    if (lRange.max <= rRange.min) result = 1;
                    else if (lRange.min > rRange.max) result = 0;
                    break;
                case BinaryInst::G:
                    if (lRange.min > rRange.max) result = 1;
                    else if (lRange.max <= rRange.min) result = 0;
                    break;
                case BinaryInst::GE:
                    if (lRange.min >= rRange.max) result = 1;
                    else if (lRange.max < rRange.min) result = 0;
                    break;
                case BinaryInst::E:
                    if (lRange.max < rRange.min || lRange.min > rRange.max) result = 0;
                    else if (lRange.isSingleValue() && rRange.isSingleValue() &&
                             lRange.min == rRange.min) result = 1;
                    break;
                case BinaryInst::NE:
                    if (lRange.max < rRange.min || lRange.min > rRange.max) result = 1;
                    else if (lRange.isSingleValue() && rRange.isSingleValue() &&
                             lRange.min == rRange.min) result = 0;
                    break;
                default: break;
            }
            if (result >= 0) {
                cmp->replaceAllUsesWith(ConstBool::newConstBool(result == 1));
                delete cmp;
                changed = true;
            }
        }
    }
    return changed;
}
//折叠比较select结果时两臂均可证明的比较结论
bool RangeAwareSimplifyPass::foldIcmpOfSelect(
    Function &F, RangeInfo &RI, DominatorTree &DT) {
    //4. 对 icmp(select(c,t,f), rhs) 分别求两臂比较结果；两臂相同则折为常量，
    //true/false 恰与 select 臂一致时折为 c，反向情形不额外制造取反指令。
    bool changed = false;
    std::vector<BinaryInst *> cmps;
    for (BinaryInst *BI : collectInstructions<BinaryInst>(F))
        if (isIntCmpOp(BI->getOp())) cmps.push_back(BI);
    for (BinaryInst *cmp : cmps) {
        Value *lhs = nullptr;
        Value *rhs = nullptr;
        if (!cmp->getOperands(lhs, rhs)) continue;
        if (!lhs->getIRType() || !rhs->getIRType()) continue;
        if (!lhs->getIRType()->isInt()) continue;
        if (!rhs->getIRType()->isInt()) continue;
        BasicBlock *BB = cmp->getParent();
        if (!BB) continue;
        auto *sel = dynamic_cast<SelectInst *>(lhs);
        if (!sel) continue;
        Value *tv = sel->getTrueValue();
        Value *fv = sel->getFalseValue();
        if (!tv->getIRType() || !tv->getIRType()->isInt()) continue;
        if (!fv->getIRType() || !fv->getIRType()->isInt()) continue;
        IntRange tr = dynamic_cast<ConstInt *>(tv) ?
            IntRange(dynamic_cast<ConstInt *>(tv)->getVal()) : queryIntExpr(tv, BB, RI, DT);
        IntRange fr = dynamic_cast<ConstInt *>(fv) ?
            IntRange(dynamic_cast<ConstInt *>(fv)->getVal()) : queryIntExpr(fv, BB, RI, DT);
        IntRange rr = dynamic_cast<ConstInt *>(rhs) ?
            IntRange(dynamic_cast<ConstInt *>(rhs)->getVal()) : queryIntExpr(rhs, BB, RI, DT);
        int resT = evalIntCmp(cmp->getOp(), tr, rr);
        int resF = evalIntCmp(cmp->getOp(), fr, rr);
        if (resT < 0 || resF < 0) continue;
        if (resT == resF) {
            cmp->replaceAllUsesWith(ConstBool::newConstBool(resT == 1));
            delete cmp;
            changed = true;
        } else if (resT == 1 && resF == 0) {
            Value *cond = sel->getCondition();
            cmp->replaceAllUsesWith(cond);
            delete cmp;
            changed = true;
        }
    }
    return changed;
}
//根据条件范围直接选择select的确定分支
bool RangeAwareSimplifyPass::foldSelectByConditionRange(
    Function &F, RangeInfo &RI, DominatorTree &DT) {
    //5. 用区间或已知位判定非直接比较条件的真假，选中对应 select 分支。
    bool changed = false;
    auto candidates = collectInstructions<SelectInst>(F);
    for (SelectInst *SI : candidates) {
        Value *cond = SI->getCondition();
        Value *tv = SI->getTrueValue();
        Value *fv = SI->getFalseValue();
        if (!cond || !tv || !fv) continue;
        BasicBlock *BB = SI->getParent();
        if (!BB) continue;
        int truth = evalTruth(cond, BB, RI, DT);
        if (truth < 0) continue;
        SI->replaceAllUsesWith(truth ? tv : fv);
        delete SI;
        changed = true;
    }
    return changed;
}
//根据操作数符号范围折叠select构成的绝对值模式
bool RangeAwareSimplifyPass::foldAbsViaSelect(
    Function &F, RangeInfo &RI, DominatorTree &DT) {
    //6.1 识别以 x 与 0 比较并在 x/-x 间选择的 abs 形态；只有区间证明 x
    //全非负或全非正时，才分别选择 x 或已有的负值表达式。
    bool changed = false;
    std::vector<SelectInst *> candidates;
    for (SelectInst *SI : collectInstructions<SelectInst>(F))
        if (SI->getIRType() && SI->getIRType()->isInt())
            candidates.push_back(SI);
    auto matchNeg = [](Value *v) -> Value * {
        auto *BI = dynamic_cast<BinaryInst *>(v);
        if (!BI || BI->getOp() != BinaryInst::SUB) return nullptr;
        Value *LHS = nullptr;
        Value *RHS = nullptr;
        if (!BI->getOperands(LHS, RHS)) return nullptr;
        auto *c = dynamic_cast<ConstInt *>(LHS);
        if (!c || c->getVal() != 0) return nullptr;
        return RHS;
    };
    for (SelectInst *SI : candidates) {
        auto *cmp = dynamic_cast<BinaryInst *>(SI->getCondition());
        if (!cmp || !isIntCmpOp(cmp->getOp())) continue;
        Value *cmpLhs = nullptr;
        Value *cmpRhsValue = nullptr;
        if (!cmp->getOperands(cmpLhs, cmpRhsValue)) continue;
        auto *cmpRhs = dynamic_cast<ConstInt *>(cmpRhsValue);
        if (!cmpRhs || cmpRhs->getVal() != 0) continue;
        Value *tv = SI->getTrueValue();
        Value *fv = SI->getFalseValue();
        Value *x = nullptr;
        Value *neg = nullptr;
        bool isAbs = false;
        auto op = cmp->getOp();
        if (op == BinaryInst::L || op == BinaryInst::LE) {
            if (matchNeg(tv) == cmpLhs && fv == cmpLhs) {
                x = cmpLhs; neg = tv; isAbs = true;
            }
        } else if (op == BinaryInst::GE || op == BinaryInst::G) {
            if (tv == cmpLhs && matchNeg(fv) == cmpLhs) {
                x = cmpLhs; neg = fv; isAbs = true;
            }
        }
        if (!isAbs) continue;
        BasicBlock *BB = SI->getParent();
        if (!BB) continue;
        IntRange xr = queryIntExpr(x, BB, RI, DT);
        if (xr.isTop() || xr.isBottom()) continue;
        Value *chosen = nullptr;
        if (xr.isNonNegative())
            chosen = x;
        else if (xr.max <= 0)
            chosen = neg;
        if (!chosen) continue;
        SI->replaceAllUsesWith(chosen);
        delete SI;
        changed = true;
    }
    return changed;
}
//根据两侧区间顺序折叠select构成的最小值或最大值模式
bool RangeAwareSimplifyPass::foldMinMaxViaSelect(
    Function &F, RangeInfo &RI, DominatorTree &DT) {
    //6.2 识别比较 a、b 后在两者间选择的 min/max 形态，严格区分 < 与 <=
    //的端点条件；范围完全决定比较结果后直接选臂。
    bool changed = false;
    std::vector<SelectInst *> candidates;
    for (SelectInst *SI : collectInstructions<SelectInst>(F))
        if (SI->getIRType() && SI->getIRType()->isInt())
            candidates.push_back(SI);
    for (SelectInst *SI : candidates) {
        auto *cmp = dynamic_cast<BinaryInst *>(SI->getCondition());
        if (!cmp || !isIntCmpOp(cmp->getOp())) continue;
        Value *a = nullptr;
        Value *b = nullptr;
        if (!cmp->getOperands(a, b)) continue;
        Value *tv = SI->getTrueValue();
        Value *fv = SI->getFalseValue();
        bool straight = (tv == a && fv == b);
        bool flipped  = (tv == b && fv == a);
        if (!straight && !flipped) continue;
        auto op = cmp->getOp();
        if (op != BinaryInst::L && op != BinaryInst::LE &&
            op != BinaryInst::G && op != BinaryInst::GE) continue;
        BasicBlock *BB = SI->getParent();
        if (!BB) continue;
        IntRange ra = dynamic_cast<ConstInt *>(a) ?
            IntRange(dynamic_cast<ConstInt *>(a)->getVal()) : queryIntExpr(a, BB, RI, DT);
        IntRange rb = dynamic_cast<ConstInt *>(b) ?
            IntRange(dynamic_cast<ConstInt *>(b)->getVal()) : queryIntExpr(b, BB, RI, DT);
        if (ra.isTop() || rb.isTop() || ra.isBottom() || rb.isBottom())
            continue;
        int cmpResult = -1;
        switch (op) {
            case BinaryInst::G:
                if (ra.min >  rb.max) cmpResult = 1;
                else if (ra.max <= rb.min) cmpResult = 0;
                break;
            case BinaryInst::GE:
                if (ra.min >= rb.max) cmpResult = 1;
                else if (ra.max <  rb.min) cmpResult = 0;
                break;
            case BinaryInst::L:
                if (ra.max <  rb.min) cmpResult = 1;
                else if (ra.min >= rb.max) cmpResult = 0;
                break;
            case BinaryInst::LE:
                if (ra.max <= rb.min) cmpResult = 1;
                else if (ra.min >  rb.max) cmpResult = 0;
                break;
            default: break;
        }
        if (cmpResult < 0) continue;
        Value *chosen = (cmpResult == 1) ? tv : fv;
        SI->replaceAllUsesWith(chosen);
        delete SI;
        changed = true;
    }
    return changed;
}
//把已证非负的二次幂有符号除余改写为移位或掩码
bool RangeAwareSimplifyPass::strengthReduce(
    Function &F, RangeInfo &RI, DominatorTree &DT) {
    //7. 收集整数除法/取模，要求除数为 2 的幂且被除数在当前程序点已证明非负。
    bool changed = false;
    std::vector<BinaryInst *> candidates;
    for (BinaryInst *BI : collectInstructions<BinaryInst>(F)) {
        auto op = BI->getOp();
        if (op == BinaryInst::DIV || op == BinaryInst::MOD)
            candidates.push_back(BI);
    }
    for (BinaryInst *BI : candidates) {
        Value *lhs = nullptr;
        Value *rhs = nullptr;
        if (!BI->getOperands(lhs, rhs)) continue;
        if (!lhs->getIRType() || !lhs->getIRType()->isInt()) continue;
        if (!rhs->getIRType() || !rhs->getIRType()->isInt()) continue;
        auto *rhsConst = dynamic_cast<ConstInt *>(rhs);
        if (!rhsConst || !IRMath::isPowerOf2(rhsConst->getVal())) continue;
        int shift = IRMath::log2i(rhsConst->getVal());
        BasicBlock *BB = BI->getParent();
        if (!BB) continue;
        if (!isKnownNonNegative(lhs, BB, RI, DT)) continue;
        //7.1 非负 sdiv 可改为算术右移，非负 srem 可改为掩码；先插入替代指令，
        //再替换 use 并删除原指令，保持遍历快照有效。
        if (BI->getOp() == BinaryInst::DIV) {
            Value *shiftVal = ConstInt::newConstInt(shift);
            auto *newInst = new BinaryInst(lhs, BinaryInst::ASHR, shiftVal);
            BasicBlock::Iterator it(BI);
            it.insertBefore(newInst);
            BI->replaceAllUsesWith(newInst);
            delete BI;
            changed = true;
        } else if (BI->getOp() == BinaryInst::MOD) {
            int maskVal = rhsConst->getVal() - 1;
            Value *mask = ConstInt::newConstInt(maskVal);
            auto *newInst = new BinaryInst(lhs, BinaryInst::AND, mask);
            BasicBlock::Iterator it(BI);
            it.insertBefore(newInst);
            BI->replaceAllUsesWith(newInst);
            delete BI;
            changed = true;
        }
    }
    return changed;
}
//把商区间受限的有符号余数改写为减法或单次条件校正
bool RangeAwareSimplifyPass::foldBoundedSignedRemainders(
    Function &F, RangeInfo &RI, DominatorTree &DT) {
    //8.只处理i32正的常量模数，商区间由程序点范围证明；不匹配特定模数、函数名
    //或指令出现顺序。二次幂机会已由前一阶段优先改写为更便宜的按位与。
    bool changed = false;
    std::vector<BinaryInst *> srems;
    for (BinaryInst *BI : collectInstructions<BinaryInst>(F))
        if (BI->getOp() == BinaryInst::MOD) srems.push_back(BI);
    for (BinaryInst *srem : srems) {
        Value *lhs = nullptr;
        Value *rhs = nullptr;
        if (!srem->getOperands(lhs, rhs)) continue;
        BasicBlock *BB = srem->getParent();
        auto *ModulusValue = dynamic_cast<ConstInt *>(rhs);
        auto IntegerType = lhs && lhs->getIRType()
            ? std::dynamic_pointer_cast<INTType>(lhs->getIRType())
            : nullptr;
        if (!BB || !ModulusValue || !IntegerType ||
            IntegerType->getBitWidth() != 32) {
            continue;
        }
        const int64_t Modulus = ModulusValue->getVal();
        if (Modulus <= 1 || Modulus > IntRange::I32_MAX) continue;
        IntRange LRange = queryIntExpr(lhs, BB, RI, DT);
        if (LRange.isBottom() || LRange.isTop() ||
            LRange.min < IntRange::I32_MIN ||
            LRange.max > IntRange::I32_MAX) {
            continue;
        }
        //8.1 C/SysY有符号除法向零截断；正常量下端点商保持单调，可直接界定商集合。
        const int64_t MinQuotient = LRange.min / Modulus;
        const int64_t MaxQuotient = LRange.max / Modulus;
        //非单值商的改写会多次使用被除数；若它可能依赖undef，各次使用不保证
        //选择同一个值，比较与校正的相关性便不成立。预算不足同样保守拒绝。
        if (MinQuotient != MaxQuotient &&
            valueMayDependOnUndef(lhs)) {
            continue;
        }
        Value *Replacement = nullptr;
        std::vector<Instruction *> NewInstructions;
        auto InsertAllBeforeRemainder = [&]() {
            for (Instruction *I : NewInstructions)
                Instruction::insertBefore(srem, I);
        };
        if (MinQuotient == MaxQuotient) {
            //8.2 商恒定时x%M精确等于x-q*M；宽整数先证明整个结果区间不溢出i32。
            const __int128 Offset = static_cast<__int128>(MinQuotient) *
                                    static_cast<__int128>(Modulus);
            const __int128 ResultMin = static_cast<__int128>(LRange.min) - Offset;
            const __int128 ResultMax = static_cast<__int128>(LRange.max) - Offset;
            if (ResultMin < IntRange::I32_MIN ||
                ResultMax > IntRange::I32_MAX ||
                Offset < std::numeric_limits<int64_t>::min() ||
                Offset > std::numeric_limits<int64_t>::max()) {
                continue;
            }
            if (Offset == 0) {
                Replacement = lhs;
            } else {
                Value *OffsetValue = ConstInt::newConstIntForType(
                    static_cast<int64_t>(Offset), IntegerType);
                auto *Subtract = new BinaryInst(
                    lhs, BinaryInst::SUB, OffsetValue);
                NewInstructions.push_back(Subtract);
                Replacement = Subtract;
            }
        } else if (MinQuotient == 0 && MaxQuotient == 1) {
            //8.3 商只可能为0或1时执行一次正向校正；两臂都会求值，故额外证明减法安全。
            const __int128 AdjustedMin =
                static_cast<__int128>(LRange.min) - Modulus;
            const __int128 AdjustedMax =
                static_cast<__int128>(LRange.max) - Modulus;
            if (AdjustedMin < IntRange::I32_MIN ||
                AdjustedMax > IntRange::I32_MAX) {
                continue;
            }
            Value *M = ConstInt::newConstIntForType(Modulus, IntegerType);
            auto *NeedsAdjust = new BinaryInst(lhs, BinaryInst::GE, M);
            auto *Adjusted = new BinaryInst(lhs, BinaryInst::SUB, M);
            auto *Select = new SelectInst(NeedsAdjust, Adjusted, lhs);
            NewInstructions.push_back(NeedsAdjust);
            NewInstructions.push_back(Adjusted);
            NewInstructions.push_back(Select);
            Replacement = Select;
        } else if (MinQuotient == -1 && MaxQuotient == 0) {
            //8.4 商只可能为-1或0时执行一次负向校正，保留余数与被除数同号的语义。
            const __int128 AdjustedMin =
                static_cast<__int128>(LRange.min) + Modulus;
            const __int128 AdjustedMax =
                static_cast<__int128>(LRange.max) + Modulus;
            if (AdjustedMin < IntRange::I32_MIN ||
                AdjustedMax > IntRange::I32_MAX) {
                continue;
            }
            Value *M = ConstInt::newConstIntForType(Modulus, IntegerType);
            Value *NegativeM = ConstInt::newConstIntForType(
                -Modulus, IntegerType);
            auto *NeedsAdjust = new BinaryInst(
                lhs, BinaryInst::LE, NegativeM);
            auto *Adjusted = new BinaryInst(lhs, BinaryInst::ADD, M);
            auto *Select = new SelectInst(NeedsAdjust, Adjusted, lhs);
            NewInstructions.push_back(NeedsAdjust);
            NewInstructions.push_back(Adjusted);
            NewInstructions.push_back(Select);
            Replacement = Select;
        }
        if (!Replacement) continue;
        InsertAllBeforeRemainder();
        srem->replaceAllUsesWith(Replacement);
        delete srem;
        changed = true;
    }
    return changed;
}
//运行范围感知化简并返回分析保留状态
PreservationStatus RangeAwareSimplifyPass::run(Function &F,
                                                FunctionAnalysisPassManager &FAM) {
    //1. 获取同一轮 RangeAnalysis 与支配树，所有折叠均基于原 IR 的程序点事实。
    auto &RI = FAM.getResult<RangeAnalysis>(F);
    auto &DT = FAM.getResult<DominantAnalysis>(F);
    bool changed = false;
    //2—8. 依次消费单值、比较、select 模式与非负证明；后一步可以继续利用前一步
    //暴露的简单值，但不会重新计算范围，因此每个局部改写仍独立完成合法性检查。
    changed |= foldConstants(F, RI, DT);
    changed |= foldComparisons(F, RI, DT);
    changed |= foldIcmpOfSelect(F, RI, DT);
    changed |= foldSelectByConditionRange(F, RI, DT);
    changed |= foldAbsViaSelect(F, RI, DT);
    changed |= foldMinMaxViaSelect(F, RI, DT);
    changed |= strengthReduce(F, RI, DT);
    changed |= foldBoundedSignedRemainders(F, RI, DT);
    if (!changed) return PreservationStatus::all();
    //9. 上述改写均不改变CFG，保留支配树与循环结构；Range结果因值图变化失效。
    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<LoopAnalysis>();
    return PA;
}
