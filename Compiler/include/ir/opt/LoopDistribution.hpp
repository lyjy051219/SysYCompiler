#pragma once

#include "ir/opt/NewPassManager.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class BasicBlock;
class Instruction;
class Loop;
class LoopDependenceInfo;
class SCEVResult;
class StoreInst;
class Value;

// Conservative loop distribution by statement slices.  The pass keeps the
// original loop for the first statement partition and serializes one cloned
// loop containing the second partition after it.
class LoopDistributionPass : public PassDescriptor<LoopDistributionPass> {
public:
    PassResult run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    static constexpr unsigned MaxDistributionsPerFunction = 2;
    static constexpr std::size_t MaxDistributionBlocks = 24;
    static constexpr std::size_t MaxDistributionInstructions = 160;
    static constexpr std::size_t MaxStatementRoots = 24;

    using InstructionSet = std::unordered_set<Instruction *>;
    using BlockSet = std::unordered_set<BasicBlock *>;

    struct DistributionShape {
        Loop *L = nullptr;
        BasicBlock *PreHeader = nullptr;
        BasicBlock *Header = nullptr;
        BasicBlock *Latch = nullptr;
        BasicBlock *Exiting = nullptr;
        BasicBlock *Exit = nullptr;
        std::vector<BasicBlock *> Blocks;
        std::vector<StoreInst *> Stores;
    };

    struct DistributionPlan {
        DistributionShape Shape;
        std::vector<StoreInst *> FirstStores;
        std::vector<StoreInst *> SecondStores;
        InstructionSet FirstSlice;
        InstructionSet SecondSlice;
        unsigned LocalityDistance = 0;
    };

    struct DimensionLocality {
        std::uint64_t WeightedBucketSum = 0;
        std::uint64_t KnownWeight = 0;
    };

    using LocalityProfile = std::vector<DimensionLocality>;

    static bool hasUnsupportedInstruction(Instruction *I);
    static bool valueDependsOnLoad(Value *V,
                                   std::unordered_set<Value *> &Seen,
                                   unsigned Depth = 0);
    static bool hasMemoryDependentControl(
        const std::vector<BasicBlock *> &Blocks);
    static bool hasFiniteLoopTree(Loop *L, SCEVResult &SE);
    static bool hasLoopLiveOut(const DistributionShape &Shape);
    static std::optional<DistributionShape> getDistributionShape(
        Function &F, Loop *L);
    static InstructionSet collectBackwardSlice(
        StoreInst *Root, const BlockSet &LoopBlocks);
    static void mergeSlice(InstructionSet &Into,
                           const InstructionSet &From);
    static unsigned strideBucket(std::int64_t Stride);
    static LocalityProfile buildLocalityProfile(
        const InstructionSet &Slice, const InstructionSet &OtherSlice,
        const std::vector<Loop *> &Dimensions, LoopDependenceInfo &LD);
    static unsigned localityDistance(const InstructionSet &First,
                                     const InstructionSet &Second, Loop *L,
                                     LoopDependenceInfo &LD);
    static std::optional<DistributionPlan> findDistributionPlan(
        Function &F, Loop *L, LoopDependenceInfo &LD);
    static void destroyClonedBlocks(std::vector<BasicBlock *> &Blocks);
    static bool executeDistribution(Function &F,
                                    const DistributionPlan &Plan);

    // The surrounding loop pipeline is a fixed point.  Bound code growth over
    // all invocations of the same pass object, rather than per invocation.
    std::unordered_map<Function *, unsigned> TransformCounts;
};
