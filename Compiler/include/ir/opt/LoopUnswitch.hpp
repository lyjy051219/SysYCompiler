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

class DominatorTree;
class Loop;
class LoopInfo;
class MemorySSAResult;
class SCEVResult;

// LoopUnswitch 的整体流程：
// 1. 每个函数在固定点轮次间保留所选模式，优先尝试 trivial，选定后不再切换到克隆模式。
// 2. 所有候选都要求 LoopSimplify + LCSSA、循环不变布尔条件在 preheader 可用，且循环子树不能到达计时调用。
// 3. trivial 路径将“继续/退出循环”分支移到 preheader，证明前缀必经且无内存定义、故障或可观察指令。
// 4. entry-known 路径从动态入口链或支配 guard 证明条件常量，批量替换循环内 use，不克隆代码。
// 5. non-trivial 路径分别求两个分支特化的可达闭包，保留较大一侧并克隆较小一侧，再以动态控制收益严格覆盖静态增长为准入。
// 6. 克隆、PHI 剪枝和版本化出口均先离线计划；提交前失败销毁全部克隆，成功后统一使函数分析失效。
class LoopUnswitchPass : public PassDescriptor<LoopUnswitchPass> {
public:
    PassResult run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    // 5.4 非平凡克隆同时受块数、指令数和函数规模百分比限制，并统一使用抽象指令成本。
    static constexpr std::size_t MaxNonTrivialCloneBlocks = 32;
    static constexpr std::size_t MaxNonTrivialCloneInstructions = 192;
    static constexpr std::size_t MaxClonePercentOfFunction = 30;
    // 5.4.1 无 profile 时未知 trip count 只估一次，已知次数也饱和到上限，防止收益无界放大。
    static constexpr std::int64_t UnknownTripCountEstimate = 1;
    static constexpr std::int64_t MaxTripCountForProfitability = 64;
    // 5.4.2 收益计算为每迭代消除的必经控制 use，再扣除一次入口版本分派。
    static constexpr std::int64_t ControlUseCost = 1;
    static constexpr std::int64_t EntryDispatchCost = 1;
    // 5.4.3 静态成本除指令外还计入块布局/跳转和出口 PHI 版本合流压力。
    static constexpr std::int64_t ClonedBlockCost = 3;
    static constexpr std::int64_t ExitPhiVersioningCost = 4;
    static constexpr unsigned RelationLessMask = 1U;
    static constexpr unsigned RelationEqualMask = 2U;
    static constexpr unsigned RelationGreaterMask = 4U;
    static constexpr std::int64_t PredicateMinimum =
        static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min());
    static constexpr std::int64_t PredicateMaximum =
        static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max());

    using BlockSet = std::unordered_set<BasicBlock *>;
    using ClosedInterval = std::pair<std::int64_t, std::int64_t>;

    enum class SelectedKind {
        None,
        Trivial,
        NonTrivial
    };

    struct FunctionState {
        SelectedKind Kind = SelectedKind::None;
        unsigned FullCloneCount = 0;
    };

    struct CanonicalLoopShape {
        BasicBlock *Header = nullptr;
        BasicBlock *PreHeader = nullptr;
        BasicBlock *Latch = nullptr;
    };

    struct TrivialCandidate {
        Loop *L = nullptr;
        CanonicalLoopShape Shape;
        BasicBlock *Parent = nullptr;
        BrInst *Branch = nullptr;
        Value *Condition = nullptr;
        BasicBlock *Exit = nullptr;
        BasicBlock *Continue = nullptr;
        bool ExitOnTrue = false;
    };

    struct EntryKnownCandidate {
        Loop *L = nullptr;
        Value *Condition = nullptr;
        std::vector<BasicBlock *> AllBlocks;
        bool KnownValue = false;
    };

    struct NonTrivialCandidate {
        Loop *L = nullptr;
        CanonicalLoopShape Shape;
        BasicBlock *Parent = nullptr;
        BrInst *Branch = nullptr;
        Value *Condition = nullptr;
        BasicBlock *TrueSucc = nullptr;
        BasicBlock *FalseSucc = nullptr;
        std::vector<BasicBlock *> AllBlocks;
        BlockSet TrueReachable;
        BlockSet FalseReachable;
        // 5.2 不猜测分支热度：Original 固定保留加权较大的区域，Clone 固定选较小的区域。
        bool OriginalValue = false;
        BasicBlock *OriginalSucc = nullptr;
        BasicBlock *CloneSucc = nullptr;
        BlockSet OriginalReachable;
        BlockSet CloneReachable;
        std::int64_t Benefit = 0;
        std::int64_t Cost = 0;
        unsigned ControlUses = 0;
        std::size_t CloneInstructions = 0;
        std::size_t CloneWeight = 0;
    };

    struct IntegerBoundPredicate {
        Value *Variable = nullptr;
        BinaryInst::Operation Predicate = BinaryInst::E;
        std::int32_t Bound = 0;
    };

    struct PhiPrunePlan {
        PhiInst *Original = nullptr;
        PhiInst *Cloned = nullptr;
        std::vector<PhiInst::IncomingEdge> Incoming;
        Value *Replacement = nullptr;
        PhiInst *NewPhi = nullptr;
    };

    struct VersionedExitPhiPlan {
        PhiInst *OldPhi = nullptr;
        std::vector<PhiInst::IncomingEdge> OriginalIncoming;
        std::vector<PhiInst::IncomingEdge> CloneIncoming;
    };

    struct VersionedExitPlan {
        BasicBlock *Exit = nullptr;
        std::vector<BasicBlock *> OriginalPreds;
        std::vector<BasicBlock *> ClonePreds;
        std::vector<VersionedExitPhiPlan> PhiPlans;
    };

    static bool isBooleanNonConstant(Value *V);
    static bool isAvailableAt(Value *V, Instruction *UsePoint,
                              const Loop &L, const DominatorTree &DT);
    static bool getCanonicalLoopShape(Loop *L, CanonicalLoopShape &Shape);
    // 2.1 实现所有路径共享的形状、条件可用性、必经性和计时调用门禁。
    static bool loopMayReachTimingCall(const Loop *L);
    static bool isDirectLoopBlock(const LoopInfo &LI, Loop *L,
                                  BasicBlock *BB);
    static std::vector<BasicBlock *> collectExitingBlocks(Loop &L);
    static bool isMustExecuteBeforeLoopProgress(BasicBlock *Parent, Loop &L,
                                                const DominatorTree &DT);
    static bool isSafeTrivialPrefix(const TrivialCandidate &C,
                                    const LoopInfo &LI,
                                    const DominatorTree &DT,
                                    const MemorySSAResult &MSSA);
    static bool validateTrivialExitPhis(const TrivialCandidate &C,
                                        const DominatorTree &DT);
    static std::optional<TrivialCandidate> findTrivialCandidate(
        Function &F, LoopInfo &LI, const DominatorTree &DT,
        const MemorySSAResult &MSSA);
    // 3.2 提交 trivial 外提，并在出口有其他前驱时重建 LCSSA/合流 PHI。
    static bool replaceConditionUsesInBlocks(Value *Condition,
                                             const BlockSet &Blocks,
                                             bool KnownValue);
    static void setHeaderPredecessor(BasicBlock *Header,
                                     BasicBlock *OldPreHeader,
                                     BasicBlock *NewPreHeader);
    static bool transformTrivial(Function &F, const TrivialCandidate &C);
    static BlockSet collectSpecializedReachable(Loop &L, BasicBlock *Parent,
                                                BasicBlock *ChosenSuccessor);
    static bool isActiveSpecializedEdge(BasicBlock *Pred, BasicBlock *Succ,
                                        BasicBlock *Parent,
                                        BasicBlock *ChosenSuccessor);
    static std::size_t countInstructions(
        const std::vector<BasicBlock *> &Blocks,
        const BlockSet *Filter = nullptr);
    static unsigned countMustExecuteControlUses(
        Value *Condition, Loop &L, const DominatorTree &DT);
    static Value *stripBooleanNot(Value *V, bool &Inverted);
    static std::optional<bool> evaluateConstantBoolean(Value *Condition);
    static unsigned predicateRelationMask(BinaryInst::Operation Op);
    static std::vector<ClosedInterval> predicateIntervals(
        BinaryInst::Operation Op, std::int64_t Bound);
    static bool intervalsAreSubset(const std::vector<ClosedInterval> &A,
                                   const std::vector<ClosedInterval> &B);
    static std::optional<IntegerBoundPredicate> normalizeIntegerBound(Value *V);
    static std::optional<bool> proveBaseConditionFromGuard(
        Value *Target, Value *Guard, bool GuardKnown);
    static std::optional<bool> proveConditionFromGuard(
        Value *Target, Value *Guard, bool GuardKnown);
    static bool conditionIsStableAcrossGuardLoop(
        Value *Condition, BasicBlock *GuardBB, BasicBlock *PreHeader,
        const LoopInfo &LI, const DominatorTree &DT);
    static std::optional<bool> getEntryKnownValue(
        Value *Condition, BasicBlock *PreHeader, const LoopInfo &LI,
        const DominatorTree &DT);
    // 5.1 证明两个特化可达闭包的 SSA/CFG 封闭性，评估预算并选出最优候选。
    static bool cloneClosureIsLegal(const NonTrivialCandidate &C);
    static bool hasAnyCall(const std::vector<BasicBlock *> &Blocks);
    static std::size_t estimateExitPhiCost(Loop &L);
    static bool validateVersionedExitPhis(const NonTrivialCandidate &C);
    static std::vector<EntryKnownCandidate> findEntryKnownCandidates(
        LoopInfo &LI, const DominatorTree &DT);
    static std::vector<BasicBlock *> collectFunctionBlocks(Function &F);
    static std::size_t weightedRegionSize(
        const std::vector<BasicBlock *> &Blocks,
        const BlockSet *Filter = nullptr);
    static bool chooseDirectionalClonePlan(NonTrivialCandidate &C);
    static std::optional<NonTrivialCandidate> findBestNonTrivialCandidate(
        Function &F, LoopInfo &LI, const DominatorTree &DT,
        SCEVResult &SE);
    // 6.1 在游离克隆上剪枝 PHI、计划版本出口；任一预检失败都可销毁整个区域。
    static Value *mapValue(Value *V, const ValueToValueMap &VMap);
    static bool pruneClonedPhis(ClonedBlockRegion &Region,
                                const NonTrivialCandidate &C,
                                BasicBlock *ClonedPreHeader);
    static void destroyBlocks(std::vector<BasicBlock *> Blocks);
    static PhiInst *buildExitProxyValue(
        const std::shared_ptr<IRType> &Ty,
        const std::vector<PhiInst::IncomingEdge> &Incoming,
        BasicBlock *Proxy, const std::string &Name);
    static std::optional<VersionedExitPlan> planVersionedExit(
        BasicBlock *Exit, const NonTrivialCandidate &C,
        ClonedBlockRegion &Region);
    static void applyVersionedExitPlan(Function &F,
                                       VersionedExitPlan &Plan);
    static void eraseDeadOriginalBlocks(
        const std::vector<BasicBlock *> &AllBlocks,
        const BlockSet &LiveBlocks);
    static bool transformEntryKnown(const EntryKnownCandidate &C);
    static bool transformNonTrivial(Function &F, NonTrivialCandidate &C);

    // 1.1 状态跨固定点调用保留，记录函数已选模式和是否已消耗唯一完整克隆额度。
    std::unordered_map<Function *, FunctionState> States;
};
