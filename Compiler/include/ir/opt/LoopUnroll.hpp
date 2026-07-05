#pragma once

#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <string>
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

    // Tuned for the SysY test set and RISC-V BOOM: favor runtime speed, but
    // cap code growth before register pressure and I-cache misses dominate.
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
