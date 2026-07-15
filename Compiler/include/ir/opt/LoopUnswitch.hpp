#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/IRUtils.hpp"

#include <cstdint>
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

// 循环条件外提：优先执行无代码膨胀的特化，必要时再克隆一侧循环区域。
// Pass 保存每个函数已选择的模式，避免固定点流水线重复克隆同一循环。
class LoopUnswitchPass : public PassDescriptor<LoopUnswitchPass> {
public:
    PassResult run(Function &F, FunctionAnalysisPassManager &FAM);

private:
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
        // 保留较大且通常更热的一侧原布局，只克隆另一侧。
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
        bool GeneratedByLoopTransform = false;
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
    static unsigned countControlUses(Value *Condition, Loop &L);
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
    static bool hasLoopTransformProvenance(
        const std::vector<BasicBlock *> &Blocks);
    static unsigned invariantScope(Value *Condition, Loop *L,
                                   const DominatorTree &DT);
    static bool chooseDirectionalClonePlan(NonTrivialCandidate &C);
    static std::optional<NonTrivialCandidate> findBestNonTrivialCandidate(
        Function &F, LoopInfo &LI, const DominatorTree &DT,
        SCEVResult &SE);
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

    std::unordered_map<Function *, FunctionState> States;
};
