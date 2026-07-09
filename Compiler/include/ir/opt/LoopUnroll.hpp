#pragma once

#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
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
    int PeelThreshold = 64;

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

class LoopUnrollPass : public PassDescriptor<LoopUnrollPass> {
public:
    explicit LoopUnrollPass(UnrollConfig Config = UnrollConfig());

    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

    const UnrollConfig &getConfig() const { return Config; }

    std::vector<Loop *> collectLoopsInPostOrder(const LoopInfo &LI) const;

    LoopUnrollResult tryToUnrollLoop(Function &F,
                                     Loop *L,
                                     LoopInfo &LI,
                                     DominatorTree &DT,
                                     SCEVResult &SE);

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
                                         SCEVResult &SE) const;

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
                                    DominatorTree &DT);

private:
    UnrollConfig Config;
};
