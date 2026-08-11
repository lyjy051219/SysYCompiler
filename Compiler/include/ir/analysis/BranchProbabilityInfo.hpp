#pragma once

#include "ir/opt/NewPassManager.hpp"
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class BasicBlock;
class Function;
class DominatorTree;
class Loop;
class LoopInfo;
class PostDominatorTree;
class SCEVResult;

// BranchProbabilityInfo 保存与 BasicBlock::getSuccessors() 顺序严格对齐的
// 定点边概率。平行边分别保存，按目标块查询时再把它们合并。
class BranchProbabilityInfo {
    friend class BranchProbabilityAnalysis;

public:
    static constexpr uint32_t ProbabilityScale = uint32_t{1} << 20;

    BranchProbabilityInfo();
    BranchProbabilityInfo(BranchProbabilityInfo &&) noexcept;
    BranchProbabilityInfo &operator=(BranchProbabilityInfo &&) noexcept;
    BranchProbabilityInfo(const BranchProbabilityInfo &) = default;
    BranchProbabilityInfo &operator=(const BranchProbabilityInfo &) = default;

    // 按 successor 下标查询单条 CFG 边的概率；非法下标返回 0。
    uint32_t getEdgeProbability(BasicBlock *From,
                                std::size_t SuccessorIndex) const;
    // 合并 From 到 To 的全部平行边概率。
    uint32_t getEdgeProbabilityTo(BasicBlock *From, BasicBlock *To) const;
    double getEdgeProbabilityAsDouble(BasicBlock *From,
                                      std::size_t SuccessorIndex) const;
    const std::vector<uint32_t> &
    getSuccessorProbabilities(BasicBlock *From) const;
    BasicBlock *getLikelySuccessor(BasicBlock *From) const;
    bool isReachable(BasicBlock *BB) const;
    bool isReliable() const;
    bool usedFallback() const;
    std::size_t getAnalyzedBlockCount() const;
    std::size_t getAnalyzedEdgeCount() const;
    bool invalidate(Function &F, const PreservationStatus &PA);

private:
    std::unordered_map<BasicBlock *, std::vector<uint32_t>> EdgeProbabilities;
    std::unordered_set<BasicBlock *> ReachableBlocks;
    std::size_t AnalyzedEdgeCount = 0;
    bool Reliable = true;
    bool UsedFallback = false;
};

// BranchProbabilityAnalysis 综合常量条件、循环/SCEV、后支配和冷出口信息，
// 不读取函数名、测试输入或目标后端名称。
class BranchProbabilityAnalysis
    : public AnalysisDescriptor<BranchProbabilityAnalysis> {
public:
    using Result = BranchProbabilityInfo;
    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;

private:
    friend AnalysisDescriptor<BranchProbabilityAnalysis>;
    inline static PassID Key;

    static constexpr std::size_t BlockBudget = 16384;
    static constexpr std::size_t EdgeBudget = 65536;
    static constexpr std::size_t LoopBudget = 4096;
    static constexpr uint64_t MaxTripWeight = uint64_t{1} << 20;

    static void assignUniformProbabilities(
        std::size_t Count, std::vector<uint32_t> &Probabilities);
    static void normalizeWeights(
        const std::vector<uint64_t> &Weights,
        std::vector<uint32_t> &Probabilities);
    static bool isDirectReturnBlock(BasicBlock *BB);
    static bool isStructuralNoReturnSink(BasicBlock *BB);
    static bool buildLoopWeights(
        Loop *L, SCEVResult &SE, std::vector<uint64_t> &Weights,
        BasicBlock *&ExitingBlock);
    static bool buildUnknownLoopWeights(
        BasicBlock *BB, LoopInfo &LI, DominatorTree &DT,
        std::vector<uint64_t> &Weights);
    static bool buildColdExitWeights(
        BasicBlock *BB, PostDominatorTree &PDT,
        std::vector<uint64_t> &Weights);
};
