#include "ir/opt/RangeAwareSimplify.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"

#include <vector>
#include <cmath>
#include <unordered_set>

// =====================================================================
// RangeAwareSimplify — 基于整数范围分析的指令化简
//
// 消费 RangeAnalysis 的结果，对以下模式做化简：
//
// 1) 强度削减（当前禁用，见 run() 的注释）：
//    x / 2^n → x >> n（逻辑右移），前提是 x 的范围已知非负
//    x % 2^n → x & (2^n - 1)（位与掩码），前提是 x 非负
//    这两个变换在 CPU 上把除法/取模（多周期）换成移位/位与（单周期）
//
// 2) 常量折叠（整数 + 浮点）：
//    i32 值的 IntRange 收窄成单值 [c, c] → 替换为 ConstInt(c)
//    float 值的 FloatRange 收窄成单值 [c, c]（且非 ±inf）→ 替换为 ConstFloat(c)
//    跳过有副作用的指令（CallInst/LoadInst/StoreInst），否则会丢副作用
//    （典型：跨函数 Range 推导出 Call 返回常量，但 Call 通过指针实参写数组）
//
// 3) 比较折叠（整数 + 浮点）：
//    BinaryInst::L/LE/G/GE/E/NE 整数浮点共用，靠操作数类型分派。
//    整数走 IntRange 端点比较；浮点走 FloatRange 端点比较
//    （FloatRange 非 Top 时假定不含 NaN —— NaN 已被吸收到 Top）
//    六种比较（L/LE/G/GE/E/NE）都覆盖
//    折叠后如果比较结果被 branch 使用，死分支会被后续 CFGSimplify 清理
//
// 4) srem → urem 转换（占位）：
//    如果被除数非负且除数为正，signed rem 等价于 unsigned rem
//    当前 IR 无独立 UREM 指令，pass 保留接口等后端区分后再实装
// =====================================================================

namespace {

// 判断一个 ConstInt 值是否是 2 的幂
static bool isPowerOf2(int val) {
    return val > 0 && (val & (val - 1)) == 0;
}

// 计算 log2(val)，val 必须是 2 的幂
static int log2i(int val) {
    int n = 0;
    while ((1 << n) < val) n++;
    return n;
}

static bool isPureIntExprOp(BinaryInst::Operation op) {
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

static IntRange queryIntExprImpl(Value *V, BasicBlock *BB, RangeInfo &RI,
                                 DominatorTree &DT, int depth,
                                 std::unordered_set<Value *> &visiting) {
    if (auto *CI = dynamic_cast<ConstInt *>(V))
        return IntRange(CI->getVal());
    if (!V || !V->getIRType() || V->getIRType()->getEnum() != IR_INT)
        return IntRange();

    IntRange base = RI.query(V, BB, DT);
    if (depth <= 0 || visiting.count(V))
        return base;

    auto *BI = dynamic_cast<BinaryInst *>(V);
    if (!BI || !isPureIntExprOp(BI->getOp()))
        return base;

    BasicBlock *DefBB = BI->ChainNode<BasicBlock, Instruction>::getParent();
    if (DefBB != BB)
        return base;

    auto &UL = BI->getUserList();
    if (UL.size() < 2) return base;
    Value *lhs = UL[0]->getValue();
    Value *rhs = UL[1]->getValue();
    if (!lhs || !rhs) return base;
    if (!lhs->getIRType() || lhs->getIRType()->getEnum() != IR_INT) return base;
    if (!rhs->getIRType() || rhs->getIRType()->getEnum() != IR_INT) return base;

    visiting.insert(V);
    IntRange l = queryIntExprImpl(lhs, BB, RI, DT, depth - 1, visiting);
    IntRange r = queryIntExprImpl(rhs, BB, RI, DT, depth - 1, visiting);
    visiting.erase(V);

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

    return base.intersectWith(expr);
}

static IntRange queryIntExpr(Value *V, BasicBlock *BB, RangeInfo &RI,
                             DominatorTree &DT) {
    std::unordered_set<Value *> visiting;
    return queryIntExprImpl(V, BB, RI, DT, 3, visiting);
}

static bool isKnownNonNegative(Value *V, BasicBlock *BB, RangeInfo &RI,
                               DominatorTree &DT) {
    if (!V || !BB) return false;

    IntRange R = queryIntExpr(V, BB, RI, DT);
    if (!R.isBottom() && R.isNonNegative()) return true;

    KnownBits KB = RI.queryBits(V, BB, DT);
    if (KB.knownNonNegative()) return true;

    IntRange FromBits = KB.toIntRange();
    return !FromBits.isBottom() && FromBits.isNonNegative();
}

static int evalTruth(Value *V, BasicBlock *BB, RangeInfo &RI, DominatorTree &DT) {
    if (auto *CB = dynamic_cast<ConstBool *>(V))
        return CB->getVal() ? 1 : 0;
    if (auto *CI = dynamic_cast<ConstInt *>(V))
        return CI->getVal() != 0 ? 1 : 0;
    if (!V || !V->getIRType() || V->getIRType()->getEnum() != IR_INT)
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

// =====================================================================
// Step 1: 强度削减 — 除法/取模 → 移位/位与
// =====================================================================
static bool strengthReduce(Function &F, RangeInfo &RI, DominatorTree &DT) {
    bool changed = false;

    std::vector<BinaryInst *> candidates;
    for (BasicBlock *BB : F) {
        for (Instruction *I : *BB) {
            auto *BI = dynamic_cast<BinaryInst *>(I);
            if (!BI) continue;
            auto op = BI->getOp();
            if (op == BinaryInst::DIV || op == BinaryInst::MOD)
                candidates.push_back(BI);
        }
    }

    for (BinaryInst *BI : candidates) {
        auto &UL = BI->getUserList();
        if (UL.size() < 2) continue;
        Value *lhs = UL[0]->getValue();
        Value *rhs = UL[1]->getValue();

        // 只处理整数除法/取模（BinaryInst::DIV/MOD 也用于 fdiv/frem，
        // 对浮点做移位/位与是致命的语义错误）
        if (!lhs->getIRType() || lhs->getIRType()->getEnum() != IR_INT) continue;
        if (!rhs->getIRType() || rhs->getIRType()->getEnum() != IR_INT) continue;

        // 右操作数必须是 2 的幂常量
        auto *rhsConst = dynamic_cast<ConstInt *>(rhs);
        if (!rhsConst || !isPowerOf2(rhsConst->getVal())) continue;
        int shift = log2i(rhsConst->getVal());

        BasicBlock *BB = BI->ChainNode<BasicBlock, Instruction>::getParent();
        if (!BB) continue;

        // 左操作数必须非负。优先使用局部表达式范围，再结合 KnownBits
        // 的符号位事实；这能覆盖 zext、and mask、循环归纳变量等常见形态。
        if (!isKnownNonNegative(lhs, BB, RI, DT)) continue;

        if (BI->getOp() == BinaryInst::DIV) {
            // x / 2^n → ashr x, n（非负输入下，算术右移等价于逻辑右移；
            // sdiv 对负被除数做"向零取整"，ashr 是"向下取整"，负数路径语义
            // 不同，我们靠 RangeAnalysis 提前排除了负数，这里才能用 ashr）
            Value *shiftVal = ConstInt::newConstInt(shift);
            auto *newInst = new BinaryInst(lhs, BinaryInst::ASHR, shiftVal);
            // 直接拿 BI 构造 Iterator，避免从 BB->begin() 线性查找（O(1) vs O(n)）
            BasicBlock::Iterator it(BI);
            it.insertBefore(newInst);
            BI->replaceAllUsesWith(newInst);
            delete BI;
            changed = true;
        } else if (BI->getOp() == BinaryInst::MOD) {
            // x % 2^n → x & (2^n - 1)（非负时等价；负数被除数时 srem 保留
            // 符号，AND 则无符号语义，所以同样依赖 lhs 非负 的前提）
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

// =====================================================================
// Step 2: 常量折叠 — 范围是单值时替换为 ConstInt / ConstFloat
// =====================================================================
static bool foldConstants(Function &F, RangeInfo &RI, DominatorTree &DT) {
    bool changed = false;

    // 整数 fold 候选 + 浮点 fold 候选分别收集
    std::vector<Instruction *> intToFold;
    std::vector<Instruction *> floatToFold;

    for (BasicBlock *BB : F) {
        for (Instruction *I : *BB) {
            if (!I->getIRType()) continue;
            // 跳过有副作用/内存副作用的指令（整数和浮点通用）：
            //   CallInst：delete 会丢掉副作用（典型：通过指针实参写数组）
            //   LoadInst：range 相等不等于值相等，且删除会丢掉依赖
            //   StoreInst：无返回值，保险加上
            // 只 fold 纯计算的结果（PHI / BinaryInst / Select / cast 等）
            if (dynamic_cast<CallInst *>(I)) continue;
            if (dynamic_cast<LoadInst *>(I)) continue;
            if (dynamic_cast<StoreInst *>(I)) continue;

            BasicBlock *block = I->ChainNode<BasicBlock, Instruction>::getParent();
            if (!block) continue;

            auto ty = I->getIRType()->getEnum();
            if (ty == IR_INT) {
                if (dynamic_cast<ConstInt *>(I)) continue;
                IntRange range = RI.query(I, block, DT);
                // 先看 IntRange 是不是单值；再退一步看 KB 是不是 32 位全锁定
                //   ConstantRange 和 KnownBits 是独立 lattice，能判的集合不完全重叠
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
            } else if (ty == IR_FLOAT) {
                if (dynamic_cast<ConstFloat *>(I)) continue;
                FloatRange range = RI.queryFloat(I, block, DT);
                // isSingleValue 已经排除了 ±inf（不是合法的 float 常量）
                if (range.isSingleValue()) {
                    floatToFold.push_back(I);
                }
            }
        }
    }

    for (Instruction *I : intToFold) {
        BasicBlock *block = I->ChainNode<BasicBlock, Instruction>::getParent();
        if (!block) continue;
        IntRange range = RI.query(I, block, DT);
        int cval;
        if (range.isSingleValue()) {
            cval = (int)range.min;
        } else {
            KnownBits kb = RI.queryBits(I, block, DT);
            if (!kb.isFullyKnown()) continue;
            cval = kb.getConstant();
        }
        I->replaceAllUsesWith(ConstInt::newConstInt(cval));
        delete I;
        changed = true;
    }

    for (Instruction *I : floatToFold) {
        BasicBlock *block = I->ChainNode<BasicBlock, Instruction>::getParent();
        if (!block) continue;
        FloatRange range = RI.queryFloat(I, block, DT);
        if (!range.isSingleValue()) continue;

        // double → float：我们的 IR ConstFloat 存 float（SysY 只有 f32）
        I->replaceAllUsesWith(ConstFloat::newConstFloat((float)range.min));
        delete I;
        changed = true;
    }

    return changed;
}

// =====================================================================
// Step 3: 比较折叠 — icmp 的两操作数范围不重叠时折叠为 true/false
// =====================================================================
static bool foldComparisons(Function &F, RangeInfo &RI, DominatorTree &DT) {
    bool changed = false;

    std::vector<BinaryInst *> cmps;
    for (BasicBlock *BB : F) {
        for (Instruction *I : *BB) {
            auto *BI = dynamic_cast<BinaryInst *>(I);
            if (!BI) continue;
            auto op = BI->getOp();
            if (op == BinaryInst::L || op == BinaryInst::LE ||
                op == BinaryInst::G || op == BinaryInst::GE ||
                op == BinaryInst::E || op == BinaryInst::NE)
                cmps.push_back(BI);
        }
    }

    for (BinaryInst *cmp : cmps) {
        auto &UL = cmp->getUserList();
        if (UL.size() < 2) continue;
        Value *lhs = UL[0]->getValue();
        Value *rhs = UL[1]->getValue();
        if (!lhs->getIRType() || !rhs->getIRType()) continue;

        BasicBlock *BB = cmp->ChainNode<BasicBlock, Instruction>::getParent();
        if (!BB) continue;

        auto opTy = lhs->getIRType()->getEnum();

        // 整数比较：BinaryInst 的 L/LE/G/GE/E/NE 整数浮点共用，靠操作数类型区分。
        // 整数路径走 IntRange，浮点路径走 FloatRange。
        if (opTy == IR_INT && rhs->getIRType()->getEnum() == IR_INT) {
            IntRange lRange = queryIntExpr(lhs, BB, RI, DT);
            IntRange rRange = queryIntExpr(rhs, BB, RI, DT);

            // —— KB 位级互斥判 eq/ne —— range 没法判但 KB 能判的情形
            //   lhs 和 rhs 的 KB 在某位上互斥（一侧 zero=1 而另一侧 one=1）
            //   → 绝不相等 → eq 恒 false、ne 恒 true
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

            // 任一操作数范围是 Top（全范围），折叠不了
            if (lRange.isTop() || rRange.isTop()) continue;

            int result = -1; // -1 = 未知, 0 = false, 1 = true

            switch (cmp->getOp()) {
                case BinaryInst::L:  // lhs < rhs
                    if (lRange.max < rRange.min) result = 1;
                    else if (lRange.min >= rRange.max) result = 0;
                    break;
                case BinaryInst::LE: // lhs <= rhs
                    if (lRange.max <= rRange.min) result = 1;
                    else if (lRange.min > rRange.max) result = 0;
                    break;
                case BinaryInst::G:  // lhs > rhs
                    if (lRange.min > rRange.max) result = 1;
                    else if (lRange.max <= rRange.min) result = 0;
                    break;
                case BinaryInst::GE: // lhs >= rhs
                    if (lRange.min >= rRange.max) result = 1;
                    else if (lRange.max < rRange.min) result = 0;
                    break;
                case BinaryInst::E:  // lhs == rhs
                    if (lRange.max < rRange.min || lRange.min > rRange.max) result = 0;
                    else if (lRange.isSingleValue() && rRange.isSingleValue() &&
                             lRange.min == rRange.min) result = 1;
                    break;
                case BinaryInst::NE: // lhs != rhs
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

        // 浮点比较（fcmp uXX）。FloatRange 的 Top 表示"不追踪/NaN 污染"，
        // 非 Top 时假定操作数不含 NaN（NaN 已被吸收到 Top）。
        // 对不含 NaN 的操作数，fcmp uXX 的语义等价于 olt/ole/ogt/oge/oeq/one
        // 区间比较，可以像整数一样按端点判定。
        if (opTy == IR_FLOAT && rhs->getIRType()->getEnum() == IR_FLOAT) {
            FloatRange lRange = RI.queryFloat(lhs, BB, DT);
            FloatRange rRange = RI.queryFloat(rhs, BB, DT);

            // 任一 Top（可能含 NaN 或无界）→ 保守不折
            if (lRange.isTop() || rRange.isTop()) continue;

            int result = -1;

            switch (cmp->getOp()) {
                case BinaryInst::L:  // lhs < rhs
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

// =====================================================================
// Helper: 用 range 判断整数 cmp 的结果（-1 = 未知, 0 = false, 1 = true）
// 抽自 foldComparisons 的核心表，供 select 相关 pattern 复用。
// 要求 lhs / rhs 都是 IR_INT 且不是 Bottom / Top（Top 返回 -1）。
// =====================================================================
static int evalIntCmp(BinaryInst::Operation op, const IntRange &l, const IntRange &r) {
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

// 整数 cmp op 的意义（返回 true 表示这是我们处理的 6 种之一）
static bool isIntCmpOp(BinaryInst::Operation op) {
    return op == BinaryInst::L || op == BinaryInst::LE ||
           op == BinaryInst::G || op == BinaryInst::GE ||
           op == BinaryInst::E || op == BinaryInst::NE;
}

// =====================================================================
// Step 3b: icmp(select(c, a, b), x) 下推 —— 双侧都能 range-fold 时折成常量/c
//
// 我们只处理"两个分支的 cmp 结果都能被 range 定住"的强情形，避免弱情形
// 引入新 select 导致指令增多。
//
// 结果矩阵：
//   两侧同值 true  → 整个 cmp 折成 true
//   两侧同值 false → 整个 cmp 折成 false
//   true/false 异  → cmp 折成 cond 或 cond 的否定（具体看 true 在哪一支）
//                    "true 在 true 支"时 cmp ≡ cond，"true 在 false 支"时
//                    cmp ≡ !cond —— 后者我们不新造 xor 1，直接跳过保守
// =====================================================================
static bool foldIcmpOfSelect(Function &F, RangeInfo &RI, DominatorTree &DT) {
    bool changed = false;

    std::vector<BinaryInst *> cmps;
    for (BasicBlock *BB : F) {
        for (Instruction *I : *BB) {
            auto *BI = dynamic_cast<BinaryInst *>(I);
            if (BI && isIntCmpOp(BI->getOp())) cmps.push_back(BI);
        }
    }

    for (BinaryInst *cmp : cmps) {
        auto &UL = cmp->getUserList();
        if (UL.size() < 2) continue;
        Value *lhs = UL[0]->getValue();
        Value *rhs = UL[1]->getValue();
        if (!lhs->getIRType() || !rhs->getIRType()) continue;
        if (lhs->getIRType()->getEnum() != IR_INT) continue;
        if (rhs->getIRType()->getEnum() != IR_INT) continue;

        BasicBlock *BB = cmp->ChainNode<BasicBlock, Instruction>::getParent();
        if (!BB) continue;

        // 只支持 lhs 是 select 的场景；rhs-是-select 可对称实现但概率低，先省
        auto *sel = dynamic_cast<SelectInst *>(lhs);
        if (!sel) continue;
        auto &selUL = sel->getUserList();
        if (selUL.size() < 3) continue;
        Value *tv = selUL[1]->getValue();
        Value *fv = selUL[2]->getValue();
        if (!tv->getIRType() || tv->getIRType()->getEnum() != IR_INT) continue;
        if (!fv->getIRType() || fv->getIRType()->getEnum() != IR_INT) continue;

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
            // 两侧一致 → 直接常量
            cmp->replaceAllUsesWith(ConstBool::newConstBool(resT == 1));
            delete cmp;
            changed = true;
        } else if (resT == 1 && resF == 0) {
            // true 支 true、false 支 false → cmp ≡ select 的 cond
            Value *cond = selUL[0]->getValue();
            cmp->replaceAllUsesWith(cond);
            delete cmp;
            changed = true;
        }
        // resT==0 && resF==1：cmp ≡ !cond，要造 xor 指令才能表达，跳过
    }

    return changed;
}

// =====================================================================
// Step 3b.5: select 条件被 range/known-bits 判成恒真/恒假时直接选边
//
// 比较条件通常会被 foldComparisons 先折掉；这里补的是非直接比较条件，
// 例如经过 zext/trunc/xor/phi 之后 RangeAnalysis 仍能证明 cond != 0。
// =====================================================================
static bool foldSelectByConditionRange(Function &F, RangeInfo &RI, DominatorTree &DT) {
    bool changed = false;

    std::vector<SelectInst *> candidates;
    for (BasicBlock *BB : F) {
        for (Instruction *I : *BB) {
            if (auto *SI = dynamic_cast<SelectInst *>(I))
                candidates.push_back(SI);
        }
    }

    for (SelectInst *SI : candidates) {
        auto &UL = SI->getUserList();
        if (UL.size() < 3) continue;
        Value *cond = UL[0]->getValue();
        Value *tv = UL[1]->getValue();
        Value *fv = UL[2]->getValue();
        if (!cond || !tv || !fv) continue;

        BasicBlock *BB = SI->ChainNode<BasicBlock, Instruction>::getParent();
        if (!BB) continue;
        int truth = evalTruth(cond, BB, RI, DT);
        if (truth < 0) continue;

        SI->replaceAllUsesWith(truth ? tv : fv);
        delete SI;
        changed = true;
    }

    return changed;
}

// =====================================================================
// Step 3c: abs(x) 手写 pattern 在 x 非负时折回 x
//
// 识别：
//   select(cmp(x, 0, L|LE), sub(0, x), x)     —— "if x<0 then -x else x"
//   select(cmp(x, 0, GE|G), x, sub(0, x))     —— "if x>=0 then x else -x"
//   select(cmp(x, 0, E), x, x) 之类同值的不用管，InstSimplify 早清了
//
// 证据：RangeAnalysis 在 select 所在块给出 x 的 range：
//   非负 → abs(x) = x
//   非正 → abs(x) = 已有的 sub(0, x)
// =====================================================================
static bool foldAbsViaSelect(Function &F, RangeInfo &RI, DominatorTree &DT) {
    bool changed = false;

    std::vector<SelectInst *> candidates;
    for (BasicBlock *BB : F) {
        for (Instruction *I : *BB) {
            if (auto *SI = dynamic_cast<SelectInst *>(I)) {
                if (SI->getIRType() && SI->getIRType()->getEnum() == IR_INT)
                    candidates.push_back(SI);
            }
        }
    }

    auto matchNeg = [](Value *v) -> Value * {
        auto *BI = dynamic_cast<BinaryInst *>(v);
        if (!BI || BI->getOp() != BinaryInst::SUB) return nullptr;
        auto &UL = BI->getUserList();
        if (UL.size() < 2) return nullptr;
        auto *c = dynamic_cast<ConstInt *>(UL[0]->getValue());
        if (!c || c->getVal() != 0) return nullptr;
        return UL[1]->getValue();
    };

    for (SelectInst *SI : candidates) {
        auto &selUL = SI->getUserList();
        if (selUL.size() < 3) continue;
        auto *cmp = dynamic_cast<BinaryInst *>(selUL[0]->getValue());
        if (!cmp || !isIntCmpOp(cmp->getOp())) continue;
        auto &cmpUL = cmp->getUserList();
        if (cmpUL.size() < 2) continue;

        Value *cmpLhs = cmpUL[0]->getValue();
        auto *cmpRhs = dynamic_cast<ConstInt *>(cmpUL[1]->getValue());
        if (!cmpRhs || cmpRhs->getVal() != 0) continue;

        Value *tv = selUL[1]->getValue();
        Value *fv = selUL[2]->getValue();

        // 判断是哪种 abs 写法
        Value *x = nullptr;
        Value *neg = nullptr;
        bool isAbs = false;
        auto op = cmp->getOp();

        if (op == BinaryInst::L || op == BinaryInst::LE) {
            // 期望：tv = -x，fv = x，cmpLhs = x
            if (matchNeg(tv) == cmpLhs && fv == cmpLhs) {
                x = cmpLhs; neg = tv; isAbs = true;
            }
        } else if (op == BinaryInst::GE || op == BinaryInst::G) {
            // 期望：tv = x，fv = -x，cmpLhs = x
            if (tv == cmpLhs && matchNeg(fv) == cmpLhs) {
                x = cmpLhs; neg = fv; isAbs = true;
            }
        }

        if (!isAbs) continue;

        BasicBlock *BB = SI->ChainNode<BasicBlock, Instruction>::getParent();
        if (!BB) continue;
        IntRange xr = queryIntExpr(x, BB, RI, DT);
        Value *chosen = nullptr;
        if (xr.isNonNegative())
            chosen = x;
        else if (!xr.isTop() && !xr.isBottom() && xr.max <= 0)
            chosen = neg;
        if (!chosen) continue;

        SI->replaceAllUsesWith(chosen);
        delete SI;
        changed = true;
    }

    return changed;
}

// =====================================================================
// Step 3d: smax/smin 手写 pattern 在 range dominate 时折成单边
//
// 识别：select(cmp(a, b, op), a, b) 或 select(cmp(a, b, op), b, a)
//   op ∈ {G, GE}：返回较大值 = smax(a, b) 或 smin(a, b)
//   op ∈ {L, LE}：返回较小值 = smin(a, b) 或 smax(a, b)
//
// Range dominate：
//   a.min >= b.max → a >= b 恒成立 → smax→a, smin→b
//   b.min >= a.max → b >= a 恒成立 → smax→b, smin→a
// =====================================================================
static bool foldMinMaxViaSelect(Function &F, RangeInfo &RI, DominatorTree &DT) {
    bool changed = false;

    std::vector<SelectInst *> candidates;
    for (BasicBlock *BB : F) {
        for (Instruction *I : *BB) {
            if (auto *SI = dynamic_cast<SelectInst *>(I)) {
                if (SI->getIRType() && SI->getIRType()->getEnum() == IR_INT)
                    candidates.push_back(SI);
            }
        }
    }

    for (SelectInst *SI : candidates) {
        auto &selUL = SI->getUserList();
        if (selUL.size() < 3) continue;
        auto *cmp = dynamic_cast<BinaryInst *>(selUL[0]->getValue());
        if (!cmp || !isIntCmpOp(cmp->getOp())) continue;
        auto &cmpUL = cmp->getUserList();
        if (cmpUL.size() < 2) continue;

        Value *a = cmpUL[0]->getValue();
        Value *b = cmpUL[1]->getValue();
        Value *tv = selUL[1]->getValue();
        Value *fv = selUL[2]->getValue();

        // 必须 tv/fv 来自 {a, b} 的排列，否则不是 min/max pattern
        bool straight = (tv == a && fv == b);  // select(cmp, a, b)
        bool flipped  = (tv == b && fv == a);  // select(cmp, b, a)
        if (!straight && !flipped) continue;

        auto op = cmp->getOp();
        if (op != BinaryInst::L && op != BinaryInst::LE &&
            op != BinaryInst::G && op != BinaryInst::GE) continue;

        BasicBlock *BB = SI->ChainNode<BasicBlock, Instruction>::getParent();
        if (!BB) continue;
        IntRange ra = dynamic_cast<ConstInt *>(a) ?
            IntRange(dynamic_cast<ConstInt *>(a)->getVal()) : queryIntExpr(a, BB, RI, DT);
        IntRange rb = dynamic_cast<ConstInt *>(b) ?
            IntRange(dynamic_cast<ConstInt *>(b)->getVal()) : queryIntExpr(b, BB, RI, DT);
        if (ra.isTop() || rb.isTop()) continue;

        // 决定 cmp(a, b, op) 在当前 range 下是不是可判定的常数：
        // 必须区分严格（G/L）和非严格（GE/LE），a==b 时两者结论不同
        //   G  (a > b) : a.min >  b.max 恒 true；a.max <= b.min 恒 false
        //   GE (a>=b)  : a.min >= b.max 恒 true；a.max <  b.min 恒 false
        //   L  (a < b) : a.max <  b.min 恒 true；a.min >= b.max 恒 false
        //   LE (a<=b)  : a.max <= b.min 恒 true；a.min >  b.max 恒 false
        int cmpResult = -1; // 0=false 恒定 / 1=true 恒定 / -1=不确定
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

        // cmp 恒定后 select 选哪支：cond=true 走 tv、cond=false 走 fv
        Value *chosen = (cmpResult == 1) ? tv : fv;
        SI->replaceAllUsesWith(chosen);
        delete SI;
        changed = true;
    }

    return changed;
}

// =====================================================================
// Step 4: srem → urem 转换
// =====================================================================
static bool convertSignedToUnsigned(Function &F, RangeInfo &RI, DominatorTree &DT) {
    bool changed = false;

    std::vector<BinaryInst *> srems;
    for (BasicBlock *BB : F) {
        for (Instruction *I : *BB) {
            auto *BI = dynamic_cast<BinaryInst *>(I);
            if (!BI || BI->getOp() != BinaryInst::MOD) continue;
            srems.push_back(BI);
        }
    }

    for (BinaryInst *srem : srems) {
        auto &UL = srem->getUserList();
        if (UL.size() < 2) continue;
        Value *lhs = UL[0]->getValue();
        Value *rhs = UL[1]->getValue();

        BasicBlock *BB = srem->ChainNode<BasicBlock, Instruction>::getParent();
        if (!BB) continue;

        IntRange lRange = RI.query(lhs, BB, DT);
        IntRange rRange = RI.query(rhs, BB, DT);

        // 被除数非负且除数为正 → srem 等价于 urem
        // 我们的 IR 只有 MOD（signed），没有单独的 urem
        // 这个转换的意义是：后端可以看到操作数非负，生成更快的指令
        // 当前 IR 没有 UREM 指令，所以暂不做实际替换，
        // 但可以在这里设置一个 flag 或者做其他后端相关的标记
        // TODO: 如果后端区分 srem/urem，在此处添加转换
        (void)lRange;
        (void)rRange;
    }

    return changed;
}

} // namespace

PreservationStatus RangeAwareSimplifyPass::run(Function &F,
                                                FunctionAnalysisPassManager &FAM) {
    auto &RI = FAM.getResult<RangeAnalysis>(F);
    auto &DT = FAM.getResult<DominantAnalysis>(F);

    bool changed = false;


    changed |= foldConstants(F, RI, DT);
    changed |= foldComparisons(F, RI, DT);
    changed |= foldIcmpOfSelect(F, RI, DT);
    changed |= foldSelectByConditionRange(F, RI, DT);
    changed |= foldAbsViaSelect(F, RI, DT);
    changed |= foldMinMaxViaSelect(F, RI, DT);
    changed |= strengthReduce(F, RI, DT);
    changed |= convertSignedToUnsigned(F, RI, DT);

    if (!changed) return PreservationStatus::all();

    // 可能替换了比较指令（影响分支），但没有直接改 CFG 结构
    // 死分支的 br(true/false) 由后续 CFGSimplify 清理
    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<LoopAnalysis>();
    return PA;
}
