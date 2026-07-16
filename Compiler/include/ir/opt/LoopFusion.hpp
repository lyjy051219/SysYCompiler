#pragma once

#include "ir/opt/NewPassManager.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class BasicBlock;
class BrInst;
class DominatorTree;
class Instruction;
class Loop;
class LoopAccessInfo;
class LoopInfo;
class LoadInst;
class PhiInst;
class PostDominatorTree;
class SCEVResult;
class StoreInst;
struct SCEVExpr;
class Value;

// Fuse two adjacent sibling loops by retaining the first loop's control and
// placing the second loop's body between the first and second latches.  The
// implementation deliberately accepts a narrow, fully preflighted CFG shape:
// both loops are finite header-tested loops over the same exact iteration
// domain.  Interleaving is accepted either by LoopAccessAnalysis or by the
// narrower modular-integer RMW proof represented below.
class LoopFusionPass : public PassDescriptor<LoopFusionPass> {
public:
    PassResult run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    static constexpr unsigned MaxFusionsPerFunction = 4;
    static constexpr std::size_t MaxFusionBlocks = 128;
    static constexpr std::size_t MaxFusionInstructions = 512;
    static constexpr std::size_t MaxFlowFusionInstructions = 128;
    static constexpr std::size_t MaxBridgeInstructions = 24;
    static constexpr std::size_t MaxMovedHeaderPhis = 32;
    static constexpr unsigned MaxEquivalentValueDepth = 24;
    static constexpr unsigned MaxDependencyWalkDepth = 64;

    using BlockSet = std::unordered_set<BasicBlock *>;
    using InstructionSet = std::unordered_set<Instruction *>;
    using ValueSet = std::unordered_set<Value *>;

    struct LoopShape {
        Loop *L = nullptr;
        BasicBlock *PreHeader = nullptr;
        BasicBlock *Header = nullptr;
        BasicBlock *BodyEntry = nullptr;
        BasicBlock *Latch = nullptr;
        BasicBlock *Exit = nullptr;
        BrInst *HeaderBranch = nullptr;
        BrInst *LatchBranch = nullptr;
        PhiInst *Induction = nullptr;
        Instruction *InductionUpdate = nullptr;
        Instruction *ExitCompare = nullptr;
        const SCEVExpr *Start = nullptr;
        const SCEVExpr *Step = nullptr;
        const SCEVExpr *TripCount = nullptr;
        std::optional<std::int64_t> ConstantStep;
        bool MayBeZeroTrip = true;
        std::vector<BasicBlock *> Blocks;
        BlockSet BlockLookup;
        std::size_t InstructionCount = 0;
    };

    struct EdgeRewrite {
        BrInst *Branch = nullptr;
        unsigned SuccessorIndex = 0;
        BasicBlock *OldSuccessor = nullptr;
        BasicBlock *NewSuccessor = nullptr;
    };

    struct PhiRewrite {
        PhiInst *Phi = nullptr;
        unsigned IncomingIndex = 0;
        BasicBlock *OldPredecessor = nullptr;
        BasicBlock *NewPredecessor = nullptr;
    };

    // A deliberately narrow proof object for an integer read-modify-write.
    // SourceRoots contains every read-only allocation touched by the loop;
    // each one must be a unique global/stack object distinct from TargetRoot.
    struct IntegerReductionUpdate {
        StoreInst *Store = nullptr;
        LoadInst *TargetLoad = nullptr;
        Value *Pointer = nullptr;
        Value *Delta = nullptr;
        Value *TargetRoot = nullptr;
        std::unordered_set<Value *> SourceRoots;
    };

    struct FusionCandidate {
        LoopShape First;
        LoopShape Second;
        BasicBlock *Bridge = nullptr;
        std::vector<Instruction *> HoistedBridgeInstructions;
        std::vector<PhiInst *> MovedSecondPhis;
        std::vector<EdgeRewrite> EdgeRewrites;
        std::vector<PhiRewrite> PhiRewrites;
        unsigned SameIterationFlowPairs = 0;
        unsigned SameIterationConflictPairs = 0;
        bool CommutativeIntegerReduction = false;
    };

    static bool expressionKeysEqual(const SCEVExpr *A,
                                    const SCEVExpr *B);
    static bool hasFiniteLoopTree(Loop *L, SCEVResult &SE);
    static bool hasNoLiveOut(const LoopShape &Shape);
    static bool isSafeReorderedInstruction(Instruction *I);
    static bool hasUnsupportedLoopInstruction(const LoopShape &Shape);
    static bool hasDistributionProvenance(const LoopShape &Shape);
    static bool isSafeBridgeInstruction(Instruction *I);
    static bool valueDominatesPreheader(
        Value *V, BasicBlock *PreHeader, BasicBlock *Bridge,
        const InstructionSet &EarlierBridgeInstructions,
        const DominatorTree &DT);
    static bool collectBridgeInstructions(
        FusionCandidate &Candidate, const DominatorTree &DT);
    static bool collectHeaderPhiRewrites(FusionCandidate &Candidate,
                                         const DominatorTree &DT);
    static bool secondHeaderContainsOnlyControl(
        const FusionCandidate &Candidate);
    static bool sameIterationDomain(const LoopShape &First,
                                    const LoopShape &Second);
    static bool isRepeatedByteConstant(Value *V);
    static bool isUnitStrideAccess(Value *Pointer, PhiInst *Induction);
    static bool isSingleStoreIdiomCandidate(const LoopShape &Shape);
    static bool isUniqueMemoryObject(Value *Root);
    static bool hasReductionControlShape(const LoopShape &Shape);
    static bool valueDependsOn(Value *V, Value *Needle,
                               unsigned Depth, ValueSet &Visited);
    static bool collectDependencySourceRoots(
        Value *V, Value *TargetRoot, unsigned Depth,
        ValueSet &Visited, std::unordered_set<Value *> &SourceRoots);
    static bool valuesEquivalentUnderInductionMap(
        Value *First, PhiInst *FirstInduction,
        Value *Second, PhiInst *SecondInduction,
        unsigned Depth);
    static bool matchIntegerReductionUpdate(
        const LoopShape &Shape, IntegerReductionUpdate &Update);
    static bool isCommutativeIntegerReductionFusion(
        const FusionCandidate &Candidate);
    static bool isProfitable(const FusionCandidate &Candidate);
    static bool isLCSSAAndStructurallyAdjacent(
        const FusionCandidate &Candidate, const LoopInfo &LI);
    static std::optional<unsigned> findUniqueSuccessorIndex(
        BrInst *Branch, BasicBlock *Successor);
    static std::optional<LoopShape> buildLoopShape(
        Function &F, Loop *L, SCEVResult &SE,
        const PostDominatorTree &PDT);
    static std::optional<FusionCandidate> buildCandidate(
        Function &F, Loop *First, LoopInfo &LI, SCEVResult &SE,
        const DominatorTree &DT, const PostDominatorTree &PDT,
        LoopAccessInfo &LA);
    static void commitCandidate(FusionCandidate &Candidate);

    std::unordered_map<Function *, unsigned> TransformCounts;
};
