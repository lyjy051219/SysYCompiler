#pragma once

#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

enum class UnrollKind {
    None,
    Full,
    Partial,
    PartialWithRemainder,
    Runtime,
    Peeling
};

enum class LoopUnrollResult {
    Unmodified,
    PartiallyUnrolled,
    FullyUnrolled
};

struct UnrollConfig {
    bool AllowFull = true;
    bool AllowPartial = true;
    bool AllowRuntime = true;
    bool AllowRemainder = true;
    bool AllowPeeling = true;

    bool UnrollRemainder = false;

    int FullThreshold = 192;
    int PartialThreshold = 160;
    int RuntimeThreshold = 96;

    int FullUnrollMaxCount = 16;
    int TinyLoopFullUnrollMaxCount = 32;

    int PartialMaxCount = 4;
    int RuntimeDefaultCount = 4;
    int RuntimeMaxCount = 4;

    int MaxLoopBodyCostForPartial = 72;
    int MaxLoopBodyCostForRuntime = 48;

    int MaxPeelCount = 2;
    int MaxGuardedPeelCount = 1;
    int PeelThreshold = 64;
    int MaxPeelBlocks = 24;

    int SCEVExpansionBudget = 8;
    bool AllowExpensiveTripCount = false;
};

struct UnrollDecision {
    UnrollKind Kind = UnrollKind::None;

    int Count = 1;

    bool ShouldUnroll = false;
    bool NeedRemainder = false;
    bool IsRuntime = false;
    bool IsPeeling = false;
    // Required by runtime count materialization.  This may come from a global
    // SCEV no-wrap proof or from contextual ranges at the loop preheader.
    bool TripCountIsProven = false;

    // A guarded peel may prove that a non-IV header PHI reaches an
    // outer-loop-invariant fixed point after the peeled iteration.  Keeping
    // the proof result in the decision lets executePeeling specialize only
    // the residual loop; cloned first-iteration uses have already been
    // remapped to the original initial value.
    PhiInst *StabilizedPhi = nullptr;
    Value *StabilizedValue = nullptr;
    // Some one-shot state loops converge to max(Initial, Bound), rather than
    // to Bound unconditionally.  Keep the two pre-loop operands in the
    // decision and materialize the exact value only after peeling commits;
    // decision making itself must never mutate IR.
    Value *StabilizedInitial = nullptr;
    Value *StabilizedBound = nullptr;

    std::string Reason;
};

struct PeelPredicate {
    BinaryInst::Operation Op = BinaryInst::E;
    int64_t Constant = 0;
};

struct LoopExecutionShape {
    BasicBlock *PreHeader = nullptr;
    BasicBlock *Header = nullptr;
    BasicBlock *Latch = nullptr;
    BasicBlock *Exiting = nullptr;
    BasicBlock *Exit = nullptr;
};

struct UnrolledBodyCore {
    LoopExecutionShape Shape;
    int Count = 0;
    std::vector<std::vector<BasicBlock *>> IterBlocks;
    std::vector<BasicBlock *> Headers;
    std::vector<BasicBlock *> Latches;
    std::vector<BasicBlock *> Exitings;
    std::vector<std::unordered_map<Value *, Value *>> ValueMaps;
};

struct PeeledBodyCore {
    LoopExecutionShape Shape;
    int PeelCount = 0;
    std::vector<std::vector<BasicBlock *>> IterBlocks;
    std::vector<BasicBlock *> Headers;
    std::vector<BasicBlock *> Latches;
    std::vector<BasicBlock *> Exitings;
    std::vector<std::unordered_map<Value *, Value *>> ValueMaps;
};

struct ExitRegionDataUse {
    Use *OperandUse = nullptr;
    Value *OriginalValue = nullptr;
    Value *LookupValue = nullptr;
    std::shared_ptr<IRType> ExpectedType = nullptr;
};

struct HeaderState {
    PhiInst *OriginalPhi = nullptr;
    PhiInst *CurrentPhi = nullptr;
    Value *CurrentValue = nullptr;
    Value *InitialValue = nullptr;
    Value *BackedgeValue = nullptr;
    Value *MainValue = nullptr;
    Value *TailStateValue = nullptr;
    int TailStartIter = std::numeric_limits<int>::max();
    bool MainValueIsOriginalPhi = false;
    bool BackedgeIsOriginalPhi = false;
};

struct PhiRewritePlan {
    PhiInst *OldPhi = nullptr;
    std::vector<PhiInst::IncomingEdge> Incoming;
    Value *Replacement = nullptr;
    PhiInst *NewPhi = nullptr;
};

class LoopUnrollPass : public PassDescriptor<LoopUnrollPass> {
public:
    explicit LoopUnrollPass(UnrollConfig Config = UnrollConfig());

    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

    const UnrollConfig &getConfig() const { return Config; }

    LoopUnrollResult tryToUnrollLoop(Function &F,
                                     Loop *L,
                                     LoopInfo &LI,
                                     DominatorTree &DT,
                                     SCEVResult &SE,
                                     BasicAAResult &AA,
                                     RangeInfo &RI);

    bool isCanonicalLoop(Loop *L) const;
    bool canCloneLoopBody(Loop *L, LoopInfo &LI) const;

    int estimateLoopBodyCost(Loop *L, LoopInfo &LI) const;
    bool containsCall(Loop *L, LoopInfo &LI) const;

    int estimateSCEVCost(const SCEVExpr *E) const;
    bool isCheapSCEV(const SCEVExpr *E, const UnrollConfig &Config) const;

    int detectPeelCount(Loop *L,
                        const InductionDescriptor &IV,
                        const UnrollConfig &Config) const;

    UnrollDecision computeUnrollDecision(Loop *L,
                                         const LoopTripCountInfo &TCI,
                                         int BodyCost,
                                         const UnrollConfig &Config,
                                         LoopInfo &LI,
                                         SCEVResult &SE,
                                         BasicAAResult &AA,
                                         RangeInfo &RI,
                                         DominatorTree &DT) const;

    LoopUnrollResult executeFullUnroll(Loop *L,
                                       const UnrollDecision &Decision,
                                       SCEVResult &SE,
                                       LoopInfo &LI,
                                       DominatorTree &DT);

    LoopUnrollResult executePartialUnroll(Loop *L,
                                          const UnrollDecision &Decision,
                                          SCEVResult &SE,
                                          LoopInfo &LI,
                                          DominatorTree &DT);

    LoopUnrollResult executePartialWithRemainder(Loop *L,
                                                 const UnrollDecision &Decision,
                                                 SCEVResult &SE,
                                                 LoopInfo &LI,
                                                 DominatorTree &DT);

    LoopUnrollResult executeRuntimeUnroll(Loop *L,
                                          const UnrollDecision &Decision,
                                          SCEVResult &SE,
                                          LoopInfo &LI,
                                          DominatorTree &DT);

    LoopUnrollResult executePeeling(Loop *L,
                                    const UnrollDecision &Decision,
                                    SCEVResult &SE,
                                    LoopInfo &LI,
                                    DominatorTree &DT,
                                    BasicAAResult &AA);

private:
    UnrollConfig Config;

    // The loop pipeline itself is a bounded fixed point and reuses this pass
    // object across rounds.  Remember original headers already transformed so
    // a partial unroll is not multiplied again (for example 4x -> 16x -> 32x)
    // merely because the outer pipeline ran another cleanup round.
    std::unordered_map<Function *, std::unordered_set<std::string>>
        TransformedLoopHeaders;

    static int nextUnrollSerial();
    static std::string makeUnrollBlockIdent(
        Function *F, BasicBlock *BB, int Iter, int UnrollSerial);
    static std::string makePeelBlockIdent(
        Function *F, BasicBlock *BB, int Iter, int PeelSerial);
    static bool isSupportedCloneInstruction(Instruction *I);
    static Value *stripSimpleCast(Value *V);
    static std::optional<PeelPredicate> matchCompareWithIV(
        BinaryInst *Cmp, Value *IV);
    static int peelCountForPredicate(const PeelPredicate &P);
    static bool isInternalConditionalBranch(Loop *L, BrInst *Br);
    static UnrollDecision makeNoUnrollDecision(std::string Reason);
    static bool withinThreshold(int64_t Cost, int Threshold);
    static int chooseFactorBySize(
        int BodyCost, int SizeLimit, int MaxCount);
    static int localSCEVNodeCost(const SCEVExpr *E);
    static std::optional<LoopExecutionShape> getLoopExecutionShape(
        Loop *L, SCEVResult &SE);
    static bool canCloneAllInstructions(
        const std::vector<BasicBlock *> &Blocks);
    static Value *mapValueAtIteration(
        Value *V, int Iter, const UnrolledBodyCore &Core);
    static bool isBlockValue(Value *V);
    static bool isDataValue(Value *V);
    static bool isValueCompatibleWithPhi(Value *V, PhiInst *Phi);
    static bool isValueCompatibleWithType(
        Value *V, std::shared_ptr<IRType> Ty);
    static Value *mapDataValueAtIteration(
        Value *V, int Iter, const UnrolledBodyCore &Core);
    static Value *mapDataValueAtPeelIteration(
        Value *V, int Iter, const PeeledBodyCore &Core);
    static Value *getCapturedDataValueMappedAtIteration(
        Value *V, int Iter, const UnrolledBodyCore &Core);
    static bool addIncomingChecked(
        PhiInst *Phi, Value *V, BasicBlock *Pred);
    static bool rebuildPhiFromIncomingChecked(
        PhiInst *Phi, const std::vector<PhiInst::IncomingEdge> &Incoming,
        Value **Replacement = nullptr);
    static bool replaceOperandsUsingMap(
        Instruction *I,
        const std::unordered_map<Value *, Value *> &ValueMap);
    static bool replaceDataOperandsUsingMap(
        Instruction *I,
        const std::unordered_map<Value *, Value *> &ValueMap);
    static bool removeClonedHeaderPhis(
        UnrolledBodyCore &Core, int Iter);
    static bool removePeeledHeaderPhis(PeeledBodyCore &Core, int Iter);
    static std::optional<UnrolledBodyCore> cloneUnrolledBodyCore(
        Loop *L, int Count, SCEVResult &SE);
    static std::optional<PeeledBodyCore> clonePeeledIterations(
        Loop *L, int PeelCount, SCEVResult &SE);
    static bool rewriteOriginalHeaderPhisForPartial(
        UnrolledBodyCore &Core, int LastIter,
        std::unordered_map<PhiInst *, Value *> *Replacements = nullptr);
    static bool rewriteOriginalHeaderPhisAfterPeeling(
        PeeledBodyCore &Core, bool ValidateOnly = false,
        std::unordered_map<PhiInst *, Value *> *Replacements = nullptr);
    static bool removeIncomingFromExitPhisForPeeledBlocks(
        PeeledBodyCore &Core, bool ValidateOnly = false);
    static bool addIncomingToExitPhisForPeeledBlocks(
        PeeledBodyCore &Core, bool ValidateOnly = false);
    static bool hasLatchExitShape(const UnrolledBodyCore &Core);
    static std::unordered_set<BasicBlock *> collectCoreExitings(
        const UnrolledBodyCore &Core);
    static bool rewriteExitPhisForSingleExiting(
        UnrolledBodyCore &Core, int ActiveIter);
    static std::vector<BasicBlock *> collectExitRegionBlocksFrom(
        BasicBlock *Exit,
        const std::unordered_set<BasicBlock *> &StopBlocks,
        bool StopAtConditionalBranch = true);
    static std::vector<ExitRegionDataUse> collectExitRegionDataUsesFrom(
        BasicBlock *Exit,
        const std::unordered_set<BasicBlock *> &StopBlocks,
        BasicBlock *Exiting = nullptr,
        bool StopAtConditionalBranch = true);
    static bool rewriteExitRegionDataOperandsForIteration(
        UnrolledBodyCore &Core, int ActiveIter,
        const std::vector<ExitRegionDataUse> &CapturedUses,
        const std::unordered_map<Value *, Value *> *HeaderExitValues = nullptr);
    static bool replaceDataOperandsInReachableRegionFrom(
        BasicBlock *Start,
        const std::unordered_set<BasicBlock *> &StopBlocks,
        const std::unordered_map<Value *, Value *> &Replacements);
    static void refreshCapturedExitRegionUses(
        std::vector<ExitRegionDataUse> &Uses,
        const std::unordered_map<Value *, Value *> &Replacements);
    static bool removeOriginalHeaderPhisForFull(UnrolledBodyCore &Core);
    static bool makeStraightLineMain(
        UnrolledBodyCore &Core, int MainCount, BasicBlock *FinalTarget,
        Value *ContinueCondition);
    static Value *makeTypedIntConstant(
        int64_t V, std::shared_ptr<IRType> Ty);
    static bool isValueAvailableAtPreheader(
        Value *V, Loop *L, BasicBlock *PreHeader,
        const DominatorTree &DT);
    static bool canMaterializeSCEV(
        const SCEVExpr *E, Loop *L, BasicBlock *PreHeader,
        const DominatorTree &DT);
    static Value *materializeSCEV(
        const SCEVExpr *E, Loop *L, BasicBlock *PreHeader,
        const DominatorTree &DT,
        std::unordered_map<const SCEVExpr *, Value *> &Cache);
    static bool replaceValuesInIterationBlocks(
        UnrolledBodyCore &Core, int FirstIter,
        const std::unordered_map<Value *, Value *> &Replacements);
    static Value *getValueBeforeLoop(
        Value *V, const UnrolledBodyCore &Core,
        std::shared_ptr<IRType> FallbackTy);
    static BasicBlock *getNonExitSuccessor(
        BasicBlock *Exiting, BasicBlock *Exit);
    static Value *mapValueAfterIteration(
        Value *V, int Iter, const std::vector<HeaderState> &States,
        const UnrolledBodyCore &Core);
    static bool buildHeaderExitValueMap(
        const std::vector<HeaderState> &HeaderStates, int ExitIter,
        const UnrolledBodyCore &Core, BasicBlock *PreHeader,
        std::unordered_map<Value *, Value *> &HeaderExitValues);
    static bool buildHeaderTailInputValueMap(
        const std::vector<HeaderState> &HeaderStates,
        BasicBlock *PreHeader,
        std::unordered_map<Value *, Value *> &TailInputValues);
    static LoopUnrollResult executeUnrollWithTail(
        Loop *L, int MainCount, int TailCopies, Value *NumGroups,
        Value *RemainderValue, bool IsRuntime, SCEVResult &SE);
};

// A peeling-only entry point for an early loop-specialization phase.  It
// deliberately shares the generic LoopUnroll implementation and legality
// checks; the separate descriptor merely lets PassBuilder schedule peeling
// before the heavier unroll decisions without recognizing source names or
// benchmark-specific patterns.
class LoopPeelingPass : public PassDescriptor<LoopPeelingPass> {
public:
    explicit LoopPeelingPass(UnrollConfig Config = UnrollConfig());
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    LoopUnrollPass Impl;
};
