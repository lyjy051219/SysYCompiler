#include "ir/opt/ConstraintElimination.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "lib/IRUtils.hpp"

#include <vector>
#include <stack>
#include <unordered_map>

// =====================================================================
// ConstraintElimination — 约束传播消除冗余分支
//
// 沿支配树 DFS 遍历，维护一个约束栈。
// 进入由条件分支支配的块时，把分支条件 push 到约束栈。
// 对块内的每个条件分支，检查其条件是否被约束栈蕴含。
// 离开支配子树时 pop 约束。
//
// 和 RangeAwareSimplify 互补：
// - RangeAwareSimplify 用"值的范围"判断比较结果
// - ConstraintElimination 用"路径上的已知事实"判断比较结果
//
// 蕴含判定分两层：
//   1) 单跳直接蕴含（implies）：同一个 val、同一个 bound（或常量可比）
//      已知 x < A → 蕴含 x < B 当 A <= B
//      已知 x < A → 蕴含 x <= B 当 A <= B+1
//      已知 x >= A → 蕴含 x >= B 当 A >= B
//      已知 x == A → 蕴含 x < B 当 A < B
//      等等
//   2) 两跳传递链（impliesTransitive）：引入一个中间值 m
//      已知 x < m 且 m <= A(const) → 蕴含 x < A
//      已知 x <= m 且 m <= A       → 蕴含 x <= A
//      对"控制流里先 if (x < m) 再 if (m <= c)"这种嵌套判断有用
//      （轻量传递链：O(stack^2) 而不是完整传递闭包）
//
// 约束来源：
//   a) 控制流：唯一前驱的条件分支（true 分支 push op，false push negateOp）
//   b) RangeAnalysis seed：对函数入口块的每个值注入 [min, max] 两条约束
//      作为根基事实，帮助靠"范围+控制流"联合推理的场景
//      （如 if (getch() < 100) 里，getch 范围 [-128,127]，控制流再加 <100 的约束）
// =====================================================================

bool ConstraintEliminationPass::isKnownNoWrapAddSub(
    BinaryInst *inst, BasicBlock *contextBlock, RangeInfo &RI,
    const DominatorTree &DT) {
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

    // 必须使用比较所在块的上下文范围，而不是只检查常量或最终总偏移。
    // 例如 (x + 1) - 1 的总偏移为 0，但第一层 x + 1 仍可能回绕。
    IntRange lhsRange = RI.query(lhs, contextBlock, DT);
    IntRange rhsRange = RI.query(rhs, contextBlock, DT);
    if (lhsRange.isBottom() || rhsRange.isBottom()) return false;

    int64_t resultMin = 0;
    int64_t resultMax = 0;
    if (inst->getOp() == BinaryInst::ADD) {
        return IRMath::addI32(lhsRange.min, rhsRange.min, resultMin) &&
               IRMath::addI32(lhsRange.max, rhsRange.max, resultMax);
    }

    // SUB 的最小值来自 lhs.min-rhs.max，最大值来自 lhs.max-rhs.min。
    return IRMath::addI32(lhsRange.min, -rhsRange.max, resultMin) &&
           IRMath::addI32(lhsRange.max, -rhsRange.min, resultMax);
}

ConstraintElimOffsetExpr ConstraintEliminationPass::decomposeOffset(
    Value *V, BasicBlock *contextBlock, RangeInfo &RI,
    const DominatorTree &DT, int depth) {
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
            // 当前 ADD 必须先有逐层无回绕证明；失败时把整条指令视为
            // 不透明 base，保留原始比较，不利用源程序整数溢出。
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

bool ConstraintEliminationPass::rewriteOffsetConstraint(
    Value *lhs, Value *rhs, BinaryInst::Operation op,
    BasicBlock *contextBlock, RangeInfo &RI, const DominatorTree &DT,
    ConstraintElimConstraint &out) {
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

// 从 icmp 指令提取约束
// 只处理整数比较：BinaryInst 的比较操作符在整数和浮点间复用，
// 浮点比较的语义（NaN、inf）和整数不同，不能用整数约束推理。
//
// 仿射归一化（轻量的 expression normalization）：
//   (x + c1) op c2  →  x op (c2 - c1)
//   (x - c1) op c2  →  x op (c2 + c1)
//   (c1 - x) op c2  →  x (反向 op) (c1 - c2)      —— x 搬到一侧要翻号
//   x op (y + c)    →  (x - y) op c               —— 这步风险大先不做
// 前三种除了要求常量结果必须落在 i32 范围内，还要求 RangeAnalysis
// 在比较所在块证明实际 ADD/SUB 不会回绕；证据不足时退回原始形式。
bool ConstraintEliminationPass::extractConstraint(
    BinaryInst *cmp, bool negate, RangeInfo &RI, const DominatorTree &DT,
    ConstraintElimConstraint &out) {
    if (!cmp || !BinaryInst::isCompareOp(cmp->getOp())) return false;
    Value *lhs = nullptr;
    Value *rhs = nullptr;
    if (!cmp->getOperands(lhs, rhs)) return false;

    // 仅整数比较
    // 当前约束格与溢出证明均按 i32 构造；其他位宽保守跳过。
    if (!IRType::hasIntegerBitWidth(lhs->getIRType(), 32) ||
        !IRType::hasIntegerBitWidth(rhs->getIRType(), 32))
        return false;

    BinaryInst::Operation op = negate ? BinaryInst::negatePredicate(cmp->getOp()) : cmp->getOp();
    Value *val = lhs;
    Value *bound = rhs;
    BasicBlock *contextBlock = cmp->getParent();

    // —— 仿射归一化：仅当 RHS 是常量 + LHS 是 add/sub 且另一侧常量时生效 ——
    // 避免了把非常量搬到 bound 侧引入新值。每一条实际算术指令都必须
    // 先由 RangeAnalysis 证明无回绕，证据不足时保留原约束。
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

                // (x + c1) op c2 → x op (c2 - c1)
                // (c1 + x) op c2 同构
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
                            // 用 interned ConstInt 做 bound，保证"同值常量 = 同指针"，
                            // 不会让 implies 的 sameBound 快速路径错判
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
                // (x - c1) op c2 → x op (c2 + c1)
                else if (lBI->getOp() == BinaryInst::SUB) {
                    auto *lbConst = dynamic_cast<ConstInt *>(lb);
                    auto *laConst = dynamic_cast<ConstInt *>(la);
                    if (lbConst) {
                        // x - c1
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
                        // c1 - x op c2  → x (反向 op) (c1 - c2)
                        int64_t newBound = (int64_t)laConst->getVal() - c2;
                        if (inI32(newBound)) {
                            // 翻号：L↔G, LE↔GE, E 不变, NE 不变
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

// 检查已知约束 known 是否蕴含目标约束 target
// 两个约束必须是关于同一个 val 和同一个 bound（或 bound 是常量可比较）
bool ConstraintEliminationPass::implies(
    const ConstraintElimConstraint &known,
    const ConstraintElimConstraint &target) {
    // 必须是同一个值
    if (known.val != target.val) return false;

    // 同一个 bound（或常量）允许携带不同偏移：x < y、x <= y+1
    // 这类约束可以直接按偏移大小比较。
    int64_t kB = 0;
    int64_t tB = 0;
    bool comparableBound = comparableBoundValues(known, target, kB, tB);
    bool sameBound = comparableBound && kB == tB;

    // 蕴含规则表：
    // 已知 val < kB:
    //   蕴含 val < tB 当 kB <= tB
    //   蕴含 val <= tB 当 kB <= tB + 1 (即 kB - 1 <= tB)
    //   蕴含 val != tB 当 kB <= tB
    // 已知 val <= kB:
    //   蕴含 val < tB 当 kB < tB (即 kB + 1 <= tB)
    //   蕴含 val <= tB 当 kB <= tB
    //   蕴含 val != tB 当 kB < tB
    // 已知 val > kB:
    //   蕴含 val > tB 当 kB >= tB
    //   蕴含 val >= tB 当 kB >= tB - 1
    //   蕴含 val != tB 当 kB >= tB
    // 已知 val >= kB:
    //   蕴含 val > tB 当 kB > tB
    //   蕴含 val >= tB 当 kB >= tB
    //   蕴含 val != tB 当 kB > tB
    // 已知 val == kB:
    //   蕴含任何可以从等式推出的关系

    if (sameBound && known.op == target.op) return true;

    if (!comparableBound) return false;

    switch (known.op) {
        case BinaryInst::L: // val < kB
            switch (target.op) {
                case BinaryInst::L:  return kB <= tB;
                case BinaryInst::LE: return kB <= tB + 1;
                case BinaryInst::NE: return kB <= tB;
                default: break;
            }
            break;
        case BinaryInst::LE: // val <= kB
            switch (target.op) {
                case BinaryInst::L:  return kB < tB;
                case BinaryInst::LE: return kB <= tB;
                case BinaryInst::NE: return kB < tB;
                default: break;
            }
            break;
        case BinaryInst::G: // val > kB
            switch (target.op) {
                case BinaryInst::G:  return kB >= tB;
                case BinaryInst::GE: return kB >= tB - 1;
                case BinaryInst::NE: return kB >= tB;
                default: break;
            }
            break;
        case BinaryInst::GE: // val >= kB
            switch (target.op) {
                case BinaryInst::G:  return kB > tB;
                case BinaryInst::GE: return kB >= tB;
                case BinaryInst::NE: return kB > tB;
                default: break;
            }
            break;
        case BinaryInst::E: // val == kB
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

// 检查约束栈是否蕴含目标约束（单跳）
bool ConstraintEliminationPass::isImplied(
    const std::vector<ConstraintElimConstraint> &stack,
    const ConstraintElimConstraint &target) {
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

// 两跳传递链：在栈里找中间值 m，使得"A -> m"和"m -> B"复合后蕴含 target。
//   只覆盖 target.bound 是常量的情形（非常量 bound 链式判定容易假阳）
//   只做一种跳跃模式：target 是 val < C，若栈里有 val < m 和 m <= C（m
//   为同类型整数），则成立；其余不等号同理展开
//
// 复杂度：对每个 target，最多扫 stack 两遍，总共 O(stack^2)，而栈大小
// 受支配树深度限制，实际很小。
bool ConstraintEliminationPass::impliesTransitive(
    const std::vector<ConstraintElimConstraint> &stack,
    const ConstraintElimConstraint &target) {
    int64_t checkedTargetConst = 0;
    if (!target.boundIsConst ||
        !IRMath::addNoOverflow(target.boundVal, target.boundOffset, checkedTargetConst))
        return false;
    (void)checkedTargetConst;

    // 先找所有"target.val op m"形式的约束，m 不是 target.bound
    for (auto &k1 : stack) {
        if (k1.val != target.val) continue;
        if (k1.boundIsConst) continue;          // 已在单跳里 cover
        if (k1.bound == target.bound) continue; // 同一 bound 是单跳
        Value *mid = k1.bound;
        // 再在栈里找"m op' C"，且 C 是常量
        for (auto &k2 : stack) {
            if (k2.val != mid) continue;
            if (!k2.boundIsConst) continue;
            // 构造两条已知 -> 目标 的蕴含链
            // 为避免写一堆 case，复用单跳 implies：先用 k1 把 target.val 替成 mid，
            // 得到中间 target'，再用 k2 检查是否蕴含 target'
            // 具体说：如果 k1 = (val op1 mid) 且 k2 蕴含 (mid op2 target.bound)，
            // 那么 op1 ∘ op2 按下面表决定是否蕴含 target.op
            //
            // 这里枚举最常见的四种组合：<-<, <=-<=, >-> , >=->=
            auto compose = [](BinaryInst::Operation a, BinaryInst::Operation b)
                -> BinaryInst::Operation {
                // a op b 的组合：两个同向不等号组合后仍保留最弱的那个
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
            if (combined == BinaryInst::NE) continue; // 不兼容的组合
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

// 综合判定：先试单跳，不行再试两跳链
bool ConstraintEliminationPass::isImpliedChained(
    const std::vector<ConstraintElimConstraint> &stack,
    const ConstraintElimConstraint &target) {
    return isImplied(stack, target) || impliesTransitive(stack, target);
}

bool ConstraintEliminationPass::isContradicted(
    const std::vector<ConstraintElimConstraint> &stack,
    const ConstraintElimConstraint &target) {
    ConstraintElimConstraint negTarget = target;
    negTarget.op = BinaryInst::negatePredicate(target.op);
    return isImpliedChained(stack, negTarget);
}

PreservationStatus ConstraintEliminationPass::run(Function &F,
                                                   FunctionAnalysisPassManager &FAM) {
    auto &DT = FAM.getResult<DominantAnalysis>(F);
    auto &RI = FAM.getResult<RangeAnalysis>(F);
    bool changed = false;

    std::vector<ConstraintElimConstraint> constraintStack;
    std::stack<ConstraintElimStackFrame> dfsStack;

    if (!DT.Root) return PreservationStatus::all();
    dfsStack.push({DT.Root, 0});

    std::vector<ConstraintElimFoldCandidate> toFold;

    while (!dfsStack.empty()) {
        auto frame = dfsStack.top();
        dfsStack.pop();

        DominatorNode *node = frame.node;
        BasicBlock *BB = node->Block;
        if (!BB) continue;

        // 恢复约束栈到进入此子树时的状态
        while ((int)constraintStack.size() > frame.constraintsBefore)
            constraintStack.pop_back();

        // 从 BB 的唯一前驱（如果有）的分支条件中提取约束
        // 只在 BB 有唯一前驱时才安全（否则条件不是所有路径都满足）
        auto preds = BB->getPredecessors();
        if (preds.size() == 1) {
            BasicBlock *pred = preds[0];
            Instruction *term = pred->back();
            if (auto *br = dynamic_cast<BrInst *>(term)) {
                if (br->isConditional()) {
                    BasicBlock *trueBB = br->getTrueSuccessor();
                    BasicBlock *falseBB = br->getFalseSuccessor();
                    // 同一条件分支的两条边若汇入同一个块，进入该块并不能
                    // 证明条件为真或为假；只有唯一命中的边才能提供路径事实。
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

        // 对块内的每个条件分支，检查是否被约束栈蕴含
        // 栈空时直接略过：没控制流约束的情况下再叠 range-seed 意义不大
        // （range-only 的事实 RangeAwareSimplify 已经折过了）
        if (!constraintStack.empty()) {
            Instruction *term = BB->back();
            if (auto *br = dynamic_cast<BrInst *>(term)) {
                if (br->isConditional()) {
                    Value *cond = br->getCondition();
                    if (auto *cmpInst = dynamic_cast<BinaryInst *>(cond)) {
                        ConstraintElimConstraint target;
                        if (extractConstraint(
                                cmpInst, false, RI, DT, target)) {
                            // 把 target.val 的 RangeAnalysis 结果当成一对临时
                            // 常量上下界约束 push 到栈尾；用完 pop 回去，
                            // 不影响子树的 frame.constraintsBefore 记账
                            size_t savedSize = constraintStack.size();
                            if (target.val->getIRType() &&
                                target.val->getIRType()->isInt()) {
                                IntRange vr = RI.query(target.val, BB, DT);
                                if (!vr.isTop() && !vr.isBottom()) {
                                    // 用 interned ConstInt 当 bound，避免 bound=nullptr
                                    // 让 implies 的 sameBound 快速路径错判"不同数值但指针同"
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

        // 推入子节点，记录当前约束栈深度
        int curSize = (int)constraintStack.size();
        std::vector<DominatorNode *> children;
        for (DominatorNode *child = node->FirstChild; child; child = child->NextSibling)
            children.push_back(child);
        for (auto it = children.rbegin(); it != children.rend(); ++it)
            dfsStack.push({*it, curSize});
    }

    // 路径约束只证明当前分支上的条件值，不能把共享的比较指令全局替换。
    // 循环克隆等变换可能让同一比较同时服务多个分支；逐个改写分支操作数
    // 既允许各路径得到不同结论，也避免第一次删除比较后再次访问悬空指针。
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

    // 分支条件被折叠后，死分支和无用比较由后续 CFGSimplify/DCE 清理。
    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<LoopAnalysis>();
    return PA;
}
