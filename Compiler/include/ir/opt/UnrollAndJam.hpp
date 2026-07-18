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
    // 0 表示根据目标延迟、局部性和寄存器压力自动选择 2/4/8。
    int Factor = 0;
    int MaxFactor = 8;
    unsigned MaxTransformsPerFunction = 2;
    unsigned MaxSharedNestDepth = 3;
    unsigned EstimatedFPAddLatency = 3;
    unsigned FPRBudget = 24;
    unsigned GPRBudget = 18;
    unsigned MaxRemainderPeel = 7;
    std::int64_t CacheLineBytes = 64;
    std::int64_t MinCoalescedAxisTripCount = 64;
    std::int64_t MinLaneTraversalBytes = 256 * 1024;
    std::size_t MaxNestBlocks = 24;
    std::size_t MaxClonedInstructions = 96;
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

struct UnrollAndJamLanePlan {
    Value *LaneIV = nullptr;
    std::unordered_map<Value *, Value *> ValueMap;
    std::vector<std::pair<Instruction *, Instruction *>> InstructionPairs;
};

struct UnrollAndJamClonePlan {
    int Factor = 1;
    Instruction *NewOuterUpdate = nullptr;
    std::vector<UnrollAndJamLanePlan> Lanes;
};

// 对外层输出循环做 2/4/8 路展开，并把各路计算合并到同一矩形内层循环树。
// 每一路保留独立的归约 PHI，严格浮点归约不会跨输出重新结合。收益判定同时
// 覆盖长距离相邻流和滑动窗口复用，并受代码增长及整数/浮点寄存器预算约束。
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
        const std::unordered_set<Instruction *> &LaneValues);
    static bool hasLaneCoalescingBenefit(
        const UnrollAndJamShape &Shape, LoopAccessInfo &LA,
        SCEVResult &SE,
        const std::unordered_set<Instruction *> &LaneValues,
        const UnrollAndJamConfig &Config);
    static bool hasSlidingWindowBenefit(
        const UnrollAndJamShape &Shape, LoopAccessInfo &LA,
        SCEVResult &SE,
        const std::unordered_set<Instruction *> &LaneValues,
        int Factor, const UnrollAndJamConfig &Config);
    static bool isStrictFPRecurrencePath(
        Value *Current, PhiInst *Phi,
        std::unordered_set<Value *> &Active, unsigned Depth = 0);
    static unsigned countStrictFPReductions(
        const UnrollAndJamShape &Shape);
    static bool fitsRegisterBudget(const UnrollAndJamShape &Shape,
                                   int Factor,
                                   const UnrollAndJamConfig &Config);
    static bool isSupportedFactor(int Factor);
    static std::optional<int> selectFactor(
        const UnrollAndJamShape &Shape, LoopAccessInfo &LA,
        SCEVResult &SE, const UnrollAndJamConfig &Config);
    static bool collectLaneInstructions(UnrollAndJamShape &Shape,
                                        const UnrollAndJamConfig &Config);
    static std::optional<UnrollAndJamShape> buildShape(
        Loop *Outer, Loop *Inner, LoopInfo &LI, SCEVResult &SE,
        LoopAccessInfo &LA, DominatorTree &DT, PostDominatorTree &PDT,
        const UnrollAndJamConfig &Config);
    static void destroyClonePlan(UnrollAndJamClonePlan &Plan);
    static bool remapClonedOperands(UnrollAndJamLanePlan &Lane);
    static bool canonicalizeLaneMemoryAddresses(
        const UnrollAndJamShape &Shape, LoopAccessInfo &LA,
        UnrollAndJamLanePlan &Lane, int LaneIndex);
    static Value *materializeSlidingInitValue(
        Value *Source, Loop *Axis, PhiInst *AxisIV,
        Value *AxisStart, BasicBlock *PreHeader,
        const DominatorTree &DT,
        std::unordered_map<Value *, Value *> &ValueMap,
        std::vector<Instruction *> &NewInstructions,
        std::unordered_set<Value *> &Active);
    static bool applySlidingWindowForwarding(
        UnrollAndJamShape &Shape, UnrollAndJamClonePlan &Plan,
        LoopAccessInfo &LA, SCEVResult &SE,
        const DominatorTree &DT,
        const UnrollAndJamConfig &Config);
    static std::optional<UnrollAndJamClonePlan> buildClonePlan(
        const UnrollAndJamShape &Shape, LoopAccessInfo &LA, int Factor,
        const UnrollAndJamConfig &Config);
    static bool validatePlanCommit(const UnrollAndJamShape &Shape,
                                   const UnrollAndJamClonePlan &Plan);
    static bool executePlan(UnrollAndJamShape &Shape,
                            UnrollAndJamClonePlan &Plan);
};
