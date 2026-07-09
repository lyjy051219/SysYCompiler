#include "ir/opt/TailRecursionElimination.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "lib/CFG.hpp"
#include "lib/BaseManager.hpp"
#include <cassert>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// 将目标 Ret 的返回值映射到 Pred 上可用的 SSA 值。
// 允许处理位于 TargetBB 中的 PHI 与 BitCast 链，其他 TargetBB 本地指令一律判为不合法。
static Value *resolveRetValueForPred(Value *V,
                                     BasicBlock *Pred,
                                     BasicBlock *TargetBB,
                                     BasicBlock *InsertBB,
                                     Instruction *InsertBefore,
                                     std::vector<Instruction *> &ClonedInsts,
                                     std::unordered_set<Value *> &Visiting) {
    if (!V) return nullptr;
    if (Visiting.count(V)) return nullptr;
    Visiting.insert(V);

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
        Value *BCSrc = BC->getOperand(0);
        if (!BCSrc) return nullptr;

        Value *Src = resolveRetValueForPred(BCSrc, Pred, TargetBB,
                                            InsertBB, InsertBefore, ClonedInsts, Visiting);
        if (!Src) return nullptr;

        auto *NewBC = new BitCastInst(Src, BC->getIRType());
        auto It = InsertBB->begin();
        while (*It != InsertBefore) ++It;
        It.insertBefore(NewBC);
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

static bool rewriteClonedRetOperandForPred(RetInst *ClonedRet,
                                           BasicBlock *Pred,
                                           BasicBlock *TargetBB,
                                           BasicBlock *InsertBB,
                                           Instruction *InsertBefore,
                                           std::vector<Instruction *> &ClonedInsts) {
    Value *RetVal = ClonedRet->getOperand(0);
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
    for (auto bb : F) {
        for (auto inst : *bb) {
            if (inst->instType != Instruction::CALL) continue;
            auto *ci = dynamic_cast<CallInst*>(inst);
            if (ci && isSelfCall(ci, F)) return true;
        }
    }
    return false;
}

static bool isInstructionStillInBlock(BasicBlock *BB, Instruction *I) {
    if (!BB || !I) return false;
    for (auto inst : *BB) {
        if (inst == I) return true;
    }
    return false;
}

static bool isReadonlyLikeCallInst(Instruction *I,
                                   FunctionAnalysisPassManager &FAM,
                                   Function &F) {
    if (I->instType != Instruction::CALL) return true;
    auto *CI = dynamic_cast<CallInst *>(I);
    Value *Callee = CI ? CI->getCallee() : nullptr;
    if (!Callee) return false;
    if (auto *CalleeFunc = dynamic_cast<Function *>(Callee)) {
        const SideEffectInfo &SEI = FAM.getResult<SideEffectAnalysis>(*CalleeFunc);
        ModRefInfo FMRI = getFunctionModRefInfo(SEI);
        return FMRI == ModRefInfo::NoModRef || FMRI == ModRefInfo::Ref;
    }
    if (auto *BIF = dynamic_cast<BuildInFunction *>(Callee)) {
        FuncAttr Attr = classifyBuiltin(BIF);
        return Attr == FuncAttr::NoModRef || Attr == FuncAttr::MemReadOnly;
    }
    return false;
}

// 更精细的“前置 self-call 干扰”判定：
// 仅当该前置 self-call 可能读写状态，或其返回值被使用时，才阻止当前候选。
static bool hasInterferingEarlierSelfCallInBlock(BasicBlock *BB,
                                                 CallInst *StopCI,
                                                 Function &F,
                                                 FunctionAnalysisPassManager &FAM) {
    if (!BB || !StopCI) return false;
    for (auto inst : *BB) {
        if (inst == StopCI) break;
        if (inst->instType != Instruction::CALL) continue;
        auto *CI = dynamic_cast<CallInst *>(inst);
        if (!CI) continue;
        if (CI->getCallee() != &F) continue;

        if (!isReadonlyLikeCallInst(CI, FAM, F)) return true;
        if (CI->getUseList().size != 0) return true;
    }
    return false;
}

static RetInst *getRetOnlyTerminatorBlockRet(BasicBlock *BB) {
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
                    auto &opUses = inst->getUserList();
                    for (auto &u : opUses) {
                        if (u->getValue())
                            worklist.push_back(u->getValue());
                    }
                }
            }
            if (hasEscapingLocalPtr) break;
        }

        if (hasEscapingLocalPtr) continue;
        if (CI->isTailCall()) continue;
        //3.检查Ret返回值与call必须匹配
        bool isVoid = (F.getIRType()->getEnum() == IR_VOID);
        if (isVoid) {
            if (CI->getIRType()->getEnum() == IR_VOID) {
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
            // 只放行 ADD 和 MUL (满足交换律+结合律)
            if (op == BinaryInst::ADD || op == BinaryInst::MUL) {
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
                if (BI->getUseList().size != 1) return false;
                Use *singleUse = BI->getUseList().getHead();
                if (!singleUse || singleUse->getUser() != RI) return false;

                AccInst = BI;
                continue;
            }
        }

        // ----- 检查指令能否移到 call 之前 -----

        // STORE 不行
        if (I->instType == Instruction::STORE) return false;

        // CALL: 被调函数必须只读
        if (I->instType == Instruction::CALL) {
            auto *Call = dynamic_cast<CallInst *>(I);
            Value *Callee = Call ? Call->getCallee() : nullptr;
            if (!Callee) return false;
            if (auto *CalleeFunc = dynamic_cast<Function *>(Callee)) {
                const SideEffectInfo &SEI = FAM.getResult<SideEffectAnalysis>(*CalleeFunc);
                ModRefInfo FMRI = getFunctionModRefInfo(SEI);
                if (FMRI != ModRefInfo::NoModRef && FMRI != ModRefInfo::Ref)
                    return false;
            } else if (auto *BIF = dynamic_cast<BuildInFunction *>(Callee)) {
                FuncAttr Attr = classifyBuiltin(BIF);
                // Sylib 中包含 putint/putch/getint 等可观察 I/O，
                // 不能当作可移动的“只读调用”。
                if (Attr != FuncAttr::NoModRef && Attr != FuncAttr::MemReadOnly)
                    return false;
            } else {
                return false;
            }
        }

        // LOAD: call 不能 Mod 这块地址
        if (I->instType == Instruction::LOAD) {
            auto *LIInst = dynamic_cast<LoadInst *>(I);
            if (LIInst) {
                Value *Ptr = LIInst->getPointerOperand();
                auto &AA = FAM.getResult<BasicAliasAnalysis>(F);
                auto &LI = FAM.getResult<LoopAnalysis>(F);
                ModRefInfo MRI = getInstModRefInfo(Ptr, CI, AA, LI, FAM);
                if (isModSet(MRI)) return false;
            }
        }

        // 操作数不能依赖 call 的返回值
        auto &UL = I->getUserList();
        for (auto &U : UL) {
            if (U->getValue() == static_cast<Value *>(CI))
                return false;
        }
    }

    return true;
}

// 执行单个尾递归 → 循环 变换
bool TailRecursionEliminationPass::eliminateTailRecursion(
        BasicBlock *BB, CallInst *CI, RetInst *RI,
        BinaryInst *AccInst, Function &F,
        FunctionAnalysisPassManager &FAM,
        BasicBlock *&LoopHeader,
        std::vector<PhiInst *> &ArgPhis,
        PhiInst *&AccPhi) {

    // ===== 3.1 创建循环头 (仅首次调用时执行) =====
    if (!LoopHeader) {
        BasicBlock *OldEntry = F.front();

        // 创建新入口块
        BasicBlock *NewEntry = new BasicBlock();
        F.push_front(NewEntry);
        NewEntry->genUnCondInst(OldEntry);

        // 将原入口块中所有 Alloca 搬到 NewEntry 的 Br 之前
        {
            std::vector<AllocaInst *> allocas;
            for (auto inst : *OldEntry) {
                if (auto *AI = dynamic_cast<AllocaInst *>(inst))
                    allocas.push_back(AI);
            }
            Instruction *brInst = NewEntry->back();
            auto brIt = NewEntry->begin();
            while (*brIt != brInst) ++brIt;
            for (AllocaInst *AI : allocas) {
                AI->EraseFromParent();
                brIt.insertBefore(AI);
            }
        }

        // 为每个形参在 OldEntry 创建 PHI
        auto &Params = F.getParams();
        ArgPhis.clear();
        for (size_t i = 0; i < Params.size(); ++i) {
            Value *Param = Params[i].get();
            auto *Phi = new PhiInst(Param->getIRType());
            OldEntry->push_front(Phi);
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
            LoopHeader->push_front(AccPhi);

            // 初始值: ADD→0, MUL→1
            Value *Identity = nullptr;
            if (accOp == BinaryInst::ADD) {
                Identity = (AccInst->getIRType()->getEnum() == IR_FLOAT)
                    ? static_cast<Value *>(ConstFloat::newConstFloat(0.0f))
                    : static_cast<Value *>(ConstInt::newConstInt(0));
            } else {
                Identity = (AccInst->getIRType()->getEnum() == IR_FLOAT)
                    ? static_cast<Value *>(ConstFloat::newConstFloat(1.0f))
                    : static_cast<Value *>(ConstInt::newConstInt(1));
            }
            AccPhi->addIncoming(Identity, F.front()); // 来自 NewEntry
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
            I->EraseFromParent();
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
    if (AccInst && isInstructionStillInBlock(BB, AccInst)) {
        if (RI->getOperand(0) == AccInst) {
            RI->replaceOperand(0, UndefValue::NewUndefValue(AccInst->getIRType()));
        }
        AccInst->EraseFromParent();
        delete AccInst;
    }

    // 删除旧的 Ret 和 Call
    delete RI;
    delete CI;

    return true;
}


// Pass 入口
PreservationStatus TailRecursionEliminationPass::run(Function &F, FunctionAnalysisPassManager &FAM) {
    bool changed = false;

    // 第1步: 标记非自递归的尾调用，打上tail标记，以便后端可以进行优化栈帧复用
    changed |= markTailCalls(F);

    // 第2步: 自递归尾递归消除
    //快速检查call：如果一个函数中没有符合要求的call就直接退出
    if (!hasSelfRecursion(F)) {
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

    // 逐站点迭代处理：每次仅应用一个变换并重新扫描，避免收集阶段缓存悬垂指针。
    bool progress = false;
    do {
        progress = false;

        auto rollbackClonedArtifacts = [&](BasicBlock *BB,
                                          RetInst *ClonedRI,
                                          std::vector<Instruction *> &SupportInsts) {
            if (ClonedRI && isInstructionStillInBlock(BB, ClonedRI)) {
                ClonedRI->EraseFromParent();
            }
            for (auto *I : SupportInsts) {
                if (I && isInstructionStillInBlock(BB, I)) {
                    I->EraseFromParent();
                }
            }
            for (auto It = SupportInsts.rbegin(); It != SupportInsts.rend(); ++It) {
                delete *It;
            }
            delete ClonedRI;
        };

        std::vector<BasicBlock *> blockSnapshot;
        for (auto bb : F) {
            blockSnapshot.push_back(bb);
        }

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

            if (!isInstructionStillInBlock(bb, CI) || !isInstructionStillInBlock(bb, WorkRI))
                continue;

            if (eliminateTailRecursion(bb, CI, WorkRI, AccInst, F, FAM,
                                       LoopHeader, ArgPhis, AccPhi)) {
                changed = true;
                progress = true;
                transformedBlocks.insert(bb);
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

            auto &retUses = RI->getUserList();
            if (retUses.empty()) continue; // void 返回

            Value *OrigRetVal = retUses[0]->getValue();
            if (OrigRetVal == AccPhi) continue;

            // Ret 之前插入 AccPhi op OrigRetVal
            auto *FinalAcc = new BinaryInst(AccPhi, accOp, OrigRetVal);
            {
                auto it = bb->begin();
                while (*it != RI) ++it;
                it.insertBefore(FinalAcc);
            }
            retUses[0]->replaceUseeWith(FinalAcc);
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
