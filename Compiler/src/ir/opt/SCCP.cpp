#include "ir/opt/SCCP.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"

#include <cmath>

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
    BasicBlock* parent = I->getParent();
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
    } else if (I->instType == Instruction::BITCAST) {
        // BitCast 是类型重解释，结果不可预测
        mergeInValue(I, LatticeValue::getBottom());
    } else if (I->instType == Instruction::SELECT) {
        visitSelectInst(I);
    }
}

// Phi 节点
void SCCPSolver::visitPhiNode(PhiInst* Phi) {
    BasicBlock* PhiBB = Phi->getParent();
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
    Value* LHS = nullptr;
    Value* RHS = nullptr;
    if (!BI->getOperands(LHS, RHS)) {
        mergeInValue(BI, LatticeValue::getBottom());
        return;
    }

    LatticeValue LV_LHS = getValueState(LHS);
    LatticeValue LV_RHS = getValueState(RHS);

    Value* lhsConst = LV_LHS.isConstant() ? LV_LHS.getConstantValue() : nullptr;
    Value* rhsConst = LV_RHS.isConstant() ? LV_RHS.getConstantValue() : nullptr;
    const auto op = BI->getOp();
    const bool IsFloat = IRType::isFloat(BI->getIRType());

    auto isNegativeFloatZero = [](Value *V) {
        float F = 0.0f;
        return V && Value::tryGetFloatConst(V, F) && F == 0.0f &&
               std::signbit(F);
    };
    auto isPositiveFloatZero = [](Value *V) {
        float F = 0.0f;
        return V && Value::tryGetFloatConst(V, F) && F == 0.0f &&
               !std::signbit(F);
    };

    // 代数恒等式：允许在一侧是 Bottom/NAC 时仍然精确传播。
    switch (op) {
    case BinaryInst::ADD:
        // In strict IEEE arithmetic only -0.0 is an additive identity:
        // +0.0 + -0.0 produces +0.0, not the RHS -0.0.
        if (lhsConst && lhsConst->isConstZero() &&
            (!IsFloat || isNegativeFloatZero(lhsConst))) {
            mergeInValue(BI, LV_RHS);
            return;
        }
        if (rhsConst && rhsConst->isConstZero() &&
            (!IsFloat || isNegativeFloatZero(rhsConst))) {
            mergeInValue(BI, LV_LHS);
            return;
        }
        break;
    case BinaryInst::SUB:
        // x - +0.0 is exact; x - -0.0 is not for x == -0.0.
        if (rhsConst && rhsConst->isConstZero() &&
            (!IsFloat || isPositiveFloatZero(rhsConst))) {
            mergeInValue(BI, LV_LHS);
            return;
        }
        break;
    case BinaryInst::MUL:
        // fmul 0.0, Inf/NaN is NaN and signed zero is observable.
        if (!IsFloat &&
            ((lhsConst && lhsConst->isConstZero()) ||
             (rhsConst && rhsConst->isConstZero()))) {
            mergeInValue(BI, LatticeValue::getConstant(Value::makeZeroForType(BI->getIRType())));
            return;
        }
        if (lhsConst && lhsConst->isConstOne()) {
            mergeInValue(BI, LV_RHS);
            return;
        }
        if (rhsConst && rhsConst->isConstOne()) {
            mergeInValue(BI, LV_LHS);
            return;
        }
        break;
    case BinaryInst::DIV:
        if (rhsConst && rhsConst->isConstOne()) {
            mergeInValue(BI, LV_LHS);
            return;
        }
        break;
    case BinaryInst::UDIV:
        if (rhsConst && rhsConst->isConstOne()) {
            mergeInValue(BI, LV_LHS);
            return;
        }
        break;
    case BinaryInst::MOD:
        // frem x, 1.0 is not generally zero.
        if (!IsFloat && rhsConst && rhsConst->isConstOne()) {
            mergeInValue(BI, LatticeValue::getConstant(Value::makeZeroForType(BI->getIRType())));
            return;
        }
        break;
    case BinaryInst::UREM:
        if (rhsConst && rhsConst->isConstOne()) {
            mergeInValue(BI, LatticeValue::getConstant(Value::makeZeroForType(BI->getIRType())));
            return;
        }
        break;
    case BinaryInst::AND:
        if ((lhsConst && lhsConst->isConstZero()) ||
            (rhsConst && rhsConst->isConstZero())) {
            mergeInValue(BI, LatticeValue::getConstant(Value::makeZeroForType(BI->getIRType())));
            return;
        }
        break;
    case BinaryInst::OR:
        if (lhsConst && lhsConst->isConstZero()) {
            mergeInValue(BI, LV_RHS);
            return;
        }
        if (rhsConst && rhsConst->isConstZero()) {
            mergeInValue(BI, LV_LHS);
            return;
        }
        break;
    case BinaryInst::XOR:
        if (lhsConst && lhsConst->isConstZero()) {
            mergeInValue(BI, LV_RHS);
            return;
        }
        if (rhsConst && rhsConst->isConstZero()) {
            mergeInValue(BI, LV_LHS);
            return;
        }
        break;
    case BinaryInst::SHL:
    case BinaryInst::LSHR:
    case BinaryInst::ASHR:
        if (rhsConst && rhsConst->isConstZero()) {
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
    Value* result =
        BinaryInst::foldConstants(op, lhsConst, rhsConst, true);
    if (result) {
        mergeInValue(BI, LatticeValue::getConstant(result));
    } else {
        mergeInValue(BI, LatticeValue::getBottom());
    }
}

// 分支指令 → 控制流发现
void SCCPSolver::visitBranchInst(BrInst* Br) {
    BasicBlock* BrBB = Br->getParent();

    if (!Br->isConditional()) {
        // 无条件跳转
        BasicBlock* dest = Br->getTrueSuccessor();
        if (dest) markEdgeFeasible(BrBB, dest);
        return;
    }

    if (Br->isConditional()) {
        Value* cond = Br->getCondition();
        BasicBlock* trueBB = Br->getTrueSuccessor();
        BasicBlock* falseBB = Br->getFalseSuccessor();

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
        if (!constCond || !constCond->tryGetBoolConst(condTrue)) {
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
    Value* operand = I->getUnaryOperand();
    if (!operand) {
        mergeInValue(I, LatticeValue::getBottom());
        return;
    }
    LatticeValue opLV = getValueState(operand);

    if (opLV.isBottom()) {
        mergeInValue(I, LatticeValue::getBottom());
        return;
    }
    if (opLV.isTop()) {
        return; // 等待更多信息
    }

    // 操作数是常量，尝试常量折叠
    Value* result = Instruction::foldConstantCast(
        I->instType, opLV.getConstantValue());
    if (result) {
        mergeInValue(I, LatticeValue::getConstant(result));
    } else {
        mergeInValue(I, LatticeValue::getBottom());
    }
}

// Select 指令: %r = select i1 %c, T %a, T %b
void SCCPSolver::visitSelectInst(Instruction* I) {
    auto *SI = dynamic_cast<SelectInst *>(I);
    if (!SI) {
        mergeInValue(I, LatticeValue::getBottom());
        return;
    }
    Value* cond = SI->getCondition();
    Value* trueVal = SI->getTrueValue();
    Value* falseVal = SI->getFalseValue();

    // 无论条件是否已知，只要两侧值相同，select 结果可直接确定。
    LatticeValue trueLV = getValueState(trueVal);
    LatticeValue falseLV = getValueState(falseVal);
    if (trueLV.isConstant() && falseLV.isConstant() &&
        Value::areEqualConstants(trueLV.getConstantValue(),
                                 falseLV.getConstantValue())) {
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
    if (!constCond || !constCond->tryGetBoolConst(isTrue)) {
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

        if (!br->isConditional()) continue;

        Value* cond = br->getCondition();
        LatticeValue condLV = getLatticeValue(cond);
        if (!condLV.isConstant()) continue;

        Value* constCond = condLV.getConstantValue();
        bool condTrue = false;
        if (!constCond || !constCond->tryGetBoolConst(condTrue))
            continue;

        BasicBlock* trueBB = br->getTrueSuccessor();
        BasicBlock* falseBB = br->getFalseSuccessor();
        BasicBlock* target = condTrue ? trueBB : falseBB;
        BasicBlock* removed = condTrue ? falseBB : trueBB;

        if (!target || !removed) continue;

        // 移除 removed 块中 Phi 来自当前块的 incoming
        if (removed != target)
            BasicBlock::removeEdgeFromSuccPhis(removed, bb, -1);

        // 替换条件分支为无条件跳转
        BasicBlock::replaceCondBrWithUncond(bb, target);
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
        // Replacing GEP indices/pointer PHIs or removing memory instructions
        // can change alias and transitive side-effect answers even when the
        // CFG itself is unchanged.
        PA.discard<BasicAliasAnalysis>();
        PA.discard<SideEffectAnalysis>();
    }
    return PA;
}
