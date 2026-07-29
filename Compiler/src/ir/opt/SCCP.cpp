#include "ir/opt/SCCP.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/MemorySSA.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "lib/IRUtils.hpp"

#include <cmath>

// 格值合并实现求解器的单调性：未提供信息的 Top 不改变现状，任一 Bottom
// 使结果过定义，两个语义相等的常量保持 Constant，否则下降到 Bottom。
LatticeValue::LatticeValue() : state(Top), constVal(nullptr) {}

LatticeValue LatticeValue::getTop() {
    return LatticeValue();
}

LatticeValue LatticeValue::getBottom() {
    LatticeValue LV;
    LV.state = Bottom;
    return LV;
}

LatticeValue LatticeValue::getConstant(Value *V) {
    LatticeValue LV;
    LV.state = Constant;
    LV.constVal = V;
    return LV;
}

bool LatticeValue::isTop() const {
    return state == Top;
}

bool LatticeValue::isBottom() const {
    return state == Bottom;
}

bool LatticeValue::isConstant() const {
    return state == Constant;
}

LatticeValue::State LatticeValue::getState() const {
    return state;
}

Value *LatticeValue::getConstantValue() const {
    return constVal;
}

bool LatticeValue::mergeIn(const LatticeValue &Other) {
    if (Other.isTop()) return false;
    if (isBottom()) return false;
    if (Other.isBottom()) {
        state = Bottom;
        constVal = nullptr;
        return true;
    }
    if (isTop()) {
        state = Constant;
        constVal = Other.constVal;
        return true;
    }
    if (constVal == Other.constVal) return false;

    // 常量对象地址可以不同，仍须按类型和值判断语义相等。
    if (Value::areEqualConstants(constVal, Other.constVal)) return false;

    state = Bottom;
    constVal = nullptr;
    return true;
}

SCCPSolver::SCCPSolver() = default;

SCCPSolver::SCCPSolver(
    Function &F, MemorySSAResult &MSSA, BasicAAResult &AA,
    const LoopInfo &LI, FunctionAnalysisPassManager &FAM) {
    initializeMemoryDependencies(F, MSSA, AA, LI, FAM);
}

// 内存依赖预处理只接受可以完整证明的Load：
// 1. MemorySSA必须完成所有反向路径的首写查询；
// 2. 每个首写都必须是与Load位置MustAlias且完整覆盖的Store；
// 3. 所有Store必须写入同一个、类型兼容的SSA值。
// 任一LiveOnEntry、Call、MayAlias、部分覆盖、不同值或查询预算耗尽都会使
// 尚未建模的Load保持过定义。
void SCCPSolver::initializeMemoryDependencies(
    Function &F, MemorySSAResult &MSSA, BasicAAResult &AA,
    const LoopInfo &LI, FunctionAnalysisPassManager &FAM) {
    constexpr std::size_t MaxMemoryLoadQueries = 512;
    std::size_t QueriedLoads = 0;
    for (BasicBlock *BB : F) {
        if (!BB) continue;
        for (Instruction *I : *BB) {
            auto *Load = I ? I->as<LoadInst>() : nullptr;
            if (!Load) continue;
            if (QueriedLoads++ >= MaxMemoryLoadQueries) return;

            MemoryLocation LoadLocation = MemoryLocation::get(Load);
            if (!LoadLocation.Pointer || !LoadLocation.Size)
                continue;

            MemoryClobberResult ClobberResult =
                MSSA.getClobberingAccesses(
                    Load, LoadLocation, AA, LI, FAM, 256);
            if (!ClobberResult.Complete ||
                ClobberResult.Clobbers.empty())
                continue;

            Value *CommonValue = nullptr;
            std::vector<StoreInst *> ReachingStores;
            bool Proven = true;

            for (MemoryAccess *Clobber : ClobberResult.Clobbers) {
                Instruction *ClobberInst =
                    Clobber && Clobber->isDef()
                        ? Clobber->getInstruction()
                        : nullptr;
                auto *Store =
                    ClobberInst
                        ? ClobberInst->as<StoreInst>()
                        : nullptr;
                Value *StoredValue =
                    Store ? Store->getValueOperand() : nullptr;
                MemoryLocation StoreLocation =
                    MemoryLocation::get(Store);

                if (!Store || !StoredValue ||
                    StoredValue->isUndef() ||
                    !Store->fullyCoversItsPointer() ||
                    !StoreLocation.Pointer || !StoreLocation.Size ||
                    *StoreLocation.Size < *LoadLocation.Size ||
                    !IRType::hasSameShape(
                        StoredValue->getIRType(),
                        Load->getIRType()) ||
                    AA.aliasWithIndexRange(
                        LoadLocation, StoreLocation, LI) !=
                        AliasInfo::MustAlias) {
                    Proven = false;
                    break;
                }

                if (!CommonValue) {
                    CommonValue = StoredValue;
                } else if (CommonValue != StoredValue) {
                    Proven = false;
                    break;
                }
                ReachingStores.push_back(Store);
            }

            if (!Proven || !CommonValue ||
                ReachingStores.empty())
                continue;

            LoadStoredValues[Load] = CommonValue;
            for (StoreInst *Store : ReachingStores)
                StoreLoadDependents[Store].push_back(Load);
        }
    }
}

// 求解步骤 1.1：按需初始化格值。字面常量直接为 Constant，全局地址因运行时
// 可观察而设为 Bottom，其余 SSA 定义从 Top 开始等待可行路径传播。
LatticeValue& SCCPSolver::getValueState(Value* V) {
    auto it = ValueState.find(V);
    if (it != ValueState.end()) return it->second;

    LatticeValue& LV = ValueState[V];
    if (V->isConst()) {
        LV = LatticeValue::getConstant(V);
    }
    else if (V->isGlobal()) {
        LV = LatticeValue::getBottom();
    }

    return LV;
}

// 求解步骤 2：格值下降后重新访问全部指令使用者，使数据流变化继续传播。
void SCCPSolver::pushUsersToWorkList(Value* V) {
    auto Users = V->getInstructionUsers();
    InstWorkList.insert(InstWorkList.end(), Users.begin(), Users.end());
}

bool SCCPSolver::mergeInValue(Value* V, const LatticeValue& NewVal) {
    LatticeValue& LV = getValueState(V);
    if (LV.mergeIn(NewVal)) {
        pushUsersToWorkList(V);
        return true;
    }
    return false;
}

// 求解步骤 1：首次发现基本块可执行时，把整块加入扫描队列。
void SCCPSolver::markBlockExecutable(BasicBlock* BB) {
    if (ExecutableBlocks.insert(BB).second) {
        BBWorkList.push_back(BB);
    }
}

// 求解步骤 1.2：记录新可行 CFG 边。若目标块早已可执行，只需重新计算其
// PHI；否则把目标块整体加入块队列。
bool SCCPSolver::markEdgeFeasible(BasicBlock* Source, BasicBlock* Dest) {
    auto edge = std::make_pair(Source, Dest);
    if (!FeasibleEdges.insert(edge).second)
        return false;

    if (ExecutableBlocks.count(Dest)) {
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

// 求解步骤 3：优先排空由格值变化产生的指令队列，再扫描新发现的可执行块；
// 任一访问器产生的新工作都会使外层循环继续，直至控制流和数值同时稳定。
void SCCPSolver::solve() {
    while (!BBWorkList.empty() || !InstWorkList.empty()) {
        while (!InstWorkList.empty()) {
            Instruction* I = InstWorkList.back();
            InstWorkList.pop_back();
            visitInstruction(I);
        }
        while (!BBWorkList.empty()) {
            BasicBlock* BB = BBWorkList.back();
            BBWorkList.pop_back();
            for (auto inst : *BB) {
                visitInstruction(inst);
            }
        }
    }
}

// 指令分发只处理已经证明可执行的块；每个访问器均为纯求解操作，不在工作
// 队列尚未收敛时改写 IR。
void SCCPSolver::visitInstruction(Instruction* I) {
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
        // 位模式转换不在当前常量求值范围内，结果保守视为过定义。
        mergeInValue(I, LatticeValue::getBottom());
    } else if (I->instType == Instruction::SELECT) {
        visitSelectInst(I);
    }
}

// 求解步骤 2.1：PHI 只合并已证明可行的入边；遇到 Bottom 可提前停止，因为
// 后续入边不可能再提高格值精度。
void SCCPSolver::visitPhiNode(PhiInst* Phi) {
    BasicBlock* PhiBB = Phi->getParent();
    LatticeValue Result = LatticeValue::getTop();

    forEachPhiIncoming(
        Phi, [&](unsigned, Value *InVal, BasicBlock *InBB) {
            if (!isEdgeFeasible(InBB, PhiBB))
                return true;

            LatticeValue InLV = getValueState(InVal);

            Result.mergeIn(InLV);
            return !Result.isBottom();
        });

    mergeInValue(Phi, Result);
}

// 求解步骤 2.2：先应用在存在过定义操作数时仍严格成立的代数恒等式，再按
// 三态格传播；两个常量最终交由统一常量折叠器计算。
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

    switch (op) {
    case BinaryInst::ADD:
        // 严格 IEEE 语义下仅 -0.0 是加法恒等元，不能把 +0.0 一概消去。
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
        // x - +0.0 恒等于 x；对 -0.0 不成立。
        if (rhsConst && rhsConst->isConstZero() &&
            (!IsFloat || isPositiveFloatZero(rhsConst))) {
            mergeInValue(BI, LV_LHS);
            return;
        }
        break;
    case BinaryInst::MUL:
        // 仅整数乘零可直接归零；浮点零与 Inf/NaN 的组合不能这样折叠。
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
        // 有符号整数模一恒为零；浮点余数不适用。
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

    // 任一操作数过定义且没有恒等式可用时，结果同样过定义。
    if (LV_LHS.isBottom() || LV_RHS.isBottom()) {
        mergeInValue(BI, LatticeValue::getBottom());
        return;
    }

    // Top 表示尚需等待更多可行路径，本轮不降低结果格值。
    if (LV_LHS.isTop() || LV_RHS.isTop()) {
        return;
    }

    // 两个操作数均为常量时执行完整语义折叠；不支持或非法的组合降为 Bottom。
    Value* result =
        BinaryInst::foldConstants(op, lhsConst, rhsConst, true);
    if (result) {
        mergeInValue(BI, LatticeValue::getConstant(result));
    } else {
        mergeInValue(BI, LatticeValue::getBottom());
    }
}

// 求解步骤 2.3：分支把数值格信息转成边可行性。常量条件只开放选中边，
// Bottom 条件开放两边，Top 条件等待后续数据流信息。
void SCCPSolver::visitBranchInst(BrInst* Br) {
    BasicBlock* BrBB = Br->getParent();

    if (!Br->isConditional()) {
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
            if (trueBB) markEdgeFeasible(BrBB, trueBB);
            if (falseBB) markEdgeFeasible(BrBB, falseBB);
            return;
        }

        if (condLV.isTop()) {
            return;
        }

        Value* constCond = condLV.getConstantValue();
        bool condTrue = false;
        if (!constCond || !constCond->tryGetBoolConst(condTrue)) {
            // 无法解释为布尔值时保守开放两条边。
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

void SCCPSolver::visitRetInst(RetInst*) {
}

// Load只传播MemorySSA证明的共同到达写入值；该值仍为Top时继续等待，
// 已知常量或Bottom则沿SCCP格正常下降。无法完成内存证明的Load保持过定义。
void SCCPSolver::visitLoadInst(LoadInst* LI) {
    auto It = LoadStoredValues.find(LI);
    if (It == LoadStoredValues.end()) {
        mergeInValue(LI, LatticeValue::getBottom());
        return;
    }
    mergeInValue(LI, getValueState(It->second));
}

// Store不产生SSA结果；其写入值的格一旦下降，Store会作为普通SSA使用者被
// 调度到这里，再把所有由该写支撑的Load压回工作队列。
void SCCPSolver::visitStoreInst(StoreInst* SI) {
    auto It = StoreLoadDependents.find(SI);
    if (It == StoreLoadDependents.end()) return;
    InstWorkList.insert(
        InstWorkList.end(), It->second.begin(), It->second.end());
}

// 调用和地址结果尚未纳入稀疏常量内存模型：有返回值的此类指令均
// 视为过定义，store/ret 因不产生 SSA 结果无需更新格值。
void SCCPSolver::visitCallInst(CallInst* CI) {
    if (CI->getIRType() && !CI->getIRType()->isVoid()) {
        mergeInValue(CI, LatticeValue::getBottom());
    }
}

void SCCPSolver::visitGEPInst(GetElementPtrInst* GEP) {
    mergeInValue(GEP, LatticeValue::getBottom());
}

// 求解步骤 2.4：cast 等待操作数稳定；常量输入按目标类型折叠，失败则过定义。
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
        return;
    }

    Value* result = Instruction::foldConstantCast(
        I->instType, opLV.getConstantValue(), I->getIRType());
    if (result) {
        mergeInValue(I, LatticeValue::getConstant(result));
    } else {
        mergeInValue(I, LatticeValue::getBottom());
    }
}

// 求解步骤 2.5：select 两臂相等时无需知道条件；过定义条件合并两臂，未知
// 条件继续等待，常量条件则只传播被选择的一臂。
void SCCPSolver::visitSelectInst(Instruction* I) {
    auto *SI = dynamic_cast<SelectInst *>(I);
    if (!SI) {
        mergeInValue(I, LatticeValue::getBottom());
        return;
    }
    Value* cond = SI->getCondition();
    Value* trueVal = SI->getTrueValue();
    Value* falseVal = SI->getFalseValue();

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
        LatticeValue result = trueLV;
        result.mergeIn(falseLV);
        mergeInValue(I, result);
        return;
    }

    if (condLV.isTop()) {
        return;
    }

    Value* constCond = condLV.getConstantValue();
    bool isTrue = false;
    if (!constCond || !constCond->tryGetBoolConst(isTrue)) {
        mergeInValue(I, LatticeValue::getBottom());
        return;
    }

    mergeInValue(I, getValueState(isTrue ? trueVal : falseVal));
}

// 改写阶段在求解完全收敛后统一执行，避免遍历期间的 CFG 变化污染可行性
// 集合。顺序固定为常量替换、不可达块清理、常量分支折叠。
bool SCCPSolver::rewriteConstants(Function& F, bool &CfgChanged) {
    bool changed = false;
    CfgChanged = false;

    // 1. 快照未进入可执行集合的基本块，后续删除时不再查询已变化的 CFG。
    std::vector<BasicBlock*> deadBlocks;
    for (auto bb : F) {
        if (!isBlockExecutable(bb)) {
            deadBlocks.push_back(bb);
        }
    }

    // 2. 在可达块中用求得的常量替换无副作用 SSA 指令；先快照指令以允许
    // 删除已失去全部使用的定义。
    for (auto bb : F) {
        if (!isBlockExecutable(bb)) continue;

        std::vector<Instruction*> insts;
        for (auto inst : *bb) {
            insts.push_back(inst);
        }

        for (Instruction* I : insts) {
            // 终结、内存写、栈分配和调用不属于可直接删除的纯值定义。
            if (I->getIRType()->isVoid()) continue;
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

            if (constVal == I) continue;

            I->replaceAllUsesWith(constVal);

            if (!I->hasUses()) {
                delete I;
            }
            changed = true;
        }
    }

    // 3. 删除死块前，先从每个可达后继 PHI 中移除对应入边；重建后的 PHI
    // 若为空或仅有一个输入，则立即折叠为 undef 或唯一输入。
    for (BasicBlock* DeadBB : deadBlocks) {
        for (BasicBlock* succ : DeadBB->getSuccessors()) {
            if (isBlockExecutable(succ)) {
                std::vector<PhiInst*> phis;
                for (auto inst : *succ) {
                    PhiInst* phi = dynamic_cast<PhiInst*>(inst);
                    if (!phi) break;
                    phis.push_back(phi);
                }
                for (PhiInst* phi : phis) {
                    bool needRebuild = false;
                    forEachPhiIncoming(
                        phi, [&](unsigned, Value *, BasicBlock *IncomingBlock) {
                            if (IncomingBlock == DeadBB) {
                                needRebuild = true;
                                return false;
                            }
                            return true;
                        });
                    if (!needRebuild) continue;

                    PhiInst* newPhi = new PhiInst(phi->getIRType());
                    forEachPhiIncoming(
                        phi, [&](unsigned, Value *IncomingValue,
                                 BasicBlock *IncomingBlock) {
                            if (IncomingBlock != DeadBB) {
                                newPhi->addIncoming(IncomingValue, IncomingBlock);
                            }
                        });

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

    // 4. 先把死块定义的残余外部使用替换为 undef，再分三轮断开指令、移除
    // 块内容和摘除基本块，避免跨死块引用形成悬空 use。
    for (BasicBlock* DeadBB : deadBlocks) {
        for (auto inst : *DeadBB) {
            if (inst->hasUses()) {
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
        DeadBB->eraseFromParent();
    }
    if (!deadBlocks.empty()) {
        CfgChanged = true;
    }

    // 5. 可达块中的已知条件分支改为无条件分支；改写前同步删除未选边在
    // 后继 PHI 中的输入。
    for (auto bb : F) {
        if (!isBlockExecutable(bb)) continue;
        if (bb->size() == 0) continue;

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

        if (removed != target)
            BasicBlock::removeEdgeFromSuccPhis(removed, bb, -1);

        BasicBlock::replaceCondBrWithUncond(bb, target);
        changed = true;
        CfgChanged = true;
    }

    return changed;
}

// Pass 主流程：初始化入口与参数边界，运行双工作队列求解到不动点，再一次性
// 将结果写回 IR。
PreservationStatus SCCPPass::run(Function &F, FunctionAnalysisPassManager &FAM) {
    // 1. 入口块是初始唯一可执行块。
    BasicBlock* entry = F.front();
    if (!entry) return PreservationStatus::all();

    // 2. 复用MemorySSA的全路径首写查询和BasicAA的区间别名结论，预建
    // Store到Load的稀疏内存依赖；求解阶段只传播已经完整证明的共同写入值。
    auto &MSSA = FAM.getResult<MemorySSAAnalysis>(F);
    auto &AA = FAM.getResult<BasicAliasAnalysis>(F);
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    SCCPSolver Solver(F, MSSA, AA, LI, FAM);
    Solver.markBlockExecutable(entry);

    // 3. 函数参数由调用者决定，初始设为过定义。
    for (auto& param : F.getParams()) {
        Solver.mergeInValue(param.get(), LatticeValue::getBottom());
    }

    // 4. 交替传播可行边和数值格，直到两者均不再变化。
    Solver.solve();

    // 5. 提交常量替换、死块删除与分支折叠。
    bool cfgChanged = false;
    bool changed = Solver.rewriteConstants(F, cfgChanged);

    if (!changed)
        return PreservationStatus::all();

    PreservationStatus PA;
    if (!cfgChanged) {
        // 6. 纯 SSA 改写保留 CFG 分析；Load常量化会删除MemoryUse，
        // GEP、指针PHI或内存指令变化仍可能改变别名和传递副作用答案。
        PA.keep<DominantAnalysis>();
        PA.keep<PostDominantAnalysis>();
        PA.keep<LoopAnalysis>();
        PA.discard<BasicAliasAnalysis>();
        PA.discard<MemorySSAAnalysis>();
        PA.discard<SideEffectAnalysis>();
    }
    // 删除基本块或折叠分支时保持空保留集，使所有依赖 CFG 的分析重新计算。
    return PA;
}
