#include "ir/opt/LoopRotate.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "ir/opt/LoopSimplify.hpp"
#include "lib/IRUtils.hpp"
#include <algorithm>
//识别约定命名的LCSSA出口PHI。
bool LoopRotatePass::isLCSSAPhi(PhiInst *Phi) {
    if (!Phi) return false;
    return Phi->getIdent().find(".lcssa") != std::string::npos;
}
//使用统一值映射接口克隆一条Header指令。
Instruction *LoopRotatePass::cloneInstructionWithMap(
    Instruction *I,
    const std::unordered_map<Value *, Value *> &VMap) {
    if (!I) return nullptr;
    return I->clone(VMap);
}
//完成单个头测循环的结构预检、Header克隆、SSA转发和CFG旋转。
bool LoopRotatePass::runLoopRotate(Function &F, Loop &L, const DominatorTree &DT, const LoopInfo &LI) {
    (void)DT;
    (void)LI;
    BasicBlock *Header = L.getHeader();
    if (!Header) return false;
    //2.结构入口：取唯一 PreHeader 和 Latch，并拒绝 Latch 本身退出的尾测循环，避免对已旋转形态重复处理。
    BasicBlock *PreHeader = nullptr;
    BasicBlock *Latch = nullptr;
    if (!L.getLoopSimplifyForm(PreHeader, Latch)) return false;
    if (!PreHeader || !Latch) return false;
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
    //2.2 只处理包含实际 Body 的多块头测循环：Header 的一个分支进入唯一 Exit，另一个进入循环体。
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
        //2.3 在破坏 PreHeader 终结指令之前试克隆每条 Header 计算，确认后续克隆路径可用。
        Instruction *Probe = cloneInstructionWithMap(OldI, {});
        if (!Probe) return false;
        delete Probe;
    }
    //2.4 提交前锁定边形：Exit 和 Body 都只由 Header 到达，Latch 仅无条件回到 Header，因此后续可按固定前驱重建 PHI。
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
    //3.预处理 Body PHI：保留 LCSSA PHI，删除无使用节点，并将空输入或全相同输入折叠成单值。
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
    //3.1 开始旋转提交：移除 PreHeader 的旧无条件分支，分别建立 Header→PreHeader 与 Header→Body 值映射。
    auto *PreHeaderTerm = dynamic_cast<BrInst *>(PreHeader->back());
    if (!PreHeaderTerm || PreHeaderTerm->isConditional()) return false;
    delete PreHeaderTerm;
    std::unordered_map<Value *, Value *> HeaderToPreValue;
    std::unordered_map<Value *, Value *> HeaderToBodyValue;
    std::vector<PhiInst *> NewBodyPhis;
    for (PhiInst *OldPhi : HeaderPhis) {
        //3.2 每个 Header PHI 在 Body 中改造为两路合流：首次来自 PreHeader 初值，后续来自旧 Header 回边值。
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
        //3.3 按原顺序把 Header 正文克隆到 PreHeader；若值在循环体中仍有使用，则在 Body 补一个首次/后续 PHI。
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
    //4.分区改写使用：Header 和循环体内使用转向 Body 值，循环外使用则经 Exit LCSSA PHI 合流。
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
        //4.1 旋转后 PreHeader 也能直接退出，因此为原有 LCSSA PHI 补齐该新前驱的首次计算值。
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
    //5.核心 CFG 旋转：PreHeader 执行克隆条件并选择 Body/Exit，同时删除 Header PHI 的旧 PreHeader 入边。
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
    //5.1 规范化新入口：折叠常量首次条件；仍有零次旁路时插入专用 preheader 并同步 Body PHI 前驱。
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
        //5.2 Header 与新 preheader 都可能退出时建立专用 Exit，复用 LoopSimplify 的合并前驱 PHI 重写。
        LoopSimplifyPass::rewritePhisForMergedPreds(Exit, MergePreds, DedicatedExit);
        DedicatedExit->genUnCondInst(Exit);
    }
    //5.3 将旧 Header 的条件计算和分支整体搬到 Latch，使循环从头测试转为尾测试。
    auto *LatchTerm = dynamic_cast<BrInst *>(Latch->back());
    if (!LatchTerm || LatchTerm->isConditional()) return false;
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
    //5.4 把旧 Header 作为前驱的所有后继 PHI 改为 Latch，然后移除空 Header，单循环旋转完成。
    return true;
}
//执行局部固定点循环旋转并在每次CFG改写后重建结构分析。
PreservationStatus LoopRotatePass::run(Function &F, FunctionAnalysisPassManager &FAM) {
    bool Changed = false;
    //1.pass 内执行局部固定点：一轮只旋转一个循环，直到完整一轮无变更。
    while (true) {
        auto &DT = FAM.getResult<DominantAnalysis>(F);
        auto &LI = FAM.getResult<LoopAnalysis>(F);
        bool ChangedInRound = false;
        for (Loop *Top : LI.getTopLevelLoops()) {
            if (!Top || !Top->getHeader()) continue;
            std::vector<Loop *> Loops = Top->getLoopsInPostOrder();
            //1.1 将 post-order 逆置后从外向内尝试，使父循环先获得规范的尾测试形态。
            for (auto It = Loops.rbegin(); It != Loops.rend(); ++It) {
                Loop *Cur = *It;
                if (!Cur || !Cur->getHeader()) continue;
                if (runLoopRotate(F, *Cur, DT, LI)) {
                    Changed = true;
                    ChangedInRound = true;
                    //1.2 CFG 已变更，所有循环指针随即过期；立即结束本轮并重建分析。
                    break;
                }
            }
            if (ChangedInRound) break;
        }
        if (!ChangedInRound) {
            break;
        }
        //1.3 每次成功后先失效支配树和循环树，下一轮必须在新 CFG 上重新获取。
        PreservationStatus RoundPA;
        RoundPA.discard<DominantAnalysis>();
        RoundPA.discard<LoopAnalysis>();
        FAM.invalidate(F, RoundPA);
    }
    if (!Changed) {
        return PreservationStatus::all();
    }
    PreservationStatus FinalPA;
    //6.旋转同时改变 CFG、循环成员和指令求值位置，相关结构、别名与副作用分析全部失效。
    FinalPA.discard<DominantAnalysis>();
    FinalPA.discard<PostDominantAnalysis>();
    FinalPA.discard<LoopAnalysis>();
    FinalPA.discard<BasicAliasAnalysis>();
    FinalPA.discard<SideEffectAnalysis>();
    return FinalPA;
}
