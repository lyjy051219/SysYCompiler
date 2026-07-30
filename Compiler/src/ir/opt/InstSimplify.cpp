#include "ir/opt/InstSimplify.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/MemorySSA.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "lib/IRUtils.hpp"
#include <cmath>
#include <cstdint>
#include <unordered_set>
#include <vector>
//匹配给定操作码的二元表达式并返回两个操作数
bool InstSimplifyPass::getBinaryOperandsForOp(
    Value *V, BinaryInst::Operation Op, Value *&L, Value *&R) {
    auto *BI = dynamic_cast<BinaryInst *>(V);
    if (!BI || BI->getOp() != Op) return false;
    return BI->getOperands(L, R);
}
//匹配零减某值表示的算术取负
bool InstSimplifyPass::matchNegation(Value *V, Value *&Y) {
    //2.1统一识别整数 0-Y；浮点负号受 NaN 与有符号零约束，不进入整数恒等式匹配。
    if (!V || !IRType::isInt(V->getIRType())) return false;
    Value *L = nullptr;
    Value *R = nullptr;
    if (!InstSimplifyPass::getBinaryOperandsForOp(V, BinaryInst::SUB, L, R)) return false;
    if (!Value::isConstZero(L)) return false;
    Y = R;
    return Y != nullptr;
}
//匹配任意减法表达式并返回两个操作数
bool InstSimplifyPass::matchSub(Value *V, Value *&LHS, Value *&RHS) {
    auto *BI = dynamic_cast<BinaryInst *>(V);
    if (!BI || BI->getOp() != BinaryInst::SUB) return false;
    return BI->getOperands(LHS, RHS);
}
//判断两个值是否构成互为算术相反数的表达式
bool InstSimplifyPass::isNegationPair(Value *A, Value *B) {
    if (!A || !B) return false;
    if (!IRType::isInt(A->getIRType()) || !IRType::isInt(B->getIRType())) return false;
    int32_t AI = 0, BI = 0;
    if (Value::tryGetIntConst(A, AI) && Value::tryGetIntConst(B, BI)) {
        return static_cast<int64_t>(AI) == -static_cast<int64_t>(BI);
    }
    Value *SL = nullptr, *SR = nullptr;
    if (InstSimplifyPass::matchSub(B, SL, SR) && Value::isConstZero(SL) && SR == A) return true;
    if (InstSimplifyPass::matchSub(A, SL, SR) && Value::isConstZero(SL) && SR == B) return true;
    return false;
}
//匹配包含指定X的Y-X表达式并返回Y
bool InstSimplifyPass::matchYMinusX(Value *MaybeSub, Value *X, Value *&Y) {
    Value *SubL = nullptr;
    Value *SubR = nullptr;
    if (!InstSimplifyPass::matchSub(MaybeSub, SubL, SubR)) return false;
    if (SubR != X) return false;
    Y = SubL;
    return Y != nullptr;
}
//构造绑定函数及支配、别名、循环和MemorySSA结果的局部化简器
InstSimplifier::InstSimplifier(
    DominatorTree *DT, Function *F, FunctionAnalysisPassManager *FAM,
    BasicAAResult *AA, LoopInfo *LI, MemorySSAResult *MSSA)
    : DT(DT), F(F), FAM(FAM), AA(AA), LI(LI), MSSA(MSSA) {}
//判断Load是否能够安全下沉到PHI各输入边
bool InstSimplifier::isLoadSuitableForSinking(LoadInst *L) const {
    //5.1验证分支 load 能否下沉到汇合块：优先比较该load点与所在前驱BlockExit
    //的目标位置clobber，只有每条反向路径都透明才接受；旧MemorySSA失效或不能
    //完成证明时退回逐指令ModRef，同时始终保留timer顺序屏障检查。
    if (!L || !FAM || !AA || !LI) return false;
    Value *Ptr = L->getPointerOperand();
    if (!Ptr) return false;
    BasicBlock *BB = L->getParent();
    if (!BB) return false;
    MemoryLocation LoadLocation = MemoryLocation::get(L);
    bool MemorySSAProvesTransparent = false;
    if (MemorySSAUsable && MSSA) {
        MemoryAccess *BlockExit = MSSA->getBlockExit(BB);
        MemorySSAProvesTransparent =
            BlockExit &&
            !MSSA->hasClobberBetween(
                L, BlockExit, LoadLocation, *AA, *LI, *FAM, 256);
    }
    bool SeenLoad = false;
    for (auto *I : *BB) {
        if (!SeenLoad) {
            SeenLoad = (I == L);
            continue;
        }
        if (auto *CI = dynamic_cast<CallInst *>(I);
            CI && CI->mayReachTimingCall()) {
            return false;
        }
        if (!MemorySSAProvesTransparent) {
            ModRefInfo MRI = getInstModRefInfo(
                LoadLocation, I, *AA, *LI, *FAM);
            if (isModSet(MRI)) return false;
        }
    }
    if (auto *Alloca = dynamic_cast<AllocaInst *>(Ptr)) {
        bool AddressTaken = false;
        for (Use *U : Alloca->getUses()) {
            auto *UserI = dynamic_cast<Instruction *>(U->getUser());
            if (!UserI) {
                AddressTaken = true;
                break;
            }
            if (dynamic_cast<LoadInst *>(UserI)) continue;
            if (auto *S = dynamic_cast<StoreInst *>(UserI)) {
                if (S->getPointerOperand() == Alloca) continue;
            }
            AddressTaken = true;
            break;
        }
        if (!AddressTaken) return false;
    }
    if (auto *GEP = dynamic_cast<GetElementPtrInst *>(Ptr)) {
        if (GEP->hasOnlyConstantIndices()) return false;
    }
    return true;
}
//尝试把二元指令跨PHI各输入臂分解并折叠
Value *InstSimplifier::foldPhiOfBinary(PhiInst *Phi) {
    //1.验证PHI形状和计时屏障，任何输入不一致时保守回退
    //2.寻找所有输入二元式的公共操作数，并要求公共定义支配汇合点
    //3.变化项先合成为新PHI，再在PHI区后重建一次等价运算以维护SSA顺序
    if (!Phi) return nullptr;
    if (!F || F->hasTimingBarrierOnPath(Phi, Phi)) return nullptr;
    const unsigned N = Phi->getNumIncomingValues();
    if (N < 2) return nullptr;
    auto *First = dynamic_cast<BinaryInst *>(Phi->getIncomingValue(0));
    if (!First) return nullptr;
    Value *FL = nullptr;
    Value *FR = nullptr;
    if (!BinaryInst::getOperands(First, FL, FR)) return nullptr;
    BinaryInst::Operation Op = First->getOp();
    Value *CommonL = FL;
    Value *CommonR = FR;
    for (unsigned i = 0; i < N; ++i) {
        auto *Bin = dynamic_cast<BinaryInst *>(Phi->getIncomingValue(i));
        if (!Bin || Bin->getOp() != Op) return nullptr;
        if (!F || F->hasTimingBarrierOnPath(Bin, Phi)) return nullptr;
        Value *L = nullptr;
        Value *R = nullptr;
        if (!BinaryInst::getOperands(Bin, L, R)) return nullptr;
        if (CommonL && CommonL != L) CommonL = nullptr;
        if (CommonR && CommonR != R) CommonR = nullptr;
        if (!CommonL && !CommonR) return nullptr;
    }
    //两侧都相同仍需在汇合块克隆表达式，不能直接拿任一前驱定义替换。
    if (CommonL && CommonR) {
        if (!DT || !DT->dominatesValue(CommonL, Phi) ||
            !DT->dominatesValue(CommonR, Phi)) {
            return nullptr;
        }
        auto *NewBin = new BinaryInst(CommonL, Op, CommonR);
        if (!BasicBlock::insertAfterPhiNodes(Phi->getParent(), NewBin)) {
            delete NewBin;
            return nullptr;
        }
        return NewBin;
    }
    Value *SampleUncommon = nullptr;
    if (CommonL) {
        Value *TmpL = nullptr;
        Value *TmpR = nullptr;
        if (!BinaryInst::getOperands(First, TmpL, TmpR)) return nullptr;
        SampleUncommon = TmpR;
    } else {
        Value *TmpL = nullptr;
        Value *TmpR = nullptr;
        if (!BinaryInst::getOperands(First, TmpL, TmpR)) return nullptr;
        SampleUncommon = TmpL;
    }
    if (!SampleUncommon) return nullptr;
    auto *UncommonPhi = new PhiInst(SampleUncommon->getIRType());
    for (unsigned i = 0; i < N; ++i) {
        auto *Bin = dynamic_cast<BinaryInst *>(Phi->getIncomingValue(i));
        Value *L = nullptr;
        Value *R = nullptr;
        if (!Bin || !BinaryInst::getOperands(Bin, L, R)) {
            delete UncommonPhi;
            return nullptr;
        }
        Value *U = CommonL ? R : L;
        UncommonPhi->addIncoming(U, Phi->getIncomingBlock(i));
    }
    if (!Instruction::insertBefore(Phi, UncommonPhi)) {
        delete UncommonPhi;
        return nullptr;
    }
    auto *NewBin = CommonL ? new BinaryInst(CommonL, Op, UncommonPhi)
                          : new BinaryInst(UncommonPhi, Op, CommonR);
    if (!BasicBlock::insertAfterPhiNodes(Phi->getParent(), NewBin)) {
        delete NewBin;
        return nullptr;
    }
    return NewBin;
}
//尝试把GEP跨PHI各输入臂分解并折叠
Value *InstSimplifier::foldPhiOfGEP(PhiInst *Phi) {
    //1.验证全部输入都是无计时屏障的动态GEP并拒绝栈地址基址
    //2.比较基址与各索引，只允许一个非公共且非常量的位置
    //3.为差异位置创建PHI并在汇合块重建GEP，保持地址计算类型与SSA布局
    if (!Phi) return nullptr;
    if (!F || F->hasTimingBarrierOnPath(Phi, Phi)) return nullptr;
    const unsigned N = Phi->getNumIncomingValues();
    if (N < 2) return nullptr;
    auto *First = dynamic_cast<GetElementPtrInst *>(Phi->getIncomingValue(0));
    if (!First || First->hasOnlyConstantIndices()) return nullptr;
    Value *FirstPtr = nullptr;
    std::vector<Value *> FirstIdxs;
    if (!GetElementPtrInst::getBaseAndIndices(First, FirstPtr, FirstIdxs)) return nullptr;
    if (dynamic_cast<AllocaInst *>(FirstPtr)) return nullptr;
    std::vector<Value *> Commons = FirstIdxs;
    Commons.push_back(FirstPtr);
    size_t UncommonCnt = 0;
    for (unsigned i = 0; i < N; ++i) {
        auto *GEP = dynamic_cast<GetElementPtrInst *>(Phi->getIncomingValue(i));
        if (!GEP || GEP->hasOnlyConstantIndices()) return nullptr;
        if (!F || F->hasTimingBarrierOnPath(GEP, Phi)) return nullptr;
        Value *Ptr = nullptr;
        std::vector<Value *> Idxs;
        if (!GetElementPtrInst::getBaseAndIndices(GEP, Ptr, Idxs)) return nullptr;
        if (Idxs.size() != Commons.size() - 1) return nullptr;
        if (dynamic_cast<AllocaInst *>(Ptr)) return nullptr;
        if (Commons.back() && Commons.back() != Ptr) {
            Commons.back() = nullptr;
            ++UncommonCnt;
        }
        for (size_t k = 0; k < Idxs.size(); ++k) {
            if (!Commons[k] || Commons[k] == Idxs[k]) continue;
            if (Commons[k]->isConst() || Idxs[k]->isConst()) return nullptr;
            Commons[k] = nullptr;
            ++UncommonCnt;
        }
        if (UncommonCnt > 1) return nullptr;
    }
    if (UncommonCnt == 0) return nullptr;
    size_t UncommonIndex = 0;
    while (UncommonIndex < Commons.size() && Commons[UncommonIndex] != nullptr) ++UncommonIndex;
    if (UncommonIndex >= Commons.size()) return nullptr;
    Value *Sample = nullptr;
    if (UncommonIndex == Commons.size() - 1) {
        Value *Ptr = nullptr;
        std::vector<Value *> Idxs;
        if (!GetElementPtrInst::getBaseAndIndices(dynamic_cast<GetElementPtrInst *>(Phi->getIncomingValue(0)), Ptr, Idxs)) return nullptr;
        Sample = Ptr;
    } else {
        Value *Ptr = nullptr;
        std::vector<Value *> Idxs;
        if (!GetElementPtrInst::getBaseAndIndices(dynamic_cast<GetElementPtrInst *>(Phi->getIncomingValue(0)), Ptr, Idxs)) return nullptr;
        Sample = Idxs[UncommonIndex];
    }
    if (!Sample) return nullptr;
    auto *UncommonPhi = new PhiInst(Sample->getIRType());
    for (unsigned i = 0; i < N; ++i) {
        auto *GEP = dynamic_cast<GetElementPtrInst *>(Phi->getIncomingValue(i));
        Value *Ptr = nullptr;
        std::vector<Value *> Idxs;
        if (!GEP || !GetElementPtrInst::getBaseAndIndices(GEP, Ptr, Idxs)) {
            delete UncommonPhi;
            return nullptr;
        }
        if (UncommonIndex == Commons.size() - 1) {
            UncommonPhi->addIncoming(Ptr, Phi->getIncomingBlock(i));
        } else {
            UncommonPhi->addIncoming(Idxs[UncommonIndex], Phi->getIncomingBlock(i));
        }
    }
    if (!Instruction::insertBefore(Phi, UncommonPhi)) {
        delete UncommonPhi;
        return nullptr;
    }
    Instruction *NewGEP = nullptr;
    if (UncommonIndex == Commons.size() - 1) {
        std::vector<Value *> Idxs = Commons;
        Idxs.pop_back();
        NewGEP = new GetElementPtrInst(UncommonPhi, Idxs);
        dynamic_cast<GetElementPtrInst *>(NewGEP)->updateType();
    } else {
        Value *Ptr = Commons.back();
        std::vector<Value *> Idxs = Commons;
        Idxs.pop_back();
        Idxs[UncommonIndex] = UncommonPhi;
        NewGEP = new GetElementPtrInst(Ptr, Idxs);
        dynamic_cast<GetElementPtrInst *>(NewGEP)->updateType();
    }
    if (!BasicBlock::insertAfterPhiNodes(Phi->getParent(), NewGEP)) {
        delete NewGEP;
        return nullptr;
    }
    return NewGEP;
}
//尝试把Load跨PHI各输入臂安全下沉
Value *InstSimplifier::foldPhiOfLoad(PhiInst *Phi) {
    //1.每条输入必须是对应前驱中的单用户Load且无计时屏障
    //2.别名与MemorySSA必须证明Load到前驱出口之间没有写入破坏
    //3.先合并地址再于汇合块创建单一Load，随后禁用陈旧MemorySSA的增量证明
    if (!Phi) return nullptr;
    if (!F || F->hasTimingBarrierOnPath(Phi, Phi)) return nullptr;
    const unsigned N = Phi->getNumIncomingValues();
    if (N < 2) return nullptr;
    auto *First = dynamic_cast<LoadInst *>(Phi->getIncomingValue(0));
    if (!First) return nullptr;
    Value *FirstPtr = First->getPointerOperand();
    if (!FirstPtr) return nullptr;
    for (unsigned i = 0; i < N; ++i) {
        auto *L = dynamic_cast<LoadInst *>(Phi->getIncomingValue(i));
        if (!L) return nullptr;
        if (!F || F->hasTimingBarrierOnPath(L, Phi)) return nullptr;
        if (!L->hasExactlyOneUse()) return nullptr;
        if (L->getParent() != Phi->getIncomingBlock(i)) return nullptr;
        if (!isLoadSuitableForSinking(L)) return nullptr;
    }
    auto *MergedPtr = new PhiInst(FirstPtr->getIRType());
    for (unsigned i = 0; i < N; ++i) {
        auto *L = dynamic_cast<LoadInst *>(Phi->getIncomingValue(i));
        Value *Ptr = L ? L->getPointerOperand() : nullptr;
        if (!L || !Ptr) {
            delete MergedPtr;
            return nullptr;
        }
        MergedPtr->addIncoming(Ptr, Phi->getIncomingBlock(i));
    }
    if (!Instruction::insertBefore(Phi, MergedPtr)) {
        delete MergedPtr;
        return nullptr;
    }
    auto *MergedLoad = new LoadInst(MergedPtr);
    if (!BasicBlock::insertAfterPhiNodes(Phi->getParent(), MergedLoad)) {
        delete MergedLoad;
        return nullptr;
    }
    //新load没有增量写入旧MemorySSA；后续PHI-of-load候选统一退回逐指令检查。
    MemorySSAUsable = false;
    return MergedLoad;
}
//按照指令种类分派并返回等价的化简结果
Value *InstSimplifier::simplifyInstruction(Instruction *I, bool &Changed) {
    //2.按指令族分派：二元运算、select 与 PHI 各自只做局部等价改写。
    if (!I) return nullptr;
    if (auto *BI = dynamic_cast<BinaryInst *>(I)) {
        return simplifyBinary(BI, Changed);
    }
    if (auto *SI = dynamic_cast<SelectInst *>(I)) {
        return simplifySelect(SI);
    }
    if (auto *PI = dynamic_cast<PhiInst *>(I)) {
        return simplifyPhi(PI);
    }
    return nullptr;
}
//按照规范顺序交换可交换二元操作数
bool InstSimplifier::maybeSwapCommutative(
    BinaryInst *BI, Value *&LHS, Value *&RHS, bool &Changed) {
    //2.2将交换律运算规范成“非常量在左、常量在右”，让后续模式只写一个方向。
    if (!BI || !LHS || !RHS) return false;
    if (!LHS->isConst() || RHS->isConst()) return false;
    switch (BI->getOp()) {
    case BinaryInst::ADD:
    case BinaryInst::MUL:
    case BinaryInst::AND:
    case BinaryInst::OR:
    case BinaryInst::XOR: {
        BI->setOperands(RHS, LHS);
        std::swap(LHS, RHS);
        Changed = true;
        return true;
    }
    default:
        return false;
    }
}
//应用加法恒等式、结合律和常量折叠规则
Value *InstSimplifier::simplifyAdd(
    BinaryInst *BI, Value *LHS, Value *RHS, bool &Changed) {
    //3.1加法依次处理双常量、undef、精确零单位元、相反数、嵌套常量合并与
    //x+(y-x) 消去；浮点仅接受保持有符号零语义的 -0.0 单位元。
    const bool IsIntArith = IRType::isInt(BI->getIRType()) && !IRType::isBool(BI->getIRType());
    if ((LHS->isConst() && RHS->isConst())) {
        return BinaryInst::foldConstants(BI->getOp(), LHS, RHS);
    }
    maybeSwapCommutative(BI, LHS, RHS, Changed);
    if (Value::isUndef(LHS) || Value::isUndef(RHS)) {
        return Value::makeUndefForType(BI->getIRType());
    }
    float FloatZero = 0.0f;
    const bool IsNegativeFloatZero = IRType::isFloat(BI->getIRType()) &&
        Value::tryGetFloatConst(RHS, FloatZero) && FloatZero == 0.0f &&
        std::signbit(FloatZero);
    if ((IsIntArith || IsNegativeFloatZero) && Value::isConstZero(RHS)) {
        return LHS;
    }
    if (IsIntArith && InstSimplifyPass::isNegationPair(LHS, RHS)) {
        return Value::makeZeroForType(BI->getIRType());
    }
    if (IsIntArith && RHS->isConst()) {
        Value *AL = nullptr;
        Value *AR = nullptr;
        if (InstSimplifyPass::getBinaryOperandsForOp(LHS, BinaryInst::ADD, AL, AR) && AR && AR->isConst()) {
            Value *SumC = BinaryInst::foldConstants(BinaryInst::ADD, AR, RHS);
            if (SumC) {
                return new BinaryInst(AL, BinaryInst::ADD, SumC);
            }
        }
    }
    Value *Y = nullptr;
    if (IsIntArith && InstSimplifyPass::matchYMinusX(RHS, LHS, Y)) return Y;
    if (IsIntArith && InstSimplifyPass::matchYMinusX(LHS, RHS, Y)) return Y;
    return nullptr;
}
//应用减法恒等式和相消规则
Value *InstSimplifier::simplifySub(
    BinaryInst *BI, Value *LHS, Value *RHS) {
    //3.2减法先做常量/undef/单位元，再应用整数专属的同值、负号、公共乘数和
    //平方差模式；浮点路径不套用会破坏 NaN 或有符号零的代数恒等式。
    const bool IsIntArith = IRType::isInt(BI->getIRType()) && !IRType::isBool(BI->getIRType());
    if (LHS->isConst() && RHS->isConst()) {
        return BinaryInst::foldConstants(BI->getOp(), LHS, RHS);
    }
    if (Value::isUndef(LHS) || Value::isUndef(RHS)) {
        return Value::makeUndefForType(BI->getIRType());
    }
    float FloatZero = 0.0f;
    const bool IsPositiveFloatZero = IRType::isFloat(BI->getIRType()) &&
        Value::tryGetFloatConst(RHS, FloatZero) && FloatZero == 0.0f &&
        !std::signbit(FloatZero);
    if ((IsIntArith || IsPositiveFloatZero) && Value::isConstZero(RHS)) {
        return LHS;
    }
    if (IsIntArith && LHS == RHS) {
        return Value::makeZeroForType(BI->getIRType());
    }
    if (IsIntArith) {
        Value *AL = nullptr;
        Value *AR = nullptr;
        if (InstSimplifyPass::getBinaryOperandsForOp(RHS, BinaryInst::ADD, AL, AR)) {
            if (AL == LHS) return new BinaryInst(Value::makeZeroForType(BI->getIRType()), BinaryInst::SUB, AR);
            if (AR == LHS) return new BinaryInst(Value::makeZeroForType(BI->getIRType()), BinaryInst::SUB, AL);
        }
    }
    if (IsIntArith) {
        Value *Y = nullptr;
        if (InstSimplifyPass::matchNegation(RHS, Y)) {
            return new BinaryInst(LHS, BinaryInst::ADD, Y);
        }
    }
    if (IsIntArith) {
        Value *L0 = nullptr;
        Value *L1 = nullptr;
        Value *R0 = nullptr;
        Value *R1 = nullptr;
        if (InstSimplifyPass::getBinaryOperandsForOp(LHS, BinaryInst::MUL, L0, L1) &&
            InstSimplifyPass::getBinaryOperandsForOp(RHS, BinaryInst::MUL, R0, R1)) {
            Value *Common = nullptr;
            Value *Y = nullptr;
            Value *Z = nullptr;
            if (L0 == R0) { Common = L0; Y = L1; Z = R1; }
            else if (L0 == R1) { Common = L0; Y = L1; Z = R0; }
            else if (L1 == R0) { Common = L1; Y = L0; Z = R1; }
            else if (L1 == R1) { Common = L1; Y = L0; Z = R0; }
            if (Common && Y && Z) {
                auto *YZ = new BinaryInst(Y, BinaryInst::SUB, Z);
                if (Instruction::insertBefore(BI, YZ)) {
                    return new BinaryInst(Common, BinaryInst::MUL, YZ);
                }
                delete YZ;
            }
        }
    }
    if (IsIntArith) {
        Value *L0 = nullptr;
        Value *L1 = nullptr;
        Value *R0 = nullptr;
        Value *R1 = nullptr;
        if (InstSimplifyPass::getBinaryOperandsForOp(LHS, BinaryInst::MUL, L0, L1) &&
            InstSimplifyPass::getBinaryOperandsForOp(RHS, BinaryInst::MUL, R0, R1) &&
            L0 == L1 && R0 == R1) {
            auto *Plus = new BinaryInst(L0, BinaryInst::ADD, R0);
            auto *Minus = new BinaryInst(L0, BinaryInst::SUB, R0);
            if (Instruction::insertBefore(BI, Plus) && Instruction::insertBefore(BI, Minus)) {
                return new BinaryInst(Plus, BinaryInst::MUL, Minus);
            }
            delete Plus;
            delete Minus;
        }
    }
    return nullptr;
}
//应用乘法恒等式、结合律和常量折叠规则
Value *InstSimplifier::simplifyMul(
    BinaryInst *BI, Value *LHS, Value *RHS, bool &Changed) {
    //3.3乘法规范操作数后处理吸收元、单位元、-1，并将正的 2 次幂整数乘法
    //改为左移；浮点零乘不折叠，以保留 NaN/无穷语义。
    const bool IsIntArith = IRType::isInt(BI->getIRType()) && !IRType::isBool(BI->getIRType());
    if (LHS->isConst() && RHS->isConst()) {
        return BinaryInst::foldConstants(BI->getOp(), LHS, RHS);
    }
    maybeSwapCommutative(BI, LHS, RHS, Changed);
    if (Value::isUndef(LHS) || Value::isUndef(RHS)) {
        return IsIntArith ? Value::makeZeroForType(BI->getIRType())
                          : Value::makeUndefForType(BI->getIRType());
    }
    if (IsIntArith && Value::isConstZero(RHS)) {
        return Value::makeZeroForType(BI->getIRType());
    }
    if (Value::isConstOne(RHS)) {
        return LHS;
    }
    if (IRType::isInt(BI->getIRType()) && !IRType::isBool(BI->getIRType()) &&
        Value::isConstMinusOne(RHS)) {
        return new BinaryInst(Value::makeZeroForType(BI->getIRType()), BinaryInst::SUB, LHS);
    }
    if (IRType::isInt(BI->getIRType())) {
        int32_t C = 0;
        if (Value::tryGetIntConst(RHS, C) && C > 0) {
            uint32_t U = static_cast<uint32_t>(C);
            if (IRMath::isUnsignedPowerOf2(U)) {
                return new BinaryInst(LHS, BinaryInst::SHL,
                    Value::makeIntegerLike(
                        LHS, IRMath::log2u(U)));
            }
        }
    }
    return nullptr;
}
//应用有符号除法恒等式和安全相消规则
Value *InstSimplifier::simplifyDiv(
    BinaryInst *BI, Value *LHS, Value *RHS) {
    //3.4除法按整数/浮点分别处理 undef、零除数与简单单位元；整数除零产生 undef，
    //浮点除零不在此折叠，x/x 也仅对整数成立。
    const auto Op = BI->getOp();
    const auto Ty = BI->getIRType();
    const bool IsFloat = IRType::isFloat(Ty);
    const bool IsIntArith = IRType::isInt(Ty) && !IRType::isBool(Ty);
    if (LHS->isConst() && RHS->isConst()) {
        return BinaryInst::foldConstants(Op, LHS, RHS);
    }
    if (Value::isUndef(RHS)) {
        return Value::makeUndefForType(Ty);
    }
    if (Value::isUndef(LHS)) {
        if (IsFloat) return Value::makeUndefForType(Ty);
        if (Value::isConstZero(RHS) || Value::isUndef(RHS)) {
            return Value::makeUndefForType(Ty);
        }
        return Value::makeZeroForType(Ty);
    }
    if (Value::isConstZero(RHS)) {
        return IsFloat ? nullptr : Value::makeUndefForType(Ty);
    }
    if (IsIntArith && Value::isConstZero(LHS)) {
        return Value::makeZeroForType(Ty);
    }
    if (IsIntArith && LHS == RHS) {
        return Value::makeOneForType(Ty);
    }
    if (Value::isConstOne(RHS)) {
        return LHS;
    }
    if (IsIntArith && Op == BinaryInst::DIV &&
        Value::isConstMinusOne(RHS)) {
        return new BinaryInst(Value::makeZeroForType(Ty), BinaryInst::SUB, LHS);
    }
    return nullptr;
}
//应用有符号取余恒等式和安全相消规则
Value *InstSimplifier::simplifyRem(
    BinaryInst *BI, Value *LHS, Value *RHS) {
    //3.5余数只在整数已定义情形使用零、±1、同值与相反数恒等式；浮点 frem
    //保留可能产生 NaN 的零除数和同值场景。
    const auto Ty = BI->getIRType();
    const bool IsFloat = IRType::isFloat(Ty);
    const bool IsIntArith = IRType::isInt(Ty) && !IRType::isBool(Ty);
    if (IsIntArith && BI->getOp() == BinaryInst::MOD &&
        InstSimplifyPass::isNegationPair(LHS, RHS)) {
        return Value::makeZeroForType(Ty);
    }
    if (LHS->isConst() && RHS->isConst()) {
        return BinaryInst::foldConstants(BI->getOp(), LHS, RHS);
    }
    if (Value::isUndef(RHS)) return Value::makeUndefForType(Ty);
    if (Value::isUndef(LHS)) {
        return IsFloat ? Value::makeUndefForType(Ty)
                       : Value::makeZeroForType(Ty);
    }
    if (IsIntArith && Value::isConstZero(LHS))
        return Value::makeZeroForType(Ty);
    if (Value::isConstZero(RHS)) {
        return IsFloat ? nullptr : Value::makeUndefForType(Ty);
    }
    if (IsIntArith && Value::isConstOne(RHS)) return Value::makeZeroForType(Ty);
    if (IsIntArith && BI->getOp() == BinaryInst::MOD &&
        Value::isConstMinusOne(RHS))
        return Value::makeZeroForType(Ty);
    if (IsIntArith && LHS == RHS)
        return Value::makeZeroForType(Ty);
    return nullptr;
}
//应用按位与恒等式、吸收律和常量折叠规则
Value *InstSimplifier::simplifyAnd(
    BinaryInst *BI, Value *LHS, Value *RHS, bool &Changed) {
    //3.6AND/OR/XOR 先做常量与单位/吸收元，再处理布尔互补、吸收律和双重取反。
    if (LHS->isConst() && RHS->isConst()) {
        return BinaryInst::foldConstants(BI->getOp(), LHS, RHS);
    }
    maybeSwapCommutative(BI, LHS, RHS, Changed);
    if (Value::isUndef(LHS) || Value::isUndef(RHS)) {
        return Value::makeZeroForType(BI->getIRType());
    }
    if (LHS == RHS) return LHS;
    if (Value::isConstZero(RHS)) return Value::makeZeroForType(BI->getIRType());
    if (IRType::isBool(BI->getIRType()) && Value::isConstOne(RHS))
        return LHS;
    if (Value::isConstMinusOne(RHS)) return LHS;
    if (IRType::isBool(BI->getIRType())) {
        auto isNotOf = [](Value *MaybeNot, Value *X) {
            auto *Not = dynamic_cast<BinaryInst *>(MaybeNot);
            Value *NL = nullptr;
            Value *NR = nullptr;
            return Not && Not->getOp() == BinaryInst::XOR &&
                BinaryInst::getOperands(Not, NL, NR) && NL == X &&
                Value::isConstOne(NR);
        };
        if (isNotOf(LHS, RHS) || isNotOf(RHS, LHS))
            return ConstBool::newConstBool(false);
    }
    auto absorbsOr = [](Value *X, Value *MaybeOr) {
        auto *Or = dynamic_cast<BinaryInst *>(MaybeOr);
        Value *OL = nullptr;
        Value *OR = nullptr;
        return Or && Or->getOp() == BinaryInst::OR &&
            BinaryInst::getOperands(Or, OL, OR) && (OL == X || OR == X);
    };
    if (absorbsOr(LHS, RHS)) return LHS;
    if (absorbsOr(RHS, LHS)) return RHS;
    return nullptr;
}
//应用按位或恒等式、吸收律和常量折叠规则
Value *InstSimplifier::simplifyOr(
    BinaryInst *BI, Value *LHS, Value *RHS, bool &Changed) {
    //3.7先折叠常量并规范操作数，再处理undef、单位元、互补与吸收律
    //3.7.1布尔比较覆盖规则只在同一整数变量和可证明常量边界上成立，其他情况保守返回空值
    if (LHS->isConst() && RHS->isConst()) {
        return BinaryInst::foldConstants(BI->getOp(), LHS, RHS);
    }
    maybeSwapCommutative(BI, LHS, RHS, Changed);
    if (Value::isUndef(LHS) || Value::isUndef(RHS)) {
        return Value::makeMinusOneForType(BI->getIRType());
    }
    if (Value::isConstMinusOne(RHS)) return Value::makeMinusOneForType(BI->getIRType());
    if (LHS == RHS) return LHS;
    if (Value::isConstZero(RHS)) return LHS;
    if (IRType::isBool(BI->getIRType()) && Value::isConstOne(RHS))
        return ConstBool::newConstBool(true);
    if (IRType::isBool(BI->getIRType())) {
        auto isNotOf = [](Value *MaybeNot, Value *X) {
            auto *Not = dynamic_cast<BinaryInst *>(MaybeNot);
            Value *NL = nullptr;
            Value *NR = nullptr;
            return Not && Not->getOp() == BinaryInst::XOR &&
                BinaryInst::getOperands(Not, NL, NR) && NL == X &&
                Value::isConstOne(NR);
        };
        if (isNotOf(LHS, RHS) || isNotOf(RHS, LHS))
            return ConstBool::newConstBool(true);
    }
    auto absorbsAnd = [](Value *X, Value *MaybeAnd) {
        auto *And = dynamic_cast<BinaryInst *>(MaybeAnd);
        Value *AL = nullptr;
        Value *AR = nullptr;
        return And && And->getOp() == BinaryInst::AND &&
            BinaryInst::getOperands(And, AL, AR) && (AL == X || AR == X);
    };
    if (absorbsAnd(LHS, RHS)) return LHS;
    if (absorbsAnd(RHS, LHS)) return RHS;
    if (IRType::isBool(BI->getIRType())) {
        Value *GL = nullptr;
        Value *GR = nullptr;
        Value *LL = nullptr;
        Value *LR = nullptr;
        if (InstSimplifyPass::getBinaryOperandsForOp(LHS, BinaryInst::G, GL, GR) &&
            InstSimplifyPass::getBinaryOperandsForOp(RHS, BinaryInst::L, LL, LR) &&
            GL == LL && GR && LR && GR->isConst() && LR->isConst() &&
            IRType::isInt(GL->getIRType()) && !IRType::isFloat(GL->getIRType())) {
            Value *Cmp = BinaryInst::foldConstants(BinaryInst::L, GR, LR);
            bool TrueCond = false;
            if (Cmp && Value::tryGetBoolConst(Cmp, TrueCond) && TrueCond) {
                return ConstBool::newConstBool(true);
            }
        }
        if (InstSimplifyPass::getBinaryOperandsForOp(LHS, BinaryInst::L, GL, GR) &&
            InstSimplifyPass::getBinaryOperandsForOp(RHS, BinaryInst::G, LL, LR) &&
            GL == LL && GR && LR && GR->isConst() && LR->isConst() &&
            IRType::isInt(GL->getIRType()) && !IRType::isFloat(GL->getIRType())) {
            Value *Cmp = BinaryInst::foldConstants(BinaryInst::L, LR, GR);
            bool TrueCond = false;
            if (Cmp && Value::tryGetBoolConst(Cmp, TrueCond) && TrueCond) {
                return ConstBool::newConstBool(true);
            }
        }
    }
    return nullptr;
}
//应用按位异或恒等式和常量折叠规则
Value *InstSimplifier::simplifyXor(
    BinaryInst *BI, Value *LHS, Value *RHS, bool &Changed) {
    //3.8常量与undef语义优先于代数恒等式，再处理零单位元、同值相消和布尔双重取反
    if (LHS->isConst() && RHS->isConst()) {
        return BinaryInst::foldConstants(BI->getOp(), LHS, RHS);
    }
    maybeSwapCommutative(BI, LHS, RHS, Changed);
    if (Value::isUndef(LHS) || Value::isUndef(RHS)) {
        return Value::makeUndefForType(BI->getIRType());
    }
    if (Value::isConstZero(RHS)) return LHS;
    if (LHS == RHS) return Value::makeZeroForType(BI->getIRType());
    if (IRType::isBool(BI->getIRType()) && Value::isConstOne(RHS)) {
        auto *Inner = dynamic_cast<BinaryInst *>(LHS);
        Value *IL = nullptr;
        Value *IR = nullptr;
        if (Inner && Inner->getOp() == BinaryInst::XOR &&
            BinaryInst::getOperands(Inner, IL, IR) &&
            Value::isConstOne(IR)) {
            return IL;
        }
    }
    return nullptr;
}
//化简整数或浮点比较并保持对应谓词语义
Value *InstSimplifier::simplifyCmp(
    BinaryInst *BI, Value *LHS, Value *RHS) {
    //1.优先折叠双常量并显式维护浮点NaN谓词语义
    //2.规范布尔扩展与常量比较，只接受可精确解释的0或真假表示
    //3.识别整数min/max选择结构与二次幂余数模式，证明不足时保守回退
    //4.最后仅对非浮点同值比较应用自反恒等式，避免破坏NaN语义
    if (LHS->isConst() && RHS->isConst()) {
        if (IRType::isFloat(LHS->getIRType())) {
            float LV = 0.0f;
            float RV = 0.0f;
            if (Value::tryGetFloatConst(LHS, LV) &&
                Value::tryGetFloatConst(RHS, RV) &&
                (std::isnan(LV) || std::isnan(RV))) {
                return ConstBool::newConstBool(
                    BI->getOp() == BinaryInst::NE);
            }
        }
        return BinaryInst::foldConstants(BI->getOp(), LHS, RHS);
    }
    if (BI->getOp() == BinaryInst::E || BI->getOp() == BinaryInst::NE) {
        auto tryBoolCmpConst = [&](Value *A, Value *B) -> Value * {
            Value::BooleanExtensionKind Kind =
                Value::BooleanExtensionKind::None;
            Value *Cond = Value::stripBooleanExtension(A, Kind);
            if (!Cond || !B) return nullptr;
            int32_t Raw = 0;
            if (auto *CB = dynamic_cast<ConstBool *>(B)) {
                Raw = CB->getVal() ? 1 : 0;
            } else if (auto *CI = dynamic_cast<ConstInt *>(B)) {
                Raw = CI->getVal();
            } else {
                return nullptr;
            }
            int32_t TrueValue =
                (Kind == Value::BooleanExtensionKind::SignExt) ? -1 : 1;
            if (Raw == TrueValue) {
                return BI->getOp() == BinaryInst::E
                    ? Cond : BinaryInst::createBooleanNot(Cond);
            }
            if (Raw == 0) {
                return BI->getOp() == BinaryInst::E
                    ? BinaryInst::createBooleanNot(Cond) : Cond;
            }
            return ConstBool::newConstBool(BI->getOp() == BinaryInst::NE);
        };
        if (Value *V = tryBoolCmpConst(LHS, RHS)) return V;
        if (Value *V = tryBoolCmpConst(RHS, LHS)) return V;
    }
    auto matchExtremum = [](Value *V, Value *Bound) {
        auto *Sel = dynamic_cast<SelectInst *>(V);
        if (!Sel || !Bound || !IRType::isInt(Sel->getIRType()) ||
            IRType::isBool(Sel->getIRType())) {
            return ExtremumKind::None;
        }
        Value *TV = Sel->getTrueValue();
        Value *FV = Sel->getFalseValue();
        if (!TV || !FV || (Bound != TV && Bound != FV) ||
            Value::isUndef(TV) || Value::isUndef(FV)) {
            return ExtremumKind::None;
        }
        auto *Cmp = dynamic_cast<BinaryInst *>(Sel->getCondition());
        Value *CL = nullptr;
        Value *CR = nullptr;
        if (!Cmp || !BinaryInst::isCompareOp(Cmp->getOp()) ||
            !BinaryInst::getOperands(Cmp, CL, CR) || !CL || !CR ||
            IRType::isFloat(CL->getIRType())) {
            return ExtremumKind::None;
        }
        auto Pred = Cmp->getOp();
        if (CL == FV && CR == TV) {
            Pred = BinaryInst::reversePredicate(Pred);
        } else if (CL != TV || CR != FV) {
            return ExtremumKind::None;
        }
        if (Pred == BinaryInst::G || Pred == BinaryInst::GE)
            return ExtremumKind::Maximum;
        if (Pred == BinaryInst::L || Pred == BinaryInst::LE)
            return ExtremumKind::Minimum;
        return ExtremumKind::None;
    };
    if (ExtremumKind Kind = matchExtremum(LHS, RHS);
        Kind != ExtremumKind::None) {
        if (Kind == ExtremumKind::Maximum && BI->getOp() == BinaryInst::L)
            return ConstBool::newConstBool(false);
        if (Kind == ExtremumKind::Maximum && BI->getOp() == BinaryInst::GE)
            return ConstBool::newConstBool(true);
        if (Kind == ExtremumKind::Minimum && BI->getOp() == BinaryInst::G)
            return ConstBool::newConstBool(false);
        if (Kind == ExtremumKind::Minimum && BI->getOp() == BinaryInst::LE)
            return ConstBool::newConstBool(true);
    }
    if (ExtremumKind Kind = matchExtremum(RHS, LHS);
        Kind != ExtremumKind::None) {
        if (Kind == ExtremumKind::Maximum && BI->getOp() == BinaryInst::G)
            return ConstBool::newConstBool(false);
        if (Kind == ExtremumKind::Maximum && BI->getOp() == BinaryInst::LE)
            return ConstBool::newConstBool(true);
        if (Kind == ExtremumKind::Minimum && BI->getOp() == BinaryInst::L)
            return ConstBool::newConstBool(false);
        if (Kind == ExtremumKind::Minimum && BI->getOp() == BinaryInst::GE)
            return ConstBool::newConstBool(true);
    }
    if (BI->getOp() == BinaryInst::E) {
        auto tryModEqPow2 = [&](Value *A, Value *B) -> Value * {
            Value *X = nullptr;
            Value *D = nullptr;
            auto *MB = dynamic_cast<BinaryInst *>(A);
            if (!MB) return nullptr;
            if (MB->getOp() != BinaryInst::MOD && MB->getOp() != BinaryInst::UREM) return nullptr;
            if (!InstSimplifyPass::getBinaryOperandsForOp(MB, MB->getOp(), X, D)) return nullptr;
            if (!B || !B->isConst()) return nullptr;
            int32_t Div = 0;
            int32_t T = 0;
            if (!Value::tryGetIntConst(D, Div) || !Value::tryGetIntConst(B, T)) return nullptr;
            if (Div <= 0) return nullptr;
            const uint32_t UDiv = static_cast<uint32_t>(Div);
            if (!IRMath::isUnsignedPowerOf2(UDiv)) return nullptr;
            const int32_t Mask = static_cast<int32_t>(UDiv - 1u);
            bool Safe = false;
            if (MB->getOp() == BinaryInst::UREM) {
                Safe = (T >= 0 && static_cast<uint32_t>(T) <= static_cast<uint32_t>(Mask));
            } else {
                Safe = (T == 0);
            }
            if (!Safe) return nullptr;
            auto *And = new BinaryInst(
                X, BinaryInst::AND,
                Value::makeIntegerLike(X, Mask));
            if (!Instruction::insertBefore(BI, And)) {
                delete And;
                return nullptr;
            }
            return new BinaryInst(And, BinaryInst::E, B);
        };
        if (Value *R = tryModEqPow2(LHS, RHS)) return R;
        if (Value *R = tryModEqPow2(RHS, LHS)) return R;
    }
    if (LHS == RHS && !IRType::isFloat(LHS->getIRType())) {
        switch (BI->getOp()) {
        case BinaryInst::E:
        case BinaryInst::GE:
        case BinaryInst::LE:
            return ConstBool::newConstBool(true);
        case BinaryInst::NE:
        case BinaryInst::G:
        case BinaryInst::L:
            return ConstBool::newConstBool(false);
        default:
            return nullptr;
        }
    }
    return nullptr;
}
//规范化二元操作数并分派到具体代数化简规则
Value *InstSimplifier::simplifyBinary(BinaryInst *BI, bool &Changed) {
    //1.先提取两个有效操作数，形状异常时不进行任何IR改写
    //2.按操作码分派到保持对应整数、浮点或位运算语义的规则组
    //3.未覆盖或无法证明等价的模式统一返回空值并保留原指令
    Value *LHS = nullptr;
    Value *RHS = nullptr;
    if (!BinaryInst::getOperands(BI, LHS, RHS)) return nullptr;
    switch (BI->getOp()) {
    case BinaryInst::ADD:
        return simplifyAdd(BI, LHS, RHS, Changed);
    case BinaryInst::SUB:
        return simplifySub(BI, LHS, RHS);
    case BinaryInst::MUL:
        return simplifyMul(BI, LHS, RHS, Changed);
    case BinaryInst::DIV:
    case BinaryInst::UDIV:
        return simplifyDiv(BI, LHS, RHS);
    case BinaryInst::MOD:
    case BinaryInst::UREM:
        return simplifyRem(BI, LHS, RHS);
    case BinaryInst::AND:
        return simplifyAnd(BI, LHS, RHS, Changed);
    case BinaryInst::OR:
        return simplifyOr(BI, LHS, RHS, Changed);
    case BinaryInst::XOR:
        return simplifyXor(BI, LHS, RHS, Changed);
    case BinaryInst::E:
    case BinaryInst::NE:
    case BinaryInst::G:
    case BinaryInst::GE:
    case BinaryInst::L:
    case BinaryInst::LE:
        return simplifyCmp(BI, LHS, RHS);
    default:
        return nullptr;
    }
}
//化简条件选择并识别布尔与极值模式
Value *InstSimplifier::simplifySelect(SelectInst *SI) {
    //1.先处理同臂、undef臂和常量条件，保持选择指令的未定义值语义
    //2.布尔结果规范成AND/OR/NOT，插入失败时删除临时指令并保守回退
    //3.整数1/0分支仅在布尔条件下转换为匹配目标类型的零扩展
    //4.最后折叠与条件比较相呼应的选择模式，浮点比较不套用整数恒等式
    Value *Cond = SI->getCondition();
    Value *TV = SI->getTrueValue();
    Value *FV = SI->getFalseValue();
    if (!Cond || !TV || !FV) return nullptr;
    if (TV == FV) return TV;
    if (Value::isUndef(TV)) return FV;
    if (Value::isUndef(FV)) return TV;
    if (Value::isUndef(Cond)) {
        if (TV->isConst() && !FV->isConst()) return TV;
        if (FV->isConst() && !TV->isConst()) return FV;
        return TV;
    }
    bool CV = false;
    if (Value::tryGetBoolConst(Cond, CV)) {
        return CV ? TV : FV;
    }
    if (Cond->isConst() && TV->isConst() && FV->isConst()) {
        bool ConstCond = false;
        if (Value::tryGetBoolConst(Cond, ConstCond)) {
            return ConstCond ? TV : FV;
        }
        return nullptr;
    }
    const auto ResTy = SI->getIRType();
    if (IRType::isBool(ResTy)) {
        if (Value::isConstZero(FV)) {
            return new BinaryInst(Cond, BinaryInst::AND, TV);
        }
        if (Value::isConstOne(TV)) {
            return new BinaryInst(Cond, BinaryInst::OR, FV);
        }
        if (IRType::isBool(Cond->getIRType()) && Value::isConstOne(FV)) {
            auto *NotX = BinaryInst::createBooleanNot(Cond);
            if (Instruction::insertBefore(SI, NotX)) {
                return new BinaryInst(NotX, BinaryInst::OR, TV);
            }
            delete NotX;
        }
        if (IRType::isBool(Cond->getIRType()) && Value::isConstZero(TV)) {
            auto *NotX = BinaryInst::createBooleanNot(Cond);
            if (Instruction::insertBefore(SI, NotX)) {
                return new BinaryInst(NotX, BinaryInst::AND, FV);
            }
            delete NotX;
        }
    }
    if (IRType::isBool(Cond->getIRType()) && IRType::isInt(ResTy) && !IRType::isBool(ResTy) &&
        Value::isConstOne(TV) && Value::isConstZero(FV)) {
        auto *ZI = new ZextInst(
            Cond, std::dynamic_pointer_cast<INTType>(ResTy));
        return ZI;
    }
    if (auto *Cmp = dynamic_cast<BinaryInst *>(Cond)) {
        Value *CmpL = nullptr;
        Value *CmpR = nullptr;
        if (BinaryInst::getOperands(Cmp, CmpL, CmpR) && CmpL && CmpR) {
            const bool IsFloatCmp = IRType::isFloat(CmpL->getIRType());
            if (!IsFloatCmp && Cmp->getOp() == BinaryInst::E &&
                ((CmpL == TV && CmpR == FV) || (CmpL == FV && CmpR == TV))) {
                return FV;
            }
            if (!IsFloatCmp && Cmp->getOp() == BinaryInst::NE &&
                ((CmpL == TV && CmpR == FV) || (CmpL == FV && CmpR == TV))) {
                return TV;
            }
            if (Cmp->getOp() == BinaryInst::E && Value::isConstZero(CmpR)) {
                if (auto *OrI = dynamic_cast<BinaryInst *>(CmpL)) {
                    Value *OrL = nullptr;
                    Value *OrR = nullptr;
                    if (OrI->getOp() == BinaryInst::OR && BinaryInst::getOperands(OrI, OrL, OrR) &&
                        (TV == OrL || TV == OrR) && Value::isConstZero(FV)) {
                        return Value::makeZeroForType(TV->getIRType());
                    }
                }
            }
            if (Cmp->getOp() == BinaryInst::E && Value::isConstMinusOne(CmpR)) {
                if (auto *AndI = dynamic_cast<BinaryInst *>(CmpL)) {
                    Value *AndL = nullptr;
                    Value *AndR = nullptr;
                    if (AndI->getOp() == BinaryInst::AND && BinaryInst::getOperands(AndI, AndL, AndR) &&
                        (TV == AndL || TV == AndR) && Value::isConstMinusOne(FV)) {
                        return Value::makeMinusOneForType(TV->getIRType());
                    }
                }
            }
        }
    }
    return nullptr;
}
//折叠PHI公共输入并尝试跨臂合并等价操作
Value *InstSimplifier::simplifyPhi(PhiInst *Phi) {
    //1.显式LCSSA PHI保持不动，避免破坏循环出口的规范SSA边界
    //2.依次尝试二元式、GEP和Load跨臂合并，任一成功即返回等价新值
    //3.退化PHI只忽略自引用和undef；含undef时公共值必须支配PHI才能替换
    if (!Phi) return nullptr;
    if (Phi->getIdent().find(".lcssa") != std::string::npos) return nullptr;
    if (Value *V = foldPhiOfBinary(Phi)) return V;
    if (Value *V = foldPhiOfGEP(Phi)) return V;
    if (Value *V = foldPhiOfLoad(Phi)) return V;
    Value *Common = nullptr;
    bool HasUndef = false;
    bool Compatible = forEachPhiIncoming(
        Phi, [&](unsigned, Value *InV, BasicBlock *) {
            if (!InV || InV == Phi) return true;
            if (Value::isUndef(InV)) {
                HasUndef = true;
                return true;
            }
            if (!Common) {
                Common = InV;
                return true;
            }
            return Common == InV;
        });
    if (!Compatible) return nullptr;
    if (!Common) return nullptr;
    if (HasUndef) {
        if (!DT) return nullptr;
        if (!DT->dominatesValue(Common, Phi)) return nullptr;
    }
    return Common;
}
//运行局部指令化简工作表直到达到固定点
bool InstSimplifier::run(Function &F) {
    //1.初始化去重工作表，只调度仍在函数中且拥有用户的指令
    bool Changed = false;
    UniqueWorkList<Instruction *> Q;
    std::unordered_set<Instruction *> Erased;
    auto enqueue = [&](Instruction *I) {
        if (!I) return;
        if (Erased.count(I)) return;
        if (!I->getParent()) return;
        Q.push(I);
    };
    for (Instruction *I : collectInstructions(F))
        if (I->hasUses()) enqueue(I);
    //2.化简接口只返回等价值；替换提交前保留旧用户以便重新调度新暴露的模式
    while (!Q.empty()) {
        Instruction *I = Q.pop();
        if (!I || Erased.count(I) || !I->getParent()) continue;
        Value *V = simplifyInstruction(I, Changed);
        if (!V || V == I) continue;
        std::vector<Instruction *> Users = I->getInstructionUsers();
        //2.1新建指令必须先占据旧指令位置，确保定义在迁移全部use前已经进入合法IR布局
        if (auto *NewI = dynamic_cast<Instruction *>(V)) {
            if (NewI->getParent() == nullptr) {
                I->replaceWith(NewI);
            }
        }
        I->replaceAllUsesWith(V);
        delete I;
        Erased.insert(I);
        Changed = true;
        //2.2重新调度旧用户和新指令，使一次折叠暴露的上层规则在同轮达到局部固定点
        for (Instruction *U : Users) enqueue(U);
        if (auto *NewI = dynamic_cast<Instruction *>(V)) enqueue(NewI);
    }
    return Changed;
}
//运行指令化简并返回能够保留的分析集合
PreservationStatus InstSimplifyPass::run(Function &F, FunctionAnalysisPassManager &FAM) {
    //1.获取支配、别名、循环和MemorySSA，供PHI可用性及load下沉的内存合法性判断。
    DominatorTree *DT = nullptr;
    DT = &FAM.getResult<DominantAnalysis>(F);
    BasicAAResult *AA = &FAM.getResult<BasicAliasAnalysis>(F);
    LoopInfo *LI = &FAM.getResult<LoopAnalysis>(F);
    MemorySSAResult *MSSA = &FAM.getResult<MemorySSAAnalysis>(F);
    InstSimplifier Simplifier(DT, &F, &FAM, AA, LI, MSSA);
    bool Changed = Simplifier.run(F);
    if (!Changed) return PreservationStatus::all();
    //6.CFG 不变，结构分析仍有效；值与内存访问形态变化使 AA/副作用摘要失效。
    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<PostDominantAnalysis>();
    PA.keep<LoopAnalysis>();
    PA.discard<BasicAliasAnalysis>();
    PA.discard<MemorySSAAnalysis>();
    PA.discard<SideEffectAnalysis>();
    return PA;
}
