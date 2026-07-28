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

// 融合两个串行相邻且迭代域相同的同级循环：
// 1. 为首循环及其退出桥之后的第二循环建立有限、规范、头部判定的结构摘要；
// 2. 验证 LCSSA、严格相邻、无 live-out、指令可重排、桥接计算可外提以及
//    两个 SCEV 迭代域完全一致；
// 3. 由统一 LoopAccessAnalysis 证明跨循环内存次序合法，再执行代码量与
//    同迭代真依赖收益筛选；
// 4. 预先记录全部边和 PHI 改写，提交时保留第一套控制结构并接入第二循环体；
// 5. 每次调用最多融合一对，交由新分析快照继续寻找下一对。
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

    // 服务步骤 1：LoopShape 同时核验规范 preheader/header/latch、唯一头部退出、
    // canonical IV、有限 trip count 和循环体至 latch 的后支配关系。
    static bool expressionKeysEqual(const SCEVExpr *A,
                                    const SCEVExpr *B);
    static bool hasFiniteLoopTree(Loop *L, SCEVResult &SE);
    static bool hasNoLiveOut(const LoopShape &Shape);
    static bool isSafeReorderedInstruction(Instruction *I);
    static bool hasUnsupportedLoopInstruction(const LoopShape &Shape);
    static bool hasDistributionProvenance(const LoopShape &Shape);
    // 服务步骤 2：桥中只允许无副作用且不会 fault/poison 的纯计算；每条指令
    // 的操作数必须支配第一循环预头，或来自桥中更早且同样会外提的指令。
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
    // 服务步骤 2：相邻性检查要求两循环同级且为 LCSSA，第一退出即第二预头，
    // 桥和第二循环入口/出口不含会使重接边歧义的额外前驱或 PHI。
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
    // 服务步骤 4：buildCandidate 已完成所有可失败验证，commit 只按记录移动
    // 指令、修复 PHI/边并删除被淘汰控制块，不再做启发式决策。
    static void commitCandidate(FusionCandidate &Candidate);

    std::unordered_map<Function *, unsigned> TransformCounts;
};
