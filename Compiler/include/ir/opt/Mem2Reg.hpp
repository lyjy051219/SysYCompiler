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

// 显式支配树 DFS frame：记录下一个子节点及本块为每个 alloca 压入值栈的次数。
struct Mem2RegRenameFrame {
    DominatorNode *Node = nullptr;
    DominatorNode *ChildIter = nullptr;
    std::vector<unsigned> PushCounts;
    bool Processed = false;
};

// 流程：1. 筛选只被直接标量 load/store 使用的入口 alloca；
// 2. 先走无 PHI 快速路径；3. 以活跃性裁剪 IDF 并放置 PHI；
// 4. 沿支配树重命名 load/store；5. 删除内存指令并迭代折叠退化 PHI。
// timer 不访问 SysY 程序对象，因此不会阻止纯 SSA 表示提升。
class PromotePass : public PassDescriptor<PromotePass> {
public:
    PassResult run(Function &F, FunctionAnalysisPassManager &FAM);
private:
    bool promoteMemoryToRegister(Function& F, DominatorTree& dominant);
    bool isAllocaPromotable(const AllocaInst* AI);
    bool areAllocaUsersReachable(const AllocaInst* AI, DominatorTree& DT);
    bool rewriteSingleStoreAlloca(AllocaInfo &info, AllocaInst* AI, DominatorTree& DT, BlockInfo &blockInfo);
    bool rewriteSingleBlockAlloca(AllocaInfo &info, AllocaInst* AI, DominatorTree& DT, BlockInfo &blockInfo);

    void computeLiveInBlocks(
        AllocaInst* AI,
        const std::set<BasicBlock*>& DefBlocks,
        std::set<BasicBlock*>& LiveInBlocks,
        const std::unordered_map<BasicBlock*, std::vector<BasicBlock*>>& PredMap);

    void computeIDF(
        const std::set<BasicBlock*>& DefBlocks,
        const std::set<BasicBlock*>& LiveInBlocks,
        std::set<BasicBlock*>& PhiBlocks,
        DominatorTree& DT,
        const std::unordered_map<DominatorNode*, unsigned>& DomLevels);

    bool simplifyPhiNode(PhiInst* Phi);
};

// 单个 alloca 的访问摘要，供唯一 store 与单块快速路径判定。
struct AllocaInfo{
    std::vector<BasicBlock*> StoreBlocks;
    std::vector<BasicBlock*> LoadBlocks;
    BasicBlock* OnlyBlock;
    StoreInst* OnlyStore;
    bool OnlyUsedOneBlock;

    void clean();

    void AnalyzeAlloca(AllocaInst*AI);
};

// 按需缓存块内 alloca load/store 的相对次序；删除指令时同步驱逐索引。
class BlockInfo{
    std::map<const Instruction*,unsigned> InstNum;
public:
    unsigned getInstIndex(const Instruction* I);
    void deleteIndex(const Instruction* I);
};
