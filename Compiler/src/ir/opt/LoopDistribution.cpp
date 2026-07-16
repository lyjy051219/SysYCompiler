#include "ir/opt/LoopDistribution.hpp"

#include "ir/analysis/LoopDependence.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/SCEV.hpp"
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
    Value *V, std::unordered_set<Value *> &Seen, unsigned Depth) {
    if (!V) return false;
    // Exhausting the proof budget is unknown, not evidence of purity.  Treat
    // it like a load-dependent condition so distribution is conservatively
    // refused; a repeated SSA node, in contrast, has already been explored.
    if (Depth > 64) return true;
    if (!Seen.insert(V).second) return false;
    auto *I = dynamic_cast<Instruction *>(V);
    if (!I) return false;
    if (dynamic_cast<LoadInst *>(I) || dynamic_cast<CallInst *>(I))
        return true;
    for (std::size_t Index = 0; Index < I->getNumOperands(); ++Index) {
        if (valueDependsOnLoad(I->getOperand(Index), Seen, Depth + 1))
            return true;
    }
    return false;
}

bool LoopDistributionPass::hasMemoryDependentControl(
    const std::vector<BasicBlock *> &Blocks) {
    for (BasicBlock *BB : Blocks) {
        auto *Br = BB ? BB->getBranchTerminator() : nullptr;
        if (!BasicBlock::isCondBr(Br)) continue;
        std::unordered_set<Value *> Seen;
        if (valueDependsOnLoad(Br->getCondition(), Seen)) return true;
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
    StoreInst *Root, const BlockSet &LoopBlocks) {
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
        for (std::size_t Index = 0; Index < I->getNumOperands(); ++Index) {
            auto *Def = dynamic_cast<Instruction *>(I->getOperand(Index));
            if (Def && Def->getParent() && LoopBlocks.count(Def->getParent()))
                WorkList.push_back(Def);
        }
    }
    return Slice;
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

std::optional<LoopDistributionPass::DistributionPlan>
LoopDistributionPass::findDistributionPlan(
    Function &F, Loop *L, LoopDependenceInfo &LD) {
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

        const unsigned Distance =
            localityDistance(First, Second, Shape->L, LD);
        if (Distance == 0 ||
            !LD.isDistributionLegal(Shape->L, First, Second)) {
            continue;
        }

        DistributionPlan Plan;
        Plan.Shape = *Shape;
        Plan.FirstStores.assign(Shape->Stores.begin(),
                                Shape->Stores.begin() + Cut);
        Plan.SecondStores.assign(Shape->Stores.begin() + Cut,
                                 Shape->Stores.end());
        Plan.FirstSlice = First;
        Plan.SecondSlice = std::move(Second);
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

    // All fallible work is complete.  Install the serial edge first; store
    // deletion below cannot fail and leaves every side effect in exactly one
    // of the two loop versions.
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
    return true;
}

PassResult LoopDistributionPass::run(Function &F,
                                     FunctionAnalysisPassManager &FAM) {
    unsigned &Count = TransformCounts[&F];
    if (Count >= MaxDistributionsPerFunction)
        return PassResult::unchanged();

    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &SE = FAM.getResult<SCEVAnalysis>(F);
    auto &LD = FAM.getResult<LoopDependenceAnalysis>(F);
    std::vector<Loop *> Loops = LI.getLoopsInPostOrder();

    // Prefer an outer domain.  Fissioning the complete surrounding nest lets
    // a later interchange choose a different order for each partition.
    for (auto It = Loops.rbegin(); It != Loops.rend(); ++It) {
        Loop *L = *It;
        if (!hasFiniteLoopTree(L, SE)) continue;
        auto Plan = findDistributionPlan(F, L, LD);
        if (!Plan) continue;
        if (!executeDistribution(F, *Plan)) continue;
        ++Count;
        return PassResult::changed(PreservationStatus::none());
    }
    return PassResult::unchanged();
}
