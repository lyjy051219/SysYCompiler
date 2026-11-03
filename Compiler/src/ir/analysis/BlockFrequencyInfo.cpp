// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#include "ir/analysis/BlockFrequencyInfo.hpp"
#include "lib/CFG.hpp"
#include <algorithm>
#include <deque>
#include <limits>
#include <unordered_map>
#include <unordered_set>

uint64_t BlockFrequencyAnalysis::scaleFrequency(
    uint64_t Frequency, uint32_t Probability) {
    const unsigned __int128 Product =
        static_cast<unsigned __int128>(Frequency) * Probability;
    const unsigned __int128 Scaled =
        Product / BranchProbabilityInfo::ProbabilityScale;
    return Scaled > MaxTrackedFrequency
               ? MaxTrackedFrequency
               : static_cast<uint64_t>(Scaled);
}
 
uint64_t BlockFrequencyAnalysis::addFrequency(
    uint64_t LHS, uint64_t RHS, bool &Saturated) {
    if (RHS > MaxTrackedFrequency - LHS) {
        Saturated = true;
        return MaxTrackedFrequency;
    }
    return LHS + RHS;
}

uint32_t BlockFrequencyAnalysis::getConservativeEdgeProbability(
    const BranchProbabilityInfo &BPI, BasicBlock *From,
    std::size_t EdgeIndex, std::size_t EdgeCount) {
    const auto &Probabilities = BPI.getSuccessorProbabilities(From);
    if (Probabilities.size() == EdgeCount)
        return Probabilities[EdgeIndex];
    if (EdgeCount == 0) return 0;
    const uint32_t Base = BranchProbabilityInfo::ProbabilityScale /
                          static_cast<uint32_t>(EdgeCount);
    const uint32_t Remainder = BranchProbabilityInfo::ProbabilityScale -
        Base * static_cast<uint32_t>(EdgeCount);
    return Base + (EdgeIndex < Remainder ? 1u : 0u);
}

void BlockFrequencyAnalysis::buildConservativeFallback(
    Function &F, const BranchProbabilityInfo &BPI,
    const std::vector<BasicBlock *> &Blocks,
    std::unordered_map<BasicBlock *, uint64_t> &Frequencies) {
    Frequencies.clear();
    for (BasicBlock *BB : Blocks) Frequencies[BB] = 0;
    BasicBlock *Entry = F.front();
    if (!Entry) return;

    Frequencies[Entry] = BlockFrequencyInfo::EntryFrequency;
    std::unordered_set<BasicBlock *> Discovered;
    std::deque<BasicBlock *> Worklist;
    Discovered.insert(Entry);
    Worklist.push_back(Entry);
    while (!Worklist.empty()) {
        BasicBlock *From = Worklist.front();
        Worklist.pop_front();
        auto Successors = From->getSuccessors();
        std::unordered_map<BasicBlock *, uint64_t> Combined;
        for (std::size_t I = 0; I < Successors.size(); ++I) {
            BasicBlock *To = Successors[I];
            if (!To || !Frequencies.count(To)) continue;
            const uint32_t Probability = getConservativeEdgeProbability(
                BPI, From, I, Successors.size());
            Combined[To] += scaleFrequency(
                Frequencies[From], Probability);
        }
        for (const auto &EntryFrequency : Combined) {
            BasicBlock *To = EntryFrequency.first;
            uint64_t Candidate = EntryFrequency.second;
            if (Candidate == 0 && BPI.isReachable(To)) Candidate = 1;
            if (!Discovered.insert(To).second) continue;
            Frequencies[To] = Candidate;
            Worklist.push_back(To);
        }
    }

    // 分析预算耗尽或不可吸收环会使传播树无法覆盖某些保守可达块；给这些块
    // 最小非零频率，而不是错误地宣称它们永不执行。
    for (BasicBlock *BB : Blocks) {
        if (BPI.isReachable(BB) && Frequencies[BB] == 0)
            Frequencies[BB] = 1;
    }
}

void BlockFrequencyAnalysis::buildEdgeFrequencies(
    const BranchProbabilityInfo &BPI,
    const std::vector<BasicBlock *> &Blocks,
    const std::unordered_map<BasicBlock *, uint64_t> &BlockFrequencies,
    std::unordered_map<BasicBlock *, std::vector<uint64_t>> &EdgeFrequencies) {
    EdgeFrequencies.clear();
    for (BasicBlock *BB : Blocks) {
        auto Successors = BB->getSuccessors();
        auto &Weights = EdgeFrequencies[BB];
        Weights.reserve(Successors.size());
        auto FrequencyIt = BlockFrequencies.find(BB);
        const uint64_t Frequency = FrequencyIt == BlockFrequencies.end()
                                       ? 0
                                       : FrequencyIt->second;
        for (std::size_t I = 0; I < Successors.size(); ++I) {
            const uint32_t Probability = getConservativeEdgeProbability(
                BPI, BB, I, Successors.size());
            Weights.push_back(scaleFrequency(Frequency, Probability));
        }
    }
}

BlockFrequencyInfo::BlockFrequencyInfo() = default;
BlockFrequencyInfo::BlockFrequencyInfo(BlockFrequencyInfo &&) noexcept = default;
BlockFrequencyInfo &BlockFrequencyInfo::operator=(
    BlockFrequencyInfo &&) noexcept = default;

uint64_t BlockFrequencyInfo::getBlockFrequency(BasicBlock *BB) const {
    auto It = BlockFrequencies.find(BB);
    return It == BlockFrequencies.end() ? 0 : It->second;
}

uint64_t BlockFrequencyInfo::getEdgeFrequency(
    BasicBlock *From, std::size_t SuccessorIndex) const {
    auto It = EdgeFrequencies.find(From);
    if (It == EdgeFrequencies.end() ||
        SuccessorIndex >= It->second.size()) return 0;
    return It->second[SuccessorIndex];
}

double BlockFrequencyInfo::getRelativeFrequency(BasicBlock *BB) const {
    return static_cast<double>(getBlockFrequency(BB)) /
           static_cast<double>(EntryFrequency);
}

bool BlockFrequencyInfo::isHotterThan(BasicBlock *LHS,
                                      BasicBlock *RHS) const {
    return getBlockFrequency(LHS) > getBlockFrequency(RHS);
}

bool BlockFrequencyInfo::isConverged() const { return Converged; }
bool BlockFrequencyInfo::isReliable() const { return Reliable; }
bool BlockFrequencyInfo::usedFallback() const { return UsedFallback; }
unsigned BlockFrequencyInfo::getIterationCount() const {
    return IterationCount;
}
std::size_t BlockFrequencyInfo::getAnalyzedBlockCount() const {
    return BlockFrequencies.size();
}

bool BlockFrequencyInfo::invalidate(
    Function &, const PreservationStatus &PA) {
    return !PA.isKept(BlockFrequencyAnalysis::ID()) ||
           !PA.isKept(BranchProbabilityAnalysis::ID());
}

BlockFrequencyAnalysis::Result BlockFrequencyAnalysis::run(
    Function &F, FunctionAnalysisPassManager &FAM) const {
    BlockFrequencyInfo Result;
    if (!F.front()) {
        Result.Converged = true;
        Result.Reliable = true;
        return Result;
    }

    auto &BPI = FAM.getResult<BranchProbabilityAnalysis>(F);
    std::vector<BasicBlock *> Blocks;
    std::unordered_map<BasicBlock *, std::size_t> BlockIndices;
    std::size_t EdgeCount = 0;
    bool EntryHasIncomingEdge = false;
    for (BasicBlock *BB : F) {
        if (!BB) continue;
        BlockIndices.emplace(BB, Blocks.size());
        Blocks.push_back(BB);
    }
    for (BasicBlock *BB : Blocks) {
        auto Successors = BB->getSuccessors();
        EdgeCount += Successors.size();
        for (BasicBlock *Succ : Successors)
            if (Succ == F.front()) EntryHasIncomingEdge = true;
    }

    if (Blocks.size() > BlockBudget || EdgeCount > EdgeBudget) {
        buildConservativeFallback(
            F, BPI, Blocks, Result.BlockFrequencies);
        buildEdgeFrequencies(BPI, Blocks, Result.BlockFrequencies,
                             Result.EdgeFrequencies);
        Result.Reliable = false;
        Result.UsedFallback = true;
        return Result;
    }

    const std::size_t Count = Blocks.size();
    std::vector<uint64_t> Current(Count, 0);
    std::vector<uint64_t> Next(Count, 0);
    Current[BlockIndices.at(F.front())] = BlockFrequencyInfo::EntryFrequency;
    const uint64_t WorkPerIteration = std::max<std::size_t>(EdgeCount, 1);
    const unsigned IterationBudget = static_cast<unsigned>(std::min<uint64_t>(
        IterationLimit,
        std::max<uint64_t>(MinimumIterationBudget,
                           PropagationWorkBudget / WorkPerIteration)));
    bool Saturated = false;
    bool MalformedCFG = false;

    for (unsigned Iteration = 1; Iteration <= IterationBudget; ++Iteration) {
        std::fill(Next.begin(), Next.end(), 0);
        Next[BlockIndices.at(F.front())] =
            BlockFrequencyInfo::EntryFrequency;
        for (std::size_t FromIndex = 0; FromIndex < Count; ++FromIndex) {
            BasicBlock *From = Blocks[FromIndex];
            if (Current[FromIndex] == 0) continue;
            auto Successors = From->getSuccessors();
            for (std::size_t EdgeIndex = 0;
                 EdgeIndex < Successors.size(); ++EdgeIndex) {
                BasicBlock *To = Successors[EdgeIndex];
                auto ToIt = BlockIndices.find(To);
                if (ToIt == BlockIndices.end()) {
                    MalformedCFG = true;
                    continue;
                }
                // 函数入口按定义表示一次调用。畸形的回入口边不累加入基准值，
                // 同时把结果降级为不可靠。
                if (To == F.front()) continue;
                const uint32_t Probability = getConservativeEdgeProbability(
                    BPI, From, EdgeIndex, Successors.size());
                const uint64_t Contribution = scaleFrequency(
                    Current[FromIndex], Probability);
                Next[ToIt->second] = addFrequency(
                    Next[ToIt->second], Contribution, Saturated);
            }
        }
        Result.IterationCount = Iteration;
        if (Next == Current) {
            Current.swap(Next);
            Result.Converged = true;
            break;
        }
        Current.swap(Next);
        if (Saturated) break;
    }

    for (std::size_t I = 0; I < Count; ++I)
        Result.BlockFrequencies[Blocks[I]] = Current[I];

    if (!Result.Converged) {
        Result.UsedFallback = true;
        if (Saturated || !BPI.isReliable()) {
            buildConservativeFallback(
                F, BPI, Blocks, Result.BlockFrequencies);
        } else {
            // 达到工作预算但没有溢出时，当前值是有界传播得到的保守下界；只为
            // 因定点舍入仍为零的可达块补最小值，保留已获得的循环热度排序。
            for (BasicBlock *BB : Blocks) {
                if (BPI.isReachable(BB) &&
                    Result.BlockFrequencies[BB] == 0)
                    Result.BlockFrequencies[BB] = 1;
            }
        }
    }

    buildEdgeFrequencies(BPI, Blocks, Result.BlockFrequencies,
                         Result.EdgeFrequencies);
    Result.Reliable = Result.Converged && BPI.isReliable() &&
                      !Saturated && !MalformedCFG &&
                      !EntryHasIncomingEdge;
    return Result;
}
