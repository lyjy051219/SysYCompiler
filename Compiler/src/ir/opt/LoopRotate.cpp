#include "ir/opt/LoopRotate.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "ir/opt/LoopSimplify.hpp"
#include "lib/IRUtils.hpp"
#include <algorithm>

bool LoopRotatePass::isLCSSAPhi(PhiInst *Phi) {
    if (!Phi) return false;
    return Phi->getIdent().find(".lcssa") != std::string::npos;
}

//clone转发函数
Instruction *LoopRotatePass::cloneInstructionWithMap(
    Instruction *I,
    const std::unordered_map<Value *, Value *> &VMap) {
    if (!I) return nullptr;
    return I->clone(VMap);
}

bool LoopRotatePass::runLoopRotate(Function &F, Loop &L, const DominatorTree &DT, const LoopInfo &LI) {
    (void)DT;
    (void)LI;
    BasicBlock *Header = L.getHeader();
    if (!Header) return false;

    // 循环旋转会把循环头指令复制到前置块，并改变各次迭代的执行位置。
    // 若循环（含子循环）调用计时函数或其封装函数，则整个候选不可旋转。
    for (BasicBlock *BB : L.getBlocksIncludingSubLoops()) {
        if (!BB) return false;
        for (Instruction *I : *BB) {
            auto *CI = dynamic_cast<CallInst *>(I);
            if (CI && CI->mayReachTimingCall()) return false;
        }
    }

    // 条件1：LoopSimplify 形态
    BasicBlock *PreHeader = nullptr;
    BasicBlock *Latch = nullptr;
    if (!L.getLoopSimplifyForm(PreHeader, Latch)) return false;
    if (!PreHeader || !Latch) return false;

    //防死循环检查
    bool latchIsExiting = false;
    for (BasicBlock *Succ : Latch->getSuccessors()) {
        if (!L.contains(Succ)) {
            latchIsExiting = true;
            break;
        }
    }
    if (latchIsExiting) {
        return false;
    }

    // 条件2：块数量、唯一出口、Header 必须是 ExitingBlock
    std::vector<BasicBlock *> LoopBlocks;
    for (auto *BB : F) {
        if (L.contains(BB)) LoopBlocks.push_back(BB);
    }
    if (LoopBlocks.size() <= 2) return false;

    auto ExitBlocks = L.getExitBlocks();
    if (ExitBlocks.size() != 1) return false;
    BasicBlock *Exit = ExitBlocks.front();
    if (!Exit) return false;

    auto *HeaderTerm = dynamic_cast<BrInst *>(Header->back());
    if (!HeaderTerm) return false;
    if (!HeaderTerm->isConditional()) return false;
    Value *HeaderCond = HeaderTerm->getCondition();
    BasicBlock *SuccA = HeaderTerm->getTrueSuccessor();
    BasicBlock *SuccB = HeaderTerm->getFalseSuccessor();
    if (!SuccA || !SuccB) return false;

    BasicBlock *Body = nullptr;
    if (SuccA == Exit && L.contains(SuccB)) Body = SuccB;
    if (SuccB == Exit && L.contains(SuccA)) Body = SuccA;
    if (!Body) return false;

    auto HeaderPhis = Header->collectPhiNodes();
    std::vector<Instruction *> HeaderNormalInsts;
    for (auto *I : *Header) {
        if (dynamic_cast<PhiInst *>(I)) continue;
        if (dynamic_cast<BrInst *>(I)) continue;
        HeaderNormalInsts.push_back(I);
    }

    for (Instruction *OldI : HeaderNormalInsts) {
        Instruction *Probe = cloneInstructionWithMap(OldI, {});
        if (!Probe) return false;
        delete Probe;
    }

    // 条件3：Exit 前驱唯一且仅 Header；Latch 不是 Exiting；Body 前驱唯一且仅 Header
    auto ExitPreds = Exit->getPredecessors();
    if (ExitPreds.size() != 1 || ExitPreds.front() != Header) return false;

    auto LatchSuccs = Latch->getSuccessors();
    for (BasicBlock *S : LatchSuccs) {
        if (!L.contains(S)) return false;
    }

    auto BodyPreds = Body->getPredecessors();
    if (BodyPreds.size() != 1 || BodyPreds.front() != Header) return false;

    auto *LatchTermCheck = dynamic_cast<BrInst *>(Latch->back());
    if (!LatchTermCheck || LatchTermCheck->isConditional()) return false;
    BasicBlock *LatchDstCheck = LatchTermCheck->getTrueSuccessor();
    if (LatchDstCheck != Header) return false;

    // 步骤5：Body PHI 折叠（保留 .lcssa）
    auto BodyPhis = Body->collectPhiNodes();
    for (PhiInst *Phi : BodyPhis) {
        if (!Phi) continue;
        if (isLCSSAPhi(Phi)) continue;
        if (!Phi->hasUses()) {
            delete Phi;
            continue;
        }
        if (Phi->getNumIncomingValues() == 0) {
            Phi->replaceAllUsesWith(UndefValue::NewUndefValue(Phi->getIRType()));
            delete Phi;
            continue;
        }
        bool AllSame = true;
        Value *First = Phi->getIncomingValue(0);
        AllSame = forEachPhiIncoming(
            Phi, [First](unsigned I, Value *Incoming, BasicBlock *) {
                return I == 0 || Incoming == First;
            });
        if (AllSame && First != Phi) {
            Phi->replaceAllUsesWith(First);
            delete Phi;
        }
    }


    // 步骤6：核心 Rotate1 - Header 指令 clone 到 PreHeader，并为 Body 建立对应 PHI
    auto *PreHeaderTerm = dynamic_cast<BrInst *>(PreHeader->back());
    if (!PreHeaderTerm || PreHeaderTerm->isConditional()) return false;
    delete PreHeaderTerm;

    std::unordered_map<Value *, Value *> HeaderToPreValue;
    std::unordered_map<Value *, Value *> HeaderToBodyValue;
    std::vector<PhiInst *> NewBodyPhis;

    for (PhiInst *OldPhi : HeaderPhis) {
        Value *InitV = nullptr;
        Value *IterV = nullptr;
        forEachPhiIncoming(
            OldPhi, [&](unsigned, Value *Incoming, BasicBlock *InB) {
                if (InB == PreHeader) InitV = Incoming;
                if (InB == Latch) IterV = Incoming;
            });
        if (!InitV || !IterV) return false;

        auto *BodyPhi = new PhiInst(OldPhi->getIRType());
        BodyPhi->addIncoming(InitV, PreHeader);
        BodyPhi->addIncoming(IterV, Header);
        NewBodyPhis.push_back(BodyPhi);

        HeaderToPreValue[OldPhi] = InitV;
        HeaderToBodyValue[OldPhi] = BodyPhi;
    }

    for (auto It = NewBodyPhis.rbegin(); It != NewBodyPhis.rend(); ++It) {
        Body->pushFront(*It);
    }

    for (Instruction *OldI : HeaderNormalInsts) {
        Instruction *Cloned = cloneInstructionWithMap(OldI, HeaderToPreValue);
        if (!Cloned) return false;
        PreHeader->pushBack(Cloned);
        HeaderToPreValue[OldI] = Cloned;

        bool UsedInLoopNonHeader = false;
        for (Use *U : OldI->getUses()) {
            auto *UserInst = dynamic_cast<Instruction *>(U ? U->getUser() : nullptr);
            if (!UserInst) continue;
            BasicBlock *UserBB = UserInst->getParent();
            if (UserBB && UserBB != Header && L.contains(UserBB)) {
                UsedInLoopNonHeader = true;
                break;
            }
        }

        if (UsedInLoopNonHeader) {
            auto *BodyPhi = new PhiInst(OldI->getIRType());
            BodyPhi->addIncoming(Cloned, PreHeader);
            BodyPhi->addIncoming(OldI, Header);
            Body->pushFront(BodyPhi);
            HeaderToBodyValue[OldI] = BodyPhi;
        }
    }

    // 步骤7：核心 Rotate2 - 更新 Uses
    std::unordered_map<Value *, PhiInst *> ExitLCSSA;
    for (auto &KV : HeaderToBodyValue) {
        Value *OldV = KV.first;
        Value *BodyV = KV.second;
        Value *PreV = HeaderToPreValue.count(OldV) ? HeaderToPreValue[OldV] : OldV;

        auto Uses = OldV->getUses();

        for (Use *U : Uses) {
            auto *UserInst = dynamic_cast<Instruction *>(U->getUser());
            if (!UserInst) continue;
            BasicBlock *UserBB = UserInst->getParent();

            if (UserBB == Header) {
                auto *UserPhi = dynamic_cast<PhiInst *>(UserInst);
                if (UserPhi) {
                    if (UserInst != BodyV) {
                        U->replaceUseeWith(BodyV);
                    }
                    auto OpIdx = U->getOperandIndex();
                    if (OpIdx && (*OpIdx % 2) == 0) {
                        unsigned InIdx = static_cast<unsigned>(*OpIdx / 2);
                        if (UserPhi->getIncomingBlock(InIdx) == Header) {
                            UserPhi->setIncomingBlock(InIdx, Body);
                        }
                    }
                }
                continue;
            }

            if (L.contains(UserBB)) {
                if (UserInst != BodyV) {
                    U->replaceUseeWith(BodyV);
                }
                continue;
            }

            if (!L.contains(UserBB)) {
                auto *UserPhi = dynamic_cast<PhiInst *>(UserInst);
                if (UserBB == Exit && UserPhi && isLCSSAPhi(UserPhi)) {
                    continue;
                }

                PhiInst *LCPhi = nullptr;
                auto It = ExitLCSSA.find(OldV);
                if (It != ExitLCSSA.end()) {
                    LCPhi = It->second;
                } else {
                    LCPhi = new PhiInst(OldV->getIRType());
                    LCPhi->setIdent(LCPhi->getIdent() + ".lcssa");
                    LCPhi->addIncoming(OldV, Header);
                    LCPhi->addIncoming(PreV, PreHeader);
                    Exit->pushFront(LCPhi);
                    ExitLCSSA[OldV] = LCPhi;
                }
                U->replaceUseeWith(LCPhi);
            }
        }
    }

    for (PhiInst *Phi : Exit->collectPhiNodes()) {
        if (!isLCSSAPhi(Phi)) continue;
        bool HasHeader = false;
        bool HasPreHeader = false;
        Value *HeaderIncoming = nullptr;
        forEachPhiIncoming(
            Phi, [&](unsigned, Value *Incoming, BasicBlock *InB) {
            if (InB == Header) {
                HasHeader = true;
                HeaderIncoming = Incoming;
            }
            if (InB == PreHeader) HasPreHeader = true;
        });
        if (HasHeader && !HasPreHeader) {
            Value *NewIn = HeaderIncoming;
            auto It = HeaderToPreValue.find(HeaderIncoming);
            if (It != HeaderToPreValue.end()) NewIn = It->second;
            Phi->addIncoming(NewIn, PreHeader);
        }
    }

    // 步骤8：核心 Rotate3 - CFG 重连
    Value *PreCond = HeaderCond;
    auto ItCond = HeaderToPreValue.find(HeaderCond);
    if (ItCond != HeaderToPreValue.end()) PreCond = ItCond->second;
    PreHeader->genCondInst(PreCond, Body, Exit);

    auto HeaderPhisAfter = Header->collectPhiNodes();
    for (PhiInst *Phi : HeaderPhisAfter) {
        auto *NewPhi = new PhiInst(Phi->getIRType());
        forEachPhiIncoming(
            Phi, [&](unsigned, Value *Incoming, BasicBlock *IncomingBlock) {
                if (IncomingBlock != PreHeader)
                    NewPhi->addIncoming(Incoming, IncomingBlock);
            });
        if (NewPhi->getNumIncomingValues() == 0) {
            Phi->replaceAllUsesWith(UndefValue::NewUndefValue(Phi->getIRType()));
            delete NewPhi;
        } else if (NewPhi->getNumIncomingValues() == 1) {
            Phi->replaceAllUsesWith(NewPhi->getIncomingValue(0));
            delete NewPhi;
        } else {
            Phi->replaceAllUsesWith(NewPhi);
            Phi->replaceWith(NewPhi);
        }
        delete Phi;
    }

    L.Header = Body;

    // 步骤9：常量折叠与 LoopSimplify 形态修复
    auto *PHBr = dynamic_cast<BrInst *>(PreHeader->back());
    if (!PHBr || !PHBr->isConditional()) {
        if (auto *OldTerm = PreHeader->back()) {
            delete OldTerm;
        }
        PreHeader->genUnCondInst(Body);
        PHBr = nullptr;
    }

    bool FoldToBody = false;
    if (PHBr) {
        Value *PHCond = PHBr->getCondition();
        BasicBlock *PHT = PHBr->getTrueSuccessor();
        BasicBlock *PHF = PHBr->getFalseSuccessor();
        bool CV = false;
        if (PHCond && PHCond->tryGetBoolConst(CV)) {
            FoldToBody = CV ? (PHT == Body) : (PHF == Body);
            if (FoldToBody) {
                delete PHBr;
                PreHeader->genUnCondInst(Body);
            }
        }
    }

    if (!FoldToBody) {
        BasicBlock *OldPreHeader = PreHeader;
        auto *NewPreHeader = new BasicBlock();
        F.pushBlock(NewPreHeader);
        NewPreHeader->genUnCondInst(Body);
        BasicBlock::redirectBranchTarget(PreHeader, Body, NewPreHeader);
        for (PhiInst *Phi : Body->collectPhiNodes()) {
            forEachPhiIncoming(
                Phi, [&](unsigned I, Value *, BasicBlock *IncomingBlock) {
                    if (IncomingBlock == OldPreHeader)
                        Phi->setIncomingBlock(I, NewPreHeader);
                });
        }
        PreHeader = NewPreHeader;
    }

    std::unordered_set<BasicBlock *> MergePreds;
    for (BasicBlock *Pred : {Header, PreHeader}) {
        if (!Pred) continue;
        auto Succs = Pred->getSuccessors();
        for (BasicBlock *S : Succs) {
            if (S == Exit) {
                MergePreds.insert(Pred);
                break;
            }
        }
    }
    if (!MergePreds.empty()) {
        auto *DedicatedExit = new BasicBlock();
        F.pushBlock(DedicatedExit);
        for (BasicBlock *Pred : MergePreds) {
            BasicBlock::redirectBranchTarget(Pred, Exit, DedicatedExit);
        }
        // 更换为 LoopSimplifyPass 的接口
        LoopSimplifyPass::rewritePhisForMergedPreds(Exit, MergePreds, DedicatedExit);
        DedicatedExit->genUnCondInst(Exit);
    }

    // 步骤10：合并 Header 到 Latch
    auto *LatchTerm = dynamic_cast<BrInst *>(Latch->back());
    if (!LatchTerm || LatchTerm->isConditional()) return false;

    // 当这一步执行完成，Header 里的条件分支（连同指向 Exit 的边）就正式转移到了 Latch 中。
    delete LatchTerm;
    std::vector<BasicBlock *> HeaderSuccs = Header->getSuccessors();
    while (Header->front() != nullptr) {
        Instruction *I = Header->front();
        I->eraseFromParent();
        Latch->pushBack(I);
    }

    for (BasicBlock *Succ : HeaderSuccs) {
        auto SuccPhis = Succ->collectPhiNodes();
        for (PhiInst *Phi : SuccPhis) {
            forEachPhiIncoming(
                Phi, [&](unsigned I, Value *, BasicBlock *IncomingBlock) {
                    if (IncomingBlock == Header)
                        Phi->setIncomingBlock(I, Latch);
                });
        }
    }
    Header->eraseFromParent();

    // 步骤11：返回 true，事务顺利结束。
    return true;
}

PreservationStatus LoopRotatePass::run(Function &F, FunctionAnalysisPassManager &FAM) {
    bool Changed = false;

    // LoopRotate级别不动点迭代：不断尝试，直到一整轮下来没有任何循环被旋转
    while (true) {
        auto &DT = FAM.getResult<DominantAnalysis>(F);
        auto &LI = FAM.getResult<LoopAnalysis>(F);

        bool ChangedInRound = false;

        for (Loop *Top : LI.getTopLevelLoops()) {
            if (!Top || !Top->getHeader()) continue;

            std::vector<Loop *> Loops = Top->getLoopsInPostOrder();

            // 逆后序遍历，从最外层循环开始尝试旋转
            for (auto It = Loops.rbegin(); It != Loops.rend(); ++It) {
                Loop *Cur = *It;
                if (!Cur || !Cur->getHeader()) continue;

                if (runLoopRotate(F, *Cur, DT, LI)) {
                    Changed = true;
                    ChangedInRound = true;
                    break; // CFG被修改，立刻打断遍历准备重建 Loop 树
                }
            }

            if (ChangedInRound) break;
        }

        if (!ChangedInRound) {
            break;
        }

        // 清除旧的分析数据缓存，准备重扫
        PreservationStatus RoundPA;
        RoundPA.discard<DominantAnalysis>();
        RoundPA.discard<LoopAnalysis>();
        FAM.invalidate(F, RoundPA);
    }

    if (!Changed) {
        return PreservationStatus::all();
    }

    PreservationStatus FinalPA;
    FinalPA.discard<DominantAnalysis>();
    FinalPA.discard<PostDominantAnalysis>();
    FinalPA.discard<LoopAnalysis>();
    FinalPA.discard<BasicAliasAnalysis>();
    FinalPA.discard<SideEffectAnalysis>();
    return FinalPA;
}
