#pragma once

#include "ir/analysis/BranchProbabilityInfo.hpp"
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

class BasicBlock;
class Function;

// BlockFrequencyInfo 以入口频率为固定基准，保存有界静态传播得到的相对频率。
// 它不表示真实执行次数；Reliable=false 时使用者必须把结果当作布局提示。
class BlockFrequencyInfo {
    friend class BlockFrequencyAnalysis;

public:
    static constexpr uint64_t EntryFrequency = uint64_t{1} << 20;

    BlockFrequencyInfo();
    BlockFrequencyInfo(BlockFrequencyInfo &&) noexcept;
    BlockFrequencyInfo &operator=(BlockFrequencyInfo &&) noexcept;
    BlockFrequencyInfo(const BlockFrequencyInfo &) = default;
    BlockFrequencyInfo &operator=(const BlockFrequencyInfo &) = default;

    uint64_t getBlockFrequency(BasicBlock *BB) const;
    uint64_t getEdgeFrequency(BasicBlock *From,
                              std::size_t SuccessorIndex) const;
    double getRelativeFrequency(BasicBlock *BB) const;
    bool isHotterThan(BasicBlock *LHS, BasicBlock *RHS) const;
    bool isConverged() const;
    bool isReliable() const;
    bool usedFallback() const;
    unsigned getIterationCount() const;
    std::size_t getAnalyzedBlockCount() const;
    bool invalidate(Function &F, const PreservationStatus &PA);

private:
    std::unordered_map<BasicBlock *, uint64_t> BlockFrequencies;
    std::unordered_map<BasicBlock *, std::vector<uint64_t>> EdgeFrequencies;
    bool Converged = false;
    bool Reliable = false;
    bool UsedFallback = false;
    unsigned IterationCount = 0;
};

class BlockFrequencyAnalysis
    : public AnalysisDescriptor<BlockFrequencyAnalysis> {
public:
    using Result = BlockFrequencyInfo;
    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;

private:
    friend AnalysisDescriptor<BlockFrequencyAnalysis>;
    inline static PassID Key;

    static constexpr std::size_t BlockBudget = 16384;
    static constexpr std::size_t EdgeBudget = 65536;
    static constexpr uint64_t PropagationWorkBudget = uint64_t{16} << 20;
    static constexpr unsigned IterationLimit = 4096;
    static constexpr unsigned MinimumIterationBudget = 32;
    static constexpr uint64_t MaxTrackedFrequency =
        std::numeric_limits<uint64_t>::max() / 4;

    static uint64_t scaleFrequency(uint64_t Frequency,
                                   uint32_t Probability);
    static uint64_t addFrequency(uint64_t LHS, uint64_t RHS,
                                 bool &Saturated);
    static uint32_t getConservativeEdgeProbability(
        const BranchProbabilityInfo &BPI, BasicBlock *From,
        std::size_t EdgeIndex, std::size_t EdgeCount);
    static void buildConservativeFallback(
        Function &F, const BranchProbabilityInfo &BPI,
        const std::vector<BasicBlock *> &Blocks,
        std::unordered_map<BasicBlock *, uint64_t> &Frequencies);
    static void buildEdgeFrequencies(
        const BranchProbabilityInfo &BPI,
        const std::vector<BasicBlock *> &Blocks,
        const std::unordered_map<BasicBlock *, uint64_t> &BlockFrequencies,
        std::unordered_map<BasicBlock *, std::vector<uint64_t>>
            &EdgeFrequencies);
};
