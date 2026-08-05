#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include "lib/IRUtils.hpp"
#include <vector>
#include <unordered_map>
#include <memory>
#include <set>
#include <stack>
#include <algorithm>
class DominatorTree;
class DominatorNode;

//SemiNCAInfo：支配树Build类，使用Semi-NCA算法构建
class SemiNCAInfo {
public:
    //InterInfo：DFS编号对应的Semi-NCA算法信息
    struct InterInfo {
        //当前基本块在可达CFG中的DFS编号
        unsigned DFSNum = 0;
        //当前基本块在DFS树中的父节点编号
        unsigned Parent = 0;
        //当前基本块的半支配者编号
        unsigned SdomNum = 0;
        //当前基本块的直接支配者编号
        unsigned IDomNum = 0;
        //并查集：半支配最小的候选节点编号
        unsigned Label = 0;
        //并查集：当前节点所在的并查集父节点编号
        unsigned Father = 0;
        //当前基本块所有CFG前驱的DFS编号
        std::vector<unsigned> Predecessors;
    };
private:
    //DFS编号 --> 每个算法节点的信息，0号元素作为哨兵
    std::vector<InterInfo> Infos;
    //可达BasicBlock* --> DFS编号
    std::unordered_map<BasicBlock*, unsigned> nodeToDFSNum;
    //DFS编号 --> BasicBlock*
    std::vector<BasicBlock*> numToNode;
    //当前已经分配的最大DFS编号
    unsigned lastNum = 0;
    //压缩并查集路径并维护路径上半支配编号最小的Label
    void compress(unsigned v);
    //返回节点所在并查集路径上半支配编号最小的候选节点
    unsigned eval(unsigned v);
    //将子节点连接到父节点所在的并查集中
    void link(unsigned parent, unsigned child);
    //构建第1步：从函数入口遍历可达CFG并记录DFS编号、父节点和前驱
    void runDFS(BasicBlock* entryBlock);
    //构建第2步：根据编号化CFG计算每个节点的半支配者和直接支配者
    void runSemiNCA();
    //构建第3步：使用IDom建树
    void buildDominatorTree(DominatorTree& DT);
public:
    //外层接口：供给run函数调用
    DominatorTree calculate(Function* func);
};

//DominatorNode：支配树结点
class DominatorNode {
public:
    //当前支配树节点对应的基本块
    BasicBlock* Block = nullptr;
    //当前节点的直接支配者节点，根节点为nullptr
    DominatorNode* IDom = nullptr;
    //当前节点的第一个直接子节点
    DominatorNode* FirstChild = nullptr;
    //当前节点在直接支配者子节点链表中的下一个兄弟节点
    DominatorNode* NextSibling = nullptr;
    //当前节点在支配树中的层级，根节点层级为0
    unsigned Level = 0;
    /*欧拉区间*/
    //当前节点进入支配树DFS时的时间戳
    unsigned DFSNumIn = 0;
    //当前节点离开支配树DFS时覆盖到的时间戳
    unsigned DFSNumOut = 0;
    //判断当前节点是否没有直接子节点
    bool isLeaf() const;
};

//DominatorTree：支配树
class DominatorTree {
    friend class SemiNCAInfo;
public:
    //支配树根节点，对应函数入口基本块
    DominatorNode* Root = nullptr;
    //保存所有支配树节点，0号元素作为哨兵
    std::vector<DominatorNode> Nodes;
    //基本块->支配树节点编号的索引
    std::unordered_map<BasicBlock*, unsigned> BlockToNode;

    void clear();
    DominatorTree();
    DominatorTree(const DominatorTree&) = delete;
    DominatorTree& operator=(const DominatorTree&) = delete;
    DominatorTree(DominatorTree&& other) noexcept;
    DominatorTree& operator=(DominatorTree&& other) noexcept;

    //返回给定基本块的直接支配者基本块
    BasicBlock* getIDom(BasicBlock* BB) const;
    //返回给定基本块对应的支配树节点
    DominatorNode* getNode(BasicBlock* BB) const;
    //判断基本块A是否支配基本块B -- 使用欧拉区间直接常数时间判断
    //A支配B：A的欧拉区间包含B的欧拉区间
    bool dominates(BasicBlock* A, BasicBlock* B) const;
    //按照后序遍历返回所有支配树节点
    std::vector<DominatorNode *> getNodesInPostOrder() const;
    //返回给定支配树节点的所有直接子节点
    std::vector<DominatorNode *> getChildNodes(DominatorNode *Node) const;
    //按照前序遍历返回所有支配树节点对应的基本块
    std::vector<BasicBlock *> getBlocksInPreOrder() const;
    //返回给定基本块在支配树中的所有直接子基本块
    std::vector<BasicBlock *> getChildren(BasicBlock *BB) const;
    //计算定义块集合的迭代支配边界，提供LiveInBlocks时执行裁剪
    std::set<BasicBlock *> getIteratedDominanceFrontier(
        const std::set<BasicBlock *> &DefBlocks,
        const std::set<BasicBlock *> *LiveInBlocks = nullptr) const;
    //DefVal定义的位置是否支配UseVal使用的位置--指令级支配
    bool dominatesValue(Value* DefVal, Value* UseVal) const;
    bool invalidate(Function &F, const PreservationStatus &PA);
};

//DominantAnalysis：最外层包装类
class DominantAnalysis : public AnalysisDescriptor<DominantAnalysis> {
public:
    using Result = DominatorTree;
    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;
private:
    friend AnalysisDescriptor<DominantAnalysis>;
    static PassID Key;
};
