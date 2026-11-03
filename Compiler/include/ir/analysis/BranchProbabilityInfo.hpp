// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

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
 
//BranchProbabilityInfo：保存与BasicBlock::getSuccessors()顺序严格对齐的定点边概率
class BranchProbabilityInfo {
    friend class BranchProbabilityAnalysis;
public:
    /*
    采用定点整数表示概率，使用 20 位定点数表示概率值，满概率刻度为 2²⁰ = 1048576，表示概率为 1.0
    概率计算：
    实际概率 = 存储值 / 2²⁰（保留了20位小数精度）
    */
    static constexpr uint32_t ProbabilityScale = uint32_t{1} << 20;

    //支持移动构造和赋值构造，以及默认构造
    BranchProbabilityInfo();
    BranchProbabilityInfo(BranchProbabilityInfo &&) noexcept;
    BranchProbabilityInfo &operator=(BranchProbabilityInfo &&) noexcept;
    BranchProbabilityInfo(const BranchProbabilityInfo &) = default;
    BranchProbabilityInfo &operator=(const BranchProbabilityInfo &) = default;
//基础查询接口：
    //按successor下标查询单条CFG边的概率；非法下标返回 0
    uint32_t getEdgeProbability(BasicBlock *From,std::size_t SuccessorIndex) const;
    //查询From到To的总概率并合并From到To的全部平行边
    uint32_t getEdgeProbabilityTo(BasicBlock *From, BasicBlock *To) const;
    //将定点整数表示转换为double表示
    double getEdgeProbabilityAsDouble(BasicBlock *From,std::size_t SuccessorIndex) const;
    //获得整个successor CFG边概率数组
    const std::vector<uint32_t> & getSuccessorProbabilities(BasicBlock *From) const;
    //先合并平行边，然后返回概率最大的后继块，无法确定时（可能概率相同）返回nullptr
    BasicBlock *getLikelySuccessor(BasicBlock *From) const;
    bool isReachable(BasicBlock *BB) const;
    bool isReliable() const;
    bool usedFallback() const;
    //返回EdgeProbabilities中有记录的基本块数量
    std::size_t getAnalyzedBlockCount() const;
    //返回EdgeProbabilities中有记录的CFG边数量（平行边分别计数）
    std::size_t getAnalyzedEdgeCount() const;
    bool invalidate(Function &F, const PreservationStatus &PA);

private:
    //核心数据结构：BasicBlock* -> successor CFG边概率向量
    std::unordered_map<BasicBlock *, std::vector<uint32_t>> EdgeProbabilities;
    //入口可达性结果快照；正常路径由Function::collectReachableBlocks填充，
    //超预算路径保守记录全部块，供run返回后O(1)查询。
    std::unordered_set<BasicBlock *> ReachableBlocks;
    std::size_t AnalyzedEdgeCount = 0;
    //是否可靠：分为超预算和CFG结构不可靠两种情况
    bool Reliable = true;
    //如果超预算，那么就直接给每个块均匀概率 -- 算法降级
    bool UsedFallback = false;
    /*
    Reliable=false, UsedFallback=true -> 超预算，算法降级
    Reliable=false, UsedFallback=false -> CFG结构使结果不能完全可靠--比如存在入口可达的无限循环
    */
};

//BranchProbabilityAnalysis：分支概率分析，提供每条CFG边的概率
//目前仅CodeMotion使用了该AnalysisPass
class BranchProbabilityAnalysis
    : public AnalysisDescriptor<BranchProbabilityAnalysis> {
public:
    using Result = BranchProbabilityInfo;
    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;
private:
    friend AnalysisDescriptor<BranchProbabilityAnalysis>;
    inline static PassID Key;

//分析预算：超过预算的函数将直接使用均匀概率
    //最大分析基本块数量
    static constexpr std::size_t BlockBudget = 16384;
    //最大分析CFG边数量（平行边分别计数）
    static constexpr std::size_t EdgeBudget = 65536;
    //最大分析循环数量
    static constexpr std::size_t LoopBudget = 4096;
    //最大分析循环回边continue数量
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
