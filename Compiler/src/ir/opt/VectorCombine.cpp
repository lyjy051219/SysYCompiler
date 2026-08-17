#include "ir/opt/VectorCombine.hpp"

#include "ir/analysis/TargetCostAnalysis.hpp"
#include "ir/opt/DCE.hpp"
#include "lib/IRUtils.hpp"

#include <cstdint>
#include <unordered_set>
#include <utility>

VectorCombinePass::VectorCombinePass(VectorCombineStage Stage)
    : Stage(Stage) {}

std::shared_ptr<VECTORType>
VectorCombinePass::getVectorType(const Value *V) {
    return V ? IRType::asVector(V->getIRType()) : nullptr;
}

bool VectorCombinePass::getValidConstantLane(
    Value *Index, const std::shared_ptr<VECTORType> &Type,
    std::size_t &Lane) {
    int64_t SignedLane = 0;
    if (!Index || !Type || !Index->tryGetInt64Const(SignedLane) ||
        SignedLane < 0)
        return false;
    Lane = static_cast<std::size_t>(SignedLane);
    return Lane < Type->getElementCount();
}

bool VectorCombinePass::isConcreteScalarConstant(Value *V) {
    auto *Constant = dynamic_cast<ConstantSystem *>(V);
    return Constant && !Value::isUndef(Constant) &&
           Constant->getIRType() && Constant->getIRType()->isScalar();
}

bool VectorCombinePass::replacementHasExpectedType(
    const Instruction *Old, const Value *Replacement) {
    return Old && Replacement && Old->getIRType() &&
           Replacement->getIRType() &&
           Old->getIRType()->hasSameShape(*Replacement->getIRType());
}

bool VectorCombinePass::replaceInstruction(
    Instruction *Old, Value *Replacement) {
    if (!Old || !Replacement || Old == Replacement ||
        !replacementHasExpectedType(Old, Replacement)) {
        auto *NewInstruction = dynamic_cast<Instruction *>(Replacement);
        if (NewInstruction && NewInstruction != Old &&
            !NewInstruction->getParent())
            delete NewInstruction;
        return false;
    }

    auto *NewInstruction = dynamic_cast<Instruction *>(Replacement);
    if (NewInstruction && !NewInstruction->getParent()) {
        if (!Old->getParent()) {
            delete NewInstruction;
            return false;
        }
        Old->replaceWith(NewInstruction);
    }
    Old->replaceAllUsesWith(Replacement);
    delete Old;
    return true;
}

Value *VectorCombinePass::foldInsertChain(InsertElementInst *Root) {
    if (!Root)
        return nullptr;
    const auto VectorType = getVectorType(Root);
    if (!VectorType)
        return nullptr;

    const std::size_t LaneCount = VectorType->getElementCount();
    std::vector<Value *> FinalElements(LaneCount, nullptr);
    std::unordered_set<Value *> Visited;
    Value *Cursor = Root;

    // 从最外层向内记录每个 lane 的最后一次写入；所有索引都必须已证明合法。
    while (auto *Insert = dynamic_cast<InsertElementInst *>(Cursor)) {
        if (!Visited.insert(Insert).second ||
            !Insert->getIRType()->hasSameShape(*VectorType))
            return nullptr;
        std::size_t Lane = 0;
        if (!getValidConstantLane(
                Insert->getIndexOperand(), VectorType, Lane))
            return nullptr;
        if (!FinalElements[Lane])
            FinalElements[Lane] = Insert->getElementOperand();
        Cursor = Insert->getVectorOperand();
    }

    if (auto *Base = dynamic_cast<ConstantVector *>(Cursor)) {
        if (!Base->getIRType()->hasSameShape(*VectorType))
            return nullptr;
        for (std::size_t Lane = 0; Lane < LaneCount; ++Lane)
            if (!FinalElements[Lane])
                FinalElements[Lane] = Base->getElement(Lane);
    } else if (!Value::isUndef(Cursor) || !Cursor->getIRType() ||
               !Cursor->getIRType()->hasSameShape(*VectorType)) {
        return nullptr;
    }

    bool AllConcrete = true;
    std::vector<ConstantSystem *> Constants;
    Constants.reserve(LaneCount);
    for (Value *Element : FinalElements) {
        if (!isConcreteScalarConstant(Element)) {
            AllConcrete = false;
            break;
        }
        Constants.push_back(dynamic_cast<ConstantSystem *>(Element));
    }
    if (AllConcrete && Constants.size() == LaneCount)
        return ConstantVector::get(VectorType, Constants);

    // 完整的 extract/insert 重建链可还原原向量；不完整的 undef lane 保持原状。
    Value *CommonSource = nullptr;
    for (std::size_t Lane = 0; Lane < LaneCount; ++Lane) {
        auto *Extract =
            dynamic_cast<ExtractElementInst *>(FinalElements[Lane]);
        std::size_t ExtractedLane = 0;
        if (!Extract ||
            !getValidConstantLane(
                Extract->getIndexOperand(), VectorType, ExtractedLane) ||
            ExtractedLane != Lane)
            return nullptr;
        Value *Source = Extract->getVectorOperand();
        if (!Source || Value::isUndef(Source) ||
            !Source->getIRType()->hasSameShape(*VectorType))
            return nullptr;
        if (!CommonSource)
            CommonSource = Source;
        else if (CommonSource != Source)
            return nullptr;
    }
    return CommonSource && !valueMayDependOnUndef(CommonSource)
               ? CommonSource
               : nullptr;
}

Value *VectorCombinePass::foldInsert(InsertElementInst *Insert) const {
    if (!Insert)
        return nullptr;
    if (Value *FoldedChain = foldInsertChain(Insert))
        return FoldedChain;

    const auto VectorType = getVectorType(Insert);
    std::size_t OuterLane = 0;
    if (!getValidConstantLane(
            Insert->getIndexOperand(), VectorType, OuterLane))
        return nullptr;

    // 写回刚从同一向量、同一合法 lane 取出的值没有任何效果。
    if (auto *Extract = dynamic_cast<ExtractElementInst *>(
            Insert->getElementOperand())) {
        std::size_t ExtractLane = 0;
        if (Extract->getVectorOperand() == Insert->getVectorOperand() &&
            getValidConstantLane(
                Extract->getIndexOperand(), VectorType, ExtractLane) &&
            ExtractLane == OuterLane)
            return Insert->getVectorOperand();
    }

    // 同一 lane 的连续写入以后一次为准，但不跨越动态或越界索引。
    auto *Inner = dynamic_cast<InsertElementInst *>(
        Insert->getVectorOperand());
    if (!Inner)
        return nullptr;
    std::size_t InnerLane = 0;
    if (!getValidConstantLane(
            Inner->getIndexOperand(), VectorType, InnerLane) ||
        InnerLane != OuterLane)
        return nullptr;
    return new InsertElementInst(
        Inner->getVectorOperand(), Insert->getElementOperand(),
        Insert->getIndexOperand());
}

Value *VectorCombinePass::foldConstantShuffle(
    ShuffleVectorInst *Shuffle) {
    if (!Shuffle)
        return nullptr;
    auto *First = dynamic_cast<ConstantVector *>(
        Shuffle->getFirstVectorOperand());
    auto *Second = dynamic_cast<ConstantVector *>(
        Shuffle->getSecondVectorOperand());
    const auto SourceType = getVectorType(
        Shuffle->getFirstVectorOperand());
    const auto ResultType = getVectorType(Shuffle);
    if (!First || !Second || !SourceType || !ResultType)
        return nullptr;

    const std::size_t SourceLanes = SourceType->getElementCount();
    std::vector<ConstantSystem *> Elements;
    Elements.reserve(Shuffle->getMask().size());
    for (int MaskLane : Shuffle->getMask()) {
        // -1 在本项目 IR 中明确打印为 poison，不能任选一个常量代替。
        if (MaskLane < 0)
            return nullptr;
        const std::size_t Lane = static_cast<std::size_t>(MaskLane);
        ConstantSystem *Element =
            Lane < SourceLanes
                ? First->getElement(Lane)
                : Second->getElement(Lane - SourceLanes);
        if (!isConcreteScalarConstant(Element))
            return nullptr;
        Elements.push_back(Element);
    }
    return ConstantVector::get(ResultType, Elements);
}

ShuffleVectorInst *VectorCombinePass::composeNestedShuffles(
    ShuffleVectorInst *Outer) {
    if (!Outer)
        return nullptr;
    auto *FirstInner = dynamic_cast<ShuffleVectorInst *>(
        Outer->getFirstVectorOperand());
    auto *SecondInner = dynamic_cast<ShuffleVectorInst *>(
        Outer->getSecondVectorOperand());
    if (!FirstInner || !SecondInner ||
        FirstInner->getFirstVectorOperand() !=
            SecondInner->getFirstVectorOperand() ||
        FirstInner->getSecondVectorOperand() !=
            SecondInner->getSecondVectorOperand())
        return nullptr;

    const std::size_t FirstWidth = FirstInner->getMask().size();
    const std::size_t SecondWidth = SecondInner->getMask().size();
    if (FirstWidth == 0 || FirstWidth != SecondWidth)
        return nullptr;

    std::vector<int> ComposedMask;
    ComposedMask.reserve(Outer->getMask().size());
    for (int OuterLane : Outer->getMask()) {
        if (OuterLane < 0) {
            ComposedMask.push_back(-1);
            continue;
        }
        const std::size_t Lane = static_cast<std::size_t>(OuterLane);
        if (Lane < FirstWidth)
            ComposedMask.push_back(FirstInner->getMask()[Lane]);
        else if (Lane - FirstWidth < SecondWidth)
            ComposedMask.push_back(
                SecondInner->getMask()[Lane - FirstWidth]);
        else
            return nullptr;
    }
    return new ShuffleVectorInst(
        FirstInner->getFirstVectorOperand(),
        FirstInner->getSecondVectorOperand(), std::move(ComposedMask));
}

Value *VectorCombinePass::foldShuffle(
    ShuffleVectorInst *Shuffle) const {
    if (!Shuffle)
        return nullptr;
    if (Value *Folded = foldConstantShuffle(Shuffle))
        return Folded;

    const auto SourceType = getVectorType(
        Shuffle->getFirstVectorOperand());
    if (!SourceType)
        return nullptr;
    const std::size_t SourceLanes = SourceType->getElementCount();
    const auto &Mask = Shuffle->getMask();

    if (Mask.size() == SourceLanes) {
        bool FirstIdentity = true;
        bool SecondIdentity = true;
        bool SameInputIdentity =
            Shuffle->getFirstVectorOperand() ==
            Shuffle->getSecondVectorOperand();
        for (std::size_t Lane = 0; Lane < Mask.size(); ++Lane) {
            if (Mask[Lane] < 0) {
                FirstIdentity = false;
                SecondIdentity = false;
                SameInputIdentity = false;
                break;
            }
            const std::size_t Selected =
                static_cast<std::size_t>(Mask[Lane]);
            FirstIdentity &= Selected == Lane;
            SecondIdentity &= Selected == SourceLanes + Lane;
            SameInputIdentity &= Selected % SourceLanes == Lane;
        }
        if (FirstIdentity || SameInputIdentity)
            return Shuffle->getFirstVectorOperand();
        if (SecondIdentity)
            return Shuffle->getSecondVectorOperand();
    }

    if (Stage == VectorCombineStage::Late)
        return composeNestedShuffles(Shuffle);
    return nullptr;
}

Value *VectorCombinePass::foldExtract(
    ExtractElementInst *Extract) const {
    if (!Extract)
        return nullptr;
    const auto VectorType = getVectorType(
        Extract->getVectorOperand());
    std::size_t Lane = 0;
    if (!getValidConstantLane(
            Extract->getIndexOperand(), VectorType, Lane))
        return nullptr;

    if (auto *Constant = dynamic_cast<ConstantVector *>(
            Extract->getVectorOperand()))
        return Constant->getElement(Lane);

    if (auto *Insert = dynamic_cast<InsertElementInst *>(
            Extract->getVectorOperand())) {
        const auto InsertType = getVectorType(Insert);
        std::size_t InsertLane = 0;
        if (!getValidConstantLane(
                Insert->getIndexOperand(), InsertType, InsertLane))
            return nullptr;
        if (InsertLane == Lane)
            return Insert->getElementOperand();
        return new ExtractElementInst(
            Insert->getVectorOperand(), Extract->getIndexOperand());
    }

    if (auto *Shuffle = dynamic_cast<ShuffleVectorInst *>(
            Extract->getVectorOperand())) {
        const int Selected = Shuffle->getMask()[Lane];
        if (Selected < 0)
            return nullptr;
        const auto SourceType = getVectorType(
            Shuffle->getFirstVectorOperand());
        if (!SourceType)
            return nullptr;
        const std::size_t SourceLanes = SourceType->getElementCount();
        const std::size_t SelectedLane =
            static_cast<std::size_t>(Selected);
        Value *Source = SelectedLane < SourceLanes
                            ? Shuffle->getFirstVectorOperand()
                            : Shuffle->getSecondVectorOperand();
        const std::size_t SourceLane =
            SelectedLane < SourceLanes
                ? SelectedLane
                : SelectedLane - SourceLanes;
        if (auto *Constant = dynamic_cast<ConstantVector *>(Source))
            return Constant->getElement(SourceLane);
        Value *NewIndex = Value::makeIntegerLike(
            Extract->getIndexOperand(),
            static_cast<int64_t>(SourceLane));
        return NewIndex
                   ? static_cast<Value *>(
                         new ExtractElementInst(Source, NewIndex))
                   : nullptr;
    }

    if (Stage == VectorCombineStage::Late)
        return scalarizeExtractedBinary(Extract);
    return nullptr;
}

Value *VectorCombinePass::foldBinary(BinaryInst *Binary) const {
    if (!Binary)
        return nullptr;
    Value *Left = nullptr;
    Value *Right = nullptr;
    if (!Binary->getOperands(Left, Right) || !Left || !Right ||
        !Left->getIRType() || !Left->getIRType()->isVector())
        return nullptr;

    auto *LeftConstant = dynamic_cast<ConstantVector *>(Left);
    auto *RightConstant = dynamic_cast<ConstantVector *>(Right);
    if (LeftConstant && RightConstant) {
        bool AllConcrete = true;
        for (ConstantSystem *Element : LeftConstant->getElements())
            AllConcrete &= isConcreteScalarConstant(Element);
        for (ConstantSystem *Element : RightConstant->getElements())
            AllConcrete &= isConcreteScalarConstant(Element);
        if (AllConcrete) {
            Value *Folded = BasicBlock::genBinaryInst(
                nullptr, Left, Binary->getOp(), Right);
            auto *FoldedVector = dynamic_cast<ConstantVector *>(Folded);
            if (!FoldedVector)
                return nullptr;
            for (ConstantSystem *Element : FoldedVector->getElements())
                if (!isConcreteScalarConstant(Element))
                    return nullptr;
            return FoldedVector;
        }
    }

    const auto ScalarType =
        IRType::getScalarElementType(Left->getIRType());
    if (!ScalarType || !ScalarType->isInt())
        return nullptr;

    switch (Binary->getOp()) {
    case BinaryInst::ADD:
        if (Value::isConstZero(Right)) return Left;
        if (Value::isConstZero(Left)) return Right;
        break;
    case BinaryInst::SUB:
        if (Value::isConstZero(Right)) return Left;
        break;
    case BinaryInst::MUL:
        if (Value::isConstOne(Right)) return Left;
        if (Value::isConstOne(Left)) return Right;
        break;
    case BinaryInst::DIV:
    case BinaryInst::UDIV:
        if (Value::isConstOne(Right)) return Left;
        break;
    case BinaryInst::AND:
        if (Left == Right || Value::isConstMinusOne(Right)) return Left;
        if (Value::isConstMinusOne(Left)) return Right;
        break;
    case BinaryInst::OR:
        if (Left == Right || Value::isConstZero(Right)) return Left;
        if (Value::isConstZero(Left)) return Right;
        break;
    case BinaryInst::XOR:
        if (Value::isConstZero(Right)) return Left;
        if (Value::isConstZero(Left)) return Right;
        break;
    case BinaryInst::SHL:
    case BinaryInst::LSHR:
    case BinaryInst::ASHR:
        if (Value::isConstZero(Right)) return Left;
        break;
    default:
        break;
    }
    return nullptr;
}

Value *VectorCombinePass::foldSelect(SelectInst *Select) const {
    if (!Select)
        return nullptr;
    Value *Condition = Select->getCondition();
    Value *TrueValue = Select->getTrueValue();
    Value *FalseValue = Select->getFalseValue();
    if (!Condition || !TrueValue || !FalseValue ||
        !Select->getIRType()->isVector())
        return nullptr;

    bool ScalarCondition = false;
    if (Condition->tryGetBoolConst(ScalarCondition))
        return ScalarCondition ? TrueValue : FalseValue;

    auto *Mask = dynamic_cast<ConstantVector *>(Condition);
    if (!Mask || !Condition->getIRType()->isBooleanVector())
        return nullptr;

    bool FirstLane = false;
    bool Uniform = true;
    bool HaveFirstLane = false;
    std::vector<bool> LaneChoices;
    LaneChoices.reserve(Mask->getNumElements());
    for (ConstantSystem *Element : Mask->getElements()) {
        bool Lane = false;
        if (!Element->tryGetBoolConst(Lane))
            return nullptr;
        LaneChoices.push_back(Lane);
        if (!HaveFirstLane) {
            FirstLane = Lane;
            HaveFirstLane = true;
        } else {
            Uniform &= Lane == FirstLane;
        }
    }
    if (HaveFirstLane && Uniform)
        return FirstLane ? TrueValue : FalseValue;

    auto *TrueConstant = dynamic_cast<ConstantVector *>(TrueValue);
    auto *FalseConstant = dynamic_cast<ConstantVector *>(FalseValue);
    const auto ResultType = getVectorType(Select);
    if (!TrueConstant || !FalseConstant || !ResultType ||
        LaneChoices.size() != ResultType->getElementCount())
        return nullptr;

    std::vector<ConstantSystem *> Result;
    Result.reserve(LaneChoices.size());
    for (std::size_t Lane = 0; Lane < LaneChoices.size(); ++Lane) {
        ConstantSystem *Element =
            LaneChoices[Lane]
                ? TrueConstant->getElement(Lane)
                : FalseConstant->getElement(Lane);
        if (!isConcreteScalarConstant(Element))
            return nullptr;
        Result.push_back(Element);
    }
    return ConstantVector::get(ResultType, Result);
}

Value *VectorCombinePass::foldCast(Instruction *Cast) const {
    if (!Cast || Cast->getNumOperands() != 1)
        return nullptr;
    Value *Input = Cast->getUnaryOperand();
    if (!Input || !Input->getIRType() ||
        (!Input->getIRType()->isVector() &&
         !Cast->getIRType()->isVector()))
        return nullptr;

    if (Cast->instType == Instruction::BITCAST) {
        if (Input->getIRType()->hasSameShape(*Cast->getIRType()))
            return Input;
        auto *Inner = dynamic_cast<BitCastInst *>(Input);
        Value *Original = Inner ? Inner->getUnaryOperand() : nullptr;
        if (Original && Original->getIRType() &&
            Original->getIRType()->hasSameShape(*Cast->getIRType()))
            return Original;
        return nullptr;
    }

    auto *Constant = dynamic_cast<ConstantVector *>(Input);
    const auto ResultType = getVectorType(Cast);
    if (!Constant || !ResultType ||
        Constant->getNumElements() != ResultType->getElementCount())
        return nullptr;

    switch (Cast->instType) {
    case Instruction::SITOFP:
    case Instruction::FPTOSI:
    case Instruction::ZEXT:
    case Instruction::SEXT:
    case Instruction::TRUNC:
        break;
    default:
        return nullptr;
    }

    std::vector<ConstantSystem *> Elements;
    Elements.reserve(Constant->getNumElements());
    for (ConstantSystem *Element : Constant->getElements()) {
        Value *Folded = Instruction::foldConstantCast(
            Cast->instType, Element, ResultType->getElementType());
        if (!isConcreteScalarConstant(Folded))
            return nullptr;
        Elements.push_back(dynamic_cast<ConstantSystem *>(Folded));
    }
    return ConstantVector::get(ResultType, Elements);
}

bool VectorCombinePass::isScalarizationOpcodeAllowed(
    BinaryInst::Operation Operation) {
    switch (Operation) {
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
        return false;
    }
}

bool VectorCombinePass::addFiniteCost(int Cost, int &Total) {
    if (Cost < 0 || Cost >= TargetCostInfo::InvalidCost ||
        Total >= TargetCostInfo::InvalidCost - Cost)
        return false;
    Total += Cost;
    return true;
}

Value *VectorCombinePass::scalarizeExtractedBinary(
    ExtractElementInst *Extract) const {
    if (!Extract || !CostInfo)
        return nullptr;
    auto *VectorBinary = dynamic_cast<BinaryInst *>(
        Extract->getVectorOperand());
    if (!VectorBinary ||
        !isScalarizationOpcodeAllowed(VectorBinary->getOp()) ||
        !VectorBinary->hasExactlyOneUse())
        return nullptr;
    Use *OnlyUse = VectorBinary->getSingleUse();
    if (!OnlyUse || OnlyUse->getUser() != Extract)
        return nullptr;

    int OldCost = 0;
    if (!addFiniteCost(
            CostInfo->getInstructionCost(VectorBinary), OldCost) ||
        !addFiniteCost(
            CostInfo->getInstructionCost(Extract), OldCost))
        return nullptr;

    Value *LeftVector = nullptr;
    Value *RightVector = nullptr;
    if (!VectorBinary->getOperands(LeftVector, RightVector))
        return nullptr;
    const auto VectorType = getVectorType(LeftVector);
    std::size_t Lane = 0;
    if (!getValidConstantLane(
            Extract->getIndexOperand(), VectorType, Lane))
        return nullptr;

    std::vector<Instruction *> NewExtracts;
    auto CreateLane = [&](Value *Vector) -> Value * {
        if (auto *Constant = dynamic_cast<ConstantVector *>(Vector))
            return Constant->getElement(Lane);
        if (Vector == LeftVector && LeftVector == RightVector &&
            !NewExtracts.empty())
            return NewExtracts.front();
        auto *LaneValue = new ExtractElementInst(
            Vector, Extract->getIndexOperand());
        NewExtracts.push_back(LaneValue);
        return LaneValue;
    };

    Value *LeftLane = CreateLane(LeftVector);
    Value *RightLane = CreateLane(RightVector);
    auto *ScalarBinary =
        new BinaryInst(LeftLane, VectorBinary->getOp(), RightLane);

    int NewCost = 0;
    bool HasFiniteCost = true;
    for (Instruction *LaneExtract : NewExtracts)
        HasFiniteCost &= addFiniteCost(
            CostInfo->getInstructionCost(LaneExtract), NewCost);
    HasFiniteCost &= addFiniteCost(
        CostInfo->getInstructionCost(ScalarBinary), NewCost);
    if (!HasFiniteCost || NewCost >= OldCost ||
        !replacementHasExpectedType(Extract, ScalarBinary)) {
        delete ScalarBinary;
        for (Instruction *LaneExtract : NewExtracts)
            delete LaneExtract;
        return nullptr;
    }

    for (Instruction *LaneExtract : NewExtracts) {
        if (!Instruction::insertBefore(Extract, LaneExtract)) {
            delete ScalarBinary;
            for (Instruction *Inserted : NewExtracts)
                delete Inserted;
            return nullptr;
        }
    }
    return ScalarBinary;
}

Value *VectorCombinePass::simplifyInstruction(Instruction *I) const {
    if (auto *Extract = dynamic_cast<ExtractElementInst *>(I))
        return foldExtract(Extract);
    if (auto *Insert = dynamic_cast<InsertElementInst *>(I))
        return foldInsert(Insert);
    if (auto *Shuffle = dynamic_cast<ShuffleVectorInst *>(I))
        return foldShuffle(Shuffle);
    if (auto *Binary = dynamic_cast<BinaryInst *>(I))
        return foldBinary(Binary);
    if (auto *Select = dynamic_cast<SelectInst *>(I))
        return foldSelect(Select);
    switch (I ? I->instType : Instruction::BinaryUnknown) {
    case Instruction::SITOFP:
    case Instruction::FPTOSI:
    case Instruction::ZEXT:
    case Instruction::SEXT:
    case Instruction::TRUNC:
    case Instruction::BITCAST:
        return foldCast(I);
    default:
        return nullptr;
    }
}

PassResult VectorCombinePass::run(
    Function &F, FunctionAnalysisPassManager &FAM) {
    CostInfo = Stage == VectorCombineStage::Late
                   ? &FAM.getResult<TargetCostAnalysis>(F)
                   : nullptr;
    bool Changed = false;
    bool ChangedThisRound = false;
    do {
        ChangedThisRound = false;
        const std::vector<Instruction *> Instructions =
            collectInstructions(F);
        for (Instruction *I : Instructions) {
            if (!I || !I->getParent())
                continue;
            Value *Replacement = simplifyInstruction(I);
            if (!Replacement || Replacement == I)
                continue;
            if (replaceInstruction(I, Replacement)) {
                Changed = true;
                ChangedThisRound = true;
            }
        }
    } while (ChangedThisRound);
    CostInfo = nullptr;
    return Changed ? PassResult::changed()
                   : PassResult::unchanged();
}
