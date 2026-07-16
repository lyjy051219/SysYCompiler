#pragma once

#include "ir/analysis/SCEV.hpp"
#include "ir/opt/NewPassManager.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class DominatorTree;
class Loop;
class LoopAccessInfo;
class LoopInfo;
class PhiInst;
class PostDominatorTree;

struct TilingConfig {
    int TileSize = 16;
    unsigned MinTilesPerAxis = 4;
    unsigned MaxTransformsPerFunction = 1;
    std::size_t MaxNestBlocks = 48;
    std::size_t MaxNestInstructions = 320;
};

struct TilingAxis {
    Loop *L = nullptr;
    LoopTripCountInfo Trip;
    InductionDescriptor Induction;
    PhiInst *IV = nullptr;
    Value *Start = nullptr;
    Value *Bound = nullptr;
    std::int64_t Step = 0;
    std::int64_t TileSpan = 0;
    unsigned BoundOperandIndex = 0;
    unsigned OutsideIncomingIndex = 0;
};

struct TilingCandidate {
    TilingAxis Outer;
    TilingAxis Inner;
    BasicBlock *OuterPreHeader = nullptr;
    BasicBlock *OuterLatch = nullptr;
    BasicBlock *InnerPreHeader = nullptr;
    BasicBlock *InnerLatch = nullptr;
    BasicBlock *OuterExit = nullptr;
    std::vector<BasicBlock *> NestBlocks;
};

// Two-dimensional rectangular strip mining.  The original point loops are
// retained and bounded by overflow-safe tile ends; two small tile loops are
// synthesized around them.  Memory legality uses the strong cross-iteration
// independence proof from LoopAccessAnalysis.
class LoopTilingPass : public PassDescriptor<LoopTilingPass> {
public:
    explicit LoopTilingPass(TilingConfig Config = TilingConfig());
    PassResult run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    TilingConfig Config;
    std::unordered_map<Function *, unsigned> TransformCounts;
    std::unordered_map<Function *, std::unordered_set<BasicBlock *>>
        TransformedHeaders;

    static bool getNormalizedBound(TilingAxis &Axis);
    static bool getOutsideIncoming(TilingAxis &Axis,
                                   BasicBlock *PreHeader);
    static bool hasOnlyCanonicalHeaderPhi(const TilingAxis &Axis);
    static bool hasControlOnlyHeader(const TilingAxis &Axis);
    static bool hasLoopLiveOut(Loop *L);
    static bool hasDirectObservableOuterWork(Loop *Outer,
                                             const LoopInfo &LI);
    static bool isValueAvailableAt(Value *V, BasicBlock *BB,
                                   const DominatorTree &DT);
    static bool descendantDomainsAreRectangular(Loop *Current,
                                                Loop *TiledOuter,
                                                Loop *TiledInner,
                                                SCEVResult &SE);
    static bool hasSingleExitEdge(Loop *L,
                                  const LoopTripCountInfo &Trip);
    static bool hasCompleteIterationPath(
        Loop *L, const LoopTripCountInfo &Trip, BasicBlock *Latch,
        const PostDominatorTree &PDT);
    static bool validateNestShape(const TilingCandidate &Candidate,
                                  const LoopInfo &LI,
                                  const DominatorTree &DT,
                                  const PostDominatorTree &PDT,
                                  const TilingConfig &Config);
    static std::optional<TilingAxis> analyzeAxis(
        Loop *L, BasicBlock *PreHeader, SCEVResult &SE,
        const TilingConfig &Config);
    static std::optional<TilingCandidate> buildCandidate(
        Loop *Outer, Loop *Inner, LoopInfo &LI, SCEVResult &SE,
        LoopAccessInfo &LA, DominatorTree &DT, PostDominatorTree &PDT,
        const TilingConfig &Config);
    static Value *makeTileEnd(BasicBlock *BB, Value *Tile, Value *Bound,
                              std::int64_t Span);
    static bool validateCommitState(const TilingCandidate &Candidate);
    static void destroyTileBlocks(
        std::vector<BasicBlock *> &TileBlocks);
    static bool executeTiling(Function &F, TilingCandidate &Candidate);
};
