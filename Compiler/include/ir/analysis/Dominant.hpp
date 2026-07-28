#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include "lib/IRUtils.hpp"
#include <vector>
#include <unordered_map>
#include <memory>
#include <stack>
#include <algorithm>

class DominatorTree;
class DominatorNode;

// Semi-NCA 构建器保存一次函数级求解的 DFS 编号、半支配者和并查集状态。
// calculate 按“编号 CFG、求解 IDom、封装支配树”三个阶段生成结果。
class SemiNCAInfo {
public:

    // 每个 DFS 编号对应一个算法节点；Predecessors 使用同一编号空间。
    struct InterInfo {
        unsigned DFSNum = 0;
        unsigned Parent = 0;
        unsigned SdomNum = 0;
        unsigned IDomNum = 0;
        unsigned Label = 0;
        unsigned Father = 0;
        std::vector<unsigned> Predecessors;
    };

private:

    std::vector<InterInfo> Infos;

    std::unordered_map<BasicBlock*, unsigned> nodeToDFSNum;

    std::vector<BasicBlock*> numToNode;
    unsigned lastNum = 0;

    void compress(unsigned v);

    unsigned eval(unsigned v);

    void link(unsigned parent, unsigned child);

    void runDFS(BasicBlock* entryBlock);
    void runSemiNCA();
    void buildDominatorTree(DominatorTree& DT);

public:
    DominatorTree calculate(Function* func);
};

// 支配树节点同时维护父子链和欧拉时间戳，供常数时间的支配查询。
class DominatorNode {
public:
    BasicBlock* Block = nullptr;
    DominatorNode* IDom = nullptr;

    DominatorNode* FirstChild = nullptr;
    DominatorNode* NextSibling = nullptr;

    unsigned DFSNumIn = 0;
    unsigned DFSNumOut = 0;

    bool isLeaf() const;
};

// 函数级分析结果拥有所有树节点，并提供块级、值级和遍历顺序查询。
class DominatorTree {
    friend class SemiNCAInfo;
public:

    DominatorNode* Root = nullptr;
    std::vector<DominatorNode> Nodes;
    std::unordered_map<BasicBlock*, unsigned> BlockToNode;

    void clear();

    DominatorTree();

    DominatorTree(const DominatorTree&) = delete;
    DominatorTree& operator=(const DominatorTree&) = delete;

    DominatorTree(DominatorTree&& other) noexcept;
    DominatorTree& operator=(DominatorTree&& other) noexcept;

    // 查询阶段只读取构建完成的树；不可达块或无父节点统一返回空结果。
    BasicBlock* getIDom(BasicBlock* BB) const;
    DominatorNode* getNode(BasicBlock* BB) const;
    bool dominates(BasicBlock* A, BasicBlock* B) const;
    std::vector<DominatorNode *> getNodesInPostOrder() const;
    std::vector<DominatorNode *> getChildNodes(DominatorNode *Node) const;
    std::vector<BasicBlock *> getBlocksInPreOrder() const;
    std::vector<BasicBlock *> getChildren(BasicBlock *BB) const;

    bool dominatesValue(Value* DefVal, Value* UseVal) const;

    bool invalidate(Function &F, const PreservationStatus &PA);

};

// Pass 入口先取得函数入口，再由 SemiNCA 构建并返回完整支配树。
class DominantAnalysis : public AnalysisDescriptor<DominantAnalysis> {
public:

    using Result = DominatorTree;
    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;

private:
    friend AnalysisDescriptor<DominantAnalysis>;
    static PassID Key;
};
