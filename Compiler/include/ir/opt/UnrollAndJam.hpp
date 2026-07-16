#pragma once

#include "ir/analysis/SCEV.hpp"
#include "ir/opt/NewPassManager.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class DominatorTree;
class Loop;
class LoopAccessInfo;
class LoopInfo;
class PhiInst;
class PostDominatorTree;

struct UnrollAndJamConfig {
    int Factor = 2;
    unsigned MaxTransformsPerFunction = 2;
    unsigned MaxSharedNestDepth = 3;
    std::int64_t CacheLineBytes = 64;
    std::int64_t MinCoalescedAxisTripCount = 64;
    std::int64_t MinLaneTraversalBytes = 256 * 1024;
    std::size_t MaxNestBlocks = 24;
    std::size_t MaxClonedInstructions = 24;
};

struct UnrollAndJamShape {
    Loop *Outer = nullptr;
    Loop *Inner = nullptr;
    BasicBlock *OuterPreHeader = nullptr;
    BasicBlock *OuterLatch = nullptr;
    BasicBlock *InnerPreHeader = nullptr;
    BasicBlock *InnerLatch = nullptr;
    LoopTripCountInfo OuterTrip;
    LoopTripCountInfo InnerTrip;
    InductionDescriptor OuterInduction;
    InductionDescriptor InnerInduction;
    unsigned OuterBackedgeIncomingIndex = 0;
    bool OuterExitUsesUpdate = false;
    unsigned OuterExitComparedOperand = 0;
    std::vector<BasicBlock *> Blocks;
    std::vector<Loop *> SharedLoops;
    std::unordered_set<BasicBlock *> SharedExitingBlocks;
    std::unordered_set<Instruction *> Control;
    std::vector<Instruction *> LaneInstructions;
};

struct UnrollAndJamClonePlan {
    Value *LaneIV = nullptr;
    Instruction *NewOuterUpdate = nullptr;
    std::unordered_map<Value *, Value *> ValueMap;
    std::vector<std::pair<Instruction *, Instruction *>> InstructionPairs;
};

// Unroll the outer output loop by two while sharing a rectangular inner loop
// tree. Each lane keeps its own reduction PHIs, so integer and strict-FP
// reductions retain their original per-output operation order. The transform
// is admitted only when the two output iterations are independent, a load
// traversed by the shared nest is consumed by both lanes, and an adjacent
// lane-varying stream crosses enough cache lines before reuse for jamming to
// improve spatial locality rather than merely sharing a resident small table.
class UnrollAndJamPass : public PassDescriptor<UnrollAndJamPass> {
public:
    explicit UnrollAndJamPass(
        UnrollAndJamConfig Config = UnrollAndJamConfig());
    PassResult run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    UnrollAndJamConfig Config;
    std::unordered_map<Function *, unsigned> TransformCounts;
    std::unordered_map<Function *, std::unordered_set<BasicBlock *>>
        TransformedHeaders;

    static bool hasLoopLiveOut(Loop *L);
    static bool hasOnlyCanonicalHeaderPhi(Loop *L, PhiInst *IV);
    static bool hasControlOnlyHeader(Loop *L, PhiInst *IV,
                                     const LoopTripCountInfo &Trip,
                                     bool AllowReductionPhis);
    static bool collectSharedLoopControl(
        UnrollAndJamShape &Shape, Loop *Current, SCEVResult &SE,
        const UnrollAndJamConfig &Config, unsigned Depth);
    static bool collectControl(UnrollAndJamShape &Shape, SCEVResult &SE,
                               const UnrollAndJamConfig &Config);
    static bool analyzeOuterControl(UnrollAndJamShape &Shape);
    static bool hasSingleExitEdge(Loop *L,
                                  const LoopTripCountInfo &Trip);
    static bool hasCompleteIterationPath(
        Loop *L, const LoopTripCountInfo &Trip, BasicBlock *Latch,
        const PostDominatorTree &PDT);
    static bool validateShape(const UnrollAndJamShape &Shape,
                              const LoopInfo &LI,
                              SCEVResult &SE,
                              const DominatorTree &DT,
                              const PostDominatorTree &PDT,
                              const UnrollAndJamConfig &Config);
    static bool isSupportedLanePhi(const UnrollAndJamShape &Shape,
                                   const LoopInfo &LI, PhiInst *Phi);
    static bool instructionDependsOnLane(
        Instruction *I,
        const std::unordered_map<Value *, Value *> &ValueMap);
    static bool hasShareableLaneRead(
        const UnrollAndJamShape &Shape, LoopAccessInfo &LA,
        const std::unordered_map<Value *, Value *> &LaneValues);
    static bool hasLaneCoalescingBenefit(
        const UnrollAndJamShape &Shape, LoopAccessInfo &LA,
        SCEVResult &SE,
        const std::unordered_map<Value *, Value *> &LaneValues,
        const UnrollAndJamConfig &Config);
    static bool collectLaneInstructions(UnrollAndJamShape &Shape,
                                        LoopAccessInfo &LA,
                                        SCEVResult &SE,
                                        const UnrollAndJamConfig &Config);
    static std::optional<UnrollAndJamShape> buildShape(
        Loop *Outer, Loop *Inner, LoopInfo &LI, SCEVResult &SE,
        LoopAccessInfo &LA, DominatorTree &DT, PostDominatorTree &PDT,
        const UnrollAndJamConfig &Config);
    static void destroyClonePlan(UnrollAndJamClonePlan &Plan);
    static bool remapClonedOperands(UnrollAndJamClonePlan &Plan);
    static std::optional<UnrollAndJamClonePlan> buildClonePlan(
        const UnrollAndJamShape &Shape,
        const UnrollAndJamConfig &Config);
    static bool validatePlanCommit(const UnrollAndJamShape &Shape,
                                   const UnrollAndJamClonePlan &Plan);
    static bool executePlan(UnrollAndJamShape &Shape,
                            UnrollAndJamClonePlan &Plan);
};
