#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "ir/analysis/Dominant.hpp"
#include "lib/CFG.hpp"
#include <algorithm>
#include <vector>
#include <utility>
#include <map>
#include <set>
#include <queue>
#include <stack>
#include <unordered_map>

class BlockInfo;
struct AllocaInfo;

struct Mem2RegRenameFrame {
    DominatorNode *Node = nullptr;
    DominatorNode *ChildIter = nullptr;
    std::vector<unsigned> PushCounts;
    bool Processed = false;
};

class PromotePass : public PassDescriptor<PromotePass> {
public:
    PassResult run(Function &F, FunctionAnalysisPassManager &FAM);
private:
    bool promoteMemoryToRegister(Function& F, DominatorTree& dominant);
    bool isAllocaPromotable(const AllocaInst* AI);
    bool areAllocaUsersReachable(const AllocaInst* AI, DominatorTree& DT);
    static bool allocaCrossesTimingBarrier(
        const AllocaInst *AI, const Function &F);
    bool rewriteSingleStoreAlloca(AllocaInfo &info, AllocaInst* AI, DominatorTree& DT, BlockInfo &blockInfo);
    bool rewriteSingleBlockAlloca(AllocaInfo &info, AllocaInst* AI, DominatorTree& DT, BlockInfo &blockInfo);

    // 计算 alloca 的 live-in 块集合（用于 pruned phi placement）
    void computeLiveInBlocks(
        AllocaInst* AI,
        const std::set<BasicBlock*>& DefBlocks,
        std::set<BasicBlock*>& LiveInBlocks,
        const std::unordered_map<BasicBlock*, std::vector<BasicBlock*>>& PredMap);

    // LLVM-style IDF: 优先队列 + 支配子树 DFS，无需预计算全局 DF
    void computeIDF(
        const std::set<BasicBlock*>& DefBlocks,
        const std::set<BasicBlock*>& LiveInBlocks,
        std::set<BasicBlock*>& PhiBlocks,
        DominatorTree& DT,
        const std::unordered_map<DominatorNode*, unsigned>& DomLevels);

    // 简化 trivial phi 结点
    bool simplifyPhiNode(PhiInst* Phi);
};

struct AllocaInfo{
    std::vector<BasicBlock*> StoreBlocks;
    std::vector<BasicBlock*> LoadBlocks;
    BasicBlock* OnlyBlock;
    StoreInst* OnlyStore;
    bool OnlyUsedOneBlock;

    void clean();

    void AnalyzeAlloca(AllocaInst*AI);
};

class BlockInfo{
    std::map<const Instruction*,unsigned> InstNum;
public:
    unsigned getInstIndex(const Instruction* I);
    void deleteIndex(const Instruction* I);
};
