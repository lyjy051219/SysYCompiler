#include "ir/opt/ConstraintElimination.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"

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
//      （如 if (getch() < 100) 里，getch 范围 [0,127]，控制流再加 <100 的约束）
// =====================================================================

namespace {

// 一条约束：val op bound
struct Constraint {
    Value *val;
    BinaryInst::Operation op; // L, LE, G, GE, E, NE
    Value *bound;
    // 如果 bound 是 ConstInt，记住它的值方便蕴含检查
    bool boundIsConst;
    int64_t boundVal;
};

// 从条件分支的 icmp 提取约束
// 如果是 br(icmp op lhs rhs, trueBB, falseBB)，
// true 分支上的约束是 (lhs op rhs)，false 分支上的约束是 (lhs !op rhs)
static BinaryInst::Operation negateOp(BinaryInst::Operation op) {
    switch (op) {
        case BinaryInst::L:  return BinaryInst::GE;
        case BinaryInst::LE: return BinaryInst::G;
        case BinaryInst::G:  return BinaryInst::LE;
        case BinaryInst::GE: return BinaryInst::L;
        case BinaryInst::E:  return BinaryInst::NE;
        case BinaryInst::NE: return BinaryInst::E;
        default: return op;
    }
}

static bool isCmpOp(BinaryInst::Operation op) {
    return op == BinaryInst::L || op == BinaryInst::LE ||
           op == BinaryInst::G || op == BinaryInst::GE ||
           op == BinaryInst::E || op == BinaryInst::NE;
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
// 前三种只要 RHS 是常量就能做；溢出检查走 int64，溢出时退回原始形式。
static bool extractConstraint(BinaryInst *cmp, bool negate, Constraint &out) {
    if (!cmp || !isCmpOp(cmp->getOp())) return false;
    auto &UL = cmp->getUserList();
    if (UL.size() < 2) return false;

    Value *lhs = UL[0]->getValue();
    Value *rhs = UL[1]->getValue();

    // 仅整数比较
    if (!lhs->getIRType() || lhs->getIRType()->getEnum() != IR_INT) return false;
    if (!rhs->getIRType() || rhs->getIRType()->getEnum() != IR_INT) return false;

    BinaryInst::Operation op = negate ? negateOp(cmp->getOp()) : cmp->getOp();
    Value *val = lhs;
    Value *bound = rhs;

    // —— 仿射归一化：仅当 RHS 是常量 + LHS 是 add/sub 且另一侧常量时生效 ——
    // 避免了把非常量搬到 bound 侧引入新值。溢出一律退回原式。
    if (auto *rConst = dynamic_cast<ConstInt *>(rhs)) {
        if (auto *lBI = dynamic_cast<BinaryInst *>(lhs)) {
            auto &lUL = lBI->getUserList();
            if (lUL.size() >= 2) {
                Value *la = lUL[0]->getValue();
                Value *lb = lUL[1]->getValue();
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
                            return true;
                        }
                    } else if (laConst) {
                        // c1 - x op c2  → x (反向 op) (c1 - c2)
                        int64_t newBound = (int64_t)laConst->getVal() - c2;
                        if (inI32(newBound)) {
                            // 翻号：L↔G, LE↔GE, E 不变, NE 不变
                            BinaryInst::Operation flipped = op;
                            switch (op) {
                                case BinaryInst::L:  flipped = BinaryInst::G;  break;
                                case BinaryInst::LE: flipped = BinaryInst::GE; break;
                                case BinaryInst::G:  flipped = BinaryInst::L;  break;
                                case BinaryInst::GE: flipped = BinaryInst::LE; break;
                                default: break;
                            }
                            out.val = lb;
                            out.op = flipped;
                            out.bound = ConstInt::newConstInt((int)newBound);
                            out.boundIsConst = true;
                            out.boundVal = newBound;
                            return true;
                        }
                    }
                }
            }
        }
    }

    out.val = val;
    out.op = op;
    out.bound = bound;
    out.boundIsConst = false;
    if (auto *CI = dynamic_cast<ConstInt *>(out.bound)) {
        out.boundIsConst = true;
        out.boundVal = CI->getVal();
    }
    return true;
}

// 检查已知约束 known 是否蕴含目标约束 target
// 两个约束必须是关于同一个 val 和同一个 bound（或 bound 是常量可比较）
static bool implies(const Constraint &known, const Constraint &target) {
    // 必须是同一个值
    if (known.val != target.val) return false;

    // 如果 bound 不是同一个值，只有都是常量才能比较
    if (known.bound != target.bound) {
        if (!known.boundIsConst || !target.boundIsConst) return false;
    }

    // 同一个 bound（或常量可比较）
    int64_t kB = known.boundIsConst ? known.boundVal : 0;
    int64_t tB = target.boundIsConst ? target.boundVal : 0;
    // sameBound 的语义：两条约束的 bound 在"代表同一个数值"意义下相等。
    // 两端都是常量时看 boundVal 是否相等（不靠 Value* 指针 —— interning 不一定能保证）；
    // 非常量看 Value* 指针。
    bool sameBound = (known.boundIsConst && target.boundIsConst)
        ? (kB == tB)
        : (known.bound == target.bound);

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

    if (!known.boundIsConst || !target.boundIsConst) {
        // 非常量 bound 只能判断完全相同的约束
        return sameBound && known.op == target.op;
    }

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
static bool isImplied(const std::vector<Constraint> &stack, const Constraint &target) {
    for (auto &known : stack) {
        if (implies(known, target)) return true;
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
static bool impliesTransitive(const std::vector<Constraint> &stack,
                              const Constraint &target) {
    if (!target.boundIsConst) return false;

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
            Constraint synthesized;
            synthesized.val = target.val;
            synthesized.op = combined;
            synthesized.bound = k2.bound;
            synthesized.boundIsConst = true;
            synthesized.boundVal = k2.boundVal;
            if (implies(synthesized, target)) return true;
        }
    }
    return false;
}

// 综合判定：先试单跳，不行再试两跳链
static bool isImpliedChained(const std::vector<Constraint> &stack,
                             const Constraint &target) {
    return isImplied(stack, target) || impliesTransitive(stack, target);
}

static bool isContradicted(const std::vector<Constraint> &stack, const Constraint &target) {
    Constraint negTarget = target;
    negTarget.op = negateOp(target.op);
    return isImpliedChained(stack, negTarget);
}

} // namespace

PreservationStatus ConstraintEliminationPass::run(Function &F,
                                                   FunctionAnalysisPassManager &FAM) {
    auto &DT = FAM.getResult<DominantAnalysis>(F);
    auto &RI = FAM.getResult<RangeAnalysis>(F);
    bool changed = false;

    // DFS 遍历支配树，维护约束栈
    struct StackFrame {
        DominatorNode *node;
        int constraintsBefore; // 进入时约束栈的大小，离开时 pop 回来
    };

    std::vector<Constraint> constraintStack;
    std::stack<StackFrame> dfsStack;

    if (!DT.Root) return PreservationStatus::all();
    dfsStack.push({DT.Root, 0});

    // 收集要处理的比较指令（先收集后处理，避免迭代中删指令影响 DFS）
    struct FoldCandidate {
        BinaryInst *cmp;
        bool result; // true or false
    };
    std::vector<FoldCandidate> toFold;

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
                auto &brUL = br->getUserList();
                if (brUL.size() == 3) {
                    // 条件分支
                    Value *cond = brUL[0]->getValue();
                    auto *trueBB = dynamic_cast<BasicBlock *>(brUL[1]->getValue());
                    auto *falseBB = dynamic_cast<BasicBlock *>(brUL[2]->getValue());

                    if (auto *cmpInst = dynamic_cast<BinaryInst *>(cond)) {
                        Constraint c;
                        bool negate = (BB == falseBB);
                        if (extractConstraint(cmpInst, negate, c)) {
                            constraintStack.push_back(c);
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
                auto &brUL = br->getUserList();
                if (brUL.size() == 3) {
                    Value *cond = brUL[0]->getValue();
                    if (auto *cmpInst = dynamic_cast<BinaryInst *>(cond)) {
                        Constraint target;
                        if (extractConstraint(cmpInst, false, target)) {
                            // 把 target.val 的 RangeAnalysis 结果当成一对临时
                            // 常量上下界约束 push 到栈尾；用完 pop 回去，
                            // 不影响子树的 frame.constraintsBefore 记账
                            size_t savedSize = constraintStack.size();
                            if (target.val->getIRType() &&
                                target.val->getIRType()->getEnum() == IR_INT) {
                                IntRange vr = RI.query(target.val, BB, DT);
                                if (!vr.isTop() && !vr.isBottom()) {
                                    // 用 interned ConstInt 当 bound，避免 bound=nullptr
                                    // 让 implies 的 sameBound 快速路径错判"不同数值但指针同"
                                    Constraint lo, hi;
                                    lo.val = target.val;
                                    lo.op = BinaryInst::GE;
                                    lo.bound = ConstInt::newConstInt((int)vr.min);
                                    lo.boundIsConst = true;
                                    lo.boundVal = vr.min;
                                    hi.val = target.val;
                                    hi.op = BinaryInst::LE;
                                    hi.bound = ConstInt::newConstInt((int)vr.max);
                                    hi.boundIsConst = true;
                                    hi.boundVal = vr.max;
                                    constraintStack.push_back(lo);
                                    constraintStack.push_back(hi);
                                }
                            }

                            if (isImpliedChained(constraintStack, target)) {
                                toFold.push_back({cmpInst, true});
                            } else if (isContradicted(constraintStack, target)) {
                                toFold.push_back({cmpInst, false});
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

    // 执行折叠
    for (auto &fc : toFold) {
        fc.cmp->replaceAllUsesWith(ConstBool::newConstBool(fc.result));
        delete fc.cmp;
        changed = true;
    }

    if (!changed) return PreservationStatus::all();

    // 比较被折叠后，死分支由后续 CFGSimplify 清理
    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<LoopAnalysis>();
    return PA;
}
