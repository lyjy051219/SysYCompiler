#include "ir/opt/LateStrengthReduce.hpp"

#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "lib/IRUtils.hpp"

#include <cstdint>
#include <unordered_set>
#include <vector>

bool LateStrengthReducePass::insertSignedDivPow2Sequence(
    BinaryInst *BI, Value *LHS, int32_t Divisor,
    Instruction *&Quotient) {
    // 3.1 对可能为负的有符号除法构造 (x + ((x>>31)&(d-1))) >> log2(d)，
    // 用符号相关偏置把算术右移的向下取整修正为 sdiv 的向零取整。
    Quotient = nullptr;
    if (!BI || !LHS || !IRMath::isPowerOf2(Divisor)) return false;

    int Shift = IRMath::log2i(Divisor);
    if (Shift == 0) return false;

    int SignShift = LHS->integerBitWidth() - 1;
    auto *Sign = new BinaryInst(LHS, BinaryInst::ASHR,
                                ConstInt::newConstInt(SignShift));
    auto *Bias = new BinaryInst(Sign, BinaryInst::AND,
                                ConstInt::newConstInt(Divisor - 1));
    auto *Biased = new BinaryInst(LHS, BinaryInst::ADD, Bias);
    auto *Q = new BinaryInst(Biased, BinaryInst::ASHR,
                             ConstInt::newConstInt(Shift));

    // 四条指令全部插入成功后才返回商；任一步失败都删除临时节点，不留下半条序列。
    if (!BI->insertBefore(Sign) || !BI->insertBefore(Bias) ||
        !BI->insertBefore(Biased) || !BI->insertBefore(Q)) {
        delete Sign;
        delete Bias;
        delete Biased;
        delete Q;
        return false;
    }

    Quotient = Q;
    return true;
}

bool LateStrengthReducePass::isGuaranteedNonNegativeImpl(
    Value *V, int Depth, std::unordered_set<Value *> &Visiting) {
    // 1.1 有界递归识别常量、zext、两臂均非负的 select，以及能保住符号位的
    // AND/移位/除余模式；递归环和未覆盖操作一律返回未知。
    if (!V || !V->getIRType() || !V->getIRType()->isInt()) return false;

    int32_t C = 0;
    if (V->tryGetIntConst(C)) return C >= 0;

    if (Depth <= 0) return false;
    ScopedActiveMark VisitingMark(Visiting, V);
    if (!VisitingMark.inserted()) return false;

    if (dynamic_cast<ZextInst *>(V)) {
        return true;
    }

    if (auto *SI = dynamic_cast<SelectInst *>(V)) {
        bool Result = false;
        if (SI->getNumOperands() == 3) {
            Value *TV = SI->getTrueValue();
            Value *FV = SI->getFalseValue();
            Result = isGuaranteedNonNegativeImpl(TV, Depth - 1, Visiting) &&
                     isGuaranteedNonNegativeImpl(FV, Depth - 1, Visiting);
        }
        return Result;
    }

    if (auto *BI = dynamic_cast<BinaryInst *>(V)) {
        bool Result = false;
        Value *LHS = nullptr;
        Value *RHS = nullptr;
        if (!BI->getOperands(LHS, RHS)) {
            return false;
        }

        int32_t RC = 0;
        switch (BI->getOp()) {
            case BinaryInst::AND:
                if (RHS->tryGetIntConst(RC) &&
                    (static_cast<uint32_t>(RC) & 0x80000000u) == 0) {
                    Result = true;
                } else if (LHS->tryGetIntConst(RC) &&
                           (static_cast<uint32_t>(RC) & 0x80000000u) == 0) {
                    Result = true;
                }
                break;
            case BinaryInst::LSHR:
                if (RHS->tryGetIntConst(RC)) {
                    Result = RC > 0 && RC < 32;
                    if (RC == 0)
                        Result = isGuaranteedNonNegativeImpl(LHS, Depth - 1,
                                                             Visiting);
                }
                break;
            case BinaryInst::ASHR:
                Result = isGuaranteedNonNegativeImpl(LHS, Depth - 1, Visiting);
                break;
            case BinaryInst::DIV:
            case BinaryInst::MOD:
                Result = RHS->tryGetIntConst(RC) && RC > 0 &&
                         isGuaranteedNonNegativeImpl(LHS, Depth - 1, Visiting);
                break;
            case BinaryInst::UREM:
                Result = RHS->tryGetIntConst(RC) && RC > 0;
                break;
            default:
                break;
        }
        return Result;
    }

    return false;
}

bool LateStrengthReducePass::isGuaranteedNonNegative(Value *V) {
    std::unordered_set<Value *> Visiting;
    return isGuaranteedNonNegativeImpl(V, 4, Visiting);
}

bool LateStrengthReducePass::reduceFunction(Function &F) {
    // 1. 快照所有整数除法/余数候选，后续删除原指令不会破坏函数遍历。
    bool Changed = false;
    std::vector<BinaryInst *> Candidates;

    for (BasicBlock *BB : F) {
        if (!BB) continue;
        for (Instruction *I : *BB) {
            auto *BI = dynamic_cast<BinaryInst *>(I);
            if (!BI) continue;
            if (BI->getOp() == BinaryInst::DIV ||
                BI->getOp() == BinaryInst::MOD ||
                BI->getOp() == BinaryInst::UDIV ||
                BI->getOp() == BinaryInst::UREM)
                Candidates.push_back(BI);
        }
    }

    for (BinaryInst *BI : Candidates) {
        if (!BI || !BI->getParent()) continue;

        Value *LHS = nullptr;
        Value *RHS = nullptr;
        if (!BI->getOperands(LHS, RHS)) continue;
        if (!LHS->getIRType() || !LHS->getIRType()->isInt() ||
            !RHS->getIRType() || !RHS->getIRType()->isInt())
            continue;

        if (LHS->integerBitWidth(0) != 32 ||
            RHS->integerBitWidth(0) != 32)
            continue;

        int32_t Divisor = 0;
        if (!RHS->tryGetIntConst(Divisor)) continue;

        Instruction *NewI = nullptr;
        // 2. 无符号 2 次幂除数可无条件改为逻辑右移或低位掩码。
        if (BI->getOp() == BinaryInst::UDIV ||
            BI->getOp() == BinaryInst::UREM) {
            if (!IRMath::isUnsignedPowerOf2(static_cast<uint32_t>(Divisor)))
                continue;

            auto UDivisor = static_cast<uint32_t>(Divisor);
            if (BI->getOp() == BinaryInst::UDIV) {
                NewI = new BinaryInst(
                    LHS, BinaryInst::LSHR,
                    ConstInt::newConstInt(IRMath::log2u(UDivisor)));
            } else {
                NewI = new BinaryInst(
                    LHS, BinaryInst::AND,
                    ConstInt::newConstInt(
                        static_cast<int32_t>(UDivisor - 1u)));
            }

            BI->insertBefore(NewI);
            BI->replaceAllUsesWith(NewI);
            delete BI;
            Changed = true;
            continue;
        }

        if (!IRMath::isPowerOf2(Divisor)) continue;

        // 3. 有符号候选在被除数已证明非负时使用算术右移/掩码，此时与 sdiv/srem
        // 的符号和舍入语义一致。
        if (isGuaranteedNonNegative(LHS)) {
            if (BI->getOp() == BinaryInst::DIV) {
                NewI = new BinaryInst(LHS, BinaryInst::ASHR,
                                      ConstInt::newConstInt(IRMath::log2i(Divisor)));
            } else {
                NewI = new BinaryInst(LHS, BinaryInst::AND,
                                      ConstInt::newConstInt(Divisor - 1));
            }
            BI->insertBefore(NewI);
            BI->replaceAllUsesWith(NewI);
            delete BI;
            Changed = true;
            continue;
        }

        // 3.1 非负证明失败时只为除以 2 使用带偏置序列；其他除数及 srem 保守保留。
        if (BI->getOp() != BinaryInst::DIV || Divisor != 2) continue;

        Instruction *Q = nullptr;
        if (!insertSignedDivPow2Sequence(BI, LHS, Divisor, Q)) continue;

        BI->replaceAllUsesWith(Q);
        delete BI;
        Changed = true;
    }

    return Changed;
}

PreservationStatus LateStrengthReducePass::run(
    Function &F, FunctionAnalysisPassManager &FAM) {
    (void)FAM;

    // 1—3. 在后端前统一降低仍残留的常量 2 次幂除余指令。
    bool Changed = reduceFunction(F);
    if (!Changed) return PreservationStatus::all();

    // 4. CFG 不变，保留结构分析；新的算术图使范围、别名与副作用相关缓存失效。
    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<PostDominantAnalysis>();
    PA.keep<LoopAnalysis>();
    PA.discard<RangeAnalysis>();
    PA.discard<BasicAliasAnalysis>();
    PA.discard<SideEffectAnalysis>();
    return PA;
}
