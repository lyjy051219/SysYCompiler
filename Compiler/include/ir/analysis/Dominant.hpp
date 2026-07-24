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

// 0. Semi-NCA算法实现类
class SemiNCAInfo {
public:
    struct InterInfo {
        unsigned DFSNum = 0;    // DFS 序号 (1-based)
        unsigned Parent = 0;    // DFS树中的父节点序号
        unsigned SdomNum = 0;   // 半支配者的序号
        unsigned IDomNum = 0;   // 直接支配者序号
        unsigned Label = 0;     // 并查集路径上sdom值最小的那个编号
        unsigned Father = 0;    // 并查集里的父节点 (0表示它是根)
        std::vector<unsigned> Predecessors; // 前驱结点--记录了该结点在CFG中的所有前驱
    };

private:
    // 映射：DFS序 -> InterInfo(0号留空)
    std::vector<InterInfo> Infos;
    // 映射：BasicBlock* -> DFSNum
    std::unordered_map<BasicBlock*, unsigned> nodeToDFSNum;
    // 映射：DFS序号 -> BasicBlock* (0号留空)
    std::vector<BasicBlock*> numToNode;
    unsigned lastNum = 0;

    // 路径压缩
    void compress(unsigned v);
    // 查找操作 (eval)
    unsigned eval(unsigned v);
    // 连接操作
    void link(unsigned parent, unsigned child);

    void runDFS(BasicBlock* entryBlock);
    void runSemiNCA();
    void buildDominatorTree(DominatorTree& DT);

public:
    DominatorTree calculate(Function* func);
};

// 1. 定义分析结果 (Result)
class DominatorNode {
public:
    BasicBlock* Block = nullptr;     // 对应的基本块
    DominatorNode* IDom = nullptr;   // 直接支配者 (父节点)

    // 树结构指针
    DominatorNode* FirstChild = nullptr;  // 第一个被支配的子节点
    DominatorNode* NextSibling = nullptr; // 下一个兄弟节点

    unsigned DFSNumIn = 0;   // 进入时间戳 (用于O(1)查询支配关系)
    unsigned DFSNumOut = 0;  // 离开时间戳

    // 辅助函数：是否是叶子节点
    bool isLeaf() const;
};

class DominatorTree {
    friend class SemiNCAInfo;
public:
    // 根节点
    DominatorNode* Root = nullptr;
    std::vector<DominatorNode> Nodes;
    std::unordered_map<BasicBlock*, unsigned> BlockToNode;

    void clear();

    DominatorTree();
    //禁用拷贝
    DominatorTree(const DominatorTree&) = delete;
    DominatorTree& operator=(const DominatorTree&) = delete;

    //允许移动
    DominatorTree(DominatorTree&& other) noexcept;
    DominatorTree& operator=(DominatorTree&& other) noexcept;

    // 查询接口
    BasicBlock* getIDom(BasicBlock* BB) const;
    DominatorNode* getNode(BasicBlock* BB) const;
    bool dominates(BasicBlock* A, BasicBlock* B) const; // A 是否支配 B
    std::vector<DominatorNode *> getNodesInPostOrder() const;
    std::vector<DominatorNode *> getChildNodes(DominatorNode *Node) const;
    std::vector<BasicBlock *> getBlocksInPreOrder() const;
    std::vector<BasicBlock *> getChildren(BasicBlock *BB) const;
    // Value级别的支配关系：判断DefVal是否支配UseVal
    bool dominatesValue(Value* DefVal, Value* UseVal) const;

    bool invalidate(Function &F, const PreservationStatus &PA);

};

// 2. 定义分析 Pass
class DominantAnalysis : public AnalysisDescriptor<DominantAnalysis> {
public:
    // 定义 Result 类型，供 Manager 推导
    using Result = DominatorTree;
    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;

private:
    friend AnalysisDescriptor<DominantAnalysis>;
    static PassID Key;
};
