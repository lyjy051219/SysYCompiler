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
    // 3.2 Factor 为 0 时根据浮点延迟、访存局部性和寄存器预算在 2/4/8 中自动选择。
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

// 2. 候选形状保存直接父子循环、完整共享内循环树、规范控制切片与需按 lane 复制的指令闭包。
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

// 4.1 每个额外 lane 使用独立 IV 和值映射，克隆在挂接前一直保持游离。
struct UnrollAndJamLanePlan {
    Value *LaneIV = nullptr;
    std::unordered_map<Value *, Value *> ValueMap;
    std::vector<std::pair<Instruction *, Instruction *>> InstructionPairs;
};

// 4.2 整个提交计划同时持有 Factor-1 组 lane 克隆和放大后的外循环更新值。
struct UnrollAndJamClonePlan {
    int Factor = 1;
    Instruction *NewOuterUpdate = nullptr;
    std::vector<UnrollAndJamLanePlan> Lanes;
};

// UnrollAndJam 的整体流程：
// 1. 从内向外枚举直接父子循环，用函数级次数预算和外循环头记录阻止重复增长。
// 2. 证明嵌套为矩形、外迭代互相独立，且共享内循环树满足 LoopSimplify/LCSSA、唯一出口和完整迭代路径。
// 3. 从外 IV 建立 lane 依赖闭包，要求存在可共享读取与合并/滑窗收益，再用代码增长和 GPR/FPR 预算选择 2/4/8 路。
// 4. 离线构造各 lane 的 IV、指令克隆和地址映射；任一步失败都销毁整个未提交计划。
// 5. 预检所有挂接点后集中插入 lane，将外 IV 更新改为 Factor*Step，必要时同步 latch 退出比较。
// 6. 常量 trip count 不整除时先调用 peeling 处理余数；jam 不改 CFG 时保留支配/循环分析，peeling 则全部失效。
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

    // 2.1 收集规范循环控制，并证明子循环域不依赖待展开的外轴。
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
    // 3.1 识别 lane 值、可共享读取、跨 lane 合并访存与滑动窗口机会。
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
    // 2.5 建立候选形状并把所有受外 IV 影响的非控制指令收入 lane 克隆集。
    static bool collectLaneInstructions(UnrollAndJamShape &Shape,
                                        const UnrollAndJamConfig &Config);
    static std::optional<UnrollAndJamShape> buildShape(
        Loop *Outer, Loop *Inner, LoopInfo &LI, SCEVResult &SE,
        LoopAccessInfo &LA, DominatorTree &DT, PostDominatorTree &PDT,
        const UnrollAndJamConfig &Config);
    // 4.3 计划失败清理保证先断开 operand/use 关系，再销毁所有游离指令。
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
    // 5. 只在提交预检成功后挂接克隆并替换外循环步长，提交阶段不再保留可回滚对象。
    static bool executePlan(UnrollAndJamShape &Shape,
                            UnrollAndJamClonePlan &Plan);
};
