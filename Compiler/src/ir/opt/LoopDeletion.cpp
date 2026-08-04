#include "ir/opt/LoopDeletion.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/opt/DCE.hpp"
#include "ir/opt/LCSSA.hpp"
#include "lib/IRUtils.hpp"
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
//判断零次迭代路径是否能够安全跳过给定循环头指令。
bool LoopDeletionPass::isSafeToSkipOnZeroTrip(Instruction *I) {
    //2.1 零次循环只能跳过控制流和无副作用纯计算；未知指令及 DCE 白名单外语义保留。
    if (!I) return false;
    if (dynamic_cast<PhiInst *>(I) || dynamic_cast<BrInst *>(I)) return true;
    if (dceInstructionMustBePreserved(I)) return false;
    //可能陷阱或产生poison的死计算也不得借未定义行为随循环一起删除。
    if (instructionMayFaultOrProducePoison(I)) return false;
    if (dynamic_cast<BinaryInst *>(I)) return true;
    if (dynamic_cast<SelectInst *>(I) ||
        dynamic_cast<GetElementPtrInst *>(I)) {
        return true;
    }
    switch (I->instType) {
    case Instruction::TRUNC:
    case Instruction::ZEXT:
    case Instruction::SEXT:
    case Instruction::FPTOSI:
    case Instruction::SITOFP:
    case Instruction::BITCAST:
        return true;
    default:
        return false;
    }
}
//检查循环及其子循环中的全部指令是否均无可观察行为。
bool LoopDeletionPass::hasNoObservableLoopInstruction(Loop *L) {
    //2.2 非零死循环删除要求循环及所有子循环中的每条指令都可无观察地跳过。
    if (!L) return false;
    for (BasicBlock *BB : L->getBlocksIncludingSubLoops()) {
        if (!BB) return false;
        for (Instruction *I : *BB) {
            if (!I) return false;
            if (!isSafeToSkipOnZeroTrip(I)) return false;
        }
    }
    return true;
}
//证明运行期trip count对应严格比较的无回绕i32单位步长有限循环。
bool LoopDeletionPass::isProvablyFiniteStrictUnitStepLoop(
    SCEVResult &SE, const LoopTripCountInfo &TCI) {
    //1.1 运行期 trip count 只有在 header 以规范 i32 归纳变量、直接 ±1 更新和
    //对应严格 < 或 > 条件继续时才证明有限，排除回绕造成的无限循环。
    if (!TCI.Valid || !TCI.IsRuntime || !TCI.L || !TCI.Header ||
        TCI.ExitingBlock != TCI.Header || !TCI.ExitCmp || !TCI.IndVar) {
        return false;
    }
    auto IV = SE.getCanonicalInduction(TCI.L);
    if (!IV || !IV->Valid || IV->Phi != TCI.IndVar || !IV->StartValue ||
        !IV->UpdateValue || !IV->ConstantStep ||
        (*IV->ConstantStep != 1 && *IV->ConstantStep != -1)) {
        return false;
    }
    auto IsI32 = [](Value *V) {
        auto Ty = V ? V->getIRType() : nullptr;
        return IRType::hasIntegerBitWidth(Ty, 32);
    };
    if (!IsI32(IV->Phi) || !IsI32(IV->StartValue)) return false;
    auto *Update = dynamic_cast<BinaryInst *>(IV->UpdateValue);
    if (!Update) return false;
    Value *UpdateL = Update->getOperand(0);
    Value *UpdateR = Update->getOperand(1);
    auto *LeftC = dynamic_cast<ConstInt *>(UpdateL);
    auto *RightC = dynamic_cast<ConstInt *>(UpdateR);
    bool DirectStep = false;
    if (*IV->ConstantStep == 1 && Update->getOp() == BinaryInst::ADD) {
        DirectStep =
            (UpdateL == IV->Phi && RightC && RightC->getVal() == 1) ||
            (UpdateR == IV->Phi && LeftC && LeftC->getVal() == 1);
    } else if (*IV->ConstantStep == -1 &&
               Update->getOp() == BinaryInst::SUB) {
        DirectStep = UpdateL == IV->Phi && RightC && RightC->getVal() == 1;
    }
    if (!DirectStep) return false;
    BinaryInst::Operation Pred = TCI.ContinueOnTrue
        ? TCI.ExitCmp->getOp()
        : BinaryInst::negatePredicate(TCI.ExitCmp->getOp());
    Value *LHS = TCI.ExitCmp->getOperand(0);
    Value *RHS = TCI.ExitCmp->getOperand(1);
    if (RHS == IV->Phi) {
        std::swap(LHS, RHS);
        Pred = BinaryInst::reversePredicate(Pred);
    }
    if (LHS != IV->Phi || !IsI32(RHS)) return false;
    return (*IV->ConstantStep == 1 && Pred == BinaryInst::L) ||
           (*IV->ConstantStep == -1 && Pred == BinaryInst::G);
}
//沿循环头PHI的预头入值递归解析零次路径上的替代值。
Value *LoopDeletionPass::resolveZeroTripValue(
    Value *V, Loop *L, BasicBlock *PreHeader,
    std::unordered_set<Value *> &Active) {
    //3.1 零次路径上的循环内值只能经 header PHI 沿 preheader incoming 递归解析；
    //其他循环内定义和 PHI 环都没有合法的零次替代值。
    if (!V || !L || !PreHeader) return nullptr;
    auto *I = dynamic_cast<Instruction *>(V);
    if (!I || !I->getParent() || !L->contains(I->getParent())) return V;
    auto *Phi = dynamic_cast<PhiInst *>(I);
    if (!Phi || Phi->getParent() != L->getHeader()) return nullptr;
    ScopedActiveMark ActiveMark(Active, V);
    if (!ActiveMark.inserted()) return nullptr;
    Value *Incoming = Phi->getIncomingFromPred(PreHeader);
    return resolveZeroTripValue(Incoming, L, PreHeader, Active);
}
//为零次循环的全部退出PHI规划类型一致的预头路径替代值。
bool LoopDeletionPass::planZeroTripPhiReplacements(
    Loop *L, const LoopTripCountInfo &TCI, PhiReplacementMap &Plan) {
    //3.2 为 exit 的每个 PHI 解析 exiting 边值在零次路径上的 preheader 值，
    //全部类型一致才提交计划，避免部分替换。
    if (!L || !TCI.PreHeader || !TCI.ExitingBlock || !TCI.ExitBlock)
        return false;
    for (PhiInst *Phi : TCI.ExitBlock->collectPhiNodes()) {
        if (!Phi) return false;
        Value *OnExit = Phi->getIncomingFromPred(TCI.ExitingBlock);
        if (!OnExit) return false;
        std::unordered_set<Value *> Active;
        Value *Replacement =
            resolveZeroTripValue(OnExit, L, TCI.PreHeader, Active);
        if (!Replacement ||
            !IRType::hasSameShape(Replacement->getIRType(),
                                  Phi->getIRType())) {
            return false;
        }
        Plan[Phi] = Replacement;
    }
    return true;
}
//为无可观察行为的非零循环规划循环外公共退出值。
bool LoopDeletionPass::planDeadLoopPhiReplacements(
    Loop *L, BasicBlock *Exit, PhiReplacementMap &Plan) {
    //3.3 对无观察但会执行的死循环，exit PHI 的所有 incoming 必须相同且定义在
    //循环外；空 PHI 可替为 undef，否则无法删除产生该值的循环。
    if (!L || !Exit) return false;
    for (PhiInst *Phi : Exit->collectPhiNodes()) {
        if (!Phi) return false;
        unsigned N = Phi->getNumIncomingValues();
        if (N == 0) {
            Plan[Phi] = UndefValue::NewUndefValue(Phi->getIRType());
            continue;
        }
        Value *Keep = Phi->getIncomingValue(0);
        if (!Keep) return false;
        if (!forEachPhiIncoming(
                Phi, [Keep](unsigned I, Value *Incoming, BasicBlock *) {
                    return I == 0 || Incoming == Keep;
                })) {
            return false;
        }
        auto *Def = dynamic_cast<Instruction *>(Keep);
        if (Def && Def->getParent() && L->contains(Def->getParent()))
            return false;
        Plan[Phi] = Keep;
    }
    return true;
}
//检查所有循环外SSA使用是否都由退出PHI替换计划覆盖。
bool LoopDeletionPass::outsideUsesAreCovered(
    Loop *L, const std::vector<BasicBlock *> &Blocks,
    const PhiReplacementMap &Plan) {
    //3.4 循环内定义的每个循环外 user 都必须是已规划替换的 exit PHI，确保删块前
    //不会遗留普通跨循环 SSA use。
    if (!L) return false;
    for (BasicBlock *BB : Blocks) {
        if (!BB) return false;
        for (Instruction *I : *BB) {
            if (!I) return false;
            for (Use *U : I->getUses()) {
                auto *UserI = dynamic_cast<Instruction *>(U->getUser());
                if (!UserI || !UserI->getParent()) return false;
                if (L->contains(UserI->getParent())) continue;
                auto *Phi = dynamic_cast<PhiInst *>(UserI);
                if (!Phi || !Plan.count(Phi)) return false;
            }
        }
    }
    return true;
}
//集中应用已验证的退出PHI替换并删除原PHI。
void LoopDeletionPass::applyPhiReplacements(const PhiReplacementMap &Plan) {
    for (const auto &KV : Plan) {
        PhiInst *Phi = KV.first;
        Value *Replacement = KV.second;
        if (Phi && Replacement) Phi->replaceAllUsesWith(Replacement);
    }
    for (const auto &KV : Plan)
        if (KV.first) delete KV.first;
}
//让预头绕过循环后断开并删除整个循环CFG区域。
bool LoopDeletionPass::deleteLoopCFG(
    Loop *L, Function &F, BasicBlock *PreHeader, BasicBlock *Exit,
    const std::vector<BasicBlock *> &LoopBlocks,
    const PhiReplacementMap &PhiPlan) {
    //4.先把 preheader 的唯一入环边改到 exit，再应用已验证的 PHI 替换；至此循环
    //与剩余 CFG 断开，后续清空/删除块不会影响外部控制流。
    if (!L || !PreHeader || !Exit || LoopBlocks.empty()) return false;
    auto *OldTerm = PreHeader->getBranchTerminator();
    if (!BasicBlock::isUncondBr(OldTerm) ||
        OldTerm->getSuccessor(0) != L->getHeader()) {
        return false;
    }
    if (!PreHeader->replaceTerminator(new BrInst(Exit))) return false;
    applyPhiReplacements(PhiPlan);
    //4.1 以 undef 断开异常残留值 use，分两轮先 clear 全部块再 delete，安全处理
    //循环内部互相引用和回边。
    for (BasicBlock *BB : LoopBlocks) {
        if (!BB || BB->getParent() != &F) continue;
        for (Instruction *I : *BB) {
            if (!I || !I->getIRType() ||
                I->getIRType()->isVoid()) {
                continue;
            }
            if (I->hasUses())
                I->replaceAllUsesWith(
                    UndefValue::NewUndefValue(I->getIRType()));
        }
    }
    for (BasicBlock *BB : LoopBlocks) {
        if (BB && BB->getParent() == &F) BB->clear();
    }
    for (BasicBlock *BB : LoopBlocks)
        if (BB && BB->getParent() == &F) delete BB;
    return true;
}
//按内到外顺序删除一个已证明有限、无观察且SSA可修复的循环。
PreservationStatus LoopDeletionPass::run(
    Function &F, FunctionAnalysisPassManager &FAM) {
    //1.获取循环树与 SCEV trip count，并按内到外顺序只考察无子循环、LCSSA 且
    //具有 preheader/latch 的规范循环。
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &SE = FAM.getResult<SCEVAnalysis>(F);
    bool Changed = false;
    for (Loop *L : LI.getLoopsInPostOrder(true)) {
        if (!L || !L->getHeader() || !L->getSubLoops().empty()) continue;
        if (L->getHeader()->getParent() != &F) continue;
        if (!LCSSAPass::isLCSSAForm(L)) continue;
        BasicBlock *PreHeader = nullptr;
        BasicBlock *Latch = nullptr;
        if (!L->getLoopSimplifyForm(PreHeader, Latch) ||
            !PreHeader || !Latch) {
            continue;
        }
        auto TCI = SE.analyzeLoopTripCount(L);
        if (!TCI || !TCI->Valid || !TCI->ExitBlock) {
            continue;
        }
        //1.2 候选必须已证明有限：常量次数要求无回绕，运行期次数要求严格单位步长；
        //零次循环还要求退出测试位于 header，确保 body 从未执行。
        bool ZeroTrip = TCI->ConstantTripCount &&
                        *TCI->ConstantTripCount == 0;
        bool FiniteConstant = TCI->ConstantTripCount &&
                              *TCI->ConstantTripCount >= 0 &&
                              (ZeroTrip || TCI->ProvenNoWrap);
        bool FiniteRuntime = !TCI->ConstantTripCount &&
                             isProvablyFiniteStrictUnitStepLoop(SE, *TCI);
        if ((!FiniteConstant && !FiniteRuntime) ||
            (ZeroTrip && TCI->ExitingBlock != TCI->Header)) {
            continue;
        }
        std::vector<BasicBlock *> Blocks = L->getBlocksInFunctionOrder();
        if (Blocks.empty()) continue;
        //2—3.零次候选只检查 header 可跳过并解析零次 PHI；其他候选要求整个循环
        //无可观察行为并规划公共 exit 值。
        PhiReplacementMap PhiPlan;
        if (ZeroTrip) {
            bool HeaderSafe = true;
            for (Instruction *I : *L->getHeader())
                if (!isSafeToSkipOnZeroTrip(I)) {
                    HeaderSafe = false;
                    break;
                }
            if (!HeaderSafe ||
                !planZeroTripPhiReplacements(L, *TCI, PhiPlan)) {
                continue;
            }
        } else {
            if (!hasNoObservableLoopInstruction(L) ||
                !planDeadLoopPhiReplacements(L, TCI->ExitBlock, PhiPlan)) {
                continue;
            }
        }
        //4.所有外部 use 被计划覆盖后执行一次 CFG 删除；循环分析已陈旧，立即停止扫描。
        if (!outsideUsesAreCovered(L, Blocks, PhiPlan)) continue;
        if (!deleteLoopCFG(L, F, PreHeader, TCI->ExitBlock,
                           Blocks, PhiPlan)) {
            continue;
        }
        Changed = true;
        break;
    }
    //5.删除基本块和边后所有函数分析均失效。
    if (!Changed) return PreservationStatus::all();
    return PreservationStatus::none();
}
