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

// 3.2 跨块返回候选的 SSA 解析：TargetBB 内的 PHI 按 Pred 选边，BitCast 在候选块重建，其他目标块本地定义全部拒绝。
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
    if (!RetVal) return true;

    std::unordered_set<Value *> Visiting;
    Value *Resolved = resolveRetValueForPred(RetVal, Pred, TargetBB,
                                             InsertBB, InsertBefore, ClonedInsts, Visiting);
    if (!Resolved) return false;
    ClonedRet->replaceOperand(0, Resolved);
    return true;
}
bool TailRecursionEliminationPass::isSelfCall(CallInst *CI, Function &F) {
    return CI && CI->getCallee() == &F;
}

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

            // 2.2 GEP、BitCast、PHI 和 Select 只传播地址身份，将其结果继续放入工作列表。
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

            // 2.2.1 作为 load/store 地址仅访问本帧对象；若地址本身被 store，则可跨递归帧存活，必须拒绝。
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
                // 2.2.2 IR 没有 nocapture 证明，因此地址传给任何调用都按逃逸处理。
                return true;
            }

            // 2.2.3 未显式建模的用法无法证明地址仅存活于本帧，保守终止整函数 TRE。
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

// 3.3.1 跨 self-call 前移 load 需要无条件可解引证明；当前 IR 无 dereferenceable 属性，所以只放行直接指向全局对象的读取。
bool TailRecursionEliminationPass::isSafelyDereferenceableLoad(LoadInst *LI) {
    if (!LI) return false;
    Value *Pointer = LI->getPointerOperand();
    return Pointer && Pointer->isGlobal();
}

// 3.3.2 递归证明被前移的用户调用必定返回且无 fault/poison：CFG 必须无环且全路径返回，指令必须可投机执行。
// 3.3.2.1 ActiveFunctions 是跨函数递归边界，任何直接或间接递归都不能作为 will-return 证明。
bool TailRecursionEliminationPass::isFunctionGuaranteedToReturnWithoutFault(
        Function &Callee,
        std::unordered_set<Function *> &ActiveFunctions) {
    ScopedActiveMark ActiveMark(ActiveFunctions, &Callee);
    if (!ActiveMark.inserted()) return false;
    if (!Callee.front() || !hasAcyclicReturningCFG(Callee)) return false;

    for (BasicBlock *BB : Callee) {
        for (Instruction *I : *BB) {
            if (!I) return false;

            // 3.3.2.2 写内存不能从 self-call 返回之后前移，即使被调函数最终会返回。
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

            // 3.3.2.3 未知指令和可产生 fault/poison 的运算不得用作跨调用代码移动的证明。
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

    // 3.3.2.4 运行库和间接调用没有可审计函数体，NoModRef 也不能替代 will-return/fault-free 证明。
    if (CI->getBuiltinCallee()) return false;
    Function *Callee = CI->getDirectCallee();
    if (!Callee) return false;
    return isFunctionGuaranteedToReturnWithoutFault(
        *Callee, ActiveFunctions);
}

// 3.3.3 只读调用跨 self-call 前移的内存证明：将被调函数 RefSet 映射到实参，逐地址查证 self-call 不会修改该位置。
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

    // 3.3.3.1 副作用摘要含运行库调用或未跟踪指针时，不能对跨调用内存次序作肯定证明。
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
            // 3.3.3.2 RefSet 中的非全局/形参地址无法映射到当前调用点，保守拒绝。
            return false;
        }

        if (!ReadPointer || !ReadPointer->isPointerValue()) return false;
        ModRefInfo MRI =
            getInstModRefInfo(ReadPointer, SelfCall, AA, LI, FAM);
        if (isModSet(MRI)) return false;
    }

    return true;
}

// 3.1.1 候选之前的自调用只在可读写状态或返回值仍被使用时构成干扰。
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

// 1. 普通尾调用标记只处理“call 紧邻 ret”的出口，它不改 CFG 或 SSA。
bool TailRecursionEliminationPass::markTailCalls(Function &F) {
    bool changed = false;

    for (auto bb : F) {
        // 1.1 形状筛选：块必须以 ret 结束，其紧邻前一条指令必须是 call。
        Instruction *terminator = bb->back();
        if (!terminator) continue;
        auto *RI = dynamic_cast<RetInst*>(terminator);
        if (!RI) continue;
        Instruction *prevInst = dynamic_cast<Instruction*>(RI->getPrev());
        if (!prevInst) continue;
        auto *CI = dynamic_cast<CallInst*>(prevInst);
        if (!CI) continue;
        // 1.2 计时调用及其封装必须保持普通后端调用序列，自调用则留给后续转循环路径。
        if (CI->mayReachTimingCall()) continue;
        if (isSelfCall(CI, F)) continue;
        // 1.3 反向追溯每个实参的操作数闭包，任何来自 alloca 的地址都不得传给尾调用。
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
        // 1.4 void 函数要求 call 也返回 void；非 void 函数则必须精确返回 call 结果。
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
// 3.1 从候选块末尾向前查找最后一个直接自调用，后续只审计该 call 到 ret 的区间。
CallInst *TailRecursionEliminationPass::findSelfCall(BasicBlock *BB, Function &F) {
    for (Instruction *I = BB->back(); I != nullptr; I = I->getPrev()) {
        if (I->instType != Instruction::CALL) continue;
        auto *CI = dynamic_cast<CallInst *>(I);
        if (CI && isSelfCall(CI, F))
            return CI;
    }
    return nullptr;
}

// 3.3 call..ret 分析同时处理真尾递归和整数加/乘伪尾递归，并证明其余后置指令可跨 self-call 前移。
bool TailRecursionEliminationPass::analyzeCallRetRange(
        CallInst *CI, RetInst *RI, Function &F,
        FunctionAnalysisPassManager &FAM,
        BinaryInst *&AccInst) {
    AccInst = nullptr;

    // 3.3.1 先快照 call 与 ret 之间的开区间，避免分析时改变链表。
    std::vector<Instruction *> between;
    for (Instruction *I = CI->getNext(); I && I != RI; I = I->getNext())
        between.push_back(I);

    for (Instruction *I : between) {
        // 3.3.2 累加器必须是区间内唯一的整数 ADD/MUL，恰好一个操作数为 self-call，且结果唯一地被 ret 使用。
        if (auto *BI = dynamic_cast<BinaryInst *>(I)) {
            auto op = BI->getOp();
            // 3.3.2.1 严格浮点运算无结合律证明，转循环会改变结合顺序，因此只允许整数累加器。
            bool IsIntegerAccumulator =
                BI->getIRType() && BI->getIRType()->isInt();
            if ((op == BinaryInst::ADD || op == BinaryInst::MUL) &&
                IsIntegerAccumulator) {
                if (AccInst) return false;

                Value *Op0 = nullptr;
                Value *Op1 = nullptr;
                if (!BI->getOperands(Op0, Op1)) return false;

                bool op0IsCall = (Op0 == static_cast<Value *>(CI));
                bool op1IsCall = (Op1 == static_cast<Value *>(CI));
                if (!(op0IsCall ^ op1IsCall)) return false;

                Value *OtherOp = op0IsCall ? Op1 : Op0;
                if (auto *OtherCI = dynamic_cast<CallInst *>(OtherOp)) {
                    if (isSelfCall(OtherCI, F)) return false;
                }

                if (!BI->hasExactlyOneUse()) return false;
                Use *singleUse = BI->getSingleUse();
                if (!singleUse || singleUse->getUser() != RI) return false;

                AccInst = BI;
                continue;
            }
        }

        // 3.3.3 其余指令要被移到可能不返回的 self-call 之前，必须同时无可观察副作用、无 fault/poison 且不依赖 call 结果。
        if (dceInstructionMustBePreserved(I) ||
            instructionMayFaultOrProducePoison(I)) {
            return false;
        }

        if (I->instType == Instruction::STORE) return false;

        // 3.3.3.1 后置 call 除了只读外，还须通过 will-return/fault-free 与逐地址 ModRef 检查。
        if (I->instType == Instruction::CALL) {
            auto *Call = dynamic_cast<CallInst *>(I);
            if (!canMoveReadonlyCallBeforeSelfCall(Call, CI, FAM, F))
                return false;
        }

        if (!forEachOperand(
                I, [CI](size_t, Use *, Value *Operand) {
                    return Operand != static_cast<Value *>(CI);
                })) {
            return false;
        }
    }

    // 3.3.4 最终返回值必须精确为 self-call 或已证明的累加器；不接受任何其他依赖递归结果的表达式。
    Value *RetValue = RI->getReturnValue();
    if (!F.getIRType() || F.getIRType()->isVoid()) {
        // 3.3.4.1 void 路径只允许 call 紧邻 ret，不将任何原本位于递归返回后的计算前移。
        return !RetValue && CI->getNext() == RI;
    }
    if (!RetValue) return false;
    if (AccInst) return RetValue == static_cast<Value *>(AccInst);
    return RetValue == static_cast<Value *>(CI);
}

// 4. 单站点提交将已证明的自调用改为通往共享循环头的回边。
bool TailRecursionEliminationPass::eliminateTailRecursion(
        BasicBlock *BB, CallInst *CI, RetInst *RI,
        BinaryInst *AccInst, Function &F,
        FunctionAnalysisPassManager &FAM,
        BasicBlock *&LoopHeader,
        std::vector<PhiInst *> &ArgPhis,
        PhiInst *&AccPhi,
        std::vector<BasicBlock *> &TailBackedgeBlocks) {

    // 4.1 首个站点初始化公共循环：新建一次性函数入口，原入口成为循环头。
    if (!LoopHeader) {
        BasicBlock *OldEntry = F.front();

        BasicBlock *NewEntry = new BasicBlock();
        F.pushFront(NewEntry);
        NewEntry->genUnCondInst(OldEntry);

        // 4.1.1 将入口 alloca 搬到循环外；阶段 2 已证明其地址不逃逸，因此单次分配不会合并可观察的递归帧对象。
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

        // 4.1.2 为每个形参在循环头建 PHI，首次输入来自 NewEntry，并把函数体内旧形参使用统一转向 PHI。
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

    // 4.2 将当前 self-call 的每个实参作为当前块回边值接入形参 PHI。
    {
        for (size_t i = 0; i < ArgPhis.size(); ++i) {
            Value *Arg = CI->getArg(static_cast<unsigned>(i));
            if (Arg)
                ArgPhis[i]->addIncoming(Arg, BB);
        }
    }

    // 4.3 累加器路径用额外 PHI 保存递归层间状态，只在首个累加候选出现时创建。
    if (AccInst) {
        BinaryInst::Operation accOp = AccInst->getOp();

        if (!AccPhi) {
            AccPhi = new PhiInst(AccInst->getIRType());
            LoopHeader->pushFront(AccPhi);

            // 4.3.1 首次输入使用 ADD 的 0 或 MUL 的 1，保持原递归结合链的单位元语义。
            Value *Identity = nullptr;
            if (accOp == BinaryInst::ADD) {
                Identity = Value::makeZeroForType(
                    AccInst->getIRType());
            } else {
                Identity = Value::makeOneForType(
                    AccInst->getIRType());
            }
            AccPhi->addIncoming(Identity, F.front());

            // 4.3.2 若早先已有无累加器回边，则为其补上 AccPhi 自转发输入，保证 PHI 与全部前驱一一对应。
            for (BasicBlock *Backedge : TailBackedgeBlocks) {
                if (Backedge)
                    AccPhi->addIncoming(AccPhi, Backedge);
            }
        }

        Value *Op0 = nullptr;
        Value *Op1 = nullptr;
        if (!AccInst->getOperands(Op0, Op1)) return false;
        Value *OtherOperand = (Op0 == static_cast<Value *>(CI)) ? Op1 : Op0;

        // 4.3.3 在 ret 前计算当前累加值，并将该值作为当前回边的 AccPhi 输入。
        auto *NewAcc = new BinaryInst(AccPhi, accOp, OtherOperand);
        {
            auto it = BB->begin();
            while (*it != RI) ++it;
            it.insertBefore(NewAcc);
        }

        AccPhi->addIncoming(NewAcc, BB);
        // 4.3.4 旧 AccInst 此时仍在 ret 的 use 链上，延后到断开 ret operand 后再删除，避免 Value 析构级联破坏 CFG。
    } else if (AccPhi) {
        // 4.3.5 全局已有累加器但当前站点无累加操作时，回边原样传递 AccPhi。
        AccPhi->addIncoming(AccPhi, BB);
    }

    // 4.4 提交控制流前，先将阶段 3 证明可前移的 call..ret 指令依原顺序放到 self-call 之前。
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

    // 4.5 在 ret 位置插入返回共享 LoopHeader 的无条件回边。
    {
        auto *NewBr = new BrInst(LoopHeader);
        auto it = BB->begin();
        while (*it != RI) ++it;
        it.insertBefore(NewBr);
    }

    // 4.6 累加器模式先用 undef 断开 ret→旧 AccInst，再脱链销毁 AccInst，保证删除 self-call 时不会级联删除用户。
    if (AccInst && AccInst->isInBlock(BB)) {
        if (RI->getOperand(0) == AccInst) {
            RI->replaceOperand(0, UndefValue::NewUndefValue(AccInst->getIRType()));
        }
        AccInst->eraseFromParent();
        delete AccInst;
    }

    // 4.7 最后删除旧 ret 和 self-call，并记录该块为已建回边，供之后初始化累加器 PHI 补边。
    delete RI;
    delete CI;

    TailBackedgeBlocks.push_back(BB);
    return true;
}
PreservationStatus TailRecursionEliminationPass::run(Function &F, FunctionAnalysisPassManager &FAM) {
    bool changed = false;

    // 1. 函数内任何可达计时调用都禁止本 pass：无论 tail 标志还是转循环，都可能改变调用序列或栈帧边界。
    if (F.mayReachTimingCall()) return PreservationStatus::all();

    // 1.1 先执行不改 CFG 的普通尾调用标记，使后端可复用调用者栈帧。
    changed |= markTailCalls(F);

    // 2. 真正 TRE 只在存在自递归且入口 alloca 地址均不逃逸时进入，因为这些对象会被搬到循环外并只分配一次。
    if (!hasSelfRecursion(F) || hasEscapingEntryAlloca(F)) {
        if (!changed) return PreservationStatus::all();
        // 6.1 若只改了 tail 元数据，CFG、支配、循环、别名和副作用结果仍全部有效。
        PreservationStatus PA;
        PA.keep<DominantAnalysis>();
        PA.keep<PostDominantAnalysis>();
        PA.keep<LoopAnalysis>();
        PA.keep<BasicAliasAnalysis>();
        PA.keep<SideEffectAnalysis>();
        return PA;
    }
    // 3. 以下状态被函数内所有尾递归站点共享：唯一 LoopHeader、形参 PHI、可选累加器 PHI 和已建回边集合。
    BasicBlock *LoopHeader = nullptr;
    std::vector<PhiInst *> ArgPhis;
    PhiInst *AccPhi = nullptr;
    // 3.1 所有站点必须使用同一种累加操作，首个累加候选决定全函数的 ADD/MUL 模式。
    BinaryInst::Operation accOp = BinaryInst::ADD;
    bool accOpInitialized = false;
    std::unordered_set<BasicBlock *> transformedBlocks;
    std::vector<BasicBlock *> TailBackedgeBlocks;

    // 3.2 每轮只提交一个站点并重新快照函数块，避免 CFG 改写后继续使用收集阶段的悬垂指针。
    bool progress = false;
    do {
        progress = false;

        auto rollbackClonedArtifacts = [&](BasicBlock *BB,
                                          RetInst *ClonedRI,
                                          std::vector<Instruction *> &SupportInsts) {
            // 3.2.1 跨块返回值解析失败时，只回滚候选块中新克隆的 ret/BitCast，原 CFG 尚未变更。
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

            // 3.3 候选可直接以 ret 结束，也可经唯一无条件边进入只含 PHI+ret 的共享返回块。
            if (term->instType == Instruction::BR) {
                BrTerm = dynamic_cast<BrInst *>(term);
                if (!BrTerm) continue;

                if (!BasicBlock::isUncondBr(BrTerm)) continue;
                targetRetBB = dynamic_cast<BasicBlock *>(BrTerm->getOperand(0));
                if (!targetRetBB) continue;

                RetInst *TargetRet = getRetOnlyTerminatorBlockRet(targetRetBB);
                if (!TargetRet) continue;

                viaBranchRet = true;

            } else if (auto *retInst = dynamic_cast<RetInst *>(term)) {
                RI = retInst;
            } else {
                continue;
            }

            CallInst *CI = findSelfCall(bb, F);
            if (!CI) continue;

            if (hasInterferingEarlierSelfCallInBlock(bb, CI, F, FAM))
                continue;

            // 3.4 直接 ret 路径当场完成 call..ret 安全性与累加器识别；跨块路径需先将 ret 特化到当前前驱。
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
                // 3.5 克隆共享 ret 前重新核验无条件边和目标块，再按当前前驱解析 PHI/BitCast 返回值。
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

                // 3.5.1 特化 ret 插入候选块后复用同一 call..ret 证明；失败则完整删除克隆产物。
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

                // 6. 单站点改写后 CFG、别名关系和调用摘要都可能过期，下一轮扫描前立即失效全部函数分析。
                FAM.invalidate(F, PreservationStatus::none());
                break;
            }
        }
    } while (progress);

    // 5. 若建立了累加器 PHI，所有未转换的正常返回都要在 ret 前与 AccPhi 做最终 ADD/MUL 合并。
    if (AccPhi) {
        for (auto bb : F) {
            Instruction *term = bb->back();
            if (!term) continue;
            auto *RI = dynamic_cast<RetInst *>(term);
            if (!RI) continue;

            Value *OrigRetVal = RI->getReturnValue();
            if (!OrigRetVal) continue;
            if (OrigRetVal == AccPhi) continue;

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
    // 6.2 只要发生过自递归转循环，CFG、循环形状、别名与跨函数副作用分析均需重算。
    PA.discard<DominantAnalysis>();
    PA.discard<PostDominantAnalysis>();
    PA.discard<LoopAnalysis>();
    PA.discard<BasicAliasAnalysis>();
    PA.discard<SideEffectAnalysis>();
    return PA;
}
