#include "ir/opt/SLPVectorize.hpp"

#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/analysis/TargetCostAnalysis.hpp"
#include "lib/IRUtils.hpp"
#include "lib/Target.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

SLPVectorizePass::SLPVectorizePass(bool ForceForTesting,
                                   unsigned ForcedVectorBitWidth)
    : ForceForTesting(ForceForTesting),
      ForcedVectorBitWidth(ForcedVectorBitWidth) {}

bool SLPVectorizePass::isVectorizableScalarType(
    const std::shared_ptr<IRType> &Type) {
    // 当前 SysY 标量计算只把 i32 和 float 作为自动向量化元素；i1 的
    // 内存布局与逐字节标量访问不同，指针和其他位宽也不在此处猜测合法化。
    return Type &&
           (Type->isFloat() ||
            (Type->isInt() && Type->hasIntegerBitWidth(32)));
}

bool SLPVectorizePass::isSupportedBinaryOperation(
    BinaryInst::Operation Op) {
    switch (Op) {
    case BinaryInst::ADD:
    case BinaryInst::SUB:
    case BinaryInst::MUL:
    case BinaryInst::AND:
    case BinaryInst::OR:
    case BinaryInst::XOR:
    case BinaryInst::SHL:
    case BinaryInst::LSHR:
    case BinaryInst::ASHR:
        return true;
    default:
        return false;
    }
}

bool SLPVectorizePass::isSupportedCastOpcode(
    Instruction::InstType Opcode) {
    switch (Opcode) {
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

bool SLPVectorizePass::haveSameScalarType(
    const std::vector<Value *> &Values) {
    if (Values.empty() || !Values.front() ||
        !isVectorizableScalarType(Values.front()->getIRType()))
        return false;
    const auto Type = Values.front()->getIRType();
    for (Value *V : Values)
        if (!V || !V->getIRType() ||
            !IRType::hasSameShape(Type, V->getIRType()))
            return false;
    return true;
}

bool SLPVectorizePass::haveDistinctValues(
    const std::vector<Value *> &Values) {
    std::unordered_set<Value *> Seen;
    for (Value *V : Values)
        if (!V || !Seen.insert(V).second)
            return false;
    return true;
}

bool SLPVectorizePass::addFiniteCost(int Cost, int &Total) {
    if (Cost < 0 || Cost >= TargetCostInfo::InvalidCost ||
        Total > TargetCostInfo::InvalidCost - Cost)
        return false;
    Total += Cost;
    return true;
}

bool SLPVectorizePass::isLegalVectorType(
    const std::shared_ptr<VECTORType> &Type) const {
    return Type && (ForceForTesting ||
                    (CostInfo && CostInfo->isLegalVectorType(Type.get())));
}

std::vector<std::size_t> SLPVectorizePass::getCandidateFactors(
    const std::shared_ptr<IRType> &ScalarType,
    std::size_t AvailableStores) const {
    std::vector<std::size_t> Factors;
    if (!isVectorizableScalarType(ScalarType) || AvailableStores < 2 ||
        !CostInfo)
        return Factors;

    std::size_t ScalarBits = 0;
    if (!ScalarType->getLogicalBitWidth(ScalarBits) || ScalarBits == 0)
        return Factors;
    const unsigned RegisterBits =
        ForceForTesting ? ForcedVectorBitWidth
                        : CostInfo->getNativeVectorBitWidth();
    if (RegisterBits < ScalarBits * 2U)
        return Factors;

    std::size_t Limit = std::min<std::size_t>(
        {AvailableStores, MaxVectorLanes,
         static_cast<std::size_t>(RegisterBits) / ScalarBits});
    std::size_t Factor = 1;
    while (Factor <= Limit / 2) Factor *= 2;
    for (; Factor >= 2; Factor /= 2) {
        try {
            auto VectorType = VECTORType::NewVector(ScalarType, Factor);
            if (isLegalVectorType(VectorType)) Factors.push_back(Factor);
        } catch (const std::invalid_argument &) {
            // 畸形或尚未支持的元素类型不是编译错误，只是不形成候选。
        } catch (const std::overflow_error &) {
            // 类型宽度溢出时保持原标量 IR。
        }
        if (Factor == 2) break;
    }
    return Factors;
}

bool SLPVectorizePass::isValueAvailableAtRoot(
    Value *V, StoreInst *Root) const {
    if (!V || !Root || !Root->getParent()) return false;
    auto *I = dynamic_cast<Instruction *>(V);
    if (!I) return true;
    return I->getParent() == Root->getParent() &&
           Instruction::comesBefore(I, Root);
}

bool SLPVectorizePass::valueDependsOn(
    Value *V, Value *Needle, std::unordered_set<Value *> &Visited,
    unsigned Depth) const {
    if (!V || !Needle || Depth > MaxTreeDepth * 2) return false;
    if (V == Needle) return true;
    auto *I = dynamic_cast<Instruction *>(V);
    if (!I || !Visited.insert(V).second) return false;
    for (std::size_t Index = 0; Index < I->getNumOperands(); ++Index)
        if (valueDependsOn(I->getOperand(Index), Needle, Visited,
                           Depth + 1))
            return true;
    return false;
}

bool SLPVectorizePass::hasCrossLaneDependence(
    const std::vector<Instruction *> &Instructions) const {
    for (std::size_t I = 0; I < Instructions.size(); ++I) {
        for (std::size_t J = 0; J < Instructions.size(); ++J) {
            if (I == J) continue;
            std::unordered_set<Value *> Visited;
            if (valueDependsOn(Instructions[I], Instructions[J],
                               Visited, 0))
                return true;
        }
    }
    return false;
}

SLPVectorizeNode *SLPVectorizePass::findExistingNode(
    const std::vector<Value *> &Scalars,
    SLPVectorizePlan &Plan) const {
    for (const auto &Node : Plan.Nodes)
        if (Node && Node->Scalars == Scalars) return Node.get();
    return nullptr;
}

bool SLPVectorizePass::analyzeConsecutivePointers(
    const std::vector<Value *> &Pointers,
    const std::shared_ptr<VECTORType> &VectorType,
    Instruction *Context, SLPAddressPack &Address) const {
    Address = SLPAddressPack{};
    if (!AA || !DT || !LI || !RI || !SE || !Context || !VectorType ||
        Pointers.size() < 2 ||
        Pointers.size() != VectorType->getElementCount())
        return false;

    const auto ElementType = VectorType->getElementType();
    const DataLayout Layout = DataLayout::forTarget();
    const std::size_t ElementBytes =
        Layout.getTypeStoreSize(ElementType);
    if (ElementBytes == 0 ||
        Layout.getTypeStoreSize(VectorType) !=
            ElementBytes * Pointers.size())
        return false;

    Value *FirstPointer = Pointers.front();
    Value *BaseObject = AA->getUniqueUnderlyingObject(FirstPointer);
    const auto FirstOffset = AA->getConstantByteOffset(FirstPointer);
    const auto ObjectSize = AA->getObjectSize(FirstPointer);
    if (!FirstPointer || !FirstPointer->isPointerValue() || !BaseObject ||
        !ObjectSize)
        return false;
    auto FirstPointee = Value::getPointeeType(FirstPointer);
    if (!FirstPointee ||
        !IRType::hasSameShape(FirstPointee, ElementType))
        return false;

    for (std::size_t Lane = 0; Lane < Pointers.size(); ++Lane) {
        Value *Pointer = Pointers[Lane];
        if (!Pointer || !Pointer->isPointerValue() ||
            AA->getUniqueUnderlyingObject(Pointer) != BaseObject)
            return false;
        auto Pointee = Value::getPointeeType(Pointer);
        if (!Pointee || !IRType::hasSameShape(Pointee, ElementType) ||
            !Pointer)
            return false;
        if (Lane != 0 &&
            ElementBytes >
                static_cast<std::size_t>(
                    std::numeric_limits<long long>::max()) /
                    Lane)
            return false;
        const long long Delta = static_cast<long long>(
            ElementBytes * Lane);
        auto RelativeOffset = AA->getConstantByteOffsetDifferenceAt(
            Pointer, FirstPointer, Context, *SE, *RI, *DT);
        if (!RelativeOffset || *RelativeOffset != Delta)
            return false;
    }

    const std::size_t VectorBytes =
        Layout.getTypeStoreSize(VectorType);
    if (VectorBytes == 0 || VectorBytes > *ObjectSize) return false;
    if (FirstOffset) {
        if (*FirstOffset < 0 ||
            static_cast<unsigned long long>(*FirstOffset) >
                static_cast<unsigned long long>(
                    *ObjectSize - VectorBytes)) {
            return false;
        }
    } else if (!AA->isLocationProvablyInBoundsAt(
                   MemoryLocation(FirstPointer, VectorBytes), Context,
                   *SE, *RI, *DT)) {
        //动态共同项必须能在新向量访存的真实插入点证明完整区间在对象内。
        return false;
    }

    //常量差之外仍由BasicAA在真实程序点逐对证明NoAlias；该查询只有在
    //共同动态项不回绕且两侧访问都位于同一已知对象内时才会增强结论。
    for (std::size_t I = 0; I < Pointers.size(); ++I)
        for (std::size_t J = I + 1; J < Pointers.size(); ++J)
            if (AA->aliasAt(
                    MemoryLocation(Pointers[I], ElementBytes),
                    MemoryLocation(Pointers[J], ElementBytes), Context,
                    *LI, *SE, *RI, *DT) != AliasInfo::NoAlias)
                return false;

    const std::size_t RequiredAlignment =
        Layout.getABITypeAlign(VectorType);
    const std::size_t KnownAlignment =
        AA->getKnownAlignment(FirstPointer);
    if (KnownAlignment < RequiredAlignment) return false;

    Address.BaseObject = BaseObject;
    Address.FirstPointer = FirstPointer;
    Address.FirstByteOffset = FirstOffset;
    Address.ElementBytes = ElementBytes;
    Address.KnownAlignment = KnownAlignment;
    return true;
}

SLPVectorizeNode *SLPVectorizePass::buildPack(
    const std::vector<Value *> &Scalars, SLPVectorizePlan &Plan,
    unsigned Depth) {
    if (!Plan.Block || Plan.Stores.empty() || !haveSameScalarType(Scalars) ||
        Scalars.size() != Plan.Stores.size())
        return nullptr;
    for (Value *V : Scalars)
        if (!isValueAvailableAtRoot(V, Plan.Stores.back())) return nullptr;
    if (SLPVectorizeNode *Existing = findExistingNode(Scalars, Plan))
        return Existing;

    const auto VectorType =
        VECTORType::NewVector(Scalars.front()->getIRType(), Scalars.size());
    if (!isLegalVectorType(VectorType)) return nullptr;

    auto AddNode = [&](SLPNodeKind Kind) {
        auto Node = std::make_unique<SLPVectorizeNode>();
        Node->Kind = Kind;
        Node->Scalars = Scalars;
        Node->VectorType = VectorType;
        SLPVectorizeNode *Result = Node.get();
        Plan.Nodes.push_back(std::move(Node));
        return Result;
    };
    auto AddGather = [&]() {
        Plan.HasGather = true;
        return AddNode(SLPNodeKind::Gather);
    };

    if (Depth >= MaxTreeDepth) return AddGather();

    bool AllConcreteConstants = true;
    for (Value *V : Scalars)
        AllConcreteConstants &=
            dynamic_cast<ConstantSystem *>(V) != nullptr &&
            !Value::isUndef(V);
    if (AllConcreteConstants) return AddNode(SLPNodeKind::Constant);

    bool CanReuseVector = haveDistinctValues(Scalars);
    Value *ReusedVector = nullptr;
    for (std::size_t Lane = 0; CanReuseVector && Lane < Scalars.size();
         ++Lane) {
        auto *Extract = dynamic_cast<ExtractElementInst *>(Scalars[Lane]);
        int64_t Index = -1;
        if (!Extract || !Extract->getIndexOperand() ||
            !Extract->getIndexOperand()->tryGetInt64Const(Index) ||
            Index != static_cast<int64_t>(Lane)) {
            CanReuseVector = false;
            break;
        }
        Value *Source = Extract->getVectorOperand();
        auto SourceType = Source ? IRType::asVector(Source->getIRType())
                                 : nullptr;
        if (!SourceType || SourceType->getElementCount() != Scalars.size() ||
            !IRType::hasSameShape(SourceType, VectorType) ||
            (ReusedVector && ReusedVector != Source)) {
            CanReuseVector = false;
            break;
        }
        ReusedVector = Source;
    }
    if (CanReuseVector && ReusedVector &&
        isValueAvailableAtRoot(ReusedVector, Plan.Stores.back())) {
        auto *Node = AddNode(SLPNodeKind::ReuseVector);
        Node->ReusedVector = ReusedVector;
        for (Value *V : Scalars)
            Plan.ScalarInstructions.insert(
                dynamic_cast<Instruction *>(V));
        return Node;
    }

    bool IsLoadPack = haveDistinctValues(Scalars);
    std::vector<Value *> LoadPointers;
    std::vector<Instruction *> LoadInstructions;
    for (Value *V : Scalars) {
        auto *Load = dynamic_cast<LoadInst *>(V);
        if (!Load || Load->getParent() != Plan.Block) {
            IsLoadPack = false;
            break;
        }
        LoadPointers.push_back(Load->getPointerOperand());
        LoadInstructions.push_back(Load);
    }
    SLPAddressPack LoadAddress;
    if (IsLoadPack && analyzeConsecutivePointers(
                          LoadPointers, VectorType,
                          Plan.Stores.back(), LoadAddress)) {
        auto *Node = AddNode(SLPNodeKind::Load);
        Node->Address = LoadAddress;
        for (Instruction *I : LoadInstructions) {
            Plan.ScalarInstructions.insert(I);
            Plan.ScalarLoads.push_back(dynamic_cast<LoadInst *>(I));
        }
        return Node;
    }

    bool IsBinaryPack = haveDistinctValues(Scalars);
    BinaryInst::Operation CommonOperation = BinaryInst::ADD;
    Instruction::InstType CommonOpcode = Instruction::BinaryUnknown;
    std::vector<Value *> LeftValues;
    std::vector<Value *> RightValues;
    std::vector<Instruction *> BinaryInstructions;
    for (std::size_t Lane = 0; Lane < Scalars.size(); ++Lane) {
        auto *Binary = dynamic_cast<BinaryInst *>(Scalars[Lane]);
        Value *Left = nullptr;
        Value *Right = nullptr;
        if (!Binary || Binary->getParent() != Plan.Block ||
            !Binary->getOperands(Left, Right) || !Left || !Right ||
            !isSupportedBinaryOperation(Binary->getOp())) {
            IsBinaryPack = false;
            break;
        }
        if (Lane == 0) {
            CommonOperation = Binary->getOp();
            CommonOpcode = Binary->instType;
        } else if (Binary->getOp() != CommonOperation ||
                   Binary->instType != CommonOpcode) {
            IsBinaryPack = false;
            break;
        }
        LeftValues.push_back(Left);
        RightValues.push_back(Right);
        BinaryInstructions.push_back(Binary);
    }
    IsBinaryPack &= haveSameScalarType(LeftValues) &&
                    haveSameScalarType(RightValues) &&
                    !hasCrossLaneDependence(BinaryInstructions);
    if (IsBinaryPack) {
        SLPVectorizeNode *Left = buildPack(LeftValues, Plan, Depth + 1);
        SLPVectorizeNode *Right = buildPack(RightValues, Plan, Depth + 1);
        if (!Left || !Right) return nullptr;
        auto *Node = AddNode(SLPNodeKind::Binary);
        Node->BinaryOperation = CommonOperation;
        Node->Opcode = CommonOpcode;
        Node->Operands = {Left, Right};
        for (Instruction *I : BinaryInstructions)
            Plan.ScalarInstructions.insert(I);
        return Node;
    }

    bool IsCastPack = haveDistinctValues(Scalars);
    Instruction::InstType CommonCast = Instruction::BinaryUnknown;
    std::vector<Value *> CastOperands;
    std::vector<Instruction *> CastInstructions;
    for (std::size_t Lane = 0; Lane < Scalars.size(); ++Lane) {
        auto *Cast = dynamic_cast<Instruction *>(Scalars[Lane]);
        Value *Operand = Instruction::getUnaryOperand(Cast);
        if (!Cast || Cast->getParent() != Plan.Block || !Operand ||
            !isSupportedCastOpcode(Cast->instType)) {
            IsCastPack = false;
            break;
        }
        if (Lane == 0)
            CommonCast = Cast->instType;
        else if (Cast->instType != CommonCast) {
            IsCastPack = false;
            break;
        }
        CastOperands.push_back(Operand);
        CastInstructions.push_back(Cast);
    }
    IsCastPack &= haveSameScalarType(CastOperands) &&
                  !hasCrossLaneDependence(CastInstructions);
    if (IsCastPack) {
        SLPVectorizeNode *Operand =
            buildPack(CastOperands, Plan, Depth + 1);
        if (!Operand) return nullptr;
        auto *Node = AddNode(SLPNodeKind::Cast);
        Node->Opcode = CommonCast;
        Node->Operands = {Operand};
        for (Instruction *I : CastInstructions)
            Plan.ScalarInstructions.insert(I);
        return Node;
    }

    return AddGather();
}

bool SLPVectorizePass::validatePlanUses(
    const SLPVectorizePlan &Plan) const {
    std::unordered_set<Instruction *> StoreRoots;
    for (StoreInst *Store : Plan.Stores)
        StoreRoots.insert(Store);

    for (Instruction *I : Plan.ScalarInstructions) {
        if (!I || I->getParent() != Plan.Block) return false;
        for (Use *U : I->getUses()) {
            auto *UserInstruction =
                U ? dynamic_cast<Instruction *>(U->getUser()) : nullptr;
            if (!UserInstruction ||
                (!Plan.ScalarInstructions.count(UserInstruction) &&
                 !StoreRoots.count(UserInstruction)))
                return false;
        }
    }
    return true;
}

bool SLPVectorizePass::validatePlanSchedule(
    const SLPVectorizePlan &Plan) const {
    if (!Plan.Block || Plan.Stores.size() < 2 ||
        !Plan.Stores.front() || !Plan.Stores.back() || !AA || !LI)
        return false;
    for (std::size_t Lane = 0; Lane < Plan.Stores.size(); ++Lane) {
        StoreInst *Store = Plan.Stores[Lane];
        if (!Store || Store->getParent() != Plan.Block) return false;
        if (Lane != 0 &&
            !Instruction::comesBefore(Plan.Stores[Lane - 1], Store))
            return false;
    }

    // 所有 lane 的标量值都在最后一个 store 之前可用，向量指令与合并后的
    // store 统一提交到这里。这样无需把后续 lane 的计算越过第一条 store
    // 向前搬动，典型的“计算 lane0/store0；计算 lane1/store1”也可打包。
    StoreInst *Root = Plan.Stores.back();
    for (Instruction *I : Plan.ScalarInstructions)
        if (!Instruction::comesBefore(I, Root)) return false;

    auto Instructions = snapshotPointers(*Plan.Block);
    std::unordered_map<Instruction *, std::size_t> Positions;
    for (std::size_t Index = 0; Index < Instructions.size(); ++Index)
        Positions.emplace(Instructions[Index], Index);
    auto RootIt = Positions.find(Root);
    if (RootIt == Positions.end()) return false;

    std::unordered_set<StoreInst *> PackedStores(
        Plan.Stores.begin(), Plan.Stores.end());
    std::unordered_set<LoadInst *> PackedLoads(
        Plan.ScalarLoads.begin(), Plan.ScalarLoads.end());

    auto FirstStoreIt = Positions.find(Plan.Stores.front());
    if (FirstStoreIt == Positions.end()) return false;
    std::size_t EarliestMoved = FirstStoreIt->second;
    for (Instruction *I : Plan.ScalarInstructions) {
        auto It = Positions.find(I);
        if (It == Positions.end() || It->second >= RootIt->second)
            return false;
        EarliestMoved = std::min(EarliestMoved, It->second);
    }
    for (LoadInst *Load : Plan.ScalarLoads) {
        auto It = Positions.find(Load);
        if (It == Positions.end() || It->second >= RootIt->second)
            return false;
        EarliestMoved = std::min(EarliestMoved, It->second);
    }

    auto MayAlias = [&](const LoadInst *Load,
                        const StoreInst *Store) {
        return !Load || !Store ||
            AA->alias(MemoryLocation::get(Load),
                      MemoryLocation::get(Store), *LI) !=
                AliasInfo::NoAlias;
    };
    auto StoresMayAlias = [&](const StoreInst *First,
                              const StoreInst *Second) {
        return !First || !Second ||
            AA->alias(MemoryLocation::get(First),
                      MemoryLocation::get(Second), *LI) !=
                AliasInfo::NoAlias;
    };

    // 提交时，打包 load 被推迟到 Root，较早的标量 store 也被推迟并合并
    // 到 Root。逐条检查被跨越的内存事件：读读可交换；其他情况只有 AA
    // 明确发布 NoAlias 才允许。Call 可能读写内存或具有可观察顺序，始终是
    // 调度屏障。ForceForTesting 只绕过收益判断，不绕过这里的语义门禁。
    for (std::size_t Index = EarliestMoved; Index < RootIt->second;
         ++Index) {
        Instruction *I = Instructions[Index];
        if (dynamic_cast<CallInst *>(I)) return false;

        if (auto *Load = dynamic_cast<LoadInst *>(I)) {
            if (PackedLoads.count(Load)) continue;
            for (StoreInst *Store : Plan.Stores) {
                auto StoreIt = Positions.find(Store);
                if (StoreIt == Positions.end()) return false;
                if (StoreIt->second < Index && MayAlias(Load, Store))
                    return false;
            }
            continue;
        }

        auto *Store = dynamic_cast<StoreInst *>(I);
        if (!Store) continue;
        if (PackedStores.count(Store)) {
            for (LoadInst *Load : Plan.ScalarLoads) {
                auto LoadIt = Positions.find(Load);
                if (LoadIt == Positions.end()) return false;
                if (Index < LoadIt->second && MayAlias(Load, Store))
                    return false;
            }
            continue;
        }

        for (LoadInst *Load : Plan.ScalarLoads) {
            auto LoadIt = Positions.find(Load);
            if (LoadIt == Positions.end()) return false;
            if (LoadIt->second < Index && MayAlias(Load, Store))
                return false;
        }
        for (StoreInst *PackedStore : Plan.Stores) {
            auto StoreIt = Positions.find(PackedStore);
            if (StoreIt == Positions.end()) return false;
            if (StoreIt->second < Index &&
                StoresMayAlias(PackedStore, Store))
                return false;
        }
    }
    return true;
}

bool SLPVectorizePass::computePlanCost(
    SLPVectorizePlan &Plan) const {
    if (!CostInfo || !Plan.StoreVectorType) return false;
    int ScalarCost = 0;
    int VectorCost = 0;
    for (Instruction *I : Plan.ScalarInstructions)
        if (!addFiniteCost(CostInfo->getInstructionCost(I), ScalarCost))
            return false;
    for (StoreInst *Store : Plan.Stores)
        if (!addFiniteCost(CostInfo->getInstructionCost(Store),
                           ScalarCost))
            return false;

    for (const auto &Node : Plan.Nodes) {
        if (!Node) return false;
        int Cost = 0;
        switch (Node->Kind) {
        case SLPNodeKind::Constant:
            // 当前 TargetCost 尚未发布向量常量物化成本，不能把其武断记为零。
            return false;
        case SLPNodeKind::ReuseVector:
            Cost = 0;
            break;
        case SLPNodeKind::Gather:
            return false;
        case SLPNodeKind::Load:
            Cost = CostInfo->getMemoryCost(
                TargetMemoryOperation::Load, Node->VectorType.get(),
                Node->Address.KnownAlignment);
            break;
        case SLPNodeKind::Binary:
        case SLPNodeKind::Cast:
            Cost = CostInfo->getArithmeticCost(
                Node->Opcode, Node->VectorType.get());
            break;
        }
        if (!addFiniteCost(Cost, VectorCost)) return false;
    }
    if (!addFiniteCost(
            CostInfo->getMemoryCost(
                TargetMemoryOperation::Store,
                Plan.StoreVectorType.get(),
                Plan.StoreAddress.KnownAlignment),
            VectorCost))
        return false;

    Plan.ScalarCost = ScalarCost;
    Plan.VectorCost = VectorCost;
    return VectorCost < ScalarCost;
}

bool SLPVectorizePass::tryBuildPlan(
    const std::vector<StoreInst *> &Stores,
    SLPVectorizePlan &Plan) {
    Plan = SLPVectorizePlan{};
    if (Stores.size() < 2 || Stores.size() > MaxVectorLanes ||
        !Stores.front() || !Stores.front()->getParent())
        return false;
    Plan.Block = Stores.front()->getParent();
    Plan.Stores = Stores;

    std::vector<Value *> StoredValues;
    std::vector<Value *> StorePointers;
    for (std::size_t Lane = 0; Lane < Stores.size(); ++Lane) {
        StoreInst *Store = Stores[Lane];
        if (!Store || Store->getParent() != Plan.Block ||
            !Store->fullyCoversItsPointer() ||
            (Lane != 0 &&
             !Instruction::comesBefore(Stores[Lane - 1], Store)))
            return false;
        StoredValues.push_back(Store->getValueOperand());
        StorePointers.push_back(Store->getPointerOperand());
    }
    if (!haveSameScalarType(StoredValues)) return false;

    Plan.StoreVectorType = VECTORType::NewVector(
        StoredValues.front()->getIRType(), Stores.size());
    if (!isLegalVectorType(Plan.StoreVectorType) ||
        !analyzeConsecutivePointers(StorePointers, Plan.StoreVectorType,
                                    Plan.Stores.back(),
                                    Plan.StoreAddress))
        return false;

    Plan.StoredValues = buildPack(StoredValues, Plan, 0);
    if (!Plan.StoredValues ||
        !IRType::hasSameShape(Plan.StoredValues->VectorType,
                              Plan.StoreVectorType))
        return false;
    if (!ForceForTesting && Plan.HasGather) return false;
    if (!validatePlanUses(Plan) || !validatePlanSchedule(Plan))
        return false;
    if (ForceForTesting) {
        (void)computePlanCost(Plan);
        return true;
    }
    return computePlanCost(Plan);
}

bool SLPVectorizePass::insertBeforeRoot(
    SLPVectorizePlan &Plan,
    Instruction *InstructionToInsert) const {
    if (!InstructionToInsert || Plan.Stores.empty() ||
        !Plan.Stores.back() ||
        !Instruction::insertBefore(Plan.Stores.back(),
                                   InstructionToInsert)) {
        delete InstructionToInsert;
        return false;
    }
    Plan.EmittedInstructions.push_back(InstructionToInsert);
    return true;
}

void SLPVectorizePass::rollbackEmittedInstructions(
    SLPVectorizePlan &Plan) const {
    // 新指令只使用旧值或更早插入的新值，逆序删除可先清理用户再清理定义。
    for (auto It = Plan.EmittedInstructions.rbegin();
         It != Plan.EmittedInstructions.rend(); ++It) {
        Instruction *I = *It;
        if (I && I->getParent() == Plan.Block) delete I;
    }
    Plan.EmittedInstructions.clear();
}

Value *SLPVectorizePass::emitGather(
    SLPVectorizeNode *Node, SLPVectorizePlan &Plan) const {
    if (!Node || !Node->VectorType ||
        Node->Scalars.size() != Node->VectorType->getElementCount())
        return nullptr;
    Value *Current = UndefValue::NewUndefValue(Node->VectorType);
    for (std::size_t Lane = 0; Lane < Node->Scalars.size(); ++Lane) {
        auto *Insert = new InsertElementInst(
            Current, Node->Scalars[Lane],
            ConstInt::newConstInt(static_cast<int64_t>(Lane)));
        if (!insertBeforeRoot(Plan, Insert)) return nullptr;
        Current = Insert;
    }
    return Current;
}

Instruction *SLPVectorizePass::createVectorCast(
    SLPVectorizeNode *Node, Value *Operand) const {
    if (!Node || !Operand || !Node->VectorType) return nullptr;
    switch (Node->Opcode) {
    case Instruction::TRUNC:
        return new TruncInst(Operand, Node->VectorType);
    case Instruction::ZEXT:
        return new ZextInst(Operand, Node->VectorType);
    case Instruction::SEXT:
        return new SextInst(Operand, Node->VectorType);
    case Instruction::FPTOSI:
        return new FPTOSIInst(Operand, Node->VectorType);
    case Instruction::SITOFP:
        return new SITOFPInst(Operand, Node->VectorType);
    case Instruction::BITCAST:
        return new BitCastInst(Operand, Node->VectorType);
    default:
        return nullptr;
    }
}

Value *SLPVectorizePass::emitNode(
    SLPVectorizeNode *Node, SLPVectorizePlan &Plan) const {
    if (!Node) return nullptr;
    if (Node->EmittedValue) return Node->EmittedValue;

    if (Node->Kind == SLPNodeKind::Constant) {
        std::vector<ConstantSystem *> Elements;
        Elements.reserve(Node->Scalars.size());
        for (Value *V : Node->Scalars) {
            auto *Constant = dynamic_cast<ConstantSystem *>(V);
            if (!Constant || Value::isUndef(Constant)) return nullptr;
            Elements.push_back(Constant);
        }
        Node->EmittedValue =
            ConstantVector::get(Node->VectorType, Elements);
        return Node->EmittedValue;
    }
    if (Node->Kind == SLPNodeKind::ReuseVector) {
        Node->EmittedValue = Node->ReusedVector;
        return Node->EmittedValue;
    }
    if (Node->Kind == SLPNodeKind::Gather) {
        Node->EmittedValue = emitGather(Node, Plan);
        return Node->EmittedValue;
    }

    Instruction *NewInstruction = nullptr;
    if (Node->Kind == SLPNodeKind::Load) {
        auto VectorPointerType =
            POINTERType::NewPointer(Node->VectorType);
        auto *PointerCast = new BitCastInst(
            Node->Address.FirstPointer, VectorPointerType);
        if (!insertBeforeRoot(Plan, PointerCast)) return nullptr;
        NewInstruction = new LoadInst(PointerCast);
    } else if (Node->Kind == SLPNodeKind::Binary) {
        if (Node->Operands.size() != 2) return nullptr;
        Value *Left = emitNode(Node->Operands[0], Plan);
        Value *Right = emitNode(Node->Operands[1], Plan);
        if (!Left || !Right) return nullptr;
        NewInstruction =
            new BinaryInst(Left, Node->BinaryOperation, Right);
    } else if (Node->Kind == SLPNodeKind::Cast) {
        if (Node->Operands.size() != 1) return nullptr;
        Value *Operand = emitNode(Node->Operands[0], Plan);
        if (!Operand) return nullptr;
        NewInstruction = createVectorCast(Node, Operand);
    }

    if (!NewInstruction || !NewInstruction->getIRType() ||
        !IRType::hasSameShape(NewInstruction->getIRType(),
                              Node->VectorType)) {
        delete NewInstruction;
        return nullptr;
    }
    if (!insertBeforeRoot(Plan, NewInstruction)) return nullptr;
    Node->EmittedValue = NewInstruction;
    return Node->EmittedValue;
}

void SLPVectorizePass::eraseScalarPlan(
    SLPVectorizePlan &Plan) const {
    for (auto It = Plan.Stores.rbegin(); It != Plan.Stores.rend(); ++It)
        if (*It && (*It)->getParent() == Plan.Block) delete *It;

    auto Instructions = snapshotPointers(*Plan.Block);
    for (auto It = Instructions.rbegin(); It != Instructions.rend(); ++It) {
        Instruction *I = *It;
        if (I && Plan.ScalarInstructions.count(I) &&
            I->getParent() == Plan.Block && !I->hasUses())
            delete I;
    }
}

bool SLPVectorizePass::applyPlan(SLPVectorizePlan &Plan) const {
    if (!Plan.StoredValues || Plan.Stores.empty() ||
        !Plan.StoreAddress.FirstPointer)
        return false;
    try {
        Value *VectorValue = emitNode(Plan.StoredValues, Plan);
        if (!VectorValue || !VectorValue->getIRType() ||
            !IRType::hasSameShape(VectorValue->getIRType(),
                                  Plan.StoreVectorType)) {
            rollbackEmittedInstructions(Plan);
            return false;
        }

        auto VectorPointerType =
            POINTERType::NewPointer(Plan.StoreVectorType);
        auto *PointerCast = new BitCastInst(
            Plan.StoreAddress.FirstPointer, VectorPointerType);
        if (!insertBeforeRoot(Plan, PointerCast)) {
            rollbackEmittedInstructions(Plan);
            return false;
        }
        auto *VectorStore = new StoreInst(VectorValue, PointerCast);
        if (!insertBeforeRoot(Plan, VectorStore)) {
            rollbackEmittedInstructions(Plan);
            return false;
        }
    } catch (const std::invalid_argument &) {
        rollbackEmittedInstructions(Plan);
        return false;
    } catch (const std::overflow_error &) {
        rollbackEmittedInstructions(Plan);
        return false;
    }

    eraseScalarPlan(Plan);
    Plan.EmittedInstructions.clear();
    return true;
}

bool SLPVectorizePass::tryVectorizeBlock(BasicBlock *Block) {
    if (!Block) return false;
    auto Instructions = snapshotPointers(*Block);
    for (std::size_t Begin = 0; Begin < Instructions.size(); ++Begin) {
        auto *FirstStore = dynamic_cast<StoreInst *>(Instructions[Begin]);
        Value *FirstValue =
            FirstStore ? FirstStore->getValueOperand() : nullptr;
        if (!FirstValue ||
            !isVectorizableScalarType(FirstValue->getIRType()))
            continue;

        std::vector<StoreInst *> CandidateStores;
        CandidateStores.reserve(MaxVectorLanes);
        const std::size_t End = Begin + std::min(
            MaxScheduleSpan, Instructions.size() - Begin);
        for (std::size_t Index = Begin;
             Index < End && CandidateStores.size() < MaxVectorLanes;
             ++Index) {
            if (Index != Begin &&
                dynamic_cast<CallInst *>(Instructions[Index]))
                break;
            auto *Store = dynamic_cast<StoreInst *>(Instructions[Index]);
            Value *Stored = Store ? Store->getValueOperand() : nullptr;
            if (!Stored || !Stored->getIRType() ||
                !IRType::hasSameShape(Stored->getIRType(),
                                      FirstValue->getIRType()))
                continue;
            CandidateStores.push_back(Store);
        }
        for (std::size_t Factor : getCandidateFactors(
                 FirstValue->getIRType(), CandidateStores.size())) {
            std::vector<StoreInst *> Stores;
            Stores.reserve(Factor);
            for (std::size_t Lane = 0; Lane < Factor; ++Lane)
                Stores.push_back(CandidateStores[Lane]);
            SLPVectorizePlan Plan;
            try {
                if (tryBuildPlan(Stores, Plan) && applyPlan(Plan))
                    return true;
            } catch (const std::invalid_argument &) {
                rollbackEmittedInstructions(Plan);
            } catch (const std::overflow_error &) {
                rollbackEmittedInstructions(Plan);
            }
        }
    }
    return false;
}

PassResult SLPVectorizePass::run(
    Function &F, FunctionAnalysisPassManager &FAM) {
    CostInfo = &FAM.getResult<TargetCostAnalysis>(F);
    if (!ForceForTesting &&
        CostInfo->getNativeVectorBitWidth() == 0)
        return PassResult::unchanged();
    DT = &FAM.getResult<DominantAnalysis>(F);
    SE = &FAM.getResult<SCEVAnalysis>(F);
    RI = &FAM.getResult<RangeAnalysis>(F);
    AA = &FAM.getResult<BasicAliasAnalysis>(F);
    LI = &FAM.getResult<LoopAnalysis>(F);

    bool Changed = false;
    while (true) {
        bool ChangedThisRound = false;
        for (BasicBlock *Block : snapshotPointers(F)) {
            if (!tryVectorizeBlock(Block)) continue;
            Changed = true;
            ChangedThisRound = true;
            break;
        }
        if (!ChangedThisRound) break;
    }
    CostInfo = nullptr;
    AA = nullptr;
    DT = nullptr;
    LI = nullptr;
    RI = nullptr;
    SE = nullptr;
    if (!Changed) return PassResult::unchanged();
    return PassResult::changed(PreservationStatus::none());
}
