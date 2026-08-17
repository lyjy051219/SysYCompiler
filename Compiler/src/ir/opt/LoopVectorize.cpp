#include "ir/opt/LoopVectorize.hpp"

#include "ir/analysis/LoopAccessAnalysis.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/analysis/TargetCostAnalysis.hpp"
#include "lib/IRUtils.hpp"
#include "lib/Target.hpp"

#include <algorithm>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <utility>

LoopVectorizePass::LoopVectorizePass(LoopVectorizeConfig Config)
    : Config(std::move(Config)) {}

bool LoopVectorizePass::isSupportedScalarType(
    const std::shared_ptr<IRType> &Type) {
    if (!Type || !Type->isScalar()) return false;
    if (Type->isFloat()) return true;
    auto Integer = std::dynamic_pointer_cast<INTType>(Type);
    if (!Integer) return false;
    const unsigned Width = Integer->getBitWidth();
    // i1 只用于比较掩码；SysY 的可向量化数值保持为 i32/float。
    return Width == 1 || Width == 32;
}

bool LoopVectorizePass::isSupportedMemoryElementType(
    const std::shared_ptr<IRType> &Type) {
    return Type &&
           (Type->isFloat() ||
            (Type->isInt() && Type->hasIntegerBitWidth(32)));
}

bool LoopVectorizePass::isSupportedCast(Instruction *I) {
    if (!I) return false;
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

bool LoopVectorizePass::isSupportedBodyInstruction(Instruction *I) {
    if (!I) return false;
    if (dynamic_cast<GetElementPtrInst *>(I) ||
        dynamic_cast<LoadInst *>(I) ||
        dynamic_cast<StoreInst *>(I) ||
        dynamic_cast<SelectInst *>(I)) {
        return true;
    }
    if (auto *Binary = dynamic_cast<BinaryInst *>(I))
        return Binary->instType != Instruction::BinaryUnknown;
    return isSupportedCast(I);
}

bool LoopVectorizePass::isInstructionInLoop(Value *V, Loop *L) {
    auto *I = dynamic_cast<Instruction *>(V);
    return I && I->getParent() && L && L->contains(I->getParent());
}

bool LoopVectorizePass::hasUseOutsideSet(
    Instruction *I,
    const std::unordered_set<Instruction *> &AllowedUsers) {
    if (!I) return true;
    for (Use *U : I->getUses()) {
        auto *UserI = dynamic_cast<Instruction *>(
            U ? U->getUser() : nullptr);
        if (!UserI || !AllowedUsers.count(UserI)) return true;
    }
    return false;
}

std::shared_ptr<VECTORType> LoopVectorizePass::getVectorType(
    const std::shared_ptr<IRType> &ElementType, unsigned Width) {
    if (!isSupportedScalarType(ElementType) || Width < 2) return nullptr;
    return VECTORType::NewVector(ElementType, Width);
}

bool LoopVectorizePass::validateControl(
    Loop *L, const LoopTripCountInfo &Trip,
    const InductionDescriptor &Induction,
    VectorizationShape &Shape) const {
    if (!L || !Trip.Valid || !Induction.Valid ||
        !Trip.ConstantTripCount || *Trip.ConstantTripCount <= 0 ||
        Config.VectorWidth < 2 ||
        *Trip.ConstantTripCount <
            static_cast<std::int64_t>(Config.VectorWidth) ||
        *Trip.ConstantTripCount %
                static_cast<std::int64_t>(Config.VectorWidth) !=
            0 ||
        !Trip.ProvenNoWrap ||
        Trip.ComparePhase != LoopExitComparePhase::HeaderPhi ||
        !Induction.ConstantStep || *Induction.ConstantStep != 1) {
        return false;
    }

    BasicBlock *PreHeader = nullptr;
    BasicBlock *Latch = nullptr;
    if (!L->getLoopSimplifyForm(PreHeader, Latch) ||
        !PreHeader || !Latch || Latch == L->getHeader()) {
        return false;
    }
    BasicBlock *Header = L->getHeader();
    if (!Header || Trip.PreHeader != PreHeader || Trip.Header != Header ||
        Trip.Latch != Latch || Trip.ExitingBlock != Header ||
        !Trip.ExitBlock || Trip.IndVar != Induction.Phi ||
        !Trip.ExitCmp || !Induction.Phi || !Induction.UpdateValue) {
        return false;
    }

    auto *Update = dynamic_cast<BinaryInst *>(Induction.UpdateValue);
    auto *Branch = Header->getBranchTerminator();
    auto *LatchBranch = Latch->getBranchTerminator();
    if (!Update || Update->getOp() != BinaryInst::ADD ||
        Update->getParent() != Latch ||
        Trip.ExitCmp->getParent() != Header ||
        !BasicBlock::isCondBr(Branch) ||
        Branch->getCondition() != Trip.ExitCmp ||
        Header->countEdgesToSucc(Latch) != 1 ||
        Header->countEdgesToSucc(Trip.ExitBlock) != 1 ||
        !BasicBlock::isUncondBr(LatchBranch) ||
        LatchBranch->getTrueSuccessor() != Header) {
        return false;
    }

    auto *PreBranch = PreHeader->getBranchTerminator();
    if (!BasicBlock::isUncondBr(PreBranch) ||
        PreBranch->getTrueSuccessor() != Header ||
        Induction.Phi->getParent() != Header ||
        Induction.Phi->getNumIncomingValues() != 2) {
        return false;
    }

    unsigned OutsideIncoming = 0;
    unsigned BackedgeIncoming = 0;
    for (unsigned Index = 0;
         Index < Induction.Phi->getNumIncomingValues(); ++Index) {
        BasicBlock *IncomingBlock =
            Induction.Phi->getIncomingBlock(Index);
        Value *IncomingValue = Induction.Phi->getIncomingValue(Index);
        if (IncomingBlock == PreHeader &&
            IncomingValue == Induction.StartValue) {
            ++OutsideIncoming;
        } else if (IncomingBlock == Latch &&
                   IncomingValue == Update) {
            ++BackedgeIncoming;
        } else {
            return false;
        }
    }
    if (OutsideIncoming != 1 || BackedgeIncoming != 1) return false;

    Value *CompareLeft = nullptr;
    Value *CompareRight = nullptr;
    if (!Trip.ExitCmp->getOperands(CompareLeft, CompareRight) ||
        ((CompareLeft == Induction.Phi) ==
         (CompareRight == Induction.Phi))) {
        return false;
    }
    const std::unordered_set<Instruction *> CompareUsers = {Branch};
    if (hasUseOutsideSet(Trip.ExitCmp, CompareUsers)) return false;

    const auto HeaderPhis = Header->collectPhiNodes();
    if (HeaderPhis.size() != 1 || HeaderPhis.front() != Induction.Phi)
        return false;

    // 更新值只能维持回边 PHI，防止把标量 lane 值误当成整组值。
    const std::unordered_set<Instruction *> UpdateUsers = {
        Induction.Phi};
    if (hasUseOutsideSet(Update, UpdateUsers)) return false;

    Shape.L = L;
    Shape.Header = Header;
    Shape.Body = Latch;
    Shape.PreHeader = PreHeader;
    Shape.Exit = Trip.ExitBlock;
    Shape.InductionPhi = Induction.Phi;
    Shape.InductionUpdate = Update;
    Shape.ExitCompare = Trip.ExitCmp;
    Shape.ExitBranch = Branch;
    Shape.LatchBranch = LatchBranch;
    return true;
}

bool LoopVectorizePass::validateBody(VectorizationShape &Shape,
                                     LoopAccessInfo &LA,
                                     BasicAAResult &AA) const {
    if (!Shape.L || !Shape.Header || !Shape.Body ||
        !Shape.InductionPhi ||
        !Shape.InductionUpdate || !Shape.ExitCompare ||
        !Shape.ExitBranch || !Shape.LatchBranch) {
        return false;
    }

    const LoopAccessSummary &Summary = LA.getSummary(Shape.L);
    if (Summary.HasUnknownMemory) return false;

    std::size_t BodyCount = 0;
    std::size_t MemoryCount = 0;
    std::size_t StoreCount = 0;
    // Header 必须是纯控制块，避免向量化时跨越尚未建模的条件计算。
    for (Instruction *I : *Shape.Header) {
        if (I != Shape.InductionPhi && I != Shape.ExitCompare &&
            I != Shape.ExitBranch) {
            return false;
        }
    }

    for (Instruction *I : *Shape.Body) {
        if (!I) return false;
        if (I == Shape.InductionUpdate || I == Shape.LatchBranch) {
            continue;
        }
        if (++BodyCount > Config.MaxBodyInstructions ||
            !isSupportedBodyInstruction(I)) {
            return false;
        }

        if (auto *GEP = dynamic_cast<GetElementPtrInst *>(I)) {
            // 地址节点保留为每个向量组计算一次，且只允许参与地址链和访存。
            for (Use *U : GEP->getUses()) {
                auto *UserI = dynamic_cast<Instruction *>(
                    U ? U->getUser() : nullptr);
                if (!UserI || UserI->getParent() != Shape.Body ||
                    (!dynamic_cast<GetElementPtrInst *>(UserI) &&
                     !dynamic_cast<LoadInst *>(UserI) &&
                     !dynamic_cast<StoreInst *>(UserI))) {
                    return false;
                }
            }
            for (std::size_t Index = 0;
                 Index < GEP->getNumIndices(); ++Index) {
                Value *Operand = GEP->getIndex(Index);
                if (isInstructionInLoop(Operand, Shape.L) &&
                    Operand != Shape.InductionPhi &&
                    !dynamic_cast<GetElementPtrInst *>(Operand)) {
                    return false;
                }
            }
            Value *Base = GEP->getBasePointer();
            if (isInstructionInLoop(Base, Shape.L) &&
                !dynamic_cast<GetElementPtrInst *>(Base)) {
                return false;
            }
            if (!GEP->comesBefore(Shape.InductionUpdate)) return false;
            Shape.AddressInstructions.push_back(GEP);
            continue;
        }

        if (auto *Load = dynamic_cast<LoadInst *>(I)) {
            // LLVM IR 的 i1 内存表示不能按普通字节数组直接成组读写；
            // 自动向量访存只接受 SysY 的 i32/float，i1 仅作寄存器掩码。
            if (!isSupportedMemoryElementType(Load->getIRType()))
                return false;
            ++MemoryCount;
        } else if (auto *Store = dynamic_cast<StoreInst *>(I)) {
            Value *Stored = Store->getValueOperand();
            if (!Stored ||
                !isSupportedMemoryElementType(Stored->getIRType()))
                return false;
            ++MemoryCount;
            ++StoreCount;
        } else if (!isSupportedScalarType(I->getIRType())) {
            return false;
        }
        Shape.ScalarInstructions.push_back(I);
        Shape.ScalarInstructionSet.insert(I);
    }

    if (MemoryCount == 0 || StoreCount == 0 ||
        Summary.Accesses.size() != MemoryCount) {
        return false;
    }

    // 所有访存都必须是正向单位步长；负步长不能直接用普通向量 load/store。
    for (const LoopMemoryAccess *Access : Summary.Accesses) {
        if (!Access || !Access->Inst ||
            Access->Inst->getParent() != Shape.Body ||
            !LA.isConsecutiveAccess(Shape.L, Access->Inst, Shape.L)) {
            return false;
        }
        auto Stride = LA.getByteStride(
            Access->Inst, Shape.L, Shape.L);
        if (!Stride || !Access->Location.Size ||
            *Access->Location.Size == 0 ||
            *Stride != static_cast<std::int64_t>(
                           *Access->Location.Size)) {
            return false;
        }

        std::shared_ptr<IRType> ElementType;
        if (auto *Load = dynamic_cast<LoadInst *>(Access->Inst)) {
            ElementType = Load->getIRType();
        } else if (auto *Store =
                       dynamic_cast<StoreInst *>(Access->Inst)) {
            Value *Stored = Store->getValueOperand();
            ElementType = Stored ? Stored->getIRType() : nullptr;
        }
        auto VectorType = getVectorType(
            ElementType, Config.VectorWidth);
        const LoopAccessFootprint *Footprint =
            LA.getFootprint(Shape.L, *Access);
        if (!VectorType || !Footprint || !Footprint->Exact)
            return false;
        std::size_t RequiredAlignment = 1;
        try {
            RequiredAlignment = DataLayout::forTarget().getABITypeAlign(
                VectorType.get());
        } catch (const std::overflow_error &) {
            return false;
        }
        if (!Footprint->Base || !Footprint->StartByteOffset)
            return false;
        const std::size_t BaseAlignment =
            std::max<std::size_t>(
                1, AA.getKnownAlignment(Footprint->Base));
        const std::int64_t SignedStart = *Footprint->StartByteOffset;
        const std::uint64_t StartMagnitude = SignedStart < 0
            ? static_cast<std::uint64_t>(-(SignedStart + 1)) + 1
            : static_cast<std::uint64_t>(SignedStart);
        if (*Stride <= 0 || Config.VectorWidth == 0) {
            return false;
        }
        const std::uint64_t UnsignedStride =
            static_cast<std::uint64_t>(*Stride);
        if (UnsignedStride >
            std::numeric_limits<std::uint64_t>::max() /
                Config.VectorWidth) {
            return false;
        }
        const std::uint64_t GroupStep =
            UnsignedStride * Config.VectorWidth;
        const std::uint64_t GuaranteedAlignment = std::gcd(
            std::gcd(static_cast<std::uint64_t>(BaseAlignment),
                     StartMagnitude),
            GroupStep);
        // BasicAA 给出底层对象对齐；精确首偏移和 VF 倍组步长共同证明
        // 每次向量访问都满足 ABI 对齐。测试开关绝不绕过该证明。
        if (GuaranteedAlignment < RequiredAlignment)
            return false;
    }

    // 优先消费 LoopAccess 的通用跨迭代结论。若一对不同对象的动态 GEP
    // 因 BasicAA 不利用潜在越界而保留 Unknown，只在本候选的两个完整足迹
    // 都已证明落在各自分配对象内时，局部使用底层对象互斥事实。
    auto HasCompleteInBoundsFootprint = [](const LoopAccessFootprint *FP) {
        if (!FP || !FP->Exact || !FP->Base || !FP->ObjectSize ||
            !FP->MinByteOffset || !FP->MaxByteOffsetExclusive ||
            *FP->MinByteOffset < 0 ||
            *FP->MaxByteOffsetExclusive < *FP->MinByteOffset) {
            return false;
        }
        return static_cast<std::uint64_t>(
                   *FP->MaxByteOffsetExclusive) <=
               static_cast<std::uint64_t>(*FP->ObjectSize);
    };
    for (std::size_t FirstIndex = 0;
         FirstIndex < Summary.Accesses.size(); ++FirstIndex) {
        for (std::size_t SecondIndex = FirstIndex;
             SecondIndex < Summary.Accesses.size(); ++SecondIndex) {
            const LoopMemoryAccess *First =
                Summary.Accesses[FirstIndex];
            const LoopMemoryAccess *Second =
                Summary.Accesses[SecondIndex];
            if (!First || !Second) return false;
            const bool HasWriteConflict =
                (First->Writes || Second->Writes) &&
                (First->Reads || First->Writes) &&
                (Second->Reads || Second->Writes);
            if (!HasWriteConflict) continue;
            if (LA.classifyPair(Shape.L, Shape.L, *First,
                                Shape.L, Shape.L, *Second) !=
                LoopAccessPairKind::Unknown) {
                continue;
            }

            const LoopAccessFootprint *FirstFootprint =
                LA.getFootprint(Shape.L, *First);
            const LoopAccessFootprint *SecondFootprint =
                LA.getFootprint(Shape.L, *Second);
            if (!HasCompleteInBoundsFootprint(FirstFootprint) ||
                !HasCompleteInBoundsFootprint(SecondFootprint) ||
                FirstFootprint->Base == SecondFootprint->Base ||
                !AA.haveKnownDistinctUnderlyingObjects(
                    First->Pointer, Second->Pointer)) {
                return false;
            }
        }
    }

    std::unordered_set<Instruction *> AllowedUsers =
        Shape.ScalarInstructionSet;

    for (Instruction *I : Shape.ScalarInstructions) {
        if (hasUseOutsideSet(I, AllowedUsers)) return false;
        for (std::size_t Index = 0; Index < I->getNumOperands(); ++Index) {
            Value *Operand = I->getOperand(Index);
            if (Operand == Shape.InductionPhi ||
                Operand == Shape.InductionUpdate ||
                Operand == Shape.ExitCompare) {
                return false;
            }
            auto *OperandI = dynamic_cast<Instruction *>(Operand);
            if (OperandI && OperandI->getParent() == Shape.Body &&
                !Shape.ScalarInstructionSet.count(OperandI) &&
                !dynamic_cast<GetElementPtrInst *>(OperandI)) {
                return false;
            }
        }
    }
    std::unordered_set<Instruction *> InductionUsers = {
        Shape.InductionUpdate, Shape.ExitCompare};
    for (GetElementPtrInst *GEP : Shape.AddressInstructions)
        InductionUsers.insert(GEP);
    if (hasUseOutsideSet(Shape.InductionPhi, InductionUsers))
        return false;
    return true;
}

bool LoopVectorizePass::matchShape(Loop *L, SCEVResult &SE,
                                   LoopAccessInfo &LA, BasicAAResult &AA,
                                   VectorizationShape &Shape) const {
    if (!L || !L->isInnermost() || !L->getSubLoops().empty())
        return false;
    const auto Blocks = L->getBlocksIncludingSubLoops();
    if (Blocks.size() != 2 ||
        std::find(Blocks.begin(), Blocks.end(), L->getHeader()) ==
            Blocks.end())
        return false;

    auto Trip = SE.analyzeLoopTripCount(L);
    auto Induction = SE.getCanonicalInduction(L);
    if (!Trip || !Induction ||
        !validateControl(L, *Trip, *Induction, Shape)) {
        return false;
    }
    return validateBody(Shape, LA, AA);
}

Value *LoopVectorizePass::getWidenedValue(
    Value *V, VectorizationPlan &Plan) const {
    if (!V || !V->getIRType()) return nullptr;
    auto Existing = Plan.WidenedValues.find(V);
    if (Existing != Plan.WidenedValues.end()) return Existing->second;
    auto Splat = Plan.InvariantSplats.find(V);
    if (Splat != Plan.InvariantSplats.end()) return Splat->second;

    if (isInstructionInLoop(V, Plan.Shape.L)) return nullptr;
    auto VectorType = getVectorType(V->getIRType(), Config.VectorWidth);
    if (!VectorType) return nullptr;

    if (auto *Constant = dynamic_cast<ConstantSystem *>(V)) {
        Value *Result = ConstantVector::getSplat(VectorType, Constant);
        Plan.InvariantSplats.emplace(V, Result);
        return Result;
    }

    if (Plan.DynamicSplatCount >= Config.MaxInvariantSplats)
        return nullptr;
    auto *Index = ConstInt::newConstIntForType(
        0, INTType::getInt32Ty());
    auto *Undef = UndefValue::NewUndefValue(VectorType);
    auto *Insert = new InsertElementInst(Undef, V, Index);
    Plan.NewInstructions.push_back(Insert);
    std::vector<int> Mask(Config.VectorWidth, 0);
    auto *Shuffle = new ShuffleVectorInst(Insert, Undef, Mask);
    Plan.NewInstructions.push_back(Shuffle);
    ++Plan.DynamicSplatCount;
    Plan.InvariantSplats.emplace(V, Shuffle);
    return Shuffle;
}

Value *LoopVectorizePass::createVectorLoad(
    LoadInst *Load, VectorizationPlan &Plan) const {
    if (!Load || !Load->getPointerOperand()) return nullptr;
    auto VectorType = getVectorType(
        Load->getIRType(), Config.VectorWidth);
    if (!VectorType) return nullptr;
    auto PointerType = POINTERType::NewPointer(VectorType);
    auto *PointerCast = new BitCastInst(
        Load->getPointerOperand(), PointerType);
    Plan.NewInstructions.push_back(PointerCast);
    auto *VectorLoad = new LoadInst(PointerCast);
    Plan.NewInstructions.push_back(VectorLoad);
    return VectorLoad;
}

bool LoopVectorizePass::createVectorStore(
    StoreInst *Store, VectorizationPlan &Plan) const {
    if (!Store || !Store->getPointerOperand()) return false;
    Value *VectorValue = getWidenedValue(
        Store->getValueOperand(), Plan);
    if (!VectorValue || !VectorValue->getIRType() ||
        !VectorValue->getIRType()->isVector()) {
        return false;
    }
    auto PointerType = POINTERType::NewPointer(
        VectorValue->getIRType());
    auto *PointerCast = new BitCastInst(
        Store->getPointerOperand(), PointerType);
    Plan.NewInstructions.push_back(PointerCast);
    auto *VectorStore = new StoreInst(VectorValue, PointerCast);
    Plan.NewInstructions.push_back(VectorStore);
    return true;
}

Value *LoopVectorizePass::createVectorBinary(
    BinaryInst *Binary, VectorizationPlan &Plan) const {
    if (!Binary) return nullptr;
    Value *Left = nullptr;
    Value *Right = nullptr;
    if (!Binary->getOperands(Left, Right)) return nullptr;
    Value *VectorLeft = getWidenedValue(Left, Plan);
    Value *VectorRight = getWidenedValue(Right, Plan);
    if (!VectorLeft || !VectorRight ||
        !VectorLeft->getIRType() || !VectorRight->getIRType() ||
        !VectorLeft->getIRType()->hasSameShape(
            *VectorRight->getIRType())) {
        return nullptr;
    }
    auto *Result = new BinaryInst(
        VectorLeft, Binary->getOp(), VectorRight);
    Plan.NewInstructions.push_back(Result);
    return Result;
}

Value *LoopVectorizePass::createVectorCast(
    Instruction *Cast, VectorizationPlan &Plan) const {
    if (!Cast || !isSupportedCast(Cast)) return nullptr;
    Value *Source = Instruction::getUnaryOperand(Cast);
    Value *VectorSource = getWidenedValue(Source, Plan);
    auto VectorResultType = getVectorType(
        Cast->getIRType(), Config.VectorWidth);
    if (!VectorSource || !VectorResultType) return nullptr;

    Instruction *Result = nullptr;
    switch (Cast->instType) {
    case Instruction::TRUNC:
        Result = new TruncInst(VectorSource, VectorResultType);
        break;
    case Instruction::ZEXT:
        Result = new ZextInst(VectorSource, VectorResultType);
        break;
    case Instruction::SEXT:
        Result = new SextInst(VectorSource, VectorResultType);
        break;
    case Instruction::FPTOSI:
        Result = new FPTOSIInst(VectorSource, VectorResultType);
        break;
    case Instruction::SITOFP:
        Result = new SITOFPInst(VectorSource, VectorResultType);
        break;
    case Instruction::BITCAST:
        Result = new BitCastInst(VectorSource, VectorResultType);
        break;
    default:
        return nullptr;
    }
    Plan.NewInstructions.push_back(Result);
    return Result;
}

Value *LoopVectorizePass::createVectorSelect(
    SelectInst *Select, VectorizationPlan &Plan) const {
    if (!Select) return nullptr;
    Value *Condition = getWidenedValue(
        Select->getCondition(), Plan);
    Value *TrueValue = getWidenedValue(
        Select->getTrueValue(), Plan);
    Value *FalseValue = getWidenedValue(
        Select->getFalseValue(), Plan);
    if (!Condition || !TrueValue || !FalseValue ||
        !Condition->getIRType() ||
        !Condition->getIRType()->isBooleanVector() ||
        !TrueValue->getIRType() || !FalseValue->getIRType() ||
        !TrueValue->getIRType()->hasSameShape(
            *FalseValue->getIRType())) {
        return nullptr;
    }
    auto *Result = new SelectInst(
        Condition, TrueValue, FalseValue);
    Plan.NewInstructions.push_back(Result);
    return Result;
}

bool LoopVectorizePass::buildPlan(
    const VectorizationShape &Shape,
    VectorizationPlan &Plan) const {
    Plan.Shape = Shape;
    try {
        for (Instruction *I : Shape.ScalarInstructions) {
            if (!I) {
                destroyPlan(Plan);
                return false;
            }
            Value *Widened = nullptr;
            if (auto *Load = dynamic_cast<LoadInst *>(I)) {
                Widened = createVectorLoad(Load, Plan);
            } else if (auto *Store = dynamic_cast<StoreInst *>(I)) {
                if (!createVectorStore(Store, Plan)) {
                    destroyPlan(Plan);
                    return false;
                }
            } else if (auto *Binary = dynamic_cast<BinaryInst *>(I)) {
                Widened = createVectorBinary(Binary, Plan);
            } else if (auto *Select = dynamic_cast<SelectInst *>(I)) {
                Widened = createVectorSelect(Select, Plan);
            } else if (isSupportedCast(I)) {
                Widened = createVectorCast(I, Plan);
            } else {
                destroyPlan(Plan);
                return false;
            }

            if (!dynamic_cast<StoreInst *>(I)) {
                if (!Widened) {
                    destroyPlan(Plan);
                    return false;
                }
                Plan.WidenedValues.emplace(I, Widened);
            }
            Plan.OldInstructions.push_back(I);
        }

        auto InductionType = std::dynamic_pointer_cast<INTType>(
            Shape.InductionPhi->getIRType());
        if (!InductionType) {
            destroyPlan(Plan);
            return false;
        }
        auto *Step = ConstInt::newConstIntForType(
            static_cast<std::int64_t>(Config.VectorWidth),
            InductionType);
        Plan.NewInductionUpdate = new BinaryInst(
            Shape.InductionPhi, BinaryInst::ADD, Step);
    } catch (const std::exception &) {
        destroyPlan(Plan);
        return false;
    }
    return true;
}

bool LoopVectorizePass::isProfitable(
    const VectorizationPlan &Plan,
    const TargetCostInfo &Target) const {
    if (Config.AllowUnsupportedTargetForTesting)
        return !Plan.NewInstructions.empty() &&
               !Plan.OldInstructions.empty();

    std::int64_t ScalarCost = 0;
    std::int64_t VectorCost = 0;
    std::unordered_set<const ConstantVector *> CountedConstants;
    for (Instruction *I : Plan.OldInstructions) {
        const int Cost = Target.getInstructionCost(
            I, TargetCostKind::Throughput);
        if (Cost < 0 || Cost >= TargetCostInfo::InvalidCost)
            return false;
        ScalarCost += Cost;
    }
    for (GetElementPtrInst *GEP :
         Plan.Shape.AddressInstructions) {
        const int Cost = Target.getInstructionCost(
            GEP, TargetCostKind::Throughput);
        if (Cost < 0 || Cost >= TargetCostInfo::InvalidCost)
            return false;
        ScalarCost += Cost;
        VectorCost += Cost;
    }

    for (Instruction *I : Plan.NewInstructions) {
        if (!I || !I->getIRType()) return false;
        if (I->getIRType()->isVector() &&
            !Target.isLegalVectorType(I->getIRType().get())) {
            return false;
        }
        for (std::size_t Index = 0;
             Index < I->getNumOperands(); ++Index) {
            Value *Operand = I->getOperand(Index);
            if (Operand && Operand->getIRType() &&
                Operand->getIRType()->isVector() &&
                !Target.isLegalVectorType(
                    Operand->getIRType().get())) {
                return false;
            }
            auto *Constant = dynamic_cast<ConstantVector *>(Operand);
            if (Constant && CountedConstants.insert(Constant).second) {
                const int ConstantCost = Target.getVectorConstantCost(
                    Constant, TargetCostKind::Throughput);
                if (ConstantCost < 0 ||
                    ConstantCost >= TargetCostInfo::InvalidCost)
                    return false;
                VectorCost += ConstantCost;
            }
        }
        if (auto *Store = dynamic_cast<StoreInst *>(I)) {
            Value *Stored = Store->getValueOperand();
            if (!Stored || !Stored->getIRType() ||
                !Target.isLegalVectorType(
                    Stored->getIRType().get())) {
                return false;
            }
        }
        const int Cost = Target.getInstructionCost(
            I, TargetCostKind::Throughput);
        if (Cost < 0 || Cost >= TargetCostInfo::InvalidCost)
            return false;
        VectorCost += Cost;
    }

    if (ScalarCost <= 0 ||
        ScalarCost > std::numeric_limits<std::int64_t>::max() /
                         static_cast<std::int64_t>(Config.VectorWidth)) {
        return false;
    }
    return VectorCost <
        ScalarCost * static_cast<std::int64_t>(Config.VectorWidth);
}

bool LoopVectorizePass::commitPlan(VectorizationPlan &Plan) const {
    if (!Plan.Shape.Header || !Plan.Shape.InductionUpdate ||
        !Plan.NewInductionUpdate || Plan.NewInstructions.empty() ||
        Plan.OldInstructions.empty()) {
        return false;
    }

    // 新指令统一放在旧归纳更新之前；此时所有保留的 GEP 都已经完成定义。
    std::vector<Instruction *> Attached;
    Attached.reserve(Plan.NewInstructions.size());
    for (Instruction *I : Plan.NewInstructions) {
        if (!I || I->getParent() ||
            !Instruction::insertBefore(
                Plan.Shape.InductionUpdate, I)) {
            for (Instruction *Inserted : Attached)
                if (Inserted && Inserted->getParent())
                    Inserted->eraseFromParent();
            return false;
        }
        Attached.push_back(I);
    }

    BinaryInst *OldUpdate = Plan.Shape.InductionUpdate;
    OldUpdate->replaceAllUsesWith(Plan.NewInductionUpdate);
    OldUpdate->replaceWith(Plan.NewInductionUpdate);
    delete OldUpdate;
    Plan.Shape.InductionUpdate = Plan.NewInductionUpdate;
    Plan.NewInductionUpdate = nullptr;

    // 先断开整片旧标量子图，再删除节点，避免侵入式 use 链递归销毁使用者。
    for (Instruction *I : Plan.OldInstructions)
        if (I) I->clearOperands();
    for (auto It = Plan.OldInstructions.rbegin();
         It != Plan.OldInstructions.rend(); ++It)
        delete *It;
    Plan.OldInstructions.clear();
    Plan.NewInstructions.clear();
    return true;
}

void LoopVectorizePass::destroyPlan(VectorizationPlan &Plan) {
    if (Plan.NewInductionUpdate) {
        Plan.NewInductionUpdate->clearOperands();
        delete Plan.NewInductionUpdate;
        Plan.NewInductionUpdate = nullptr;
    }
    for (Instruction *I : Plan.NewInstructions)
        if (I && !I->getParent()) I->clearOperands();
    for (auto It = Plan.NewInstructions.rbegin();
         It != Plan.NewInstructions.rend(); ++It) {
        Instruction *I = *It;
        if (I && !I->getParent()) delete I;
    }
    Plan.NewInstructions.clear();
    Plan.OldInstructions.clear();
    Plan.WidenedValues.clear();
    Plan.InvariantSplats.clear();
    Plan.DynamicSplatCount = 0;
}

PassResult LoopVectorizePass::run(
    Function &F, FunctionAnalysisPassManager &FAM) {
    if (Config.VectorWidth == 1 || Config.VectorWidth > 64 ||
        Config.MaxBodyInstructions == 0) {
        return PassResult::unchanged();
    }

    const auto &Target = FAM.getResult<TargetCostAnalysis>(F);
    // 生产路径先做全局目标门禁，避免在不含 V/SIMD 的 RV64GC 上浪费分析时间。
    if (!Config.AllowUnsupportedTargetForTesting &&
        Target.getNativeVectorBitWidth() == 0) {
        return PassResult::unchanged();
    }

    std::vector<unsigned> CandidateWidths;
    if (Config.VectorWidth == 0) {
        const unsigned NativeBits = Target.getNativeVectorBitWidth();
        // 自动模式以 SysY 的 i32/float 为基准，从首选原生宽度向较小
        // 二次幂宽度枚举。TargetCost 会过滤目标没有声明的子宽度，因此
        // SIMD128 模式只用 VF4，SIMD64And128 可在 VF4 失败后回退 VF2。
        // 测试目标必须显式给出 VF，避免用测试开关虚构硬件位宽。
        if (Config.AllowUnsupportedTargetForTesting ||
            NativeBits < 64 || NativeBits % 32 != 0) {
            return PassResult::unchanged();
        }
        unsigned Width = NativeBits / 32;
        if (Width < 2 || Width > 64) {
            return PassResult::unchanged();
        }
        for (; Width >= 2; Width /= 2) {
            CandidateWidths.push_back(Width);
            if (Width == 2)
                break;
        }
    } else {
        CandidateWidths.push_back(Config.VectorWidth);
    }

    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &SE = FAM.getResult<SCEVAnalysis>(F);
    auto &LA = FAM.getResult<LoopAccessAnalysis>(F);
    auto &AA = FAM.getResult<BasicAliasAnalysis>(F);
    for (Loop *L : LI.getLoopsInPostOrder()) {
        for (unsigned Width : CandidateWidths) {
            LoopVectorizeConfig EffectiveConfig = Config;
            EffectiveConfig.VectorWidth = Width;
            LoopVectorizePass EffectivePass(EffectiveConfig);

            VectorizationShape Shape;
            if (!EffectivePass.matchShape(L, SE, LA, AA, Shape))
                continue;
            VectorizationPlan Plan;
            if (!EffectivePass.buildPlan(Shape, Plan))
                continue;
            if (!EffectivePass.isProfitable(Plan, Target)) {
                EffectivePass.destroyPlan(Plan);
                continue;
            }
            if (!EffectivePass.commitPlan(Plan)) {
                // commit 前完成全部结构预检，正常路径不会在挂接中途失败。
                // 若底层链表状态异常，保守销毁仍游离的节点并停止本轮。
                EffectivePass.destroyPlan(Plan);
                return PassResult::unchanged();
            }
            return PassResult::changed(PreservationStatus::none());
        }
    }
    return PassResult::unchanged();
}
