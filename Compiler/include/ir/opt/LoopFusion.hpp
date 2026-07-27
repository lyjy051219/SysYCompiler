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
class PhiInst;
class PostDominatorTree;
class SCEVResult;
struct SCEVExpr;
class Value;

// 融合两个相邻的同级循环：保留第一个循环的控制结构，并把第二个循环的
// 主体接入第一个循环的迭代路径。候选必须先完整验证 CFG 形状，两个循环
// 都必须是有限、头部判定且迭代域严格相同的循环；所有跨循环内存次序都
// 必须由统一的 LoopAccessAnalysis 证明合法，不允许指令形状专用旁路。
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

    using BlockSet = std::unordered_set<BasicBlock *>;
    using InstructionSet = std::unordered_set<Instruction *>;

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

    struct FusionCandidate {
        LoopShape First;
        LoopShape Second;
        BasicBlock *Bridge = nullptr;
        std::vector<Instruction *> HoistedBridgeInstructions;
        std::vector<PhiInst *> MovedSecondPhis;
        std::vector<EdgeRewrite> EdgeRewrites;
        std::vector<PhiRewrite> PhiRewrites;
        unsigned SameIterationFlowPairs = 0;
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
