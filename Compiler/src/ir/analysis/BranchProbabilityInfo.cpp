#include "ir/analysis/BranchProbabilityInfo.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SCEV.hpp"
#include "lib/CFG.hpp"
#include <algorithm>
#include <limits>
#include <unordered_map>
//BranchProbabilityInfo基础接口
BranchProbabilityInfo::BranchProbabilityInfo() = default;
BranchProbabilityInfo::BranchProbabilityInfo(BranchProbabilityInfo &&) noexcept = default;
BranchProbabilityInfo &BranchProbabilityInfo::operator=(BranchProbabilityInfo &&) noexcept = default;
uint32_t BranchProbabilityInfo::getEdgeProbability(
    BasicBlock *From, std::size_t SuccessorIndex) const {
    auto It = EdgeProbabilities.find(From);
    if (It == EdgeProbabilities.end() ||
        SuccessorIndex >= It->second.size()) return 0;
    return It->second[SuccessorIndex];
}
uint32_t BranchProbabilityInfo::getEdgeProbabilityTo(
    BasicBlock *From, BasicBlock *To) const {
    if (!From || !To) return 0;
    auto It = EdgeProbabilities.find(From);
    if (It == EdgeProbabilities.end()) return 0;
    auto Successors = From->getSuccessors();
    uint64_t Sum = 0;
    const std::size_t Count = std::min(Successors.size(), It->second.size());
    for (std::size_t I = 0; I < Count; ++I) {
        if (Successors[I] == To) Sum += It->second[I];
    }
    return static_cast<uint32_t>(std::min<uint64_t>(
        Sum, ProbabilityScale));
}
double BranchProbabilityInfo::getEdgeProbabilityAsDouble(
    BasicBlock *From, std::size_t SuccessorIndex) const {
    return static_cast<double>(getEdgeProbability(From, SuccessorIndex)) /
           static_cast<double>(ProbabilityScale);
}
const std::vector<uint32_t> &
BranchProbabilityInfo::getSuccessorProbabilities(BasicBlock *From) const {
    static const std::vector<uint32_t> Empty;
    auto It = EdgeProbabilities.find(From);
    return It == EdgeProbabilities.end() ? Empty : It->second;
}
BasicBlock *BranchProbabilityInfo::getLikelySuccessor(
    BasicBlock *From) const {
    if (!From) return nullptr;
    auto Successors = From->getSuccessors();
    auto It = EdgeProbabilities.find(From);
    if (It == EdgeProbabilities.end() ||
        It->second.size() != Successors.size() || Successors.empty())
        return nullptr;
    std::unordered_map<BasicBlock *, uint64_t> Combined;
    for (std::size_t I = 0; I < Successors.size(); ++I)
        Combined[Successors[I]] += It->second[I];
    BasicBlock *Best = nullptr;
    uint64_t BestProbability = 0;
    bool Tied = false;
    for (const auto &Entry : Combined) {
        if (Entry.second > BestProbability) {
            Best = Entry.first;
            BestProbability = Entry.second;
            Tied = false;
        } else if (Entry.second == BestProbability) {
            Tied = true;
        }
    }
    return Tied ? nullptr : Best;
}
bool BranchProbabilityInfo::isReachable(BasicBlock *BB) const {
    return BB && ReachableBlocks.count(BB) != 0;
}
bool BranchProbabilityInfo::isReliable() const { return Reliable; }
bool BranchProbabilityInfo::usedFallback() const { return UsedFallback; }
std::size_t BranchProbabilityInfo::getAnalyzedBlockCount() const {
    return EdgeProbabilities.size();
}
std::size_t BranchProbabilityInfo::getAnalyzedEdgeCount() const {
    return AnalyzedEdgeCount;
}
bool BranchProbabilityInfo::invalidate(
    Function &, const PreservationStatus &PA) {
    return !PA.isKept(BranchProbabilityAnalysis::ID()) ||
           !PA.isKept(DominantAnalysis::ID()) ||
           !PA.isKept(PostDominantAnalysis::ID()) ||
           !PA.isKept(LoopAnalysis::ID()) ||
           !PA.isKept(SCEVAnalysis::ID());
}

//辅助接口1：均匀分配ProbabilityScale给每个后继
/*例子：
ProbabilityScale = 1048576
1048576 / 3 = 349525，余 1
结果：[349526, 349525, 349525]
*/
void BranchProbabilityAnalysis::assignUniformProbabilities(
    std::size_t Count, std::vector<uint32_t> &Probabilities) {
    Probabilities.assign(Count, 0);
    if (Count == 0) return;
    const uint32_t Quotient =
        BranchProbabilityInfo::ProbabilityScale / static_cast<uint32_t>(Count);
    uint32_t Remainder =
        BranchProbabilityInfo::ProbabilityScale - Quotient * static_cast<uint32_t>(Count);
    for (std::size_t I = 0; I < Count; ++I) {
        Probabilities[I] = Quotient + (Remainder != 0 ? 1u : 0u);
        if (Remainder != 0) --Remainder;
    }
}

void BranchProbabilityAnalysis::normalizeWeights(
    const std::vector<uint64_t> &Weights,
    std::vector<uint32_t> &Probabilities) {
    uint64_t TotalWeight = 0;
    for (uint64_t Weight : Weights) {
        if (std::numeric_limits<uint64_t>::max() - TotalWeight < Weight) {
            assignUniformProbabilities(Weights.size(), Probabilities);
            return;
        }
        TotalWeight += Weight;
    }
    if (Weights.empty() || TotalWeight == 0) {
        assignUniformProbabilities(Weights.size(), Probabilities);
        return;
    }

    Probabilities.assign(Weights.size(), 0);
    uint32_t Assigned = 0;
    for (std::size_t I = 0; I < Weights.size(); ++I) {
        const unsigned __int128 Product =
            static_cast<unsigned __int128>(
                BranchProbabilityInfo::ProbabilityScale) * Weights[I];
        Probabilities[I] = static_cast<uint32_t>(Product / TotalWeight);
        Assigned += Probabilities[I];
    }

    // 向权重非零的边发放定点除法余数，保证每个块的出边概率之和严格为满刻度。
    for (std::size_t I = 0;
         Assigned < BranchProbabilityInfo::ProbabilityScale; ++I) {
        const std::size_t Index = I % Weights.size();
        if (Weights[Index] == 0) continue;
        ++Probabilities[Index];
        ++Assigned;
    }
}

bool BranchProbabilityAnalysis::isDirectReturnBlock(BasicBlock *BB) {
    return BB && dynamic_cast<RetInst *>(BB->getTerminator()) != nullptr;
}

bool BranchProbabilityAnalysis::isStructuralNoReturnSink(BasicBlock *BB) {
    if (!BB || !BB->getSuccessors().empty()) return false;
    // 当前 IR 没有 noreturn 属性和 unreachable 指令；无 CFG 终结指令的叶块
    // 是唯一能够在不猜测函数名的前提下识别出的不可返回结构。
    return BB->getTerminator() == nullptr;
}

bool BranchProbabilityAnalysis::buildLoopWeights(
    Loop *L, SCEVResult &SE,
    std::vector<uint64_t> &Weights,
    BasicBlock *&ExitingBlock) {
    if (!L) return false;
    auto Trip = SE.analyzeLoopTripCount(L);
    if (!Trip || !Trip->Valid || !Trip->ExitingBlock) return false;
    BasicBlock *BB = Trip->ExitingBlock;
    auto Successors = BB->getSuccessors();
    if (Successors.size() != 2) return false;

    std::size_t ContinueIndex = Successors.size();
    std::size_t ExitIndex = Successors.size();
    for (std::size_t I = 0; I < Successors.size(); ++I) {
        if (L->contains(Successors[I])) ContinueIndex = I;
        else ExitIndex = I;
    }
    if (ContinueIndex >= Successors.size() || ExitIndex >= Successors.size())
        return false;

    uint64_t ContinueWeight = 7;
    uint64_t ExitWeight = 1;
    if (Trip->ConstantTripCount && *Trip->ConstantTripCount >= 0) {
        const uint64_t TripCount = static_cast<uint64_t>(
            *Trip->ConstantTripCount);
        uint64_t TakenCount = 0;
        if (Trip->ExitingBlock == Trip->Header) {
            // Header guard 对执行 N 次的循环通常被观察 N+1 次。
            TakenCount = TripCount;
        } else if (Trip->ConstantBackedgeTakenCount &&
                   *Trip->ConstantBackedgeTakenCount >= 0) {
            TakenCount = static_cast<uint64_t>(
                *Trip->ConstantBackedgeTakenCount);
        } else {
            TakenCount = TripCount == 0 ? 0 : TripCount - 1;
        }
        ContinueWeight = std::min(TakenCount, MaxTripWeight);
        ExitWeight = 1;
    }

    Weights.assign(Successors.size(), 0);
    Weights[ContinueIndex] = ContinueWeight;
    Weights[ExitIndex] = ExitWeight;
    ExitingBlock = BB;
    return true;
}

bool BranchProbabilityAnalysis::buildUnknownLoopWeights(
    BasicBlock *BB, LoopInfo &LI, DominatorTree &DT,
    std::vector<uint64_t> &Weights) {
    if (!BB) return false;
    auto Successors = BB->getSuccessors();
    if (Successors.size() != 2) return false;

    // 优先使用最内层循环归属：一条边留在当前循环而另一条离环时，全部未知
    // trip 循环统一采用 7:1，避免针对某个源码形状调参。
    if (Loop *L = LI.getLoopFor(BB)) {
        const bool FirstInside = L->contains(Successors[0]);
        const bool SecondInside = L->contains(Successors[1]);
        if (FirstInside != SecondInside) {
            Weights.assign(2, 1);
            Weights[FirstInside ? 0 : 1] = 7;
            return true;
        }
    }

    // 非规范循环仍可由支配关系识别自然回边；所有此类回边使用同一权重。
    bool FoundBackedge = false;
    Weights.assign(Successors.size(), 1);
    for (std::size_t I = 0; I < Successors.size(); ++I) {
        if (Successors[I] && DT.dominates(Successors[I], BB)) {
            Weights[I] = 7;
            FoundBackedge = true;
        }
    }
    return FoundBackedge;
}

bool BranchProbabilityAnalysis::buildColdExitWeights(
    BasicBlock *BB, PostDominatorTree &PDT,
    std::vector<uint64_t> &Weights) {
    if (!BB) return false;
    auto Successors = BB->getSuccessors();
    if (Successors.size() != 2 || Successors[0] == Successors[1])
        return false;

    bool HasExitReachableSuccessor = false;
    for (BasicBlock *Succ : Successors) {
        if (Succ && PDT.isReachableToExit(Succ) &&
            !isStructuralNoReturnSink(Succ)) {
            HasExitReachableSuccessor = true;
        }
    }

    std::vector<bool> Cold(Successors.size(), false);
    for (std::size_t I = 0; I < Successors.size(); ++I) {
        BasicBlock *Succ = Successors[I];
        if (isStructuralNoReturnSink(Succ) ||
            (HasExitReachableSuccessor && !PDT.isReachableToExit(Succ))) {
            Cold[I] = true;
            continue;
        }
        if (isDirectReturnBlock(Succ) &&
            !isDirectReturnBlock(Successors[1 - I])) {
            Cold[I] = true;
        }
    }
    if (Cold[0] != Cold[1]) {
        Weights.assign(2, 16);
        Weights[Cold[0] ? 0 : 1] = 1;
        return true;
    }

    // 若一个直接后继本身后支配当前分支，而另一个不后支配，后者是提前退出路径。
    const bool FirstPostDominates = PDT.postDominates(Successors[0], BB);
    const bool SecondPostDominates = PDT.postDominates(Successors[1], BB);
    if (FirstPostDominates != SecondPostDominates) {
        Weights.assign(2, 1);
        Weights[FirstPostDominates ? 0 : 1] = 8;
        return true;
    }
    return false;
}


BranchProbabilityAnalysis::Result BranchProbabilityAnalysis::run(
    Function &F, FunctionAnalysisPassManager &FAM) const {
    BranchProbabilityInfo Result;
    if (!F.front()) return Result;
    //1.收集基本块和CFG边数量
    std::vector<BasicBlock *> Blocks;
    std::size_t EdgeCount = 0;
    for (BasicBlock *BB : F) {
        if (!BB) continue;
        Blocks.push_back(BB);
        EdgeCount += BB->getSuccessors().size();
    }
    Result.AnalyzedEdgeCount = EdgeCount;

    //2.检查基本块和CFG边是否超预算
    const bool OverBudget = Blocks.size() > BlockBudget ||
                            EdgeCount > EdgeBudget;
    if (OverBudget) {
        //超预算：直接给每个块均匀概率，降级算法
        for (BasicBlock *BB : Blocks) {
            auto Successors = BB->getSuccessors();
            assignUniformProbabilities(Successors.size(), Result.EdgeProbabilities[BB]);
            Result.ReachableBlocks.insert(BB);
        }
        Result.Reliable = false;
        Result.UsedFallback = true;
        return Result;
    }

    //3.收集函数入口可达块
    F.collectReachableBlocks(Result.ReachableBlocks);

    auto &DT = FAM.getResult<DominantAnalysis>(F);
    auto &PDT = FAM.getResult<PostDominantAnalysis>(F);
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &SE = FAM.getResult<SCEVAnalysis>(F);

    //4.计算循环权重
    //循环权重：ExitingBlock -> successor CFG边权重向量
    std::unordered_map<BasicBlock *, std::vector<uint64_t>> LoopWeights;
    std::size_t ExaminedLoops = 0;
    //后序：先处理内层循环，再处理外层循环
    for (Loop *L : LI.getLoopsInPostOrder()) {
        if (!L) continue;
        if (++ExaminedLoops > LoopBudget) {
            Result.Reliable = false;
            Result.UsedFallback = true;
            break;
        }
        std::vector<uint64_t> Weights;
        BasicBlock *ExitingBlock = nullptr;
        if (buildLoopWeights(L, SE, Weights, ExitingBlock) &&
            ExitingBlock && !LoopWeights.count(ExitingBlock)) {
            LoopWeights.emplace(ExitingBlock, std::move(Weights));
        }
    }

    for (BasicBlock *BB : Blocks) {
        auto Successors = BB->getSuccessors();
        auto &Probabilities = Result.EdgeProbabilities[BB];
        if (Successors.empty()) continue;
        if (Successors.size() == 1) {
            Probabilities.assign(1, BranchProbabilityInfo::ProbabilityScale);
            continue;
        }

        BrInst *Branch = BB->getBranchTerminator();
        bool ConstantCondition = false;
        bool ConditionValue = false;
        if (BasicBlock::isCondBr(Branch) && Branch->getCondition()) {
            ConstantCondition = Branch->getCondition()->tryGetBoolConst(
                ConditionValue);
        }
        if (ConstantCondition) {
            Probabilities.assign(Successors.size(), 0);
            Probabilities[ConditionValue ? 0 : 1] =
                BranchProbabilityInfo::ProbabilityScale;
            continue;
        }

        auto LoopIt = LoopWeights.find(BB);
        if (LoopIt != LoopWeights.end() &&
            LoopIt->second.size() == Successors.size()) {
            normalizeWeights(LoopIt->second, Probabilities);
            continue;
        }

        std::vector<uint64_t> Weights;
        if (Result.ReachableBlocks.count(BB) &&
            buildUnknownLoopWeights(BB, LI, DT, Weights)) {
            normalizeWeights(Weights, Probabilities);
            continue;
        }
        if (Result.ReachableBlocks.count(BB) &&
            buildColdExitWeights(BB, PDT, Weights)) {
            normalizeWeights(Weights, Probabilities);
            continue;
        }
        assignUniformProbabilities(
            Successors.size(), Probabilities);
    }

    // 存在入口可达但不能到达真实出口的区域时，概率仍可用作提示，但频率方程
    // 可能含不可吸收环，因此向下游明确降级可靠性。
    for (BasicBlock *BB : Result.ReachableBlocks) {
        if (!PDT.isReachableToExit(BB)) {
            Result.Reliable = false;
            break;
        }
    }
    return Result;
}
