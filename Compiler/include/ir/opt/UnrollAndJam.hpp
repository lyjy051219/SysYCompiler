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

//UnrollAndJamConfig集中描述展开并合并的次数、收益模型和代码增长限制。
struct UnrollAndJamConfig {
    //3.2 Factor 为 0 时根据浮点延迟、访存局部性和寄存器预算在 2/4/8 中自动选择。
    int Factor = 0;
    //限制自动或显式选择的最大展开因子。
    int MaxFactor = 8;
    //限制单个函数在固定点迭代期间最多执行的变换次数。
    unsigned MaxTransformsPerFunction = 2;
    //限制可由所有lane共享的嵌套循环树深度。
    unsigned MaxSharedNestDepth = 3;
    //给出收益模型采用的浮点加法估算延迟。
    unsigned EstimatedFPAddLatency = 3;
    //给出变换后允许占用的浮点寄存器预算。
    unsigned FPRBudget = 24;
    //给出变换后允许占用的通用寄存器预算。
    unsigned GPRBudget = 18;
    //限制为处理常量余数而允许剥离的最大迭代次数。
    unsigned MaxRemainderPeel = 7;
    //给出局部性模型采用的目标缓存行字节数。
    std::int64_t CacheLineBytes = 64;
    //限定把lane合并访存视为有收益所需的最小共享轴迭代次数。
    std::int64_t MinCoalescedAxisTripCount = 64;
    //限定把长距离lane遍历视为有收益所需的最小字节跨度。
    std::int64_t MinLaneTraversalBytes = 256 * 1024;
    //限制候选循环嵌套允许包含的基本块数量。
    std::size_t MaxNestBlocks = 24;
    //限制所有额外lane合计允许克隆的指令数量。
    std::size_t MaxClonedInstructions = 96;
};

//2. 候选形状保存直接父子循环、完整共享内循环树、规范控制切片与需按 lane 复制的指令闭包。
struct UnrollAndJamShape {
    //记录需要按因子展开的外循环。
    Loop *Outer = nullptr;
    //记录作为共享控制骨架的直接内循环。
    Loop *Inner = nullptr;
    //记录外循环的唯一预头块。
    BasicBlock *OuterPreHeader = nullptr;
    //记录外循环的唯一回边块。
    BasicBlock *OuterLatch = nullptr;
    //记录直接内循环的唯一预头块。
    BasicBlock *InnerPreHeader = nullptr;
    //记录直接内循环的唯一回边块。
    BasicBlock *InnerLatch = nullptr;
    //缓存外循环的规范迭代次数分析结果。
    LoopTripCountInfo OuterTrip;
    //缓存直接内循环的规范迭代次数分析结果。
    LoopTripCountInfo InnerTrip;
    //缓存外循环的规范归纳变量描述。
    InductionDescriptor OuterInduction;
    //缓存直接内循环的规范归纳变量描述。
    InductionDescriptor InnerInduction;
    //记录外归纳PHI回边入值的索引。
    unsigned OuterBackedgeIncomingIndex = 0;
    //记录外循环退出比较是否读取更新后的归纳变量。
    bool OuterExitUsesUpdate = false;
    //记录退出比较中引用外归纳变量或其更新值的操作数索引。
    unsigned OuterExitComparedOperand = 0;
    //按函数顺序记录候选嵌套涉及的全部基本块。
    std::vector<BasicBlock *> Blocks;
    //记录由所有lane共同复用的内循环树。
    std::vector<Loop *> SharedLoops;
    //记录共享内循环树中的所有退出控制块。
    std::unordered_set<BasicBlock *> SharedExitingBlocks;
    //记录不得为各lane克隆的循环控制指令。
    std::unordered_set<Instruction *> Control;
    //记录闭包内需要为每个额外lane克隆的指令。
    std::vector<Instruction *> LaneInstructions;
};

//4.1 每个额外 lane 使用独立 IV 和值映射，克隆在挂接前一直保持游离。
struct UnrollAndJamLanePlan {
    //保存当前lane偏移后的外归纳变量。
    Value *LaneIV = nullptr;
    //把原值映射到当前lane对应的新值。
    std::unordered_map<Value *, Value *> ValueMap;
    //按提交顺序保存源指令及其游离克隆。
    std::vector<std::pair<Instruction *, Instruction *>> InstructionPairs;
};

//4.2 整个提交计划同时持有 Factor-1 组 lane 克隆和放大后的外循环更新值。
struct UnrollAndJamClonePlan {
    //记录本计划采用的展开因子。
    int Factor = 1;
    //保存步长放大Factor倍后的游离外循环更新指令。
    Instruction *NewOuterUpdate = nullptr;
    //保存除原始lane之外的全部额外lane计划。
    std::vector<UnrollAndJamLanePlan> Lanes;
};

//UnrollAndJam 的整体流程：
//1. 从内向外枚举直接父子循环，用函数级次数预算和外循环头记录阻止重复增长。
//2. 证明嵌套为矩形、外迭代互相独立，且共享内循环树满足 LoopSimplify/LCSSA、唯一出口和完整迭代路径。
//3. 从外 IV 建立 lane 依赖闭包，要求存在可共享读取与合并/滑窗收益，再用代码增长和 GPR/FPR 预算选择 2/4/8 路。
//4. 离线构造各 lane 的 IV、指令克隆和地址映射；任一步失败都销毁整个未提交计划。
//5. 预检所有挂接点后集中插入 lane，将外 IV 更新改为 Factor*Step，必要时同步 latch 退出比较。
//6. 常量 trip count 不整除时先调用 peeling 处理余数；jam 不改 CFG 时保留支配/循环分析，peeling 则全部失效。
class UnrollAndJamPass : public PassDescriptor<UnrollAndJamPass> {
public:
    //使用给定配置构造循环展开并合并Pass。
    explicit UnrollAndJamPass(
        UnrollAndJamConfig Config = UnrollAndJamConfig());
    //在函数中寻找一个合法且有收益的相邻循环嵌套并执行展开并合并。
    PassResult run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    //保存本Pass采用的收益模型和资源限制。
    UnrollAndJamConfig Config;
    //记录各函数在固定点迭代期间已成功执行的变换次数。
    std::unordered_map<Function *, unsigned> TransformCounts;
    //记录各函数已经变换过的外循环头以阻止重复代码增长。
    std::unordered_map<Function *, std::unordered_set<BasicBlock *>>
        TransformedHeaders;

    //2.1 收集规范循环控制，并证明子循环域不依赖待展开的外轴。
    //检查循环内是否存在被循环外指令使用的活跃值。
    static bool hasLoopLiveOut(Loop *L);
    //检查循环头是否只包含给定的规范归纳PHI。
    static bool hasOnlyCanonicalHeaderPhi(Loop *L, PhiInst *IV);
    //检查循环头除规范归纳量和允许的归约外是否只包含控制计算。
    static bool hasControlOnlyHeader(Loop *L, PhiInst *IV,
                                     const LoopTripCountInfo &Trip,
                                     bool AllowReductionPhis);
    //递归收集共享内循环树的规范控制切片并验证每一层迭代域。
    static bool collectSharedLoopControl(
        UnrollAndJamShape &Shape, Loop *Current, SCEVResult &SE,
        const UnrollAndJamConfig &Config, unsigned Depth);
    //收集外循环和完整共享内循环树中不得按lane复制的控制指令。
    static bool collectControl(UnrollAndJamShape &Shape, SCEVResult &SE,
                               const UnrollAndJamConfig &Config);
    //定位外归纳量的回边入值和退出比较使用形式。
    static bool analyzeOuterControl(UnrollAndJamShape &Shape);
    //检查循环是否恰好通过分析出的退出边离开。
    static bool hasSingleExitEdge(Loop *L,
                                  const LoopTripCountInfo &Trip);
    //证明每次进入循环的迭代都必经指定回边块。
    static bool hasCompleteIterationPath(
        Loop *L, const LoopTripCountInfo &Trip, BasicBlock *Latch,
        const PostDominatorTree &PDT);
    //综合验证候选嵌套的结构、迭代域、支配关系和副作用限制。
    static bool validateShape(const UnrollAndJamShape &Shape,
                              const LoopInfo &LI,
                              SCEVResult &SE,
                              const DominatorTree &DT,
                              const PostDominatorTree &PDT,
                              const UnrollAndJamConfig &Config);
    //3.1 识别 lane 值、可共享读取、跨 lane 合并访存与滑动窗口机会。
    //判断PHI是否能为每个lane独立克隆且保持递推语义。
    static bool isSupportedLanePhi(const UnrollAndJamShape &Shape,
                                   const LoopInfo &LI, PhiInst *Phi);
    //判断指令的任一操作数是否已经映射为当前lane的值。
    static bool instructionDependsOnLane(
        Instruction *I,
        const std::unordered_map<Value *, Value *> &ValueMap);
    //检查是否存在跨lane复用的外轴不变读取。
    static bool hasShareableLaneRead(
        const UnrollAndJamShape &Shape, LoopAccessInfo &LA,
        const std::unordered_set<Instruction *> &LaneValues);
    //检查lane轴相邻访存能否在长共享轴上形成合并收益。
    static bool hasLaneCoalescingBenefit(
        const UnrollAndJamShape &Shape, LoopAccessInfo &LA,
        SCEVResult &SE,
        const std::unordered_set<Instruction *> &LaneValues,
        const UnrollAndJamConfig &Config);
    //检查相邻lane访存是否能形成缓存行内的滑动窗口复用。
    static bool hasSlidingWindowBenefit(
        const UnrollAndJamShape &Shape, LoopAccessInfo &LA,
        SCEVResult &SE,
        const std::unordered_set<Instruction *> &LaneValues,
        int Factor, const UnrollAndJamConfig &Config);
    //递归判断值链是否构成严格的浮点加减归约回路。
    static bool isStrictFPRecurrencePath(
        Value *Current, PhiInst *Phi,
        std::unordered_set<Value *> &Active, unsigned Depth = 0);
    //统计候选嵌套中需要为每个lane保留的严格浮点归约数量。
    static unsigned countStrictFPReductions(
        const UnrollAndJamShape &Shape);
    //估算克隆后的整数与浮点活跃值是否落在寄存器预算内。
    static bool fitsRegisterBudget(const UnrollAndJamShape &Shape,
                                   int Factor,
                                   const UnrollAndJamConfig &Config);
    //判断展开因子是否属于实现支持的2、4或8。
    static bool isSupportedFactor(int Factor);
    //结合显式配置、收益模式和资源预算选择最终展开因子。
    static std::optional<int> selectFactor(
        const UnrollAndJamShape &Shape, LoopAccessInfo &LA,
        SCEVResult &SE, const UnrollAndJamConfig &Config);
    //2.5 建立候选形状并把所有受外 IV 影响的非控制指令收入 lane 克隆集。
    //构造受外归纳变量影响且对使用封闭的lane指令克隆集合。
    static bool collectLaneInstructions(UnrollAndJamShape &Shape,
                                        const UnrollAndJamConfig &Config);
    //从相邻循环建立通过全部结构、控制和访存门禁的候选形状。
    static std::optional<UnrollAndJamShape> buildShape(
        Loop *Outer, Loop *Inner, LoopInfo &LI, SCEVResult &SE,
        LoopAccessInfo &LA, DominatorTree &DT, PostDominatorTree &PDT,
        const UnrollAndJamConfig &Config);
    //4.3 计划失败清理保证先断开 operand/use 关系，再销毁所有游离指令。
    static void destroyClonePlan(UnrollAndJamClonePlan &Plan);
    //依据当前lane值映射重新绑定全部克隆指令的操作数。
    static bool remapClonedOperands(UnrollAndJamLanePlan &Lane);
    //把相邻lane的克隆GEP规范化为原地址加减lane偏移。
    static bool canonicalizeLaneMemoryAddresses(
        const UnrollAndJamShape &Shape, LoopAccessInfo &LA,
        UnrollAndJamLanePlan &Lane, int LaneIndex);
    //在循环预头中物化滑动窗口首值所需的循环不变表达式。
    static Value *materializeSlidingInitValue(
        Value *Source, Loop *Axis, PhiInst *AxisIV,
        Value *AxisStart, BasicBlock *PreHeader,
        const DominatorTree &DT,
        std::unordered_map<Value *, Value *> &ValueMap,
        std::vector<Instruction *> &NewInstructions,
        std::unordered_set<Value *> &Active);
    //把相邻lane的重复读取改写为由上一lane转发的滑动窗口值。
    static bool applySlidingWindowForwarding(
        UnrollAndJamShape &Shape, UnrollAndJamClonePlan &Plan,
        LoopAccessInfo &LA, SCEVResult &SE,
        const DominatorTree &DT,
        const UnrollAndJamConfig &Config);
    //离线构造额外lane克隆、地址映射和放大后的外循环更新值。
    static std::optional<UnrollAndJamClonePlan> buildClonePlan(
        const UnrollAndJamShape &Shape, LoopAccessInfo &LA, int Factor,
        const UnrollAndJamConfig &Config);
    //在提交前重新验证所有源对象、挂接点和游离克隆仍保持预期状态。
    static bool validatePlanCommit(const UnrollAndJamShape &Shape,
                                   const UnrollAndJamClonePlan &Plan);
    //5. 只在提交预检成功后挂接克隆并替换外循环步长，提交阶段不再保留可回滚对象。
    static bool executePlan(UnrollAndJamShape &Shape,
                            UnrollAndJamClonePlan &Plan);
};
