// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#include "ir/opt/CodeMotion.hpp"

#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/BlockFrequencyInfo.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "ir/analysis/TargetCostAnalysis.hpp"
#include "ir/opt/DCE.hpp"
#include "lib/IRUtils.hpp"

#include <algorithm>
#include <limits>
#include <unordered_set>

//证明常量下标inbounds GEP不会越过已知对象边界。
bool CodeMotionPass::isProvablySafeGEP(GetElementPtrInst *GEP,
                                       BasicAAResult &AA) {
    //仅凭inbounds标志不能把程序中的承诺当作优化器已经完成的证明；这里还要求
    //来源分析给出唯一对象、精确非负偏移和完整对象容量。one-past地址允许计算，
    //但本Pass从不移动后续load/store，因此不会把one-past误作可解引用地址。
    if (!GEP || !GEP->isInBounds() || !GEP->hasOnlyConstantIndices())
        return false;
    const PointerProvenanceInfo &Info = AA.getPointerProvenance(GEP);
    std::optional<long long> Offset = Info.getExactByteOffset();
    if (!Info.getUniqueBase() || !Info.ObjectSize || !Offset ||
        *Offset < 0) {
        return false;
    }
    const unsigned long long UnsignedOffset =
        static_cast<unsigned long long>(*Offset);
    return UnsignedOffset <=
        static_cast<unsigned long long>(*Info.ObjectSize);
}

//筛出可以安全减少执行次数的纯SSA表达式
bool CodeMotionPass::isSafeCandidate(Instruction *I,
                                     BasicAAResult &AA) {
    if (!I || !I->getParent() || !I->hasUses() ||
        !I->getIRType() || I->getIRType()->isVoid() ||
        dceInstructionMustBePreserved(I) ||
        instructionMayFaultOrProducePoison(I) ||
        valueMayDependOnUndef(I, 128)) {
        return false;
    }
    if (auto *GEP = dynamic_cast<GetElementPtrInst *>(I))
        return isProvablySafeGEP(GEP, AA);
    if (auto *Binary = dynamic_cast<BinaryInst *>(I)) {
        Value *Left = nullptr;
        Value *Right = nullptr;
        if (!Binary->getOperands(Left, Right) || !Left || !Right ||
            !Left->getIRType() || !Right->getIRType() ||
            !Left->getIRType()->isInt() ||
            !Right->getIRType()->isInt()) {
            return false;
        }
        switch (Binary->getOp()) {
        case BinaryInst::ADD:
        case BinaryInst::SUB:
        case BinaryInst::MUL:
        case BinaryInst::AND:
        case BinaryInst::OR:
        case BinaryInst::XOR:
        case BinaryInst::E:
        case BinaryInst::NE:
        case BinaryInst::GE:
        case BinaryInst::L:
        case BinaryInst::LE:
        case BinaryInst::G:
            return true;
        default:
            //除余和所有移位均不进入首版跨块移动，即使局部常量看似安全。
            return false;
        }
    }
    if (dynamic_cast<ZextInst *>(I) || dynamic_cast<SextInst *>(I) ||
        dynamic_cast<TruncInst *>(I)) {
        Value *Operand = I->getUnaryOperand();
        return Operand && Operand->getIRType() &&
               Operand->getIRType()->isInt() && I->getIRType()->isInt();
    }
    if (auto *Select = dynamic_cast<SelectInst *>(I)) {
        Value *Condition = Select->getCondition();
        Value *TrueValue = Select->getTrueValue();
        Value *FalseValue = Select->getFalseValue();
        return Condition && TrueValue && FalseValue &&
               Condition->getIRType() && Condition->getIRType()->isBool() &&
               TrueValue->getIRType() && TrueValue->getIRType()->isInt() &&
               FalseValue->getIRType() && FalseValue->getIRType()->isInt() &&
               I->getIRType()->isInt();
    }
    return false;
}

//把每个Use转换到其真正执行的块；PHI值Use位于对应前驱边末端。
bool CodeMotionPass::collectUseSites(
    Instruction *I, std::vector<UseSite> &Sites) {
    Sites.clear();
    if (!I) return false;
    std::vector<Use *> Uses = I->getUses();
    if (Uses.empty() || Uses.size() > UseBudget) return false;
    Sites.reserve(Uses.size());
    for (Use *U : Uses) {
        auto *UserI = U ? dynamic_cast<Instruction *>(U->getUser()) : nullptr;
        if (!UserI || !UserI->getParent()) return false;
        if (auto *Phi = dynamic_cast<PhiInst *>(UserI)) {
            std::optional<std::size_t> OperandIndex = U->getOperandIndex();
            //PHI的偶数操作数是值，奇数操作数是BasicBlock。
            if (!OperandIndex || (*OperandIndex % 2U) != 0U) return false;
            const unsigned Incoming =
                static_cast<unsigned>(*OperandIndex / 2U);
            if (Incoming >= Phi->getNumIncomingValues()) return false;
            BasicBlock *Pred = Phi->getIncomingBlock(Incoming);
            if (!Pred || !Pred->getTerminator()) return false;
            //平行CFG边会产生多个相同Pred的incoming。它们都映射到Pred终结点
            //之前，而不伪造某一条边专属的插入位置；重复Site在NCD折叠中无害。
            Sites.push_back({UserI, Pred, true});
            continue;
        }
        Sites.push_back({UserI, UserI->getParent(), false});
    }
    return !Sites.empty();
}

//使用支配树层级求两个块的最近公共支配者。
BasicBlock *CodeMotionPass::nearestCommonDominator(
    BasicBlock *First, BasicBlock *Second, const DominatorTree &DT) {
    if (!First || !Second) return nullptr;
    DominatorNode *Left = DT.getNode(First);
    DominatorNode *Right = DT.getNode(Second);
    if (!Left || !Right) return nullptr;
    while (Left->Level > Right->Level) Left = Left->IDom;
    while (Right->Level > Left->Level) Right = Right->IDom;
    while (Left && Right && Left != Right) {
        Left = Left->IDom;
        Right = Right->IDom;
    }
    return Left && Left == Right ? Left->Block : nullptr;
}

//折叠全部Use执行块，得到支配全部Use的最深基本块。
BasicBlock *CodeMotionPass::computeLatestBlock(
    const std::vector<UseSite> &Sites, const DominatorTree &DT) {
    if (Sites.empty()) return nullptr;
    BasicBlock *Latest = Sites.front().ExecutionBlock;
    for (std::size_t Index = 1; Index < Sites.size(); ++Index) {
        Latest = nearestCommonDominator(
            Latest, Sites[Index].ExecutionBlock, DT);
        if (!Latest) return nullptr;
    }
    return Latest;
}

//查询块所在的最内层循环深度。
unsigned CodeMotionPass::getLoopDepth(BasicBlock *BB,
                                      const LoopInfo &LI) {
    Loop *L = BB ? LI.getLoopFor(BB) : nullptr;
    return L ? L->getDepth() : 0U;
}

//找到目标块内最早普通User，PHI边Use只要求值在终结指令前可用。
Instruction *CodeMotionPass::findInsertionAnchor(
    BasicBlock *Destination, const std::vector<UseSite> &Sites) {
    if (!Destination) return nullptr;
    Instruction *Earliest = nullptr;
    for (const UseSite &Site : Sites) {
        if (Site.ExecutionBlock != Destination || Site.IsPhiEdge ||
            !Site.User || Site.User->getParent() != Destination) {
            continue;
        }
        if (!Earliest || Instruction::comesBefore(Site.User, Earliest))
            Earliest = Site.User;
    }
    return Earliest ? Earliest : Destination->getTerminator();
}

//检查每个操作数定义在目标程序点之前可用。
bool CodeMotionPass::operandsDominatePlacement(
    Instruction *I, BasicBlock *Destination, Instruction *Before,
    const DominatorTree &DT) {
    if (!I || !Destination || !Before || Before->getParent() != Destination)
        return false;
    for (std::size_t Index = 0; Index < I->getNumOperands(); ++Index) {
        Value *Operand = I->getOperand(Index);
        if (!Operand || Value::isUndef(Operand)) return false;
        auto *Definition = dynamic_cast<Instruction *>(Operand);
        if (!Definition) continue;
        BasicBlock *DefinitionBlock = Definition->getParent();
        if (!DefinitionBlock) return false;
        if (DefinitionBlock == Destination) {
            if (Definition == Before ||
                !Instruction::comesBefore(Definition, Before)) {
                return false;
            }
            continue;
        }
        if (!DT.dominates(DefinitionBlock, Destination)) return false;
    }
    return true;
}

//估计被下沉表达式会延长多少条操作数活跃区间。
unsigned CodeMotionPass::estimateExtendedOperands(
    Instruction *I, BasicBlock *Destination) {
    if (!I || !Destination) return MaxExtendedOperands + 1U;
    std::unordered_set<Value *> Extended;
    for (std::size_t Index = 0; Index < I->getNumOperands(); ++Index) {
        Value *Operand = I->getOperand(Index);
        if (!Operand || Operand->isConst() || Operand->isGlobal() ||
            dynamic_cast<BasicBlock *>(Operand) ||
            dynamic_cast<Function *>(Operand) ||
            dynamic_cast<BuildInFunction *>(Operand)) {
            continue;
        }
        auto *Definition = dynamic_cast<Instruction *>(Operand);
        if (Definition && Definition->getParent() == Destination) continue;
        //函数形参和跨块指令值都会因下沉延长寄存器活跃区间。
        if (!Definition && !dynamic_cast<Param *>(Operand)) continue;
        Extended.insert(Operand);
        if (Extended.size() > MaxExtendedOperands)
            return static_cast<unsigned>(Extended.size());
    }
    return static_cast<unsigned>(Extended.size());
}

//评估一次下沉是否节省动态执行，并限制活跃区间扩张。
bool CodeMotionPass::isProfitable(
    Instruction *I, BasicBlock *Source, const Placement &Candidate,
    const LoopInfo &LI, const BlockFrequencyInfo &BFI,
    const TargetCostInfo &TargetCost) {
    if (!I || !Source || !Candidate.Block || Candidate.Block == Source)
        return false;
    const unsigned SourceDepth = getLoopDepth(Source, LI);
    //块频率是静态估计，不能用它覆盖确定的循环层次事实。从循环外
    //下沉到更深循环会把一次计算放大为多次，因此无条件拒绝。
    if (Candidate.LoopDepth > SourceDepth) return false;
    const bool LeavesLoop = Candidate.LoopDepth < SourceDepth;
    const std::uint64_t SourceFrequency = BFI.getBlockFrequency(Source);
    const bool ReducesReliableFrequency =
        BFI.isReliable() && Candidate.Frequency < SourceFrequency;
    if (!LeavesLoop && !ReducesReliableFrequency) return false;
    if (BFI.isReliable() && Candidate.Frequency > SourceFrequency)
        return false;

    //RV64GC使用真实目标吞吐代价与活跃寄存器预算。
    const int Cost = TargetCost.getInstructionCost(
        I, TargetCostKind::Throughput);
    const unsigned RegisterBudget = TargetCost.getActiveRegisterBudget(
        TargetRegisterClass::GeneralPurpose);
    if (Cost <= 0 || Cost >= TargetCostInfo::InvalidCost) return false;
    const unsigned Extended = estimateExtendedOperands(I, Candidate.Block);
    const unsigned PressureLimit = std::max(
        1U, std::min(MaxExtendedOperands, RegisterBudget / 4U));
    if (Extended > PressureLimit) return false;

    //同一循环层中若延长了活跃区间，则要求至少约二倍频率下降；跨出循环本身
    //已经提供明确收益，仍受上面的绝对寄存器预算约束。
    if (!LeavesLoop && Extended != 0U) {
        if (Candidate.Frequency >
            std::numeric_limits<std::uint64_t>::max() / 2U) {
            return false;
        }
        if (SourceFrequency < Candidate.Frequency * 2U) return false;
    }
    return Cost > 0;
}

//沿Latest到Source的IDom链选择最合适的合法位置。
CodeMotionPass::Placement CodeMotionPass::choosePlacement(
    Instruction *I, BasicBlock *Latest,
    const std::vector<UseSite> &Sites, const DominatorTree &DT,
    const LoopInfo &LI, const BlockFrequencyInfo &BFI,
    const TargetCostInfo &TargetCost) {
    Placement Best;
    BasicBlock *Source = I ? I->getParent() : nullptr;
    if (!Source || !Latest || Latest == Source ||
        !DT.dominates(Source, Latest)) {
        return Best;
    }
    for (BasicBlock *Block = Latest; Block && Block != Source;
         Block = DT.getIDom(Block)) {
        Instruction *Before = findInsertionAnchor(Block, Sites);
        if (!Before || !operandsDominatePlacement(I, Block, Before, DT))
            continue;
        Placement Current;
        Current.Block = Block;
        Current.Before = Before;
        Current.LoopDepth = getLoopDepth(Block, LI);
        Current.Frequency = BFI.getBlockFrequency(Block);
        if (!isProfitable(I, Source, Current, LI, BFI, TargetCost))
            continue;
        if (!Best.Block || Current.LoopDepth < Best.LoopDepth ||
            (Current.LoopDepth == Best.LoopDepth &&
             Current.Frequency < Best.Frequency)) {
            Best = Current;
        }
    }
    return Best;
}

//提交一次不改变CFG的指令搬移。
bool CodeMotionPass::moveToPlacement(
    Instruction *I, const Placement &Destination) {
    if (!I || !I->getParent() || !Destination.Block ||
        !Destination.Before ||
        Destination.Before->getParent() != Destination.Block ||
        I == Destination.Before) {
        return false;
    }
    BasicBlock *Source = I->getParent();
    Instruction *OriginalNext = I->getNext();
    I->eraseFromParent();
    if (Instruction::insertBefore(Destination.Before, I)) return true;
    //侵入式插入在预检后的锚点上理论上不会失败；若结构被意外改变，必须把
    //指令精确恢复到原后继之前，不能留下未挂接定义或误报IR已变化。
    if (OriginalNext && OriginalNext->getParent() == Source) {
        if (Instruction::insertBefore(OriginalNext, I)) return false;
    } else if (Source) {
        Source->pushBack(I);
        if (I->getParent() == Source) return false;
    }
    //最后防线仅处理原后继也已失效的异常IR；重新挂到原块末尾仍优于悬空。
    if (Source && !I->getParent()) Source->pushBack(I);
    return false;
}

//运行保守GCM/CodeSink。
PreservationStatus CodeMotionPass::run(
    Function &F, FunctionAnalysisPassManager &FAM) {
    if (!F.front()) return PreservationStatus::all();
    auto &DT = FAM.getResult<DominantAnalysis>(F);
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &BFI = FAM.getResult<BlockFrequencyAnalysis>(F);
    const auto &TargetCost = FAM.getResult<TargetCostAnalysis>(F);
    auto &AA = FAM.getResult<BasicAliasAnalysis>(F);
    std::vector<Instruction *> Instructions = collectInstructions(F);
    if (Instructions.size() > CandidateBudget)
        Instructions.resize(CandidateBudget);

    //逆定义顺序优先处理用户，使后续操作数下沉能够看到更新后的Use位置。
    bool Changed = false;
    std::size_t Moves = 0;
    for (auto It = Instructions.rbegin(); It != Instructions.rend(); ++It) {
        Instruction *I = *It;
        if (Moves >= MoveBudget) break;
        if (!isSafeCandidate(I, AA)) continue;
        std::vector<UseSite> Sites;
        if (!collectUseSites(I, Sites)) continue;
        BasicBlock *Latest = computeLatestBlock(Sites, DT);
        Placement Destination = choosePlacement(
            I, Latest, Sites, DT, LI, BFI, TargetCost);
        if (!Destination.Block) continue;
        //即使纯SSA值不访问程序内存，也不能跨过计时调用改变区间内工作量。
        if (hasOrderingBarrierOnPath(F, I, Destination.Before, FAM))
            continue;
        if (moveToPlacement(I, Destination)) {
            Changed = true;
            ++Moves;
        }
    }
    if (!Changed) return PreservationStatus::all();

    //基本块和边没有变化，支配与循环结构仍精确；定义跨越循环边界后SCEV的
    //上下文查询可能变化，依赖SCEV的BPI/BFI也一并重算，避免发布陈旧收益信息。
    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<LoopAnalysis>();
    PA.keep<TargetCostAnalysis>();
    return PA;
}
