// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/IRUtils.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
 
//DominatorTree表示函数级支配关系分析结果
class DominatorTree;
//Loop表示一个自然循环
class Loop;
//LoopInfo表示函数的循环森林分析结果
class LoopInfo;
//MemorySSAResult表示函数的内存SSA分析结果
class MemorySSAResult;
//SCEVResult表示函数的标量演化分析结果
class SCEVResult;

//LoopUnswitchPass将循环不变分支外提或版本化以消除循环内动态控制
class LoopUnswitchPass : public PassDescriptor<LoopUnswitchPass> {
public:
    //运行循环非交换变换并返回IR和CFG修改状态
    PassResult run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    //限制非平凡特化最多克隆的基本块数量
    static constexpr std::size_t MaxNonTrivialCloneBlocks = 32;
    //限制非平凡特化最多克隆的指令数量
    static constexpr std::size_t MaxNonTrivialCloneInstructions = 192;
    //限制克隆区域占原函数加权规模的最大百分比
    static constexpr std::size_t MaxClonePercentOfFunction = 30;
    //没有可用次数信息时采用的保守循环次数估计
    static constexpr std::int64_t UnknownTripCountEstimate = 1;
    //收益模型允许使用的最大循环次数
    static constexpr std::int64_t MaxTripCountForProfitability = 64;
    //每次迭代消除一个必经控制使用获得的抽象收益
    static constexpr std::int64_t ControlUseCost = 1;
    //入口新增一次版本选择分派产生的抽象成本
    static constexpr std::int64_t EntryDispatchCost = 1;
    //每个克隆基本块产生的额外布局和跳转成本
    static constexpr std::int64_t ClonedBlockCost = 3;
    //每个需要版本合流的出口PHI产生的抽象成本
    static constexpr std::int64_t ExitPhiVersioningCost = 4;
    //整数谓词结果中小于关系对应的位掩码
    static constexpr unsigned RelationLessMask = 1U;
    //整数谓词结果中等于关系对应的位掩码
    static constexpr unsigned RelationEqualMask = 2U;
    //整数谓词结果中大于关系对应的位掩码
    static constexpr unsigned RelationGreaterMask = 4U;
    //整数谓词区间推理使用的最小i32边界
    static constexpr std::int64_t PredicateMinimum =
        static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min());
    //整数谓词区间推理使用的最大i32边界
    static constexpr std::int64_t PredicateMaximum =
        static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max());

    //BlockSet表示一组去重的基本块
    using BlockSet = std::unordered_set<BasicBlock *>;
    //ClosedInterval表示包含两个端点的有符号整数闭区间
    using ClosedInterval = std::pair<std::int64_t, std::int64_t>;

    //SelectedKind枚举函数在固定点迭代中已经选定的非交换模式
    enum class SelectedKind {
        //当前函数尚未选择非交换模式
        None,
        //当前函数选择不克隆循环的平凡模式
        Trivial,
        //当前函数选择需要克隆循环区域的非平凡模式
        NonTrivial
    };

    //FunctionState记录一个函数跨固定点调用保留的模式与克隆额度
    struct FunctionState {
        //当前函数已经选定的非交换模式
        SelectedKind Kind = SelectedKind::None;
        //当前函数已经执行的完整克隆次数
        unsigned FullCloneCount = 0;
    };

    //CanonicalLoopShape保存LoopSimplify循环的头、预头和回边块
    struct CanonicalLoopShape {
        //循环头基本块
        BasicBlock *Header = nullptr;
        //循环唯一预头基本块
        BasicBlock *PreHeader = nullptr;
        //循环唯一回边基本块
        BasicBlock *Latch = nullptr;
    };

    //TrivialCandidate描述能够把退出或继续分支直接外提到预头的候选
    struct TrivialCandidate {
        //候选所属的循环
        Loop *L = nullptr;
        //候选循环的规范控制流形状
        CanonicalLoopShape Shape;
        //包含候选条件分支的循环内基本块
        BasicBlock *Parent = nullptr;
        //候选条件分支指令
        BrInst *Branch = nullptr;
        //循环不变的布尔分支条件
        Value *Condition = nullptr;
        //候选分支离开循环时到达的基本块
        BasicBlock *Exit = nullptr;
        //候选分支继续循环时到达的基本块
        BasicBlock *Continue = nullptr;
        //标记条件为真时是否离开循环
        bool ExitOnTrue = false;
    };

    //EntryKnownCandidate描述入口守卫已经确定条件值的无克隆候选
    struct EntryKnownCandidate {
        //候选所属的循环
        Loop *L = nullptr;
        //在循环入口处已经确定的布尔条件
        Value *Condition = nullptr;
        //循环及其全部子循环包含的基本块
        std::vector<BasicBlock *> AllBlocks;
        //条件进入当前循环时的已知布尔值
        bool KnownValue = false;
    };

    //NonTrivialCandidate描述需要克隆一侧可达闭包的循环版本化候选
    struct NonTrivialCandidate {
        //候选所属的循环
        Loop *L = nullptr;
        //候选循环的规范控制流形状
        CanonicalLoopShape Shape;
        //包含候选条件分支的循环内基本块
        BasicBlock *Parent = nullptr;
        //候选条件分支指令
        BrInst *Branch = nullptr;
        //循环不变的布尔分支条件
        Value *Condition = nullptr;
        //条件为真时进入的后继基本块
        BasicBlock *TrueSucc = nullptr;
        //条件为假时进入的后继基本块
        BasicBlock *FalseSucc = nullptr;
        //循环及其全部子循环包含的基本块
        std::vector<BasicBlock *> AllBlocks;
        //条件恒真时在循环内仍然可达的基本块集合
        BlockSet TrueReachable;
        //条件恒假时在循环内仍然可达的基本块集合
        BlockSet FalseReachable;
        //原循环版本采用的已知条件值
        bool OriginalValue = false;
        //原循环版本从候选分支选择的后继
        BasicBlock *OriginalSucc = nullptr;
        //克隆循环版本从候选分支选择的后继
        BasicBlock *CloneSucc = nullptr;
        //原循环版本保留的可达基本块集合
        BlockSet OriginalReachable;
        //克隆循环版本保留的可达基本块集合
        BlockSet CloneReachable;
        //消除循环内动态控制得到的估计收益
        std::int64_t Benefit = 0;
        //入口分派、克隆和出口版本化产生的估计成本
        std::int64_t Cost = 0;
        //候选条件在循环中必经的控制使用数量
        unsigned ControlUses = 0;
        //克隆可达闭包包含的指令数量
        std::size_t CloneInstructions = 0;
        //克隆可达闭包的加权静态规模
        std::size_t CloneWeight = 0;
    };

    //IntegerBoundPredicate描述规范化后的变量与i32常量边界比较
    struct IntegerBoundPredicate {
        //参与整数边界比较的非恒定变量
        Value *Variable = nullptr;
        //变量位于左侧时采用的规范化比较谓词
        BinaryInst::Operation Predicate = BinaryInst::E;
        //参与比较的i32常量边界
        std::int32_t Bound = 0;
    };

    //PhiPrunePlan描述游离克隆中一个PHI的剪枝或替换方案
    struct PhiPrunePlan {
        //克隆来源对应的原始PHI
        PhiInst *Original = nullptr;
        //当前需要剪枝的克隆PHI
        PhiInst *Cloned = nullptr;
        //剪枝后仍然有效的输入边集合
        std::vector<PhiInst::IncomingEdge> Incoming;
        //单一输入或无输入时替换克隆PHI的值
        Value *Replacement = nullptr;
        //保留多个有效输入时离线创建的新PHI
        PhiInst *NewPhi = nullptr;
    };

    //VersionedExitPhiPlan描述一个循环出口PHI在两个版本中的输入
    struct VersionedExitPhiPlan {
        //版本化前的原始出口PHI
        PhiInst *OldPhi = nullptr;
        //来自原循环版本的有效输入边
        std::vector<PhiInst::IncomingEdge> OriginalIncoming;
        //来自克隆循环版本的有效输入边
        std::vector<PhiInst::IncomingEdge> CloneIncoming;
    };

    //VersionedExitPlan描述一个循环出口的代理块、前驱和PHI修复计划
    struct VersionedExitPlan {
        //需要版本化修复的循环出口基本块
        BasicBlock *Exit = nullptr;
        //原循环版本到达出口的前驱块
        std::vector<BasicBlock *> OriginalPreds;
        //克隆循环版本到达出口的前驱块
        std::vector<BasicBlock *> ClonePreds;
        //出口中每个PHI的版本化修复计划
        std::vector<VersionedExitPhiPlan> PhiPlans;
    };

    //判断给定值是否为非常量布尔值
    static bool isBooleanNonConstant(Value *V);
    //判断给定值是否能在指定循环内使用点获得且支配该使用
    static bool isAvailableAt(Value *V, Instruction *UsePoint,
                              const Loop &L, const DominatorTree &DT);
    //检查循环是否满足规范形状并填充头、预头和回边块
    static bool getCanonicalLoopShape(Loop *L, CanonicalLoopShape &Shape);
    //判断基本块是否直接属于给定循环而非其子循环
    static bool isDirectLoopBlock(const LoopInfo &LI, Loop *L,
                                  BasicBlock *BB);
    //收集给定循环中所有能够直接离环的基本块
    static std::vector<BasicBlock *> collectExitingBlocks(Loop &L);
    //判断候选分支块是否在循环取得进展前必定执行
    static bool isMustExecuteBeforeLoopProgress(BasicBlock *Parent, Loop &L,
                                                const DominatorTree &DT);
    //检查平凡外提前缀是否无内存定义、故障和可观察副作用
    static bool isSafeTrivialPrefix(const TrivialCandidate &C,
                                    const LoopInfo &LI,
                                    const DominatorTree &DT,
                                    const MemorySSAResult &MSSA);
    //检查平凡外提后出口PHI是否能够保持合法SSA输入
    static bool validateTrivialExitPhis(const TrivialCandidate &C,
                                        const DominatorTree &DT);
    //扫描循环森林并返回首个合法平凡非交换候选
    static std::optional<TrivialCandidate> findTrivialCandidate(
        Function &F, LoopInfo &LI, const DominatorTree &DT,
        const MemorySSAResult &MSSA);
    //将指定块集合内对条件的使用替换为已知布尔常量
    static bool replaceConditionUsesInBlocks(Value *Condition,
                                             const BlockSet &Blocks,
                                             bool KnownValue);
    //把循环Header PHI中旧预头对应的前驱替换为新预头
    static void setHeaderPredecessor(BasicBlock *Header,
                                     BasicBlock *OldPreHeader,
                                     BasicBlock *NewPreHeader);
    //提交平凡非交换并修复入口控制流与出口LCSSA值
    static bool transformTrivial(Function &F, const TrivialCandidate &C);
    //收集选择指定分支后仍在循环内可达的特化闭包
    static BlockSet collectSpecializedReachable(Loop &L, BasicBlock *Parent,
                                                BasicBlock *ChosenSuccessor);
    //判断给定CFG边是否在指定分支特化后仍然有效
    static bool isActiveSpecializedEdge(BasicBlock *Pred, BasicBlock *Succ,
                                        BasicBlock *Parent,
                                        BasicBlock *ChosenSuccessor);
    //统计给定块列表或过滤集合中的指令数量
    static std::size_t countInstructions(
        const std::vector<BasicBlock *> &Blocks,
        const BlockSet *Filter = nullptr);
    //统计循环内由候选条件控制且必经的动态使用数量
    static unsigned countMustExecuteControlUses(
        Value *Condition, Loop &L, const DominatorTree &DT);
    //剥离布尔取反链并记录最终条件是否反相
    static Value *stripBooleanNot(Value *V, bool &Inverted);
    //尝试把给定条件求值为编译期布尔常量
    static std::optional<bool> evaluateConstantBoolean(Value *Condition);
    //将整数比较谓词映射为小于、等于和大于关系掩码
    static unsigned predicateRelationMask(BinaryInst::Operation Op);
    //返回整数比较谓词成立时变量允许落入的闭区间集合
    static std::vector<ClosedInterval> predicateIntervals(
        BinaryInst::Operation Op, std::int64_t Bound);
    //判断闭区间集合A是否完全包含于闭区间集合B
    static bool intervalsAreSubset(const std::vector<ClosedInterval> &A,
                                   const std::vector<ClosedInterval> &B);
    //尝试将整数比较规范化为变量与i32常量边界的谓词
    static std::optional<IntegerBoundPredicate> normalizeIntegerBound(Value *V);
    //根据已知守卫结果证明基础目标条件的布尔值
    static std::optional<bool> proveBaseConditionFromGuard(
        Value *Target, Value *Guard, bool GuardKnown);
    //剥离布尔取反后根据已知守卫结果证明目标条件
    static std::optional<bool> proveConditionFromGuard(
        Value *Target, Value *Guard, bool GuardKnown);
    //判断条件在守卫块到循环预头之间是否保持稳定
    static bool conditionIsStableAcrossGuardLoop(
        Value *Condition, BasicBlock *GuardBB, BasicBlock *PreHeader,
        const LoopInfo &LI, const DominatorTree &DT);
    //沿循环入口控制链和支配守卫推导条件的已知值
    static std::optional<bool> getEntryKnownValue(
        Value *Condition, BasicBlock *PreHeader, const LoopInfo &LI,
        const DominatorTree &DT);
    //检查克隆闭包在CFG和SSA使用上是否保持封闭合法
    static bool cloneClosureIsLegal(const NonTrivialCandidate &C);
    //判断给定基本块集合中是否包含任意函数调用
    static bool hasAnyCall(const std::vector<BasicBlock *> &Blocks);
    //估算循环所有出口PHI版本化产生的抽象成本
    static std::size_t estimateExitPhiCost(Loop &L);
    //检查非平凡版本化后所有出口PHI是否能够合法修复
    static bool validateVersionedExitPhis(const NonTrivialCandidate &C);
    //扫描循环森林并收集入口条件已经确定的无克隆候选
    static std::vector<EntryKnownCandidate> findEntryKnownCandidates(
        LoopInfo &LI, const DominatorTree &DT);
    //按照函数布局顺序收集全部基本块
    static std::vector<BasicBlock *> collectFunctionBlocks(Function &F);
    //计算给定块列表或过滤区域的加权静态规模
    static std::size_t weightedRegionSize(
        const std::vector<BasicBlock *> &Blocks,
        const BlockSet *Filter = nullptr);
    //选择保留较大区域并克隆较小区域的方向性版本方案
    static bool chooseDirectionalClonePlan(NonTrivialCandidate &C);
    //扫描并返回收益严格覆盖静态成本的最佳非平凡候选
    static std::optional<NonTrivialCandidate> findBestNonTrivialCandidate(
        Function &F, LoopInfo &LI, const DominatorTree &DT,
        SCEVResult &SE);
    //通过值映射返回原值在克隆区域中的对应值
    static Value *mapValue(Value *V, const ValueToValueMap &VMap);
    //按照特化可达边剪枝游离克隆中的PHI并修复其使用
    static bool pruneClonedPhis(ClonedBlockRegion &Region,
                                const NonTrivialCandidate &C,
                                BasicBlock *ClonedPreHeader);
    //销毁尚未提交到函数中的一组游离基本块
    static void destroyBlocks(std::vector<BasicBlock *> Blocks);
    //在代理块中根据多条输入构建出口值或直接返回单一输入
    static PhiInst *buildExitProxyValue(
        const std::shared_ptr<IRType> &Ty,
        const std::vector<PhiInst::IncomingEdge> &Incoming,
        BasicBlock *Proxy, const std::string &Name);
    //离线规划单个循环出口的双版本前驱和PHI修复
    static std::optional<VersionedExitPlan> planVersionedExit(
        BasicBlock *Exit, const NonTrivialCandidate &C,
        ClonedBlockRegion &Region);
    //提交一个出口版本化计划并重建出口前驱与PHI
    static void applyVersionedExitPlan(Function &F,
                                       VersionedExitPlan &Plan);
    //删除原循环版本中特化后不可达且无外部前驱的死块
    static void eraseDeadOriginalBlocks(
        const std::vector<BasicBlock *> &AllBlocks,
        const BlockSet &LiveBlocks);
    //将入口已知条件替换到循环全部基本块而不克隆CFG
    static bool transformEntryKnown(const EntryKnownCandidate &C);
    //事务式克隆一侧特化闭包并修复入口、PHI和所有出口
    static bool transformNonTrivial(Function &F, NonTrivialCandidate &C);

    //记录每个函数跨固定点调用保留的模式和完整克隆额度
    std::unordered_map<Function *, FunctionState> States;
};
