#include "ir/opt/SCCP.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"
#include <cassert>
#include <algorithm>
#include <variant>
#include <cmath>
#include <cstdint>
extern bool checkBinaryBoolean(BinaryInst::Operation op);

static bool isConstantZeroValue(Value* V) {
    return V && V->isConstZero();
}

static bool isConstantOneValue(Value* V) {
    return V && V->isConstOne();
}

static Value* makeZeroForType(std::shared_ptr<IRType> Ty) {
    return Value::makeZeroForType(Ty);
}

static unsigned getIntBitWidthFromType(std::shared_ptr<IRType> Ty) {
    int Bits = Ty ? Ty->getIntegerBitWidth(32) : 32;
    if (Bits < 1) return 32;
    if (Bits > 32) return 32;
    return static_cast<unsigned>(Bits);
}

static int32_t normalizeSignedToWidth(int64_t v, unsigned bits) {
    if (bits == 0 || bits >= 32) {
        return static_cast<int32_t>(v);
    }
    const uint32_t mask = (1u << bits) - 1u;
    uint32_t x = static_cast<uint32_t>(v) & mask;
    const uint32_t sign = 1u << (bits - 1);
    if (x & sign) x |= ~mask;
    return static_cast<int32_t>(x);
}

static bool getIntLikeConstWithWidth(Value* V, unsigned bits, int32_t& out) {
    if (auto* ci = dynamic_cast<ConstInt*>(V)) {
        out = normalizeSignedToWidth(ci->getVal(), bits);
        return true;
    }
    if (auto* cb = dynamic_cast<ConstBool*>(V)) {
        out = normalizeSignedToWidth(cb->getVal() ? 1 : 0, bits);
        return true;
    }
    return false;
}

static bool getFloatLikeConst(Value* V, float& out) {
    if (auto* cf = dynamic_cast<ConstFloat*>(V)) {
        out = cf->getVal();
        return true;
    }
    if (auto* ci = dynamic_cast<ConstInt*>(V)) {
        out = static_cast<float>(ci->getVal());
        return true;
    }
    if (auto* cb = dynamic_cast<ConstBool*>(V)) {
        out = cb->getVal() ? 1.0f : 0.0f;
        return true;
    }
    return false;
}

// LatticeValue 辅助
bool LatticeValue::areSameConstant(Value* a, Value* b) {
    if (a == b) return true;
    // ConstInt
    auto* ai = dynamic_cast<ConstInt*>(a);
    auto* bi = dynamic_cast<ConstInt*>(b);
    if (ai && bi) return ai->getVal() == bi->getVal();
    // ConstFloat
    auto* af = dynamic_cast<ConstFloat*>(a);
    auto* bf = dynamic_cast<ConstFloat*>(b);
    if (af && bf) return af->getVal() == bf->getVal();
    // ConstBool
    auto* ab = dynamic_cast<ConstBool*>(a);
    auto* bb = dynamic_cast<ConstBool*>(b);
    if (ab && bb) return ab->getVal() == bb->getVal();
    return false;
}

// SCCPSolver 实现
LatticeValue& SCCPSolver::getValueState(Value* V) {
    auto it = ValueState.find(V);
    if (it != ValueState.end()) return it->second;

    // 初始化：常量 → Constant，其他 → Top
    LatticeValue& LV = ValueState[V];
    if (V->isConst()) {
        LV = LatticeValue::getConstant(V);
    }
    // 全局变量（非常量）→ Bottom（地址不可预测）
    else if (V->isGlobal()) {
        LV = LatticeValue::getBottom();
    }
    // 其他默认 Top
    return LV;
}

void SCCPSolver::pushUsersToWorkList(Value* V) {
    for (auto it = V->getUseList().begin(); it != V->getUseList().end(); ++it) {
        Use* U = *it;
        if (Instruction* UserInst = dynamic_cast<Instruction*>(U->getUser())) {
            InstWorkList.push_back(UserInst);
        }
    }
}

bool SCCPSolver::mergeInValue(Value* V, const LatticeValue& NewVal) {
    LatticeValue& LV = getValueState(V);
    if (LV.mergeIn(NewVal)) {
        pushUsersToWorkList(V);
        return true;
    }
    return false;
}

void SCCPSolver::markBlockExecutable(BasicBlock* BB) {
    if (ExecutableBlocks.insert(BB).second) {
        BBWorkList.push_back(BB);
    }
}

bool SCCPSolver::markEdgeFeasible(BasicBlock* Source, BasicBlock* Dest) {
    auto edge = std::make_pair(Source, Dest);
    if (!FeasibleEdges.insert(edge).second)
        return false; // 已经可行

    if (ExecutableBlocks.count(Dest)) {
        // 目标块已经可执行，重新访问其 Phi 节点
        for (auto inst : *Dest) {
            PhiInst* Phi = dynamic_cast<PhiInst*>(inst);
            if (!Phi) break;
            InstWorkList.push_back(Phi);
        }
    } else {
        markBlockExecutable(Dest);
    }
    return true;
}

bool SCCPSolver::isBlockExecutable(BasicBlock* BB) const {
    return ExecutableBlocks.count(BB) > 0;
}

bool SCCPSolver::isEdgeFeasible(BasicBlock* From, BasicBlock* To) const {
    return FeasibleEdges.count({From, To}) > 0;
}

LatticeValue SCCPSolver::getLatticeValue(Value* V) const {
    auto it = ValueState.find(V);
    if (it != ValueState.end()) return it->second;
    if (V->isConst()) return LatticeValue::getConstant(V);
    return LatticeValue::getTop();
}

// 核心求解循环
void SCCPSolver::solve() {
    while (!BBWorkList.empty() || !InstWorkList.empty()) {
        // 优先处理指令工作队列
        while (!InstWorkList.empty()) {
            Instruction* I = InstWorkList.back();
            InstWorkList.pop_back();
            visitInstruction(I);
        }
        // 处理基本块工作队列
        while (!BBWorkList.empty()) {
            BasicBlock* BB = BBWorkList.back();
            BBWorkList.pop_back();
            for (auto inst : *BB) {
                visitInstruction(inst);
            }
        }
    }
}

//指令分发
void SCCPSolver::visitInstruction(Instruction* I) {
    // 只处理所在基本块可执行的指令
    BasicBlock* parent = I->ChainNode<BasicBlock, Instruction>::getParent();
    if (!parent || !isBlockExecutable(parent)) return;

    if (auto* Phi = dynamic_cast<PhiInst*>(I)) {
        visitPhiNode(Phi);
    } else if (auto* BI = dynamic_cast<BinaryInst*>(I)) {
        visitBinaryInst(BI);
    } else if (auto* Br = dynamic_cast<BrInst*>(I)) {
        visitBranchInst(Br);
    } else if (auto* Ret = dynamic_cast<RetInst*>(I)) {
        visitRetInst(Ret);
    } else if (auto* LI = dynamic_cast<LoadInst*>(I)) {
        visitLoadInst(LI);
    } else if (auto* SI = dynamic_cast<StoreInst*>(I)) {
        visitStoreInst(SI);
    } else if (auto* CI = dynamic_cast<CallInst*>(I)) {
        visitCallInst(CI);
    } else if (auto* GEP = dynamic_cast<GetElementPtrInst*>(I)) {
        visitGEPInst(GEP);
    } else if (I->instType == Instruction::SITOFP ||
               I->instType == Instruction::FPTOSI ||
               I->instType == Instruction::ZEXT ||
               I->instType == Instruction::SEXT ||
               I->instType == Instruction::TRUNC) {
        visitCastInst(I);
    } else if (I->instType == Instruction::SHL ||
               I->instType == Instruction::LSHR ||
               I->instType == Instruction::ASHR) {
        visitShiftInst(I);
    } else if (I->instType == Instruction::BITCAST) {
        // BitCast 是类型重解释，结果不可预测
        mergeInValue(I, LatticeValue::getBottom());
    } else if (I->instType == Instruction::SELECT) {
        visitSelectInst(I);
    }
}

// Phi 节点
void SCCPSolver::visitPhiNode(PhiInst* Phi) {
    BasicBlock* PhiBB = Phi->ChainNode<BasicBlock, Instruction>::getParent();
    LatticeValue Result = LatticeValue::getTop();

    for (unsigned i = 0; i < Phi->getNumIncomingValues(); i++) {
        BasicBlock* InBB = Phi->getIncomingBlock(i);
        // 只考虑可行边
        if (!isEdgeFeasible(InBB, PhiBB))
            continue;

        Value* InVal = Phi->getIncomingValue(i);
        LatticeValue InLV = getValueState(InVal);

        Result.mergeIn(InLV);
        if (Result.isBottom()) break; // 已经是 Bottom，无需继续
    }

    mergeInValue(Phi, Result);
}

// 二元运算指令
void SCCPSolver::visitBinaryInst(BinaryInst* BI) {
    auto& uses = BI->getUserList();
    if (uses.size() < 2) {
        mergeInValue(BI, LatticeValue::getBottom());
        return;
    }

    Value* LHS = uses[0]->getValue();
    Value* RHS = uses[1]->getValue();

    LatticeValue LV_LHS = getValueState(LHS);
    LatticeValue LV_RHS = getValueState(RHS);

    Value* lhsConst = LV_LHS.isConstant() ? LV_LHS.getConstantValue() : nullptr;
    Value* rhsConst = LV_RHS.isConstant() ? LV_RHS.getConstantValue() : nullptr;
    const auto op = BI->getOp();

    // 代数恒等式：允许在一侧是 Bottom/NAC 时仍然精确传播。
    switch (op) {
    case BinaryInst::ADD:
        if (lhsConst && isConstantZeroValue(lhsConst)) {
            mergeInValue(BI, LV_RHS);
            return;
        }
        if (rhsConst && isConstantZeroValue(rhsConst)) {
            mergeInValue(BI, LV_LHS);
            return;
        }
        break;
    case BinaryInst::SUB:
        if (rhsConst && isConstantZeroValue(rhsConst)) {
            mergeInValue(BI, LV_LHS);
            return;
        }
        break;
    case BinaryInst::MUL:
        if ((lhsConst && isConstantZeroValue(lhsConst)) ||
            (rhsConst && isConstantZeroValue(rhsConst))) {
            mergeInValue(BI, LatticeValue::getConstant(makeZeroForType(BI->getIRType())));
            return;
        }
        if (lhsConst && isConstantOneValue(lhsConst)) {
            mergeInValue(BI, LV_RHS);
            return;
        }
        if (rhsConst && isConstantOneValue(rhsConst)) {
            mergeInValue(BI, LV_LHS);
            return;
        }
        break;
    case BinaryInst::DIV:
        if (rhsConst && isConstantOneValue(rhsConst)) {
            mergeInValue(BI, LV_LHS);
            return;
        }
        break;
    case BinaryInst::UDIV:
        if (rhsConst && isConstantOneValue(rhsConst)) {
            mergeInValue(BI, LV_LHS);
            return;
        }
        break;
    case BinaryInst::MOD:
        if (rhsConst && isConstantOneValue(rhsConst)) {
            mergeInValue(BI, LatticeValue::getConstant(makeZeroForType(BI->getIRType())));
            return;
        }
        break;
    case BinaryInst::UREM:
        if (rhsConst && isConstantOneValue(rhsConst)) {
            mergeInValue(BI, LatticeValue::getConstant(makeZeroForType(BI->getIRType())));
            return;
        }
        break;
    case BinaryInst::AND:
        if ((lhsConst && isConstantZeroValue(lhsConst)) ||
            (rhsConst && isConstantZeroValue(rhsConst))) {
            mergeInValue(BI, LatticeValue::getConstant(makeZeroForType(BI->getIRType())));
            return;
        }
        break;
    case BinaryInst::OR:
        if (lhsConst && isConstantZeroValue(lhsConst)) {
            mergeInValue(BI, LV_RHS);
            return;
        }
        if (rhsConst && isConstantZeroValue(rhsConst)) {
            mergeInValue(BI, LV_LHS);
            return;
        }
        break;
    case BinaryInst::XOR:
        if (lhsConst && isConstantZeroValue(lhsConst)) {
            mergeInValue(BI, LV_RHS);
            return;
        }
        if (rhsConst && isConstantZeroValue(rhsConst)) {
            mergeInValue(BI, LV_LHS);
            return;
        }
        break;
    case BinaryInst::SHL:
    case BinaryInst::LSHR:
    case BinaryInst::ASHR:
        if (rhsConst && isConstantZeroValue(rhsConst)) {
            mergeInValue(BI, LV_LHS);
            return;
        }
        break;
    default:
        break;
    }

    // 如果任一操作数为 Bottom → 结果 Bottom
    if (LV_LHS.isBottom() || LV_RHS.isBottom()) {
        mergeInValue(BI, LatticeValue::getBottom());
        return;
    }

    // 如果任一操作数为 Top → 结果暂时为 Top（等待更多信息）
    if (LV_LHS.isTop() || LV_RHS.isTop()) {
        return; // 保持 Top，不做改变
    }

    // 两个操作数都是 Constant → 尝试常量折叠
    Value* result = tryConstantFoldBinary(op, lhsConst, rhsConst);
    if (result) {
        mergeInValue(BI, LatticeValue::getConstant(result));
    } else {
        mergeInValue(BI, LatticeValue::getBottom());
    }
}

// 分支指令 → 控制流发现
void SCCPSolver::visitBranchInst(BrInst* Br) {
    BasicBlock* BrBB = Br->ChainNode<BasicBlock, Instruction>::getParent();
    auto& uses = Br->getUserList();

    if (uses.size() == 1) {
        // 无条件跳转
        BasicBlock* dest = dynamic_cast<BasicBlock*>(uses[0]->getValue());
        if (dest) markEdgeFeasible(BrBB, dest);
        return;
    }

    if (uses.size() == 3) {
        // 条件跳转: uses[0]=cond, uses[1]=trueBB, uses[2]=falseBB
        Value* cond = uses[0]->getValue();
        BasicBlock* trueBB = dynamic_cast<BasicBlock*>(uses[1]->getValue());
        BasicBlock* falseBB = dynamic_cast<BasicBlock*>(uses[2]->getValue());

        LatticeValue condLV = getValueState(cond);

        if (condLV.isBottom()) {
            // 条件不确定，两条边都可行
            if (trueBB) markEdgeFeasible(BrBB, trueBB);
            if (falseBB) markEdgeFeasible(BrBB, falseBB);
            return;
        }

        if (condLV.isTop()) {
            // 尚未确定，先不标记任何边
            return;
        }

        // 条件是常量
        Value* constCond = condLV.getConstantValue();
        bool condTrue = false;
        if (auto* cb = dynamic_cast<ConstBool*>(constCond)) {
            condTrue = cb->getVal();
        } else if (auto* ci = dynamic_cast<ConstInt*>(constCond)) {
            condTrue = (ci->getVal() != 0);
        } else {
            // 无法判断，保守处理
            if (trueBB) markEdgeFeasible(BrBB, trueBB);
            if (falseBB) markEdgeFeasible(BrBB, falseBB);
            return;
        }

        if (condTrue) {
            if (trueBB) markEdgeFeasible(BrBB, trueBB);
        } else {
            if (falseBB) markEdgeFeasible(BrBB, falseBB);
        }
    }
}

// Ret, Load, Store, Call, GEP, Cast
void SCCPSolver::visitRetInst(RetInst*) {
    // Ret 不产生新值，无需传播
}

void SCCPSolver::visitLoadInst(LoadInst* LI) {
    // 保守处理 Load：标记为 Bottom（不做 load 常量传播）
    mergeInValue(LI, LatticeValue::getBottom());
}

void SCCPSolver::visitStoreInst(StoreInst*) {
    // Store 不产生值
}

void SCCPSolver::visitCallInst(CallInst* CI) {
    // 保守处理：Call 返回值标为 Bottom
    if (CI->getIRType() && CI->getIRType()->getEnum() != IR_VOID) {
        mergeInValue(CI, LatticeValue::getBottom());
    }
}

void SCCPSolver::visitGEPInst(GetElementPtrInst* GEP) {
    // GEP 结果为指针，标为 Bottom
    mergeInValue(GEP, LatticeValue::getBottom());
}

void SCCPSolver::visitCastInst(Instruction* I) {
    auto& uses = I->getUserList();
    if (uses.empty()) {
        mergeInValue(I, LatticeValue::getBottom());
        return;
    }

    Value* operand = uses[0]->getValue();
    LatticeValue opLV = getValueState(operand);

    if (opLV.isBottom()) {
        mergeInValue(I, LatticeValue::getBottom());
        return;
    }
    if (opLV.isTop()) {
        return; // 等待更多信息
    }

    // 操作数是常量，尝试常量折叠
    Value* result = tryConstantFoldCast(I->instType, opLV.getConstantValue());
    if (result) {
        mergeInValue(I, LatticeValue::getConstant(result));
    } else {
        mergeInValue(I, LatticeValue::getBottom());
    }
}

// 常量折叠：二元运算
Value* SCCPSolver::tryConstantFoldBinary(BinaryInst::Operation op,
                                          Value* LHS, Value* RHS) {
    // 除法除以零 → 返回 undef
    if ((op == BinaryInst::DIV || op == BinaryInst::UDIV) && RHS->isConstZero()) {
        if (LHS->getIRType()->getEnum() == IR_INT)
            return UndefValue::NewUndefValue(INTType::NewInt(4));
        else
            return UndefValue::NewUndefValue(FLOATType::NewFloat());
    }
    if ((op == BinaryInst::MOD || op == BinaryInst::UREM) && RHS->isConstZero()) {
        return UndefValue::NewUndefValue(INTType::NewInt(4));
    }

    auto* lhsTy = dynamic_cast<INTType*>(LHS->getIRType().get());
    const bool lhsIsIntLike = lhsTy != nullptr;

    // 比较运算：按被比较类型区分整数比较与浮点比较。
    if (checkBinaryBoolean(op)) {
        if (lhsIsIntLike) {
            const unsigned bw = lhsTy->getIsBool() ? 1u : static_cast<unsigned>(lhsTy->getSize() * 8);
            int32_t li = 0;
            int32_t ri = 0;
            if (!getIntLikeConstWithWidth(LHS, bw, li) || !getIntLikeConstWithWidth(RHS, bw, ri)) {
                return nullptr;
            }
            bool cmp = false;
            switch (op) {
            case BinaryInst::E:  cmp = (li == ri); break;
            case BinaryInst::NE: cmp = (li != ri); break;
            case BinaryInst::G:  cmp = (li > ri); break;
            case BinaryInst::GE: cmp = (li >= ri); break;
            case BinaryInst::L:  cmp = (li < ri); break;
            case BinaryInst::LE: cmp = (li <= ri); break;
            default: return nullptr;
            }
            return ConstBool::newConstBool(cmp);
        }

        float lf = 0.0f;
        float rf = 0.0f;
        if (!getFloatLikeConst(LHS, lf) || !getFloatLikeConst(RHS, rf)) {
            return nullptr;
        }
        bool cmp = false;
        switch (op) {
        case BinaryInst::E:  cmp = (lf == rf); break;
        case BinaryInst::NE: cmp = (lf != rf); break;
        case BinaryInst::G:  cmp = (lf > rf); break;
        case BinaryInst::GE: cmp = (lf >= rf); break;
        case BinaryInst::L:  cmp = (lf < rf); break;
        case BinaryInst::LE: cmp = (lf <= rf); break;
        default: return nullptr;
        }
        return ConstBool::newConstBool(cmp);
    }

    // 整数算术/按位：按位宽进行归一化，避免 i1/i8 等窄类型折叠不精确。
    if (lhsIsIntLike) {
        const unsigned bw = lhsTy->getIsBool() ? 1u : static_cast<unsigned>(lhsTy->getSize() * 8);
        int32_t li = 0;
        int32_t ri = 0;
        if (!getIntLikeConstWithWidth(LHS, bw, li) || !getIntLikeConstWithWidth(RHS, bw, ri)) {
            return nullptr;
        }

        int64_t raw = 0;
        switch (op) {
        case BinaryInst::ADD: raw = static_cast<int64_t>(li) + static_cast<int64_t>(ri); break;
        case BinaryInst::SUB: raw = static_cast<int64_t>(li) - static_cast<int64_t>(ri); break;
        case BinaryInst::MUL: raw = static_cast<int64_t>(li) * static_cast<int64_t>(ri); break;
        case BinaryInst::DIV:
            if (ri == 0) return UndefValue::NewUndefValue(LHS->getIRType());
            raw = li / ri;
            break;
        case BinaryInst::UDIV: {
            if (ri == 0) return UndefValue::NewUndefValue(LHS->getIRType());
            uint32_t l = static_cast<uint32_t>(li);
            uint32_t r = static_cast<uint32_t>(ri);
            raw = static_cast<int64_t>(l / r);
            break;
        }
        case BinaryInst::MOD:
            if (ri == 0) return UndefValue::NewUndefValue(LHS->getIRType());
            raw = li % ri;
            break;
        case BinaryInst::UREM: {
            if (ri == 0) return UndefValue::NewUndefValue(LHS->getIRType());
            uint32_t l = static_cast<uint32_t>(li);
            uint32_t r = static_cast<uint32_t>(ri);
            raw = static_cast<int64_t>(l % r);
            break;
        }
        case BinaryInst::AND: raw = static_cast<int64_t>(li & ri); break;
        case BinaryInst::OR:  raw = static_cast<int64_t>(li | ri); break;
        case BinaryInst::XOR: raw = static_cast<int64_t>(li ^ ri); break;
        case BinaryInst::SHL:
            if (ri < 0 || ri >= static_cast<int32_t>(bw)) return UndefValue::NewUndefValue(LHS->getIRType());
            raw = static_cast<int64_t>(static_cast<uint32_t>(li) << ri);
            break;
        case BinaryInst::LSHR:
            if (ri < 0 || ri >= static_cast<int32_t>(bw)) return UndefValue::NewUndefValue(LHS->getIRType());
            raw = static_cast<int64_t>(static_cast<uint32_t>(li) >> ri);
            break;
        case BinaryInst::ASHR:
            if (ri < 0 || ri >= static_cast<int32_t>(bw)) return UndefValue::NewUndefValue(LHS->getIRType());
            raw = static_cast<int64_t>(li >> ri);
            break;
        default:
            return nullptr;
        }

        const int32_t norm = normalizeSignedToWidth(raw, bw);
        if (lhsTy->getIsBool()) {
            return ConstBool::newConstBool((norm & 1) != 0);
        }
        return ConstInt::newConstInt(norm, lhsTy->getSize());
    }

    // 浮点算术
    float lf = 0.0f;
    float rf = 0.0f;
    if (!getFloatLikeConst(LHS, lf) || !getFloatLikeConst(RHS, rf)) {
        return nullptr;
    }

    switch (op) {
    case BinaryInst::ADD: return ConstFloat::newConstFloat(lf + rf);
    case BinaryInst::SUB: return ConstFloat::newConstFloat(lf - rf);
    case BinaryInst::MUL: return ConstFloat::newConstFloat(lf * rf);
    case BinaryInst::DIV:
        if (rf == 0.0f) return UndefValue::NewUndefValue(FLOATType::NewFloat());
        return ConstFloat::newConstFloat(lf / rf);
    default:
        return nullptr;
    }
}

// 常量折叠：类型转换
Value* SCCPSolver::tryConstantFoldCast(Instruction::InstType castTy,
                                        Value* Operand) {
    switch (castTy) {
    case Instruction::SITOFP: {
        if (auto* ci = dynamic_cast<ConstInt*>(Operand))
            return ConstFloat::newConstFloat((float)ci->getVal());
        if (auto* cb = dynamic_cast<ConstBool*>(Operand))
            return ConstFloat::newConstFloat((float)cb->getVal());
        return nullptr;
    }
    case Instruction::FPTOSI: {
        if (auto* cf = dynamic_cast<ConstFloat*>(Operand))
            return ConstInt::newConstInt((int)cf->getVal());
        return nullptr;
    }
    case Instruction::ZEXT: {
        if (auto* cb = dynamic_cast<ConstBool*>(Operand))
            return ConstInt::newConstInt((int)cb->getVal());
        if (auto* ci = dynamic_cast<ConstInt*>(Operand))
            return ConstInt::newConstInt(ci->getVal());
        return nullptr;
    }
        case Instruction::SEXT: {
        // 符号扩展: i1 -> i32 时 true 符号扩展为 -1, false 为 0
        if (auto* cb = dynamic_cast<ConstBool*>(Operand))
            return ConstInt::newConstInt(cb->getVal() ? -1 : 0);
        // 同宽度 sext 是恒等变换
        if (auto* ci = dynamic_cast<ConstInt*>(Operand))
            return ConstInt::newConstInt(ci->getVal());
        return nullptr;
    }
    case Instruction::TRUNC: {
        // 截断: i32 -> i1 取最低位
        if (auto* ci = dynamic_cast<ConstInt*>(Operand))
            return ConstBool::newConstBool((ci->getVal() & 1) != 0);
        if (auto* cb = dynamic_cast<ConstBool*>(Operand))
            return ConstBool::newConstBool(cb->getVal());
        return nullptr;
    }
    default:
        return nullptr;
    }
}

// 常量折叠：位移运算
Value* SCCPSolver::tryConstantFoldShift(Instruction::InstType shiftTy,
                                         Value* LHS, Value* RHS) {
    // 位移操作仅对整数有意义
    auto* li = dynamic_cast<ConstInt*>(LHS);
    auto* ri = dynamic_cast<ConstInt*>(RHS);

    // 也处理 ConstBool 操作数（提升为 int）
    int lhs, rhs;
    if (li) lhs = li->getVal();
    else if (auto* lb = dynamic_cast<ConstBool*>(LHS)) lhs = (int)lb->getVal();
    else return nullptr;

    if (ri) rhs = ri->getVal();
    else if (auto* rb = dynamic_cast<ConstBool*>(RHS)) rhs = (int)rb->getVal();
    else return nullptr;

    unsigned lhsBits = getIntBitWidthFromType(LHS->getIRType());

    // 位移量越界 → 未定义行为，返回 undef
    if (rhs < 0 || rhs >= static_cast<int>(lhsBits))
        return UndefValue::NewUndefValue(INTType::NewInt(4));

    int result;
    switch (shiftTy) {
    case Instruction::SHL:
        result = normalizeSignedToWidth(static_cast<int64_t>(lhs) << rhs, lhsBits);
        break;
    case Instruction::LSHR:
        if (lhsBits >= 32) {
            result = static_cast<int32_t>(static_cast<uint32_t>(lhs) >> rhs);
        } else {
            uint32_t mask = (1u << lhsBits) - 1u;
            uint32_t ux = static_cast<uint32_t>(lhs) & mask;
            result = normalizeSignedToWidth(static_cast<int64_t>(ux >> rhs), lhsBits);
        }
        break;
    case Instruction::ASHR:
        result = normalizeSignedToWidth(lhs, lhsBits) >> rhs; // 算术右移保持符号位
        break;
    default:
        return nullptr;
    }
    auto* iTy = dynamic_cast<INTType*>(LHS->getIRType().get());
    if (iTy && iTy->getIsBool()) return ConstBool::newConstBool((result & 1) != 0);
    if (iTy) return ConstInt::newConstInt(result, iTy->getSize());
    return ConstInt::newConstInt(result);
}

// 位移指令
void SCCPSolver::visitShiftInst(Instruction* I) {
    auto& uses = I->getUserList();
    if (uses.size() < 2) {
        mergeInValue(I, LatticeValue::getBottom());
        return;
    }

    Value* LHS = uses[0]->getValue();
    Value* RHS = uses[1]->getValue();

    LatticeValue LV_LHS = getValueState(LHS);
    LatticeValue LV_RHS = getValueState(RHS);

    if (LV_LHS.isBottom() || LV_RHS.isBottom()) {
        mergeInValue(I, LatticeValue::getBottom());
        return;
    }
    if (LV_LHS.isTop() || LV_RHS.isTop()) {
        return;
    }

    Value* result = tryConstantFoldShift(I->instType,
                                          LV_LHS.getConstantValue(),
                                          LV_RHS.getConstantValue());
    if (result)
        mergeInValue(I, LatticeValue::getConstant(result));
    else
        mergeInValue(I, LatticeValue::getBottom());
}

// Select 指令: %r = select i1 %c, T %a, T %b
void SCCPSolver::visitSelectInst(Instruction* I) {
    auto& uses = I->getUserList();
    if (uses.size() < 3) {
        mergeInValue(I, LatticeValue::getBottom());
        return;
    }

    Value* cond = uses[0]->getValue();
    Value* trueVal = uses[1]->getValue();
    Value* falseVal = uses[2]->getValue();

    // 无论条件是否已知，只要两侧值相同，select 结果可直接确定。
    LatticeValue trueLV = getValueState(trueVal);
    LatticeValue falseLV = getValueState(falseVal);
    if (trueLV.isConstant() && falseLV.isConstant() &&
        LatticeValue::areSameConstant(trueLV.getConstantValue(), falseLV.getConstantValue())) {
        mergeInValue(I, trueLV);
        return;
    }

    LatticeValue condLV = getValueState(cond);

    if (condLV.isBottom()) {
        // 条件不确定，两条路径都可能，合并 true 和 false 的格值
        LatticeValue result = trueLV;
        result.mergeIn(falseLV);
        mergeInValue(I, result);
        return;
    }

    if (condLV.isTop()) {
        return; // 条件尚未确定
    }

    // 条件是常量 → 直接选择对应分支的值
    Value* constCond = condLV.getConstantValue();
    bool isTrue = false;
    if (auto* cb = dynamic_cast<ConstBool*>(constCond))
        isTrue = cb->getVal();
    else if (auto* ci = dynamic_cast<ConstInt*>(constCond))
        isTrue = (ci->getVal() != 0);
    else {
        mergeInValue(I, LatticeValue::getBottom());
        return;
    }

    mergeInValue(I, getValueState(isTrue ? trueVal : falseVal));
}

// 变换阶段：将求解结果应用到 IR
bool SCCPSolver::rewriteConstants(Function& F, bool &CfgChanged) {
    bool changed = false;
    CfgChanged = false;

    // 1. 收集不可达块
    std::vector<BasicBlock*> deadBlocks;
    for (auto bb : F) {
        if (!isBlockExecutable(bb)) {
            deadBlocks.push_back(bb);
        }
    }

    // 2. 在可达块中替换常量
    for (auto bb : F) {
        if (!isBlockExecutable(bb)) continue;

        // 安全遍历：先收集再处理
        std::vector<Instruction*> insts;
        for (auto inst : *bb) {
            insts.push_back(inst);
        }

        for (Instruction* I : insts) {
            // 跳过无结果类型（void）、终止指令、有副作用的指令
            if (I->getIRType()->getEnum() == IR_VOID) continue;
            if (dynamic_cast<RetInst*>(I)) continue;
            if (dynamic_cast<BrInst*>(I)) continue;
            if (dynamic_cast<StoreInst*>(I)) continue;
            if (dynamic_cast<AllocaInst*>(I)) continue;
            if (dynamic_cast<CallInst*>(I)) continue;

            auto it = ValueState.find(I);
            if (it == ValueState.end()) continue;

            const LatticeValue& LV = it->second;
            if (!LV.isConstant()) continue;

            Value* constVal = LV.getConstantValue();
            if (!constVal) continue;

            // 不要用自己替换自己
            if (constVal == I) continue;

            // 替换所有使用
            I->replaceAllUsesWith(constVal);

            // 如果指令不再被使用，且无副作用，可以删除
            if (I->getUseList().is_empty()) {
                delete I; // ~Node 会自动从链表摘除，~Use 会断开关联
            }
            changed = true;
        }
    }

    // 3. 处理不可达块
    // 3a. 清理不可达块 → 可达后继 的 Phi 条目
    for (BasicBlock* DeadBB : deadBlocks) {
        for (BasicBlock* succ : DeadBB->getSuccessors()) {
            if (isBlockExecutable(succ)) {
                // 移除 succ 中 Phi 来自 DeadBB 的 incoming
                std::vector<PhiInst*> phis;
                for (auto inst : *succ) {
                    PhiInst* phi = dynamic_cast<PhiInst*>(inst);
                    if (!phi) break;
                    phis.push_back(phi);
                }
                for (PhiInst* phi : phis) {
                    bool needRebuild = false;
                    for (unsigned i = 0; i < phi->getNumIncomingValues(); i++) {
                        if (phi->getIncomingBlock(i) == DeadBB) {
                            needRebuild = true;
                            break;
                        }
                    }
                    if (!needRebuild) continue;

                    PhiInst* newPhi = new PhiInst(phi->getIRType());
                    for (unsigned i = 0; i < phi->getNumIncomingValues(); i++) {
                        if (phi->getIncomingBlock(i) != DeadBB) {
                            newPhi->addIncoming(phi->getIncomingValue(i),
                                                phi->getIncomingBlock(i));
                        }
                    }

                    if (newPhi->getNumIncomingValues() == 0) {
                        phi->replaceAllUsesWith(
                            UndefValue::NewUndefValue(phi->getIRType()));
                        delete newPhi;
                    } else if (newPhi->getNumIncomingValues() == 1) {
                        phi->replaceAllUsesWith(newPhi->getIncomingValue(0));
                        delete newPhi;
                    } else {
                        phi->replaceAllUsesWith(newPhi);
                        phi->replaceWith(newPhi);
                    }
                    delete phi;
                }
            }
        }
    }

    // 3b. 断开不可达块中指令的 Use，然后删除指令和块
    for (BasicBlock* DeadBB : deadBlocks) {
        for (auto inst : *DeadBB) {
            if (!inst->getUseList().is_empty()) {
                inst->replaceAllUsesWith(
                    UndefValue::NewUndefValue(inst->getIRType()));
            }
        }
    }
    for (BasicBlock* DeadBB : deadBlocks) {
        while (DeadBB->front() != nullptr) {
            Instruction* inst = DeadBB->front();
            delete inst;
        }
        changed = true;
    }
    for (BasicBlock* DeadBB : deadBlocks) {
        DeadBB->EraseFromParent();
    }
    if (!deadBlocks.empty()) {
        CfgChanged = true;
    }

    // 4. 对于可达块中条件为常量的分支，替换为无条件跳转
    for (auto bb : F) {
        if (!isBlockExecutable(bb)) continue;
        if (bb->Size() == 0) continue;

        Instruction* term = bb->back();
        BrInst* br = dynamic_cast<BrInst*>(term);
        if (!br) continue;

        auto& uses = br->getUserList();
        if (uses.size() != 3) continue; // 只处理条件分支

        Value* cond = uses[0]->getValue();
        LatticeValue condLV = getLatticeValue(cond);
        if (!condLV.isConstant()) continue;

        Value* constCond = condLV.getConstantValue();
        bool condTrue = false;
        if (auto* cb = dynamic_cast<ConstBool*>(constCond))
            condTrue = cb->getVal();
        else if (auto* ci = dynamic_cast<ConstInt*>(constCond))
            condTrue = (ci->getVal() != 0);
        else
            continue;

        BasicBlock* trueBB  = dynamic_cast<BasicBlock*>(uses[1]->getValue());
        BasicBlock* falseBB = dynamic_cast<BasicBlock*>(uses[2]->getValue());
        BasicBlock* target = condTrue ? trueBB : falseBB;
        BasicBlock* removed = condTrue ? falseBB : trueBB;

        if (!target || !removed) continue;

        // 移除 removed 块中 Phi 来自当前块的 incoming
        if (removed != target) {
            std::vector<PhiInst*> phis;
            for (auto inst : *removed) {
                PhiInst* phi = dynamic_cast<PhiInst*>(inst);
                if (!phi) break;
                phis.push_back(phi);
            }
            for (PhiInst* phi : phis) {
                bool needRebuild = false;
                for (unsigned i = 0; i < phi->getNumIncomingValues(); i++) {
                    if (phi->getIncomingBlock(i) == bb) {
                        needRebuild = true;
                        break;
                    }
                }
                if (!needRebuild) continue;

                PhiInst* newPhi = new PhiInst(phi->getIRType());
                for (unsigned i = 0; i < phi->getNumIncomingValues(); i++) {
                    if (phi->getIncomingBlock(i) != bb) {
                        newPhi->addIncoming(phi->getIncomingValue(i),
                                            phi->getIncomingBlock(i));
                    }
                }
                if (newPhi->getNumIncomingValues() == 0) {
                    phi->replaceAllUsesWith(
                        UndefValue::NewUndefValue(phi->getIRType()));
                    delete newPhi;
                } else if (newPhi->getNumIncomingValues() == 1) {
                    phi->replaceAllUsesWith(newPhi->getIncomingValue(0));
                    delete newPhi;
                } else {
                    phi->replaceAllUsesWith(newPhi);
                    phi->replaceWith(newPhi);
                }
                delete phi;
            }
        }

        // 替换条件分支为无条件跳转
        delete br; // 从链表中移除旧的条件分支
        BrInst* newBr = new BrInst(target);
        bb->push_back(newBr);
        changed = true;
        CfgChanged = true;
    }

    return changed;
}

// SCCP Pass 入口
PreservationStatus SCCPPass::run(Function &F, FunctionAnalysisPassManager &FAM) {
    SCCPSolver Solver;

    // 1. 标记入口块为可执行
    BasicBlock* entry = F.front();
    if (!entry) return PreservationStatus::all();
    Solver.markBlockExecutable(entry);

    // 2. 函数参数标记为 Bottom（非常量）
    for (auto& param : F.getParams()) {
        Solver.mergeInValue(param.get(), LatticeValue::getBottom());
    }

    // 3. 求解
    Solver.solve();

    // 4. 应用变换
    bool cfgChanged = false;
    bool changed = Solver.rewriteConstants(F, cfgChanged);

    if (!changed)
        return PreservationStatus::all();

    PreservationStatus PA;
    if (!cfgChanged) {
        PA.keep<DominantAnalysis>();
        PA.keep<PostDominantAnalysis>();
        PA.keep<LoopAnalysis>();
        PA.keep<BasicAliasAnalysis>();
        PA.keep<SideEffectAnalysis>();
    }
    return PA;
}
