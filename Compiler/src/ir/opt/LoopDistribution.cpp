#include "ir/opt/LoopDistribution.hpp"

#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopDependence.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/opt/DCE.hpp"
#include "ir/opt/LCSSA.hpp"
#include "lib/CFG.hpp"
#include "lib/IRUtils.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

// This pass duplicates ordinary computations but never duplicates an
// operation whose execution is externally observable.  Calls are rejected
// regardless of attributes: even a readonly call may fault or encode input.
bool LoopDistributionPass::hasUnsupportedInstruction(Instruction *I) {
    if (!I) return true;
    if (dynamic_cast<CallInst *>(I) || dynamic_cast<AllocaInst *>(I) ||
        dynamic_cast<RetInst *>(I)) {
        return true;
    }
    return I->instType == Instruction::BinaryUnknown;
}

bool LoopDistributionPass::valueDependsOnLoad(
    Value *V, std::unordered_set<Value *> &Seen,
    const BlockSet &LoopBlocks, unsigned Depth) {
    if (!V) return false;
    // Exhausting the proof budget is unknown, not evidence of purity.  Treat
    // it like a load-dependent condition so distribution is conservatively
    // refused; a repeated SSA node, in contrast, has already been explored.
    if (Depth > 64) return true;
    if (!Seen.insert(V).second) return false;
    auto *I = dynamic_cast<Instruction *>(V);
    if (!I) return false;
    // 循环外已经求值的 load/call 是分布域的不变量，克隆循环不会重复执行
    // 它们；只有循环内部计算出的内存相关条件才可能改变两分区的控制轨迹。
    if (!I->getParent() || !LoopBlocks.count(I->getParent())) return false;
    if (dynamic_cast<LoadInst *>(I) || dynamic_cast<CallInst *>(I))
        return true;
    for (std::size_t Index = 0; Index < I->getNumOperands(); ++Index) {
        if (valueDependsOnLoad(I->getOperand(Index), Seen, LoopBlocks,
                               Depth + 1))
            return true;
    }
    return false;
}

bool LoopDistributionPass::hasMemoryDependentControl(
    const std::vector<BasicBlock *> &Blocks) {
    BlockSet LoopBlocks(Blocks.begin(), Blocks.end());
    for (BasicBlock *BB : Blocks) {
        auto *Br = BB ? BB->getBranchTerminator() : nullptr;
        if (!BasicBlock::isCondBr(Br)) continue;
        std::unordered_set<Value *> Seen;
        if (valueDependsOnLoad(Br->getCondition(), Seen, LoopBlocks))
            return true;
    }
    return false;
}

bool LoopDistributionPass::hasFiniteLoopTree(Loop *L, SCEVResult &SE) {
    if (!L || !SE.isKnownFiniteLoop(L)) return false;
    for (Loop *SubLoop : L->getSubLoops()) {
        if (!hasFiniteLoopTree(SubLoop, SE)) return false;
    }
    return true;
}

bool LoopDistributionPass::hasLoopLiveOut(
    const DistributionShape &Shape) {
    for (BasicBlock *BB : Shape.Blocks) {
        for (Instruction *I : *BB) {
            for (Use *U : I->getUses()) {
                auto *UserI = dynamic_cast<Instruction *>(
                    U ? U->getUser() : nullptr);
                if (!UserI || !UserI->getParent() ||
                    !Shape.L->contains(UserI->getParent())) {
                    return true;
                }
            }
        }
    }
    return false;
}

std::optional<LoopDistributionPass::DistributionShape>
LoopDistributionPass::getDistributionShape(Function &F, Loop *L) {
    if (!L || !L->getHeader() || !LCSSAPass::isLCSSAForm(L))
        return std::nullopt;

    DistributionShape Shape;
    Shape.L = L;
    Shape.Header = L->getHeader();
    if (!L->getLoopSimplifyForm(Shape.PreHeader, Shape.Latch) ||
        !Shape.PreHeader || !Shape.Latch) {
        return std::nullopt;
    }
    auto *PreBr = Shape.PreHeader->getBranchTerminator();
    if (!BasicBlock::isUncondBr(PreBr) ||
        PreBr->getTrueSuccessor() != Shape.Header) {
        return std::nullopt;
    }

    const auto Exits = L->getExitBlocks();
    if (Exits.size() != 1 || !Exits.front() ||
        !Exits.front()->collectPhiNodes().empty()) {
        return std::nullopt;
    }
    Shape.Exit = Exits.front();
    Shape.Blocks = L->getBlocksInFunctionOrder();
    if (Shape.Blocks.empty() ||
        Shape.Blocks.size() > MaxDistributionBlocks) {
        return std::nullopt;
    }

    BlockSet BlockLookup(Shape.Blocks.begin(), Shape.Blocks.end());
    std::size_t InstructionCount = 0;
    unsigned ExitEdgeCount = 0;
    for (BasicBlock *BB : Shape.Blocks) {
        if (!BB || BB->getParent() != &F) return std::nullopt;
        InstructionCount += static_cast<std::size_t>(BB->size());
        if (InstructionCount > MaxDistributionInstructions)
            return std::nullopt;

        for (Instruction *I : *BB) {
            if (hasUnsupportedInstruction(I)) return std::nullopt;
            if (auto *Store = dynamic_cast<StoreInst *>(I))
                Shape.Stores.push_back(Store);
        }

        for (BasicBlock *Succ : BB->getSuccessors()) {
            if (!Succ || BlockLookup.count(Succ)) continue;
            if (Succ != Shape.Exit) return std::nullopt;
            ++ExitEdgeCount;
            if (Shape.Exiting && Shape.Exiting != BB)
                return std::nullopt;
            Shape.Exiting = BB;
        }
    }

    if (ExitEdgeCount != 1 || !Shape.Exiting ||
        Shape.Exiting->countEdgesToSucc(Shape.Exit) != 1 ||
        Shape.Stores.size() < 2 ||
        Shape.Stores.size() > MaxStatementRoots ||
        hasMemoryDependentControl(Shape.Blocks) ||
        hasLoopLiveOut(Shape)) {
        return std::nullopt;
    }
    return Shape;
}

LoopDistributionPass::InstructionSet
LoopDistributionPass::collectBackwardSlice(
    StoreInst *Root, const BlockSet &LoopBlocks,
    const InstructionSet *StopValues) {
    InstructionSet Slice;
    std::vector<Instruction *> WorkList;
    if (Root) WorkList.push_back(Root);

    while (!WorkList.empty()) {
        Instruction *I = WorkList.back();
        WorkList.pop_back();
        if (!I || !I->getParent() || !LoopBlocks.count(I->getParent()) ||
            !Slice.insert(I).second) {
            continue;
        }
        if (StopValues && StopValues->count(I)) continue;
        for (std::size_t Index = 0; Index < I->getNumOperands(); ++Index) {
            auto *Def = dynamic_cast<Instruction *>(I->getOperand(Index));
            if (Def && Def->getParent() && LoopBlocks.count(Def->getParent()))
                WorkList.push_back(Def);
        }
    }
    return Slice;
}

bool LoopDistributionPass::isAnchorOrderedDistributionLegal(
    const DistributionPlan &Plan, LoopDependenceInfo &LD,
    DominatorTree &DT) {
    const DistributionShape &Shape = Plan.Shape;
    if (!Shape.L || !Shape.Header || Plan.FirstStores.size() != 1)
        return false;

    StoreInst *Anchor = Plan.FirstStores.front();
    BasicBlock *AnchorBlock = Anchor ? Anchor->getParent() : nullptr;
    if (!AnchorBlock || !Shape.L->contains(AnchorBlock) ||
        !DT.dominates(Shape.Header, AnchorBlock) ||
        !DT.dominates(AnchorBlock, Shape.Latch)) {
        return false;
    }

    std::vector<Instruction *> FirstMemory;
    for (Instruction *I : Plan.FirstSlice) {
        if (!dynamic_cast<LoadInst *>(I) &&
            !dynamic_cast<StoreInst *>(I)) {
            continue;
        }
        // 该保守子集要求第一分区的内存访问都与桥接 store 位于同一块，
        // 并在它之前完成，从而无需猜测复杂 CFG 中的动态语句顺序。
        if (I->getParent() != AnchorBlock ||
            (I != Anchor && !I->comesBefore(Anchor))) {
            return false;
        }
        FirstMemory.push_back(I);
    }

    for (Instruction *Second : Plan.SecondLegalitySlice) {
        if (!dynamic_cast<LoadInst *>(Second) &&
            !dynamic_cast<StoreInst *>(Second)) {
            continue;
        }
        if (!Second->getParent() || !Shape.L->contains(Second->getParent()))
            return false;
        if (Second->getParent() == AnchorBlock) {
            if (!Anchor->comesBefore(Second)) return false;
        } else if (!DT.dominates(AnchorBlock, Second->getParent())) {
            return false;
        }
        for (Instruction *First : FirstMemory) {
            if (!LD.isConflictLocalToIteration(First, Second, Shape.L))
                return false;
        }
    }
    return true;
}

bool LoopDistributionPass::buildForwardingPlan(
    DistributionPlan &Plan, const BlockSet &Blocks,
    LoopDependenceInfo &LD, DominatorTree &DT,
    const LoopInfo &LI) {
    if (!Plan.Shape.L || !Plan.Shape.Header ||
        Plan.FirstStores.size() != 1 || Plan.SecondStores.empty()) {
        return false;
    }

    StoreInst *Store = Plan.FirstStores.front();
    auto *Producer = Store
        ? dynamic_cast<Instruction *>(Store->getValueOperand())
        : nullptr;
    BasicBlock *StoreBlock = Store ? Store->getParent() : nullptr;
    if (!Store || !Producer || !StoreBlock ||
        LI.getLoopFor(StoreBlock) != Plan.Shape.L ||
        !DT.dominates(Plan.Shape.Header, StoreBlock) ||
        !DT.dominates(StoreBlock, Plan.Shape.Latch) ||
        !Plan.FirstSlice.count(Producer) ||
        !Plan.SecondSlice.count(Producer) ||
        !LD.isConflictLocalToIteration(Store, Store, Plan.Shape.L)) {
        return false;
    }

    bool HasForwardedUse = false;
    for (Use *U : Producer->getUses()) {
        auto *UserI = dynamic_cast<Instruction *>(U ? U->getUser() : nullptr);
        if (!UserI || !UserI->getParent() ||
            !Blocks.count(UserI->getParent())) {
            return false;
        }
        if (!Plan.SecondSlice.count(UserI)) continue;
        // reload 会插在原 store 的位置；同块内位于 store 之前的使用不受
        // 该定义支配，不能作为跨分区桥接点。
        if (UserI->getParent() == StoreBlock) {
            if (!Store->comesBefore(UserI)) return false;
        } else if (!DT.dominates(StoreBlock, UserI->getParent())) {
            return false;
        }
        HasForwardedUse = true;
    }
    if (!HasForwardedUse) return false;

    InstructionSet Stops;
    Stops.insert(Producer);
    InstructionSet EffectiveSecond;
    for (StoreInst *Root : Plan.SecondStores) {
        mergeSlice(EffectiveSecond,
                   collectBackwardSlice(Root, Blocks, &Stops));
    }
    if (EffectiveSecond.empty()) return false;

    Plan.SecondLegalitySlice = std::move(EffectiveSecond);
    Plan.ForwardedValues.push_back({Store, Producer});
    return isAnchorOrderedDistributionLegal(Plan, LD, DT);
}

void LoopDistributionPass::mergeSlice(InstructionSet &Into,
                                      const InstructionSet &From) {
    Into.insert(From.begin(), From.end());
}

unsigned LoopDistributionPass::strideBucket(std::int64_t Stride) {
    uint64_t Magnitude = 0;
    if (Stride < 0) {
        // Avoid negating INT64_MIN.
        Magnitude = static_cast<uint64_t>(-(Stride + 1));
        ++Magnitude;
    } else {
        Magnitude = static_cast<uint64_t>(Stride);
    }
    if (Magnitude == 0) return 0;
    if (Magnitude <= 8) return 1;
    if (Magnitude <= 64) return 2;
    if (Magnitude <= 512) return 3;
    return 4;
}

LoopDistributionPass::LocalityProfile
LoopDistributionPass::buildLocalityProfile(
    const InstructionSet &Slice, const InstructionSet &OtherSlice,
    const std::vector<Loop *> &Dimensions, LoopDependenceInfo &LD) {
    LocalityProfile Profile(Dimensions.size());
    for (Instruction *I : Slice) {
        if (!I || OtherSlice.count(I)) continue;
        const bool IsStore = dynamic_cast<StoreInst *>(I) != nullptr;
        const bool IsLoad = dynamic_cast<LoadInst *>(I) != nullptr;
        if (!IsStore && !IsLoad) continue;
        const uint64_t Weight = IsStore ? 2 : 1;
        for (std::size_t D = 0; D < Dimensions.size(); ++D) {
            auto Stride = LD.getByteStride(I, Dimensions[D]);
            if (!Stride) continue;
            Profile[D].WeightedBucketSum += Weight * strideBucket(*Stride);
            Profile[D].KnownWeight += Weight;
        }
    }
    return Profile;
}

unsigned LoopDistributionPass::localityDistance(
    const InstructionSet &First, const InstructionSet &Second, Loop *L,
    LoopDependenceInfo &LD) {
    std::vector<Loop *> Dimensions = L->getLoopsInPostOrder();
    std::reverse(Dimensions.begin(), Dimensions.end());
    if (Dimensions.empty()) return 0;

    LocalityProfile A =
        buildLocalityProfile(First, Second, Dimensions, LD);
    LocalityProfile B =
        buildLocalityProfile(Second, First, Dimensions, LD);
    unsigned Distance = 0;
    for (std::size_t D = 0; D < Dimensions.size(); ++D) {
        if (A[D].KnownWeight == 0 || B[D].KnownWeight == 0) continue;
        const uint64_t AvgA =
            100 * A[D].WeightedBucketSum / A[D].KnownWeight;
        const uint64_t AvgB =
            100 * B[D].WeightedBucketSum / B[D].KnownWeight;
        const uint64_t Delta = AvgA > AvgB ? AvgA - AvgB : AvgB - AvgA;
        Distance += static_cast<unsigned>(std::min<uint64_t>(Delta, 400));
    }

    // One full bucket (for example unit-stride versus medium-stride) is the
    // minimum signal accepted.  Equal access-direction profiles do not pay
    // the cost of another complete loop nest.
    return Distance >= 100 ? Distance : 0;
}

unsigned LoopDistributionPass::interchangeOpportunity(
    const InstructionSet &Slice, Loop *L, LoopDependenceInfo &LD) {
    if (!L || L->getSubLoops().size() != 1) return 0;
    Loop *Inner = L->getSubLoops().front();
    if (!Inner || !LD.isInterchangeLegal(L, Inner)) return 0;

    unsigned Benefit = 0;
    for (Instruction *I : Slice) {
        const bool IsStore = dynamic_cast<StoreInst *>(I) != nullptr;
        const bool IsLoad = dynamic_cast<LoadInst *>(I) != nullptr;
        if (!IsStore && !IsLoad) continue;
        auto OuterStride = LD.getByteStride(I, L);
        auto InnerStride = LD.getByteStride(I, Inner);
        if (!OuterStride || !InnerStride) continue;
        const unsigned OuterCost = strideBucket(*OuterStride);
        const unsigned InnerCost = strideBucket(*InnerStride);
        if (InnerCost <= OuterCost) continue;
        Benefit += (IsStore ? 2U : 1U) * (InnerCost - OuterCost);
    }
    return Benefit;
}

std::optional<LoopDistributionPass::DistributionPlan>
LoopDistributionPass::findDistributionPlan(
    Function &F, Loop *L, LoopDependenceInfo &LD,
    DominatorTree &DT, const LoopInfo &LI, SCEVResult &SE) {
    auto Shape = getDistributionShape(F, L);
    if (!Shape) return std::nullopt;

    BlockSet Blocks(Shape->Blocks.begin(), Shape->Blocks.end());
    std::vector<InstructionSet> Slices;
    Slices.reserve(Shape->Stores.size());
    for (StoreInst *Store : Shape->Stores)
        Slices.push_back(collectBackwardSlice(Store, Blocks));

    std::optional<DistributionPlan> Best;
    InstructionSet First;
    for (std::size_t Cut = 1; Cut < Shape->Stores.size(); ++Cut) {
        mergeSlice(First, Slices[Cut - 1]);
        InstructionSet Second;
        for (std::size_t I = Cut; I < Slices.size(); ++I)
            mergeSlice(Second, Slices[I]);

        DistributionPlan Plan;
        Plan.Shape = *Shape;
        Plan.FirstStores.assign(Shape->Stores.begin(),
                                Shape->Stores.begin() + Cut);
        Plan.SecondStores.assign(Shape->Stores.begin() + Cut,
                                 Shape->Stores.end());
        Plan.FirstSlice = First;
        Plan.SecondSlice = std::move(Second);
        Plan.SecondLegalitySlice = Plan.SecondSlice;
        const bool DirectlyLegal = LD.isDistributionLegal(
            Shape->L, Plan.FirstSlice, Plan.SecondLegalitySlice);
        const bool ForwardLegal = !DirectlyLegal &&
            buildForwardingPlan(Plan, Blocks, LD, DT, LI);
        if (!DirectlyLegal && !ForwardLegal)
            continue;

        unsigned Distance = localityDistance(
            Plan.FirstSlice, Plan.SecondLegalitySlice, Shape->L, LD);
        if (Distance == 0 && !Plan.ForwardedValues.empty()) {
            // imperfect nest 的整体画像可能相近，但第二分区独立交换外轴和
            // 唯一子轴后仍能把大步长访问变成连续访问。只有依赖分析已经
            // 证明可交换且至少改善一个 stride 档位时才承担克隆开销。
            const unsigned Opportunity = interchangeOpportunity(
                Plan.SecondLegalitySlice, Shape->L, LD);
            if (Opportunity != 0) Distance = 100 * Opportunity;
        }
        if (Distance == 0) continue;
        for (Loop *SubLoop : Shape->L->getSubLoops()) {
            if (isPrunableFirstLoop(SubLoop, Plan, SE))
                Plan.PrunedFirstLoops.push_back(SubLoop);
        }
        // 当前 CFG 提交只删除一个独立叶循环；多个待删子循环需要额外的
        // 出口与支配关系重建，不能复用单循环证明。
        if (Plan.PrunedFirstLoops.size() > 1) continue;
        if (!Plan.ForwardedValues.empty() &&
            Plan.PrunedFirstLoops.empty()) {
            continue;
        }
        Plan.LocalityDistance = Distance;
        if (!Best || Plan.LocalityDistance > Best->LocalityDistance)
            Best = std::move(Plan);
    }
    return Best;
}

void LoopDistributionPass::destroyClonedBlocks(
    std::vector<BasicBlock *> &Blocks) {
    for (BasicBlock *BB : Blocks) {
        if (!BB) continue;
        for (Instruction *I : *BB)
            if (I) I->clearOperands();
    }
    for (BasicBlock *BB : Blocks)
        if (BB) BB->clear();
    for (BasicBlock *BB : Blocks)
        delete BB;
    Blocks.clear();
}

void LoopDistributionPass::deleteDeadClonedSlice(
    Instruction *Root, const BlockSet &ClonedBlocks) {
    std::vector<Instruction *> WorkList;
    std::unordered_set<Instruction *> Pending;
    if (Root) {
        WorkList.push_back(Root);
        Pending.insert(Root);
    }
    while (!WorkList.empty()) {
        Instruction *I = WorkList.back();
        WorkList.pop_back();
        Pending.erase(I);
        if (!I || !I->getParent() ||
            !ClonedBlocks.count(I->getParent()) ||
            I->hasUses() ||
            dynamic_cast<StoreInst *>(I) || dynamic_cast<CallInst *>(I) ||
            dynamic_cast<BrInst *>(I) || dynamic_cast<RetInst *>(I) ||
            dynamic_cast<PhiInst *>(I) || dynamic_cast<AllocaInst *>(I) ||
            dceInstructionMustBePreserved(I)) {
            continue;
        }

        std::vector<Instruction *> Operands;
        for (std::size_t Index = 0; Index < I->getNumOperands(); ++Index) {
            auto *Operand = dynamic_cast<Instruction *>(I->getOperand(Index));
            if (Operand && Operand->getParent() &&
                ClonedBlocks.count(Operand->getParent())) {
                Operands.push_back(Operand);
            }
        }
        I->clearOperands();
        delete I;
        for (Instruction *Operand : Operands) {
            if (Operand && Pending.insert(Operand).second)
                WorkList.push_back(Operand);
        }
    }
}

bool LoopDistributionPass::isPrunableFirstLoop(
    Loop *L, const DistributionPlan &Plan, SCEVResult &SE) {
    if (!L || !L->getHeader() || !L->getSubLoops().empty() ||
        !SE.isKnownFiniteLoop(L)) {
        return false;
    }

    BasicBlock *PreHeader = nullptr;
    BasicBlock *Latch = nullptr;
    if (!L->getLoopSimplifyForm(PreHeader, Latch) ||
        !PreHeader || !Latch) {
        return false;
    }
    auto *PreBr = PreHeader->getBranchTerminator();
    const auto Exits = L->getExitBlocks();
    if (!BasicBlock::isUncondBr(PreBr) ||
        PreBr->getTrueSuccessor() != L->getHeader() ||
        Exits.size() != 1 || !Exits.front() ||
        L->contains(Plan.Shape.Exiting) ||
        !Plan.Shape.L->contains(Exits.front()) ||
        !Exits.front()->collectPhiNodes().empty()) {
        return false;
    }

    std::unordered_set<StoreInst *> RemovedStores(
        Plan.SecondStores.begin(), Plan.SecondStores.end());
    bool RemovesStatement = false;
    for (BasicBlock *BB : L->getBlocksInFunctionOrder()) {
        if (!BB) return false;
        for (Instruction *I : *BB) {
            if (!I) return false;
            if (auto *Store = dynamic_cast<StoreInst *>(I)) {
                if (!RemovedStores.count(Store)) return false;
                RemovesStatement = true;
                continue;
            }
            if (dynamic_cast<CallInst *>(I) ||
                dynamic_cast<RetInst *>(I) ||
                dynamic_cast<AllocaInst *>(I) ||
                dceInstructionMustBePreserved(I)) {
                return false;
            }
            for (Use *U : I->getUses()) {
                auto *UserI = dynamic_cast<Instruction *>(
                    U ? U->getUser() : nullptr);
                if (!UserI || !UserI->getParent() ||
                    !L->contains(UserI->getParent())) {
                    return false;
                }
            }
        }
    }
    return RemovesStatement;
}

void LoopDistributionPass::pruneFirstLoopCFG(Loop *L) {
    if (!L || !L->getHeader()) return;
    BasicBlock *PreHeader = nullptr;
    BasicBlock *Latch = nullptr;
    if (!L->getLoopSimplifyForm(PreHeader, Latch) || !PreHeader || !Latch)
        return;
    const auto Exits = L->getExitBlocks();
    if (Exits.size() != 1 || !Exits.front()) return;
    std::vector<BasicBlock *> Blocks = L->getBlocksInFunctionOrder();
    if (!PreHeader->replaceTerminator(new BrInst(Exits.front()))) return;

    // 先断开整棵无副作用子循环的 use-def 与 CFG 边，再统一释放块，避免
    // 逐块删除时其他块仍持有已释放的标签或指令引用。
    for (BasicBlock *BB : Blocks) {
        if (!BB) continue;
        for (Instruction *I : *BB)
            if (I) I->clearOperands();
    }
    for (BasicBlock *BB : Blocks)
        if (BB) BB->clear();
    for (BasicBlock *BB : Blocks)
        if (BB) delete BB;
}

bool LoopDistributionPass::executeDistribution(
    Function &F, const DistributionPlan &Plan) {
    const DistributionShape &Shape = Plan.Shape;
    if (!Shape.L || !Shape.PreHeader || !Shape.Header || !Shape.Exiting ||
        !Shape.Exit || Shape.Blocks.empty()) {
        return false;
    }

    auto *NewPreHeader = new BasicBlock();
    F.pushBlock(NewPreHeader);
    NewPreHeader->setIdent(F.makeUniqueBlockIdent(
        Shape.Header->getIdent() + ".dist.preheader"));

    ValueToValueMap InitialMap;
    InitialMap[Shape.PreHeader] = NewPreHeader;
    CloneBlockRegionOptions Options;
    Options.MakeBlockIdent = [&F](const BasicBlock *BB, std::size_t) {
        const std::string Base = BB ? BB->getIdent() : "bb";
        return F.makeUniqueBlockIdent(Base + ".dist");
    };

    auto Region =
        cloneBasicBlockRegion(F, Shape.Blocks, InitialMap, Options);
    if (!Region) {
        delete NewPreHeader;
        return false;
    }

    BasicBlock *CloneHeader = Region->getClonedBlock(Shape.Header);
    BasicBlock *CloneExiting = Region->getClonedBlock(Shape.Exiting);
    BlockSet ClonedBlockSet(Region->ClonedBlocks.begin(),
                            Region->ClonedBlocks.end());
    std::vector<Instruction *> MappedProducers;
    std::vector<StoreInst *> ClonedFirstStores;
    ClonedFirstStores.reserve(Plan.FirstStores.size());
    for (StoreInst *Store : Plan.FirstStores) {
        auto *Mapped = dynamic_cast<StoreInst *>(
            Region->getClonedInstruction(Store));
        if (!Mapped) {
            destroyClonedBlocks(Region->ClonedBlocks);
            delete NewPreHeader;
            return false;
        }
        ClonedFirstStores.push_back(Mapped);
    }

    if (!CloneHeader || !CloneExiting ||
        CloneExiting->countEdgesToSucc(Shape.Exit) != 1) {
        destroyClonedBlocks(Region->ClonedBlocks);
        delete NewPreHeader;
        return false;
    }

    for (const DistributionPlan::ForwardedValue &Bridge :
         Plan.ForwardedValues) {
        auto *MappedStore = dynamic_cast<StoreInst *>(
            Region->getClonedInstruction(Bridge.Store));
        auto *MappedProducer = dynamic_cast<Instruction *>(
            Region->getClonedInstruction(Bridge.Producer));
        if (!MappedStore || !MappedProducer ||
            !MappedStore->getPointerOperand()) {
            destroyClonedBlocks(Region->ClonedBlocks);
            delete NewPreHeader;
            return false;
        }
        MappedProducers.push_back(MappedProducer);

        std::vector<Use *> ForwardedUses;
        for (Use *OriginalUse : Bridge.Producer->getUses()) {
            auto *OriginalUser = dynamic_cast<Instruction *>(
                OriginalUse ? OriginalUse->getUser() : nullptr);
            if (!OriginalUser || !Plan.SecondSlice.count(OriginalUser))
                continue;
            Instruction *MappedUser =
                Region->getClonedInstruction(OriginalUser);
            if (!MappedUser) {
                destroyClonedBlocks(Region->ClonedBlocks);
                delete NewPreHeader;
                return false;
            }
            for (std::size_t Index = 0;
                 Index < MappedUser->getNumOperands(); ++Index) {
                if (MappedUser->getOperand(Index) == MappedProducer)
                    ForwardedUses.push_back(
                        MappedUser->getOperandUse(Index));
            }
        }
        if (ForwardedUses.empty()) {
            destroyClonedBlocks(Region->ClonedBlocks);
            delete NewPreHeader;
            return false;
        }

        auto *Reload = new LoadInst(MappedStore->getPointerOperand());
        Reload->setIdent(Bridge.Producer->getIdent() + ".dist.reload");
        if (!MappedStore->insertBefore(Reload)) {
            delete Reload;
            destroyClonedBlocks(Region->ClonedBlocks);
            delete NewPreHeader;
            return false;
        }
        for (Use *U : ForwardedUses)
            U->replaceUseeWith(Reload);
    }

    // 所有可能失败的检查均已完成。先接好串行边，再删除两个分区中各自
    // 不属于该分区的 store；桥接 reload 只读取第一循环已经提交的值。
    NewPreHeader->genUnCondInst(CloneHeader);
    if (!BasicBlock::redirectBranchTarget(Shape.Exiting, Shape.Exit,
                                          NewPreHeader)) {
        NewPreHeader->clear();
        destroyClonedBlocks(Region->ClonedBlocks);
        delete NewPreHeader;
        return false;
    }

    for (StoreInst *Store : Plan.SecondStores)
        delete Store;
    for (StoreInst *Store : ClonedFirstStores)
        delete Store;
    for (Instruction *Producer : MappedProducers)
        deleteDeadClonedSlice(Producer, ClonedBlockSet);
    for (Loop *DeadLoop : Plan.PrunedFirstLoops)
        pruneFirstLoopCFG(DeadLoop);
    return true;
}

PassResult LoopDistributionPass::run(Function &F,
                                     FunctionAnalysisPassManager &FAM) {
    unsigned &Count = TransformCounts[&F];
    if (Count >= MaxDistributionsPerFunction)
        return PassResult::unchanged();

    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &DT = FAM.getResult<DominantAnalysis>(F);
    auto &SE = FAM.getResult<SCEVAnalysis>(F);
    auto &LD = FAM.getResult<LoopDependenceAnalysis>(F);
    std::vector<Loop *> Loops = LI.getLoopsInPostOrder();

    // Prefer an outer domain.  Fissioning the complete surrounding nest lets
    // a later interchange choose a different order for each partition.
    for (auto It = Loops.rbegin(); It != Loops.rend(); ++It) {
        Loop *L = *It;
        if (!hasFiniteLoopTree(L, SE)) continue;
        auto Plan = findDistributionPlan(F, L, LD, DT, LI, SE);
        if (!Plan) continue;
        if (!executeDistribution(F, *Plan)) continue;
        ++Count;
        return PassResult::changed(PreservationStatus::none());
    }
    return PassResult::unchanged();
}
