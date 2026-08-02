#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "ir/analysis/Dominant.hpp"
#include "lib/CFG.hpp"
#include <algorithm>
#include <vector>
#include <utility>
#include <map>
#include <set>
#include <stack>
#include <unordered_map>
class BlockInfo;
struct AllocaInfo;

//显式支配树 DFS frame：记录下一个子节点及本块为每个 alloca 压入值栈的次数。
struct Mem2RegRenameFrame {
    //记录当前正在遍历的支配树节点。
    DominatorNode *Node = nullptr;
    //记录下一棵尚未访问的直接支配子树。
    DominatorNode *ChildIter = nullptr;
    //记录当前基本块为各alloca压入SSA值栈的次数。
    std::vector<unsigned> PushCounts;
    //标记当前基本块的PHI、load和store是否已经完成重命名。
    bool Processed = false;
};

//流程：1. 筛选只被直接标量 load/store 使用的入口 alloca；
//2. 先走无 PHI 快速路径；3. 以活跃性裁剪 IDF 并放置 PHI；
//4. 沿支配树重命名 load/store；5. 删除内存指令并迭代折叠退化 PHI。
class PromotePass : public PassDescriptor<PromotePass> {
public:
    PassResult run(Function &F, FunctionAnalysisPassManager &FAM);
private:
    //循环执行快速路径、裁剪PHI放置和支配树重命名直到不再发生提升。
    bool promoteMemoryToRegister(Function& F, DominatorTree& dominant);
    //检查alloca是否只被类型匹配的直接标量load和store使用。
    bool isAllocaPromotable(const AllocaInst* AI);
    //检查alloca的全部访问是否位于入口支配树覆盖的可达区域。
    bool areAllocaUsersReachable(const AllocaInst* AI, DominatorTree& DT);
    //用唯一且支配全部读取的store值直接替换alloca的所有load。
    bool rewriteSingleStoreAlloca(AllocaInfo &info, AllocaInst* AI, DominatorTree& DT, BlockInfo &blockInfo);
    //在单个基本块内用每次load之前最近的store值完成提升。
    bool rewriteSingleBlockAlloca(AllocaInfo &info, AllocaInst* AI, DominatorTree& DT, BlockInfo &blockInfo);

    //反向计算alloca在首次定义前可能被读取的活跃入口块集合。
    void computeLiveInBlocks(
        AllocaInst* AI,
        const std::set<BasicBlock*>& DefBlocks,
        std::set<BasicBlock*>& LiveInBlocks,
        const std::unordered_map<BasicBlock*, std::vector<BasicBlock*>>& PredMap);

    //折叠除自引用外所有入值相同的退化PHI。
    bool simplifyPhiNode(PhiInst* Phi);
};

//单个 alloca 的访问摘要，供唯一 store 与单块快速路径判定。
struct AllocaInfo{
    //记录该alloca全部store所在的基本块。
    std::vector<BasicBlock*> StoreBlocks;
    //记录该alloca全部load所在的基本块。
    std::vector<BasicBlock*> LoadBlocks;
    //当所有访问位于同一块时记录该唯一基本块。
    BasicBlock* OnlyBlock;
    //当alloca只有一次写入时记录该唯一store。
    StoreInst* OnlyStore;
    //标记alloca的全部访问是否位于同一个基本块。
    bool OnlyUsedOneBlock;

    void clean();

    //扫描alloca的使用并建立访问块、唯一store和单块属性摘要。
    void AnalyzeAlloca(AllocaInst*AI);
};

//按需缓存块内 alloca load/store 的相对次序；删除指令时同步驱逐索引。
class BlockInfo{
    //缓存各alloca访问指令在所属基本块中的相对序号。
    std::map<const Instruction*,unsigned> InstNum;
public:
    //返回指令在块内alloca访问序列中的编号并按需建立缓存。
    unsigned getInstIndex(const Instruction* I);
    //在删除指令时同步移除对应的顺序缓存。
    void deleteIndex(const Instruction* I);
};
