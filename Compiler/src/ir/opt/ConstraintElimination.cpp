#include "ir/opt/ConstraintElimination.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "lib/IRUtils.hpp"
#include <vector>
#include <stack>
#include <unordered_map>
//利用程序点范围证明i32加减指令在全部可能输入下均不回绕。
bool ConstraintEliminationPass::isKnownNoWrapAddSub(
    BinaryInst *inst, BasicBlock *contextBlock, RangeInfo &RI,
    const DominatorTree &DT) {
    //1.1 用当前程序点的操作数区间证明 add/sub 的最小、最大结果均落在 i32；
    //只有无回绕时，后续才能把常量偏移跨过比较符号。
    if (!inst || !contextBlock) return false;
    if (inst->getOp() != BinaryInst::ADD &&
        inst->getOp() != BinaryInst::SUB)
        return false;
    Value *lhs = nullptr;
    Value *rhs = nullptr;
    if (!inst->getOperands(lhs, rhs) || !lhs || !rhs) return false;
    if (!IRType::hasIntegerBitWidth(lhs->getIRType(), 32) ||
        !IRType::hasIntegerBitWidth(rhs->getIRType(), 32))
        return false;
    IntRange lhsRange = RI.query(lhs, contextBlock, DT);
    IntRange rhsRange = RI.query(rhs, contextBlock, DT);
    if (lhsRange.isBottom() || rhsRange.isBottom()) return false;
    int64_t resultMin = 0;
    int64_t resultMax = 0;
    if (inst->getOp() == BinaryInst::ADD) {
        return IRMath::addI32(lhsRange.min, rhsRange.min, resultMin) &&
               IRMath::addI32(lhsRange.max, rhsRange.max, resultMax);
    }
    return IRMath::addI32(lhsRange.min, -rhsRange.max, resultMin) &&
           IRMath::addI32(lhsRange.max, -rhsRange.min, resultMax);
}
//有界递归地把无回绕常量加减表达式分解为Base+Offset。
ConstraintElimOffsetExpr ConstraintEliminationPass::decomposeOffset(
    Value *V, BasicBlock *contextBlock, RangeInfo &RI,
    const DominatorTree &DT, int depth) {
    //1.2 有界剥离 x+C、C+x、x-C，累计为“基值 + 常量偏移”；任何未经证明
    //无回绕的算术都作为新的不可分基值，避免改变有符号比较语义。
    if (!V || !V->getIRType() || !V->getIRType()->isInt())
        return {};
    if (depth <= 0)
        return {V, 0, true};
    auto *BI = dynamic_cast<BinaryInst *>(V);
    if (!BI)
        return {V, 0, true};
    Value *lhs = nullptr;
    Value *rhs = nullptr;
    if (!BI->getOperands(lhs, rhs))
        return {V, 0, true};
    if (BI->getOp() == BinaryInst::ADD) {
        if (auto *C = dynamic_cast<ConstInt *>(lhs)) {
            if (!isKnownNoWrapAddSub(BI, contextBlock, RI, DT))
                return {V, 0, true};
            ConstraintElimOffsetExpr E =
                decomposeOffset(rhs, contextBlock, RI, DT, depth - 1);
            int64_t NewOffset = 0;
            if (E.Valid && IRMath::addI32(E.Offset, C->getVal(), NewOffset))
                return {E.Base, NewOffset, true};
        }
        if (auto *C = dynamic_cast<ConstInt *>(rhs)) {
            if (!isKnownNoWrapAddSub(BI, contextBlock, RI, DT))
                return {V, 0, true};
            ConstraintElimOffsetExpr E =
                decomposeOffset(lhs, contextBlock, RI, DT, depth - 1);
            int64_t NewOffset = 0;
            if (E.Valid && IRMath::addI32(E.Offset, C->getVal(), NewOffset))
                return {E.Base, NewOffset, true};
        }
    } else if (BI->getOp() == BinaryInst::SUB) {
        if (auto *C = dynamic_cast<ConstInt *>(rhs)) {
            if (!isKnownNoWrapAddSub(BI, contextBlock, RI, DT))
                return {V, 0, true};
            ConstraintElimOffsetExpr E =
                decomposeOffset(lhs, contextBlock, RI, DT, depth - 1);
            int64_t NewOffset = 0;
            if (E.Valid && IRMath::addI32(E.Offset, -(int64_t)C->getVal(), NewOffset))
                return {E.Base, NewOffset, true};
        }
    }
    return {V, 0, true};
}
//将比较两侧偏移吸收到谓词中并产生统一的规范约束。
bool ConstraintEliminationPass::rewriteOffsetConstraint(
    Value *lhs, Value *rhs, BinaryInst::Operation op,
    BasicBlock *contextBlock, RangeInfo &RI, const DominatorTree &DT,
    ConstraintElimConstraint &out) {
    //1.3 将两侧统一成 Base+Offset，只在偏移差为 0 或可用严格/非严格边界
    //等价吸收的 +/-1 时重写谓词，其余情况保持原比较。
    ConstraintElimOffsetExpr L =
        decomposeOffset(lhs, contextBlock, RI, DT);
    ConstraintElimOffsetExpr R =
        decomposeOffset(rhs, contextBlock, RI, DT);
    if (!L.Valid || !R.Valid || !L.Base || !R.Base) return false;
    int64_t Delta = R.Offset - L.Offset;
    BinaryInst::Operation NewOp = op;
    bool ok = false;
    switch (op) {
        case BinaryInst::L:
            if (Delta == 0) { NewOp = BinaryInst::L; ok = true; }
            else if (Delta == 1) { NewOp = BinaryInst::LE; ok = true; }
            break;
        case BinaryInst::LE:
            if (Delta == 0) { NewOp = BinaryInst::LE; ok = true; }
            else if (Delta == -1) { NewOp = BinaryInst::L; ok = true; }
            break;
        case BinaryInst::G:
            if (Delta == 0) { NewOp = BinaryInst::G; ok = true; }
            else if (Delta == -1) { NewOp = BinaryInst::GE; ok = true; }
            break;
        case BinaryInst::GE:
            if (Delta == 0) { NewOp = BinaryInst::GE; ok = true; }
            else if (Delta == 1) { NewOp = BinaryInst::G; ok = true; }
            break;
        case BinaryInst::E:
        case BinaryInst::NE:
            if (Delta == 0) { NewOp = op; ok = true; }
            break;
        default:
            break;
    }
    if (!ok) return false;
    out.val = L.Base;
    out.op = NewOp;
    out.bound = R.Base;
    out.boundIsConst = false;
    out.boundVal = 0;
    out.boundOffset = 0;
    if (auto *CI = dynamic_cast<ConstInt *>(out.bound)) {
        out.boundIsConst = true;
        out.boundVal = CI->getVal();
    }
    return true;
}
//从比较及所选真假边提取适合路径传播的规范约束。
bool ConstraintEliminationPass::extractConstraint(
    BinaryInst *cmp, bool negate, RangeInfo &RI, const DominatorTree &DT,
    ConstraintElimConstraint &out) {
    //1.把条件比较规范化为 val op bound；走 false 边时先取反谓词，且只接受 i32。
    if (!cmp || !BinaryInst::isCompareOp(cmp->getOp())) return false;
    Value *lhs = nullptr;
    Value *rhs = nullptr;
    if (!cmp->getOperands(lhs, rhs)) return false;
    if (!IRType::hasIntegerBitWidth(lhs->getIRType(), 32) ||
        !IRType::hasIntegerBitWidth(rhs->getIRType(), 32))
        return false;
    BinaryInst::Operation op = negate ? BinaryInst::negatePredicate(cmp->getOp()) : cmp->getOp();
    Value *val = lhs;
    Value *bound = rhs;
    BasicBlock *contextBlock = cmp->getParent();
    //1.4 对 (x+C) op K、(x-C) op K、(C-x) op K 做常量移项；要求原算术无回绕、
    //新边界仍在 i32，最后一种形态还需反转谓词方向。
    if (auto *rConst = dynamic_cast<ConstInt *>(rhs)) {
        if (auto *lBI = dynamic_cast<BinaryInst *>(lhs)) {
            Value *la = nullptr;
            Value *lb = nullptr;
            if (lBI->getOperands(la, lb) &&
                isKnownNoWrapAddSub(lBI, contextBlock, RI, DT)) {
                int64_t c2 = rConst->getVal();
                auto inI32 = [](int64_t v) {
                    return v >= (int64_t)INT32_MIN && v <= (int64_t)INT32_MAX;
                };
                if (lBI->getOp() == BinaryInst::ADD) {
                    ConstInt *cConst = dynamic_cast<ConstInt *>(la);
                    Value *xVal = lb;
                    if (!cConst) {
                        cConst = dynamic_cast<ConstInt *>(lb);
                        xVal = la;
                    }
                    if (cConst) {
                        int64_t newBound = c2 - (int64_t)cConst->getVal();
                        if (inI32(newBound)) {
                            out.val = xVal;
                            out.op = op;
                            out.bound = ConstInt::newConstInt((int)newBound);
                            out.boundIsConst = true;
                            out.boundVal = newBound;
                            out.boundOffset = 0;
                            return true;
                        }
                    }
                }
                else if (lBI->getOp() == BinaryInst::SUB) {
                    auto *lbConst = dynamic_cast<ConstInt *>(lb);
                    auto *laConst = dynamic_cast<ConstInt *>(la);
                    if (lbConst) {
                        int64_t newBound = c2 + (int64_t)lbConst->getVal();
                        if (inI32(newBound)) {
                            out.val = la;
                            out.op = op;
                            out.bound = ConstInt::newConstInt((int)newBound);
                            out.boundIsConst = true;
                            out.boundVal = newBound;
                            out.boundOffset = 0;
                            return true;
                        }
                    } else if (laConst) {
                        int64_t newBound = (int64_t)laConst->getVal() - c2;
                        if (inI32(newBound)) {
                            BinaryInst::Operation flipped = BinaryInst::reversePredicate(op);
                            out.val = lb;
                            out.op = flipped;
                            out.bound = ConstInt::newConstInt((int)newBound);
                            out.boundIsConst = true;
                            out.boundVal = newBound;
                            out.boundOffset = 0;
                            return true;
                        }
                    }
                }
            }
        }
    }
    //1.5 若常量移项未命中，再尝试两侧偏移规范化；否则保留原始两值约束。
    if (rewriteOffsetConstraint(
            lhs, rhs, op, contextBlock, RI, DT, out))
        return true;
    out.val = val;
    out.op = op;
    out.bound = bound;
    out.boundIsConst = false;
    out.boundVal = 0;
    out.boundOffset = 0;
    if (auto *CI = dynamic_cast<ConstInt *>(out.bound)) {
        out.boundIsConst = true;
        out.boundVal = CI->getVal();
    }
    return true;
}
//把两个常量或同基值边界换算为可直接比较的整数。
bool ConstraintEliminationPass::comparableBoundValues(
    const ConstraintElimConstraint &A, const ConstraintElimConstraint &B,
    int64_t &AV, int64_t &BV) {
    if (A.boundIsConst && B.boundIsConst) {
        return IRMath::addNoOverflow(A.boundVal, A.boundOffset, AV) &&
               IRMath::addNoOverflow(B.boundVal, B.boundOffset, BV);
    }
    if (!A.boundIsConst && !B.boundIsConst && A.bound == B.bound) {
        AV = A.boundOffset;
        BV = B.boundOffset;
        return true;
    }
    return false;
}
//依据整数谓词与端点关系判断已知约束是否直接蕴含目标约束。
bool ConstraintEliminationPass::implies(
    const ConstraintElimConstraint &known,
    const ConstraintElimConstraint &target) {
    //2.1 仅比较同一 val 且边界可换算的约束，按整数严格/非严格端点关系判断
    //known 是否足以推出 target；等式可推出对应范围与不等式。
    if (known.val != target.val) return false;
    int64_t kB = 0;
    int64_t tB = 0;
    bool comparableBound = comparableBoundValues(known, target, kB, tB);
    bool sameBound = comparableBound && kB == tB;
    if (sameBound && known.op == target.op) return true;
    if (!comparableBound) return false;
    switch (known.op) {
        case BinaryInst::L:
            switch (target.op) {
                case BinaryInst::L:  return kB <= tB;
                case BinaryInst::LE: return kB <= tB + 1;
                case BinaryInst::NE: return kB <= tB;
                default: break;
            }
            break;
        case BinaryInst::LE:
            switch (target.op) {
                case BinaryInst::L:  return kB < tB;
                case BinaryInst::LE: return kB <= tB;
                case BinaryInst::NE: return kB < tB;
                default: break;
            }
            break;
        case BinaryInst::G:
            switch (target.op) {
                case BinaryInst::G:  return kB >= tB;
                case BinaryInst::GE: return kB >= tB - 1;
                case BinaryInst::NE: return kB >= tB;
                default: break;
            }
            break;
        case BinaryInst::GE:
            switch (target.op) {
                case BinaryInst::G:  return kB > tB;
                case BinaryInst::GE: return kB >= tB;
                case BinaryInst::NE: return kB > tB;
                default: break;
            }
            break;
        case BinaryInst::E:
            switch (target.op) {
                case BinaryInst::E:  return kB == tB;
                case BinaryInst::L:  return kB < tB;
                case BinaryInst::LE: return kB <= tB;
                case BinaryInst::G:  return kB > tB;
                case BinaryInst::GE: return kB >= tB;
                case BinaryInst::NE: return kB != tB;
                default: break;
            }
            break;
        default: break;
    }
    return false;
}
//检查路径约束栈中是否存在目标约束的直接或反向等价证明。
bool ConstraintEliminationPass::isImplied(
    const std::vector<ConstraintElimConstraint> &stack,
    const ConstraintElimConstraint &target) {
    //2.2 先检查栈内直接约束；对两个变量的零偏移比较，同时构造交换两侧并反转
    //谓词的等价形式，避免因条件书写方向不同漏掉证明。
    for (auto &known : stack) {
        if (implies(known, target)) return true;
        if (!known.boundIsConst && !target.boundIsConst &&
            known.boundOffset == 0 && target.boundOffset == 0) {
            ConstraintElimConstraint reversed;
            reversed.val = known.bound;
            reversed.op = BinaryInst::reversePredicate(known.op);
            reversed.bound = known.val;
            reversed.boundIsConst = false;
            reversed.boundVal = 0;
            reversed.boundOffset = 0;
            if (implies(reversed, target)) return true;
        }
    }
    return false;
}
//通过一个中间值组合一跳序关系并尝试证明常量边界目标。
bool ConstraintEliminationPass::impliesTransitive(
    const std::vector<ConstraintElimConstraint> &stack,
    const ConstraintElimConstraint &target) {
    //2.3 对目标为常量边界的情况尝试一跳传递：val op1 mid 与 mid op2 const
    //合成为 val op const。仅组合同方向序关系或等式，其他谓词保守跳过。
    int64_t checkedTargetConst = 0;
    if (!target.boundIsConst ||
        !IRMath::addNoOverflow(target.boundVal, target.boundOffset, checkedTargetConst))
        return false;
    (void)checkedTargetConst;
    for (auto &k1 : stack) {
        if (k1.val != target.val) continue;
        if (k1.boundIsConst) continue;
        if (k1.bound == target.bound) continue;
        Value *mid = k1.bound;
        for (auto &k2 : stack) {
            if (k2.val != mid) continue;
            if (!k2.boundIsConst) continue;
            auto compose = [](BinaryInst::Operation a, BinaryInst::Operation b)
                -> BinaryInst::Operation {
                if ((a == BinaryInst::L  || a == BinaryInst::LE) &&
                    (b == BinaryInst::L  || b == BinaryInst::LE))
                    return (a == BinaryInst::L || b == BinaryInst::L)
                        ? BinaryInst::L : BinaryInst::LE;
                if ((a == BinaryInst::G  || a == BinaryInst::GE) &&
                    (b == BinaryInst::G  || b == BinaryInst::GE))
                    return (a == BinaryInst::G || b == BinaryInst::G)
                        ? BinaryInst::G : BinaryInst::GE;
                if (a == BinaryInst::E) return b;
                if (b == BinaryInst::E) return a;
                return BinaryInst::BinaryUnknown == 0 ? BinaryInst::NE : BinaryInst::NE;
            };
            BinaryInst::Operation combined = compose(k1.op, k2.op);
            if (combined == BinaryInst::NE) continue;
            ConstraintElimConstraint synthesized;
            synthesized.val = target.val;
            synthesized.op = combined;
            synthesized.bound = k2.bound;
            synthesized.boundIsConst = true;
            if (!IRMath::addNoOverflow(k2.boundVal, k2.boundOffset, synthesized.boundVal))
                continue;
            synthesized.boundOffset = 0;
            if (implies(synthesized, target)) return true;
        }
    }
    return false;
}
//依次尝试直接约束和一跳传递约束证明。
bool ConstraintEliminationPass::isImpliedChained(
    const std::vector<ConstraintElimConstraint> &stack,
    const ConstraintElimConstraint &target) {
    return isImplied(stack, target) || impliesTransitive(stack, target);
}
//通过证明目标约束的否定来判断当前路径是否与其矛盾。
bool ConstraintEliminationPass::isContradicted(
    const std::vector<ConstraintElimConstraint> &stack,
    const ConstraintElimConstraint &target) {
    ConstraintElimConstraint negTarget = target;
    negTarget.op = BinaryInst::negatePredicate(target.op);
    return isImpliedChained(stack, negTarget);
}
//沿支配树维护路径约束并集中折叠能够证明恒定的条件分支。
PreservationStatus ConstraintEliminationPass::run(Function &F,
                                                   FunctionAnalysisPassManager &FAM) {
    //1.获取支配树与范围分析，前者确定条件生效区域，后者证明偏移算术无回绕。
    auto &DT = FAM.getResult<DominantAnalysis>(F);
    auto &RI = FAM.getResult<RangeAnalysis>(F);
    bool changed = false;
    std::vector<ConstraintElimConstraint> constraintStack;
    std::stack<ConstraintElimStackFrame> dfsStack;
    if (!DT.Root) return PreservationStatus::all();
    dfsStack.push({DT.Root, 0});
    std::vector<ConstraintElimFoldCandidate> toFold;
    //2.显式深度优先遍历支配树。每个 frame 记录进入节点前的约束栈深度，
    //回到兄弟子树时精确弹出只在上一条支配路径成立的条件。
    while (!dfsStack.empty()) {
        auto frame = dfsStack.top();
        dfsStack.pop();
        DominatorNode *node = frame.node;
        BasicBlock *BB = node->Block;
        if (!BB) continue;
        while ((int)constraintStack.size() > frame.constraintsBefore)
            constraintStack.pop_back();
        //2.1 若当前块只有一个条件分支前驱，把所走 true/false 边对应的规范化条件
        //压栈；多前驱块不能假定某一条边条件必然成立。
        auto preds = BB->getPredecessors();
        if (preds.size() == 1) {
            BasicBlock *pred = preds[0];
            Instruction *term = pred->back();
            if (auto *br = dynamic_cast<BrInst *>(term)) {
                if (br->isConditional()) {
                    BasicBlock *trueBB = br->getTrueSuccessor();
                    BasicBlock *falseBB = br->getFalseSuccessor();
                    if (trueBB && falseBB && trueBB != falseBB &&
                        (BB == trueBB || BB == falseBB)) {
                        Value *cond = br->getCondition();
                        if (auto *cmpInst =
                                dynamic_cast<BinaryInst *>(cond)) {
                            ConstraintElimConstraint c;
                            bool negate = (BB == falseBB);
                            if (extractConstraint(
                                    cmpInst, negate, RI, DT, c)) {
                                constraintStack.push_back(c);
                            }
                        }
                    }
                }
            }
        }
        //2.2 对当前条件分支提取目标约束，并临时加入 RangeAnalysis 给出的上下界；
        //依次尝试直接/一跳蕴含及其否定，记录可折叠的布尔结果。
        if (!constraintStack.empty()) {
            Instruction *term = BB->back();
            if (auto *br = dynamic_cast<BrInst *>(term)) {
                if (br->isConditional()) {
                    Value *cond = br->getCondition();
                    if (auto *cmpInst = dynamic_cast<BinaryInst *>(cond)) {
                        ConstraintElimConstraint target;
                        if (extractConstraint(
                                cmpInst, false, RI, DT, target)) {
                            size_t savedSize = constraintStack.size();
                            if (target.val->getIRType() &&
                                target.val->getIRType()->isInt()) {
                                IntRange vr = RI.query(target.val, BB, DT);
                                if (!vr.isTop() && !vr.isBottom()) {
                                    ConstraintElimConstraint lo, hi;
                                    lo.val = target.val;
                                    lo.op = BinaryInst::GE;
                                    lo.bound = ConstInt::newConstInt((int)vr.min);
                                    lo.boundIsConst = true;
                                    lo.boundVal = vr.min;
                                    lo.boundOffset = 0;
                                    hi.val = target.val;
                                    hi.op = BinaryInst::LE;
                                    hi.bound = ConstInt::newConstInt((int)vr.max);
                                    hi.boundIsConst = true;
                                    hi.boundVal = vr.max;
                                    hi.boundOffset = 0;
                                    constraintStack.push_back(lo);
                                    constraintStack.push_back(hi);
                                }
                            }
                            if (isImpliedChained(constraintStack, target)) {
                                toFold.push_back({br, cmpInst, true});
                            } else if (isContradicted(constraintStack, target)) {
                                toFold.push_back({br, cmpInst, false});
                            }
                            while (constraintStack.size() > savedSize)
                                constraintStack.pop_back();
                        }
                    }
                }
            }
        }
        //2.3 将当前约束深度随子节点入栈，使该路径事实传播到整个支配子树。
        int curSize = (int)constraintStack.size();
        std::vector<DominatorNode *> children;
        for (DominatorNode *child = node->FirstChild; child; child = child->NextSibling)
            children.push_back(child);
        for (auto it = children.rbegin(); it != children.rend(); ++it)
            dfsStack.push({*it, curSize});
    }
    //3.遍历结束后再改写分支条件；重新校验候选仍是原条件，避免收集阶段修改 CFG
    //或条件 use，并把证明结果替换为布尔常量交给 CFGSimplify 清边。
    for (const auto &Candidate : toFold) {
        if (!Candidate.branch || !Candidate.condition ||
            !Candidate.branch->isConditional() ||
            Candidate.branch->getCondition() != Candidate.condition) {
            continue;
        }
        changed |= Candidate.branch->replaceOperand(
            0, ConstBool::newConstBool(Candidate.result));
    }
    if (!changed) return PreservationStatus::all();
    //4.此处只替换条件值，CFG 尚未改变，因此保留支配树与循环树。
    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<LoopAnalysis>();
    return PA;
}
