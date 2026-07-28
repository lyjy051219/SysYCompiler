#include "ir/opt/TailRecursionElimination.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "ir/opt/DCE.hpp"
#include "lib/CFG.hpp"
#include "lib/BaseManager.hpp"
#include "lib/IRUtils.hpp"
#include "lib/TypeSystem.hpp"
#include <cassert>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// 将目标 Ret 的返回值映射到 Pred 上可用的 SSA 值。
// 允许处理位于 TargetBB 中的 PHI 与 BitCast 链，其他 TargetBB 本地指令一律判为不合法。
Value *TailRecursionEliminationPass::resolveRetValueForPred(Value *V,
                                     BasicBlock *Pred,
                                     BasicBlock *TargetBB,
                                     BasicBlock *InsertBB,
                                     Instruction *InsertBefore,
                                     std::vector<Instruction *> &ClonedInsts,
                                     std::unordered_set<Value *> &Visiting) {
    if (!V) return nullptr;
    ScopedActiveMark ActiveMark(Visiting, V);
    if (!ActiveMark.inserted()) return nullptr;

    if (auto *Phi = dynamic_cast<PhiInst *>(V)) {
        if (Phi->getParent() != TargetBB) {
            return Phi;
        }
        Value *Incoming = Phi->getIncomingFromPred(Pred);
        if (!Incoming || Incoming == V) return nullptr;
        return resolveRetValueForPred(Incoming, Pred, TargetBB,
                                      InsertBB, InsertBefore, ClonedInsts, Visiting);
    }

    if (auto *BC = dynamic_cast<BitCastInst *>(V)) {
        if (BC->getParent() != TargetBB) return BC;
        Value *BCSrc = BC->getUnaryOperand();
        if (!BCSrc) return nullptr;

        Value *Src = resolveRetValueForPred(BCSrc, Pred, TargetBB,
                                            InsertBB, InsertBefore, ClonedInsts, Visiting);
        if (!Src) return nullptr;

        auto *NewBC = new BitCastInst(Src, BC->getIRType());
        if (!InsertBefore->isInBlock(InsertBB) ||
            !InsertBefore->insertBefore(NewBC)) {
            delete NewBC;
            return nullptr;
        }
        ClonedInsts.push_back(NewBC);
        return NewBC;
    }

    if (auto *I = dynamic_cast<Instruction *>(V)) {
        if (I->getParent() == TargetBB) {
            return nullptr;
        }
    }

    return V;
}

bool TailRecursionEliminationPass::rewriteClonedRetOperandForPred(RetInst *ClonedRet,
                                           BasicBlock *Pred,
                                           BasicBlock *TargetBB,
                                           BasicBlock *InsertBB,
                                           Instruction *InsertBefore,
                                           std::vector<Instruction *> &ClonedInsts) {
    Value *RetVal = ClonedRet->getReturnValue();
    if (!RetVal) return true; // void ret

    std::unordered_set<Value *> Visiting;
    Value *Resolved = resolveRetValueForPred(RetVal, Pred, TargetBB,
                                             InsertBB, InsertBefore, ClonedInsts, Visiting);
    if (!Resolved) return false;
    ClonedRet->replaceOperand(0, Resolved);
    return true;
}


//  判断 CallInst 是否是对当前函数 F 的自递归调用
bool TailRecursionEliminationPass::isSelfCall(CallInst *CI, Function &F) {
    return CI && CI->getCallee() == &F;
}

//  快速检查：函数是否包含自递归调用
bool TailRecursionEliminationPass::hasSelfRecursion(Function &F) {
    return !forEachInstruction<CallInst>(
        F, [&](CallInst *CI) { return !isSelfCall(CI, F); });
}

bool TailRecursionEliminationPass::allocaAddressEscapes(
    AllocaInst *AI) {
    if (!AI) return false;

    std::vector<Value *> WorkList{AI};
    std::unordered_set<Value *> Visited;

    while (!WorkList.empty()) {
        Value *Current = WorkList.back();
        WorkList.pop_back();
        if (!Current || !Visited.insert(Current).second) continue;

        for (Use *UseEdge : Current->getUses()) {
            auto *UserInst =
                dynamic_cast<Instruction *>(UseEdge ? UseEdge->getUser()
                                                    : nullptr);
            if (!UserInst) return true;

            // GEP、位转换、PHI 和 select 只传播地址身份；继续沿结果
            // 检查，直到遇到真实的内存访问或逃逸点。
            if (auto *GEP = dynamic_cast<GetElementPtrInst *>(UserInst)) {
                if (GEP->getBasePointer() != Current) return true;
                WorkList.push_back(GEP);
                continue;
            }
            if (auto *BitCast = dynamic_cast<BitCastInst *>(UserInst)) {
                if (BitCast->getUnaryOperand() != Current) return true;
                WorkList.push_back(BitCast);
                continue;
            }
            if (dynamic_cast<PhiInst *>(UserInst)) {
                WorkList.push_back(UserInst);
                continue;
            }
            if (auto *Select = dynamic_cast<SelectInst *>(UserInst)) {
                if (Select->getTrueValue() != Current &&
                    Select->getFalseValue() != Current) {
                    return true;
                }
                WorkList.push_back(Select);
                continue;
            }

            // 作为 load/store 的地址仅访问当前帧对象，不会让地址逃逸；
            // 把地址本身存入任意内存则可能跨递归帧继续存活，必须拒绝。
            if (auto *Load = dynamic_cast<LoadInst *>(UserInst)) {
                if (Load->getPointerOperand() != Current) return true;
                continue;
            }
            if (auto *Store = dynamic_cast<StoreInst *>(UserInst)) {
                if (Store->getValueOperand() == Current) return true;
                if (Store->getPointerOperand() != Current) return true;
                continue;
            }

            if (dynamic_cast<CallInst *>(UserInst)) {
                // 当前 IR 没有 nocapture 属性；传给任意调用都按逃逸处理。
                return true;
            }

            // 返回地址、间接调用以及当前未显式建模的新指令都不能证明
            // 地址只在本帧内使用，保守拒绝整函数的尾递归消除。
            return true;
        }
    }

    return false;
}

bool TailRecursionEliminationPass::hasEscapingEntryAlloca(Function &F) {
    BasicBlock *Entry = F.front();
    if (!Entry) return false;

    for (AllocaInst *AI : collectInstructions<AllocaInst>(*Entry)) {
        if (allocaAddressEscapes(AI)) return true;
    }
    return false;
}

bool TailRecursionEliminationPass::isReadonlyLikeCallInst(Instruction *I,
                                   FunctionAnalysisPassManager &FAM,
                                   Function &F) {
    if (I->instType != Instruction::CALL) return true;
    auto *CI = dynamic_cast<CallInst *>(I);
    if (!CI || !CI->getCallee()) return false;
    if (auto *CalleeFunc = CI->getDirectCallee()) {
        const SideEffectInfo &SEI = FAM.getResult<SideEffectAnalysis>(*CalleeFunc);
        ModRefInfo FMRI = getFunctionModRefInfo(SEI);
        return FMRI == ModRefInfo::NoModRef || FMRI == ModRefInfo::Ref;
    }
    if (auto *BIF = CI->getBuiltinCallee()) {
        FuncAttr Attr = classifyBuiltin(BIF);
        return Attr == FuncAttr::NoModRef || Attr == FuncAttr::MemReadOnly;
    }
    return false;
}

// 当前 IR 没有显式 dereferenceable 属性。只有直接指向模块全局对象的
// 普通 load 可以在不依赖运行时路径的情况下证明地址始终有效；参数、
// GEP、PHI、select 与调用返回的指针都保守拒绝。
bool TailRecursionEliminationPass::isSafelyDereferenceableLoad(LoadInst *LI) {
    if (!LI) return false;
    Value *Pointer = LI->getPointerOperand();
    return Pointer && Pointer->isGlobal();
}

// 递归检查用户函数调用是否一定返回且执行过程中不产生 fault/poison。
// CFG 的“无环且所有路径返回”部分复用 DCE 的共享证明；指令部分复用
// DCE 的未知指令和投机安全判定。ActiveFunctions 用于拒绝直接或间接递归。
bool TailRecursionEliminationPass::isFunctionGuaranteedToReturnWithoutFault(
        Function &Callee,
        std::unordered_set<Function *> &ActiveFunctions) {
    ScopedActiveMark ActiveMark(ActiveFunctions, &Callee);
    if (!ActiveMark.inserted()) return false;
    if (!Callee.front() || !hasAcyclicReturningCFG(Callee)) return false;

    for (BasicBlock *BB : Callee) {
        for (Instruction *I : *BB) {
            if (!I) return false;

            // 写内存具有可观察效果，不能从 self-call 之后提前。
            if (dynamic_cast<StoreInst *>(I)) return false;

            if (auto *LI = dynamic_cast<LoadInst *>(I)) {
                if (!isSafelyDereferenceableLoad(LI)) return false;
                continue;
            }

            if (auto *NestedCall = dynamic_cast<CallInst *>(I)) {
                if (!isCallGuaranteedToReturnWithoutFault(
                        NestedCall, ActiveFunctions)) {
                    return false;
                }
                continue;
            }

            // 未知指令以及除法、取模、动态移位、FPTOSI 等都不能作为
            // “提前执行一定安全”的依据。
            if (dceInstructionMustBePreserved(I) ||
                instructionMayFaultOrProducePoison(I)) {
                return false;
            }
        }
    }

    return true;
}

bool TailRecursionEliminationPass::isCallGuaranteedToReturnWithoutFault(
        CallInst *CI,
        std::unordered_set<Function *> &ActiveFunctions) {
    if (!CI) return false;

    // 运行库和间接调用没有函数体可供本 pass 证明 will-return/fault-free。
    // 即使其内存属性是 NoModRef，也不能据此猜测控制流与异常行为。
    if (CI->getBuiltinCallee()) return false;
    Function *Callee = CI->getDirectCallee();
    if (!Callee) return false;
    return isFunctionGuaranteedToReturnWithoutFault(
        *Callee, ActiveFunctions);
}

// 证明后置只读调用可以越过 self-call：
// 对用户函数，把 SideEffectAnalysis 中的 RefSet 映射到当前调用实参；
// 对每个实际读取位置，再用统一的 ModRef 接口检查 self-call 不会修改它。
// 任何未知来源、未跟踪指针或可观察的运行库调用都保守拒绝移动。
bool TailRecursionEliminationPass::canMoveReadonlyCallBeforeSelfCall(
        CallInst *MovedCall, CallInst *SelfCall,
        FunctionAnalysisPassManager &FAM, Function &F) {
    if (!MovedCall || !SelfCall) return false;

    std::unordered_set<Function *> ActiveFunctions;
    if (!isCallGuaranteedToReturnWithoutFault(
            MovedCall, ActiveFunctions)) {
        return false;
    }

    auto &AA = FAM.getResult<BasicAliasAnalysis>(F);
    auto &LI = FAM.getResult<LoopAnalysis>(F);

    auto *CalleeFunc = MovedCall->getDirectCallee();
    if (!CalleeFunc) return false;
    const SideEffectInfo &SEI =
        FAM.getResult<SideEffectAnalysis>(*CalleeFunc);

    // 含运行库调用或未跟踪指针时，摘要不足以证明跨调用重排安全。
    if (SEI.hasBuiltin) return false;
    for (const auto &Entry : SEI.ptrMap) {
        if (Entry.second.untracked) return false;
    }

    ModRefInfo FMRI = getFunctionModRefInfo(SEI);
    if (FMRI == ModRefInfo::NoModRef) return true;
    if (FMRI != ModRefInfo::Ref || !SEI.ModSet.empty()) return false;

    for (Value *ReadValue : SEI.RefSet) {
        if (!ReadValue) return false;

        Value *ReadPointer = nullptr;
        if (ReadValue->isGlobal()) {
            ReadPointer = ReadValue;
        } else if (ReadValue->isParam()) {
            int ParamIndex = CalleeFunc->getParamIndex(ReadValue);
            if (ParamIndex < 0 ||
                static_cast<unsigned>(ParamIndex) >=
                    MovedCall->getArgCount()) {
                return false;
            }
            ReadPointer =
                MovedCall->getArg(static_cast<unsigned>(ParamIndex));
        } else {
            // RefSet 中出现无法映射到调用点的地址，不能猜测其来源。
            return false;
        }

        if (!ReadPointer || !ReadPointer->isPointerValue()) return false;
        ModRefInfo MRI =
            getInstModRefInfo(ReadPointer, SelfCall, AA, LI, FAM);
        if (isModSet(MRI)) return false;
    }

    return true;
}

// 更精细的“前置 self-call 干扰”判定：
// 仅当该前置 self-call 可能读写状态，或其返回值被使用时，才阻止当前候选。
bool TailRecursionEliminationPass::hasInterferingEarlierSelfCallInBlock(BasicBlock *BB,
                                                 CallInst *StopCI,
                                                 Function &F,
                                                 FunctionAnalysisPassManager &FAM) {
    if (!BB || !StopCI) return false;
    bool Interferes = false;
    forEachInstruction<CallInst>(*BB, [&](CallInst *CI) {
        if (CI == StopCI) return false;
        if (CI->getCallee() != &F) return true;
        if (!isReadonlyLikeCallInst(CI, FAM, F) || CI->hasUses()) {
            Interferes = true;
            return false;
        }
        return true;
    });
    return Interferes;
}

RetInst *TailRecursionEliminationPass::getRetOnlyTerminatorBlockRet(BasicBlock *BB) {
    if (!BB) return nullptr;
    RetInst *FoundRet = nullptr;
    bool seenNonPhi = false;
    for (auto inst : *BB) {
        if (!seenNonPhi && dynamic_cast<PhiInst *>(inst)) {
            continue;
        }
        seenNonPhi = true;
        auto *RI = dynamic_cast<RetInst *>(inst);
        if (!RI) return nullptr;
        if (FoundRet) return nullptr;
        FoundRet = RI;
    }
    return FoundRet;
}

/*  标记非自递归的尾调用
  扫描所有出口块 (以 RetInst 结尾)，若 ret 前一条指令是
  非自递归的 CallInst，且满足尾调用条件，则标记为 tail call*/
bool TailRecursionEliminationPass::markTailCalls(Function &F) {
    bool changed = false;

    for (auto bb : F) {
        //1.call必须紧跟ret
        Instruction *terminator = bb->back();
        if (!terminator) continue;
        auto *RI = dynamic_cast<RetInst*>(terminator);
        if (!RI) continue;
        Instruction *prevInst = dynamic_cast<Instruction*>(RI->getPrev());
        if (!prevInst) continue;
        auto *CI = dynamic_cast<CallInst*>(prevInst);
        if (!CI) continue;
        // tail 标记会改变后端的调用序列，计时函数及其封装调用必须保持
        // 普通调用语义，不能参与尾调用优化。
        if (CI->mayReachTimingCall()) continue;
        //4.call调用的函数不是自身
        if (isSelfCall(CI, F)) continue;
        //2.简单逃逸分析：没有任何Alloca被call指令当作参数传走
        bool hasEscapingLocalPtr = false;
        for (unsigned i = 0; i < CI->getArgCount(); ++i) {
            Value *argVal = CI->getArg(i);
            std::unordered_set<Value*> visited;
            std::vector<Value*> worklist;
            worklist.push_back(argVal);

            while (!worklist.empty()) {
                Value *curr = worklist.back();
                worklist.pop_back();
                if (!curr || visited.count(curr)) continue;
                visited.insert(curr);

                if (dynamic_cast<AllocaInst*>(curr)) {
                    hasEscapingLocalPtr = true;
                    break;
                }

                if (auto *inst = dynamic_cast<Instruction*>(curr)) {
                    forEachOperand(
                        inst, [&](size_t, Use *, Value *Operand) {
                        if (Operand)
                            worklist.push_back(Operand);
                    });
                }
            }
            if (hasEscapingLocalPtr) break;
        }

        if (hasEscapingLocalPtr) continue;
        if (CI->isTailCall()) continue;
        //3.检查Ret返回值与call必须匹配
        bool isVoid = F.getIRType()->isVoid();
        if (isVoid) {
            if (CI->getIRType()->isVoid()) {
                CI->setTailCall(true);
                changed = true;
            }
        } else {
            if (RI->getOperand(0) == CI) {
                CI->setTailCall(true);
                changed = true;
            }
        }
    }

    return changed;
}


// 第二部分: 自递归尾递归消除 (Tail Recursion → Loop)
// 从基本块末尾向前查找对自身的 CallInst（跳过 Ret/Br 终结指令）
CallInst *TailRecursionEliminationPass::findSelfCall(BasicBlock *BB, Function &F) {
    for (Instruction *I = BB->back(); I != nullptr; I = I->getPrev()) {
        if (I->instType != Instruction::CALL) continue;
        auto *CI = dynamic_cast<CallInst *>(I);
        if (CI && isSelfCall(CI, F))
            return CI;
    }
    return nullptr;
}

// 分析 call..ret 之间的指令:
//   1. 检查每条指令是否可移动到 call 之前
//   2. 识别累加/累乘伪尾递归模式
// 返回 true 表示尾递归可消除
bool TailRecursionEliminationPass::analyzeCallRetRange(
        CallInst *CI, RetInst *RI, Function &F,
        FunctionAnalysisPassManager &FAM,
        BinaryInst *&AccInst) {
    AccInst = nullptr;

    // 收集 call 与 ret 之间的指令 (不含 call 和 ret)
    std::vector<Instruction *> between;
    for (Instruction *I = CI->getNext(); I && I != RI; I = I->getNext())
        between.push_back(I);

    for (Instruction *I : between) {
        // ----- 识别累加器模式 -----
        if (auto *BI = dynamic_cast<BinaryInst *>(I)) {
            auto op = BI->getOp();
            // 严格浮点加法/乘法不满足结合律，改变递归结合顺序会影响
            // 舍入、NaN、无穷和有符号零；当前 IR 没有 fast-math 标记，
            // 因而累加器只允许使用 TypeSystem 中的整数类型。
            bool IsIntegerAccumulator =
                BI->getIRType() && BI->getIRType()->isInt();
            if ((op == BinaryInst::ADD || op == BinaryInst::MUL) &&
                IsIntegerAccumulator) {
                // 不支持多条累加指令
                if (AccInst) return false;

                Value *Op0 = nullptr;
                Value *Op1 = nullptr;
                if (!BI->getOperands(Op0, Op1)) return false;

                // 恰好有一个操作数是 call 返回值
                bool op0IsCall = (Op0 == static_cast<Value *>(CI));
                bool op1IsCall = (Op1 == static_cast<Value *>(CI));
                if (!(op0IsCall ^ op1IsCall)) return false;

                // 另一个操作数不能是对自身的递归调用
                Value *OtherOp = op0IsCall ? Op1 : Op0;
                if (auto *OtherCI = dynamic_cast<CallInst *>(OtherOp)) {
                    if (isSelfCall(OtherCI, F)) return false;
                }

                // 只能被使用一次且 User 是 Ret
                if (!BI->hasExactlyOneUse()) return false;
                Use *singleUse = BI->getSingleUse();
                if (!singleUse || singleUse->getUser() != RI) return false;

                AccInst = BI;
                continue;
            }
        }

        // ----- 检查指令能否移到 call 之前 -----

        // 这里发生的是跨 self-call 的代码移动，而不是死代码删除。
        // 即使别名分析证明递归调用不修改某个地址，也不能把原本只在
        // 递归返回后执行的 load、除法、非法/动态移位或 FPTOSI 提前到
        // 可能不返回的路径上。统一复用 DCE.hpp 的未知指令与投机
        // 安全判定，避免新增 IR 指令后默认被当作可移动，也避免各 pass
        // 对 poison/fault 语义给出不一致结论。
        if (dceInstructionMustBePreserved(I) ||
            instructionMayFaultOrProducePoison(I)) {
            return false;
        }

        // STORE 不行
        if (I->instType == Instruction::STORE) return false;

        // CALL: 被调函数必须只读
        if (I->instType == Instruction::CALL) {
            auto *Call = dynamic_cast<CallInst *>(I);
            if (!canMoveReadonlyCallBeforeSelfCall(Call, CI, FAM, F))
                return false;
        }

        // 操作数不能依赖 call 的返回值
        if (!forEachOperand(
                I, [CI](size_t, Use *, Value *Operand) {
                    return Operand != static_cast<Value *>(CI);
                })) {
            return false;
        }
    }

    // 非 void 返回必须精确返回 self-call，或返回上面完整验证过的
    // 整数累加器；不能仅凭块内存在一个递归调用就删除任意返回值。
    Value *RetValue = RI->getReturnValue();
    if (!F.getIRType() || F.getIRType()->isVoid()) {
        // void 情况只接受 call 紧邻 ret 的真正尾位置，不把原本位于
        // 递归返回之后的计算提前到可能不返回的 self-call 之前。
        return !RetValue && CI->getNext() == RI;
    }
    if (!RetValue) return false;
    if (AccInst) return RetValue == static_cast<Value *>(AccInst);
    return RetValue == static_cast<Value *>(CI);
}

// 执行单个尾递归 → 循环 变换
bool TailRecursionEliminationPass::eliminateTailRecursion(
        BasicBlock *BB, CallInst *CI, RetInst *RI,
        BinaryInst *AccInst, Function &F,
        FunctionAnalysisPassManager &FAM,
        BasicBlock *&LoopHeader,
        std::vector<PhiInst *> &ArgPhis,
        PhiInst *&AccPhi,
        std::vector<BasicBlock *> &TailBackedgeBlocks) {

    // ===== 3.1 创建循环头 (仅首次调用时执行) =====
    if (!LoopHeader) {
        BasicBlock *OldEntry = F.front();

        // 创建新入口块
        BasicBlock *NewEntry = new BasicBlock();
        F.pushFront(NewEntry);
        NewEntry->genUnCondInst(OldEntry);

        // 将原入口块中所有 Alloca 搬到 NewEntry 的 Br 之前
        {
            auto allocas = collectInstructions<AllocaInst>(*OldEntry);
            Instruction *brInst = NewEntry->back();
            auto brIt = NewEntry->begin();
            while (*brIt != brInst) ++brIt;
            for (AllocaInst *AI : allocas) {
                AI->eraseFromParent();
                brIt.insertBefore(AI);
            }
        }

        // 为每个形参在 OldEntry 创建 PHI
        auto &Params = F.getParams();
        ArgPhis.clear();
        for (size_t i = 0; i < Params.size(); ++i) {
            Value *Param = Params[i].get();
            auto *Phi = new PhiInst(Param->getIRType());
            OldEntry->pushFront(Phi);
            Param->replaceAllUsesWith(Phi);
            Phi->addIncoming(Param, NewEntry);
            ArgPhis.push_back(Phi);
        }

        LoopHeader = OldEntry;
    }

    // ===== 3.2 将尾递归实参添加到原入口块 PHI =====
    {
        for (size_t i = 0; i < ArgPhis.size(); ++i) {
            Value *Arg = CI->getArg(static_cast<unsigned>(i));
            if (Arg)
                ArgPhis[i]->addIncoming(Arg, BB);
        }
    }

    // ===== 3.3 累加器处理 =====
    if (AccInst) {
        BinaryInst::Operation accOp = AccInst->getOp();

        if (!AccPhi) {
            // 首次: 创建累加器 PHI
            AccPhi = new PhiInst(AccInst->getIRType());
            LoopHeader->pushFront(AccPhi);

            // 初始值: ADD→0, MUL→1
            Value *Identity = nullptr;
            if (accOp == BinaryInst::ADD) {
                Identity = Value::makeZeroForType(
                    AccInst->getIRType());
            } else {
                Identity = Value::makeOneForType(
                    AccInst->getIRType());
            }
            AccPhi->addIncoming(Identity, F.front()); // 来自 NewEntry

            // 可能已有不带累加器的尾递归点先被改成回边。新建 AccPhi
            // 时必须为这些既有前驱补齐 incoming，语义是原样传递当前
            // 累加值；否则会产生缺边的非法 PHI。
            for (BasicBlock *Backedge : TailBackedgeBlocks) {
                if (Backedge)
                    AccPhi->addIncoming(AccPhi, Backedge);
            }
        }

        // 找累加器中非 call 的操作数
        Value *Op0 = nullptr;
        Value *Op1 = nullptr;
        if (!AccInst->getOperands(Op0, Op1)) return false;
        Value *OtherOperand = (Op0 == static_cast<Value *>(CI)) ? Op1 : Op0;

        // 创建 AccPhi op OtherOperand, 插到 RI 之前
        auto *NewAcc = new BinaryInst(AccPhi, accOp, OtherOperand);
        {
            auto it = BB->begin();
            while (*it != RI) ++it;
            it.insertBefore(NewAcc);
        }

        // 将新的累加结果接到 AccPhi
        AccPhi->addIncoming(NewAcc, BB);
        // 旧的 AccInst 不在这里手动删除。
        // 当前 IR 的 Value 析构会沿 use 链级联删除 user，
        // 若此处直接 delete 可能连带删掉 Ret，破坏 CFG。
        // 无用指令交给后续 DCE 回收。
    } else if (AccPhi) {
        // 此尾递归点无累加器但全局已有: 传递 AccPhi 自身
        AccPhi->addIncoming(AccPhi, BB);
    }

    // ===== 3.4 收尾 =====

    // 将 call..ret 间可移动指令移到 call 之前
    {
        std::vector<Instruction *> toMove;
        for (Instruction *I = CI->getNext(); I && I != RI; I = I->getNext()) {
            if (I == AccInst) continue;
            toMove.push_back(I);
        }
        auto callIt = BB->begin();
        while (*callIt != CI) ++callIt;
        for (Instruction *I : toMove) {
            I->eraseFromParent();
            callIt.insertBefore(I);
        }
    }

    // 创建 Br 代替 Ret
    {
        auto *NewBr = new BrInst(LoopHeader);
        auto it = BB->begin();
        while (*it != RI) ++it;
        it.insertBefore(NewBr);
    }

    // 累加器模式下，旧 AccInst 仍引用 CI，若直接 delete CI 会触发 Value::~Value 级联删除用户。
    // 先断开 RI->AccInst，再将旧 AccInst 从块中移除并销毁，避免悬垂/级联破坏 CFG。
    if (AccInst && AccInst->isInBlock(BB)) {
        if (RI->getOperand(0) == AccInst) {
            RI->replaceOperand(0, UndefValue::NewUndefValue(AccInst->getIRType()));
        }
        AccInst->eraseFromParent();
        delete AccInst;
    }

    // 删除旧的 Ret 和 Call
    delete RI;
    delete CI;

    TailBackedgeBlocks.push_back(BB);
    return true;
}


// Pass 入口
PreservationStatus TailRecursionEliminationPass::run(Function &F, FunctionAnalysisPassManager &FAM) {
    bool changed = false;

    // 尾递归转循环会改变递归调用次数与栈帧边界。只要函数体中的任意
    // 直接调用能够到达计时函数，就保守保留整段递归控制流。
    if (F.mayReachTimingCall()) return PreservationStatus::all();

    // 第1步: 标记非自递归的尾调用，打上tail标记，以便后端可以进行优化栈帧复用
    changed |= markTailCalls(F);

    // 第2步: 自递归尾递归消除
    // 入口 alloca 会被搬到循环外并只分配一次。若其派生地址能逃逸，
    // 原本逐递归帧独立的对象会被错误折叠为同一个对象，因此整函数
    // 必须保守跳过 TRE；没有自递归时同样直接退出。
    if (!hasSelfRecursion(F) || hasEscapingEntryAlloca(F)) {
        if (!changed) return PreservationStatus::all();
        // 仅打 tail-call 标记，不改 CFG/内存访问关系。
        PreservationStatus PA;
        PA.keep<DominantAnalysis>();
        PA.keep<PostDominantAnalysis>();
        PA.keep<LoopAnalysis>();
        PA.keep<BasicAliasAnalysis>();
        PA.keep<SideEffectAnalysis>();
        return PA;
    }


    // 跨多个尾递归点共享
    BasicBlock *LoopHeader = nullptr;
    std::vector<PhiInst *> ArgPhis;
    PhiInst *AccPhi = nullptr;
    BinaryInst::Operation accOp = BinaryInst::ADD; // 记录累加器操作类型
    bool accOpInitialized = false;
    std::unordered_set<BasicBlock *> transformedBlocks;
    std::vector<BasicBlock *> TailBackedgeBlocks;

    // 逐站点迭代处理：每次仅应用一个变换并重新扫描，避免收集阶段缓存悬垂指针。
    bool progress = false;
    do {
        progress = false;

        auto rollbackClonedArtifacts = [&](BasicBlock *BB,
                                          RetInst *ClonedRI,
                                          std::vector<Instruction *> &SupportInsts) {
            if (ClonedRI && ClonedRI->isInBlock(BB)) {
                ClonedRI->eraseFromParent();
            }
            for (auto *I : SupportInsts) {
                if (I && I->isInBlock(BB)) {
                    I->eraseFromParent();
                }
            }
            for (auto It = SupportInsts.rbegin(); It != SupportInsts.rend(); ++It) {
                delete *It;
            }
            delete ClonedRI;
        };

        auto blockSnapshot = snapshotPointers(F);

        for (auto bb : blockSnapshot) {
            Instruction *term = bb->back();
            if (!term) continue;
            if (transformedBlocks.count(bb)) continue;

            RetInst *RI = nullptr;
            bool viaBranchRet = false;
            BasicBlock *targetRetBB = nullptr;
            BrInst *BrTerm = nullptr;

            // ---- 情况 1: 终结指令是无条件跳转 ----
            if (term->instType == Instruction::BR) {
                BrTerm = dynamic_cast<BrInst *>(term);
                if (!BrTerm) continue;

                if (!BasicBlock::isUncondBr(BrTerm)) continue;
                targetRetBB = dynamic_cast<BasicBlock *>(BrTerm->getOperand(0));
                if (!targetRetBB) continue;

                RetInst *TargetRet = getRetOnlyTerminatorBlockRet(targetRetBB);
                if (!TargetRet) continue;

                viaBranchRet = true;

            // ---- 情况 2: 终结指令是 Ret ----
            } else if (auto *retInst = dynamic_cast<RetInst *>(term)) {
                RI = retInst;
            } else {
                continue;
            }

            CallInst *CI = findSelfCall(bb, F);
            if (!CI) continue;

            if (hasInterferingEarlierSelfCallInBlock(bb, CI, F, FAM))
                continue;

            BinaryInst *AccInst = nullptr;
            if (!viaBranchRet) {
                if (!analyzeCallRetRange(CI, RI, F, FAM, AccInst))
                    continue;
            }

            if (AccInst) {
                if (!accOpInitialized) {
                    accOp = AccInst->getOp();
                    accOpInitialized = true;
                } else if (accOp != AccInst->getOp()) {
                    continue;
                }
            }

            RetInst *WorkRI = RI;
            std::vector<Instruction *> ClonedSupportInsts;
            if (viaBranchRet) {
                // 在真正变换前再做一次形态校验。
                term = bb->back();
                if (!term || term->instType != Instruction::BR)
                    continue;
                auto *CurBr = dynamic_cast<BrInst *>(term);
                if (!BasicBlock::isUncondBr(CurBr)) continue;
                BasicBlock *CurTarget = dynamic_cast<BasicBlock *>(CurBr->getOperand(0));
                if (!CurTarget || CurTarget != targetRetBB)
                    continue;

                RetInst *TargetRet = getRetOnlyTerminatorBlockRet(CurTarget);
                if (!TargetRet)
                    continue;

                std::unordered_map<Value *, Value *> VMap;
                Instruction *ClonedRetInst = TargetRet->clone(VMap);
                if (!ClonedRetInst)
                    continue;

                auto *ClonedRI = dynamic_cast<RetInst *>(ClonedRetInst);
                if (!ClonedRI) {
                    delete ClonedRetInst;
                    continue;
                }

                if (!rewriteClonedRetOperandForPred(ClonedRI, bb, CurTarget, bb, term,
                                                    ClonedSupportInsts)) {
                    rollbackClonedArtifacts(bb, ClonedRI, ClonedSupportInsts);
                    continue;
                }

                {
                    auto bIt = bb->begin();
                    while (*bIt != term) ++bIt;
                    bIt.insertBefore(ClonedRetInst);
                }

                // 跨块路径也复用 call..ret 分析逻辑，支持累加器识别。
                if (!analyzeCallRetRange(CI, ClonedRI, F, FAM, AccInst)) {
                    rollbackClonedArtifacts(bb, ClonedRI, ClonedSupportInsts);
                    continue;
                }

                if (AccInst) {
                    if (!accOpInitialized) {
                        accOp = AccInst->getOp();
                        accOpInitialized = true;
                    } else if (accOp != AccInst->getOp()) {
                        rollbackClonedArtifacts(bb, ClonedRI, ClonedSupportInsts);
                        continue;
                    }
                }

                int edgeCount = bb->countEdgesToSucc(CurTarget);
                if (edgeCount > 0) {
                    BasicBlock::removeEdgeFromSuccPhis(CurTarget, bb, -1);
                }

                delete term;
                WorkRI = ClonedRI;
            }

            if (!CI->isInBlock(bb) || !WorkRI->isInBlock(bb))
                continue;

            if (eliminateTailRecursion(bb, CI, WorkRI, AccInst, F, FAM,
                                       LoopHeader, ArgPhis, AccPhi,
                                       TailBackedgeBlocks)) {
                changed = true;
                progress = true;
                transformedBlocks.insert(bb);

                // 下一轮还会继续查询别名、循环和跨函数副作用信息。
                // 当前站点已经改写 CFG，必须先通过现有 FAM 接口失效
                // 缓存，避免后续站点使用变换前的分析结果作合法性证明。
                FAM.invalidate(F, PreservationStatus::none());
                break;
            }
        }
    } while (progress);

    // 累加器存在时修复所有剩余 Ret
    if (AccPhi) {
        for (auto bb : F) {
            Instruction *term = bb->back();
            if (!term) continue;
            auto *RI = dynamic_cast<RetInst *>(term);
            if (!RI) continue;

            Value *OrigRetVal = RI->getReturnValue();
            if (!OrigRetVal) continue; // void 返回
            if (OrigRetVal == AccPhi) continue;

            // Ret 之前插入 AccPhi op OrigRetVal
            auto *FinalAcc = new BinaryInst(AccPhi, accOp, OrigRetVal);
            {
                auto it = bb->begin();
                while (*it != RI) ++it;
                it.insertBefore(FinalAcc);
            }
            RI->replaceOperand(0, FinalAcc);
        }
    }

    if (!changed)
        return PreservationStatus::all();

    PreservationStatus PA;
    PA.discard<DominantAnalysis>();
    PA.discard<PostDominantAnalysis>();
    PA.discard<LoopAnalysis>();
    PA.discard<BasicAliasAnalysis>();
    PA.discard<SideEffectAnalysis>();
    return PA;
}
