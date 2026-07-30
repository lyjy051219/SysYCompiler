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

//DominatorTree表示函数的支配树分析结果
class DominatorTree;
//DominatorNode表示支配树中的一个节点
class DominatorNode;

//SemiNCAInfo保存一次函数级Semi-NCA求解所需的编号和并查集状态
class SemiNCAInfo {
public:
    //InterInfo保存一个DFS编号对应的Semi-NCA算法信息
    struct InterInfo {
        //当前基本块在可达CFG中的DFS编号
        unsigned DFSNum = 0;
        //当前基本块在DFS树中的父节点编号
        unsigned Parent = 0;
        //当前基本块的半支配者编号
        unsigned SdomNum = 0;
        //当前基本块的直接支配者编号
        unsigned IDomNum = 0;
        //并查集求值时半支配编号最小的候选节点编号
        unsigned Label = 0;
        //当前节点在并查集中的父节点编号
        unsigned Father = 0;
        //当前基本块所有CFG前驱的DFS编号
        std::vector<unsigned> Predecessors;
    };
private:
    //按照DFS编号保存每个算法节点的信息，0号元素作为哨兵
    std::vector<InterInfo> Infos;
    //将可达基本块映射到对应的DFS编号
    std::unordered_map<BasicBlock*, unsigned> nodeToDFSNum;
    //将DFS编号映射回对应的基本块，0号元素作为哨兵
    std::vector<BasicBlock*> numToNode;
    //记录当前已经分配的最大DFS编号
    unsigned lastNum = 0;
    //压缩并查集路径并维护路径上半支配编号最小的Label
    void compress(unsigned v);
    //返回节点所在并查集路径上半支配编号最小的候选节点
    unsigned eval(unsigned v);
    //将子节点连接到父节点所在的并查集中
    void link(unsigned parent, unsigned child);
    //从函数入口遍历可达CFG并记录DFS编号、父节点和前驱
    void runDFS(BasicBlock* entryBlock);
    //根据编号化CFG计算每个节点的半支配者和直接支配者
    void runSemiNCA();
    //将Semi-NCA编号结果封装为支配树并建立查询信息
    void buildDominatorTree(DominatorTree& DT);
public:
    //计算给定函数的完整支配树
    DominatorTree calculate(Function* func);
};

//DominatorNode表示支配树节点及其父子关系和查询时间戳
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
    //当前节点进入支配树DFS时的时间戳
    unsigned DFSNumIn = 0;
    //当前节点离开支配树DFS时覆盖到的时间戳
    unsigned DFSNumOut = 0;
    //判断当前节点是否没有直接子节点
    bool isLeaf() const;
};

//DominatorTree表示函数级支配树并提供块级和值级支配关系查询
class DominatorTree {
    //允许SemiNCAInfo直接构建支配树的内部存储
    friend class SemiNCAInfo;
public:
    //支配树根节点，对应函数入口基本块
    DominatorNode* Root = nullptr;
    //保存所有支配树节点，0号元素作为哨兵
    std::vector<DominatorNode> Nodes;
    //将基本块映射到Nodes中的节点编号
    std::unordered_map<BasicBlock*, unsigned> BlockToNode;
    //清空当前支配树的所有节点和索引
    void clear();
    //构造一棵空支配树
    DominatorTree();
    //禁止拷贝构造支配树以避免内部节点指针失效
    DominatorTree(const DominatorTree&) = delete;
    //禁止拷贝赋值支配树以避免内部节点指针失效
    DominatorTree& operator=(const DominatorTree&) = delete;
    //移动构造支配树并接管其节点存储
    DominatorTree(DominatorTree&& other) noexcept;
    //移动赋值支配树并接管其节点存储
    DominatorTree& operator=(DominatorTree&& other) noexcept;
    //返回给定基本块的直接支配者基本块
    BasicBlock* getIDom(BasicBlock* BB) const;
    //返回给定基本块对应的支配树节点
    DominatorNode* getNode(BasicBlock* BB) const;
    //判断基本块A是否支配基本块B
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
    //判断定义值是否在控制流和同块指令顺序上支配使用值
    bool dominatesValue(Value* DefVal, Value* UseVal) const;
    //根据PreservationStatus判断当前分析结果是否失效
    bool invalidate(Function &F, const PreservationStatus &PA);
};

//DominantAnalysis是构建函数级支配树的分析Pass
class DominantAnalysis : public AnalysisDescriptor<DominantAnalysis> {
public:
    //分析Pass返回的结果类型
    using Result = DominatorTree;
    //运行支配分析并返回函数的支配树
    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;
private:
    //允许AnalysisDescriptor访问本分析Pass的唯一标识
    friend AnalysisDescriptor<DominantAnalysis>;
    //当前分析Pass的唯一标识
    static PassID Key;
};
