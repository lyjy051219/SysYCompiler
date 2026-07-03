#include "ir/opt/LateStrengthReduce.hpp"

#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/analysis/SideEffect.hpp"

#include <cstdint>
#include <unordered_set>
#include <vector>

namespace {

static bool isIntType(const std::shared_ptr<IRType> &Ty) {
    return Ty && Ty->getEnum() == IR_INT;
}

static bool isPowerOf2(int32_t Val) {
    if (Val <= 0) return false;
    auto U = static_cast<uint32_t>(Val);
    return (U & (U - 1)) == 0;
}

static bool isUnsignedPowerOf2(int32_t Val) {
    auto U = static_cast<uint32_t>(Val);
    return U != 0 && ((U & (U - 1)) == 0);
}

static int log2u(uint32_t Val) {
    int N = 0;
    while (Val > 1) {
        Val >>= 1;
        ++N;
    }
    return N;
}

static int log2i(int32_t Val) {
    return log2u(static_cast<uint32_t>(Val));
}

static bool getIntConst(Value *V, int32_t &Out) {
    if (auto *CI = dynamic_cast<ConstInt *>(V)) {
        Out = CI->getVal();
        return true;
    }
    if (auto *CB = dynamic_cast<ConstBool *>(V)) {
        Out = CB->getVal() ? 1 : 0;
        return true;
    }
    return false;
}

static bool getBinaryOperands(BinaryInst *BI, Value *&LHS, Value *&RHS) {
    LHS = nullptr;
    RHS = nullptr;
    if (!BI) return false;
    auto &UL = BI->getUserList();
    if (UL.size() < 2) return false;
    LHS = UL[0]->getValue();
    RHS = UL[1]->getValue();
    return LHS && RHS;
}

static bool isGuaranteedNonNegativeImpl(Value *V, int Depth,
                                        std::unordered_set<Value *> &Visiting) {
    if (!V || !isIntType(V->getIRType())) return false;

    int32_t C = 0;
    if (getIntConst(V, C)) return C >= 0;

    if (Depth <= 0 || Visiting.count(V)) return false;
    Visiting.insert(V);

    if (dynamic_cast<ZextInst *>(V)) {
        Visiting.erase(V);
        return true;
    }

    if (auto *SI = dynamic_cast<SelectInst *>(V)) {
        bool Result = false;
        auto &UL = SI->getUserList();
        if (UL.size() == 3) {
            Value *TV = UL[1]->getValue();
            Value *FV = UL[2]->getValue();
            Result = isGuaranteedNonNegativeImpl(TV, Depth - 1, Visiting) &&
                     isGuaranteedNonNegativeImpl(FV, Depth - 1, Visiting);
        }
        Visiting.erase(V);
        return Result;
    }

    if (auto *BI = dynamic_cast<BinaryInst *>(V)) {
        bool Result = false;
        Value *LHS = nullptr;
        Value *RHS = nullptr;
        if (!getBinaryOperands(BI, LHS, RHS)) {
            Visiting.erase(V);
            return false;
        }

        int32_t RC = 0;
        switch (BI->getOp()) {
            case BinaryInst::AND:
                if (getIntConst(RHS, RC) &&
                    (static_cast<uint32_t>(RC) & 0x80000000u) == 0) {
                    Result = true;
                } else if (getIntConst(LHS, RC) &&
                           (static_cast<uint32_t>(RC) & 0x80000000u) == 0) {
                    Result = true;
                }
                break;
            case BinaryInst::LSHR:
                if (getIntConst(RHS, RC)) {
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
                Result = getIntConst(RHS, RC) && RC > 0 &&
                         isGuaranteedNonNegativeImpl(LHS, Depth - 1, Visiting);
                break;
            case BinaryInst::UREM:
                Result = getIntConst(RHS, RC) && RC > 0;
                break;
            default:
                break;
        }
        Visiting.erase(V);
        return Result;
    }

    Visiting.erase(V);
    return false;
}

static bool isGuaranteedNonNegative(Value *V) {
    std::unordered_set<Value *> Visiting;
    return isGuaranteedNonNegativeImpl(V, 4, Visiting);
}

static bool reduceFunction(Function &F) {
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
        if (!getBinaryOperands(BI, LHS, RHS)) continue;
        if (!isIntType(LHS->getIRType()) || !isIntType(RHS->getIRType()))
            continue;

        int32_t Divisor = 0;
        if (!getIntConst(RHS, Divisor)) continue;

        Instruction *NewI = nullptr;
        if (BI->getOp() == BinaryInst::UDIV ||
            BI->getOp() == BinaryInst::UREM) {
            if (!isUnsignedPowerOf2(Divisor)) continue;

            auto UDivisor = static_cast<uint32_t>(Divisor);
            if (BI->getOp() == BinaryInst::UDIV) {
                NewI = new BinaryInst(
                    LHS, BinaryInst::LSHR,
                    ConstInt::newConstInt(log2u(UDivisor)));
            } else {
                NewI = new BinaryInst(
                    LHS, BinaryInst::AND,
                    ConstInt::newConstInt(
                        static_cast<int32_t>(UDivisor - 1u)));
            }

            BasicBlock::Iterator It(BI);
            It.insertBefore(NewI);
            BI->replaceAllUsesWith(NewI);
            delete BI;
            Changed = true;
            continue;
        }

        if (!isPowerOf2(Divisor)) continue;
        if (!isGuaranteedNonNegative(LHS)) continue;

        if (BI->getOp() == BinaryInst::DIV) {
            NewI = new BinaryInst(LHS, BinaryInst::ASHR,
                                  ConstInt::newConstInt(log2i(Divisor)));
        } else {
            NewI = new BinaryInst(LHS, BinaryInst::AND,
                                  ConstInt::newConstInt(Divisor - 1));
        }
        BasicBlock::Iterator It(BI);
        It.insertBefore(NewI);
        BI->replaceAllUsesWith(NewI);
        delete BI;
        Changed = true;
    }

    return Changed;
}

} // namespace

PreservationStatus LateStrengthReducePass::run(
    Function &F, FunctionAnalysisPassManager &FAM) {
    (void)FAM;

    bool Changed = reduceFunction(F);
    if (!Changed) return PreservationStatus::all();

    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<PostDominantAnalysis>();
    PA.keep<LoopAnalysis>();
    PA.discard<RangeAnalysis>();
    PA.discard<BasicAliasAnalysis>();
    PA.discard<SideEffectAnalysis>();
    return PA;
}
