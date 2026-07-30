#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include "lib/IRUtils.hpp"
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <stack>
#include <algorithm>

//提前声明后支配树分析结果
class PostDominatorTree;
//提前声明后支配树节点
class PostDominatorNode;

//PostSemiNCAInfo：在反向CFG视图上使用Semi-NCA算法构建后支配树
class PostSemiNCAInfo {
public:
    //InterInfo：记录反向CFG中单个DFS编号对应的Semi-NCA求解状态
    struct InterInfo {
        //当前节点在反向CFG中的DFS编号
        unsigned DFSNum  = 0;
        //当前节点在反向DFS树中的父节点编号
        unsigned Parent  = 0;
        //当前节点的半支配节点编号
        unsigned SdomNum = 0;
        //当前节点的直接后支配节点编号
        unsigned IDomNum = 0;
        //并查集求值时当前节点对应的最优半支配候选编号
        unsigned Label   = 0;
        //并查集中当前节点的父节点编号
        unsigned Father  = 0;
        //反向CFG中所有指向当前节点的DFS编号
        std::vector<unsigned> Predecessors;
    };
private:
    //按照DFS编号保存所有节点的Semi-NCA求解状态
    std::vector<InterInfo> Infos;
    //将真实基本块映射到其反向CFG的DFS编号
    std::unordered_map<BasicBlock*, unsigned> nodeToDFSNum;
    //将反向CFG的DFS编号映射回真实基本块，其中虚拟根对应nullptr
    std::vector<BasicBlock*> numToNode;
    //记录当前已经分配的最大DFS编号
    unsigned lastNum = 0;
    //保存入口可达区域的反向CFG邻接表
    std::unordered_map<BasicBlock*, std::vector<BasicBlock*>> reverseCFG;
    //保存所有从函数入口可达且能够到达真实出口的基本块
    std::unordered_set<BasicBlock*> exitReachableSet;
    //压缩并查集路径并维护路径上的最优半支配候选
    void compress(unsigned v);
    //返回指定DFS编号对应的最优半支配候选编号
    unsigned eval(unsigned v);
    //在并查集中连接父节点编号和子节点编号
    void link(unsigned parent, unsigned child);
    //在反向CFG的编号空间中求解半支配节点和直接后支配节点
    void runSemiNCA();
    //预计算入口可达区域中每个基本块的反向CFG邻接关系
    void precomputeReverseCFG(Function* func,
                              const std::unordered_set<BasicBlock*>& reachable);
    //创建连接所有真实出口的虚拟根节点
    void addVirtualRoot();
    //从指定出口沿反向CFG进行DFS编号并记录前驱关系
    void runDFS(BasicBlock* startBlock, unsigned parentDFSNum);
    //根据求解完成的直接后支配编号构建后支配树
    void buildPostDominatorTree(PostDominatorTree& PDT);
    //返回入口可达区域中没有后继的所有真实出口块
    std::vector<BasicBlock*> findExitBlocks(
        Function* func, const std::unordered_set<BasicBlock*>& reachable);
public:
    //分析指定函数并返回其后支配树及可达出口集合
    PostDominatorTree calculate(Function* func);
};

//PostDominatorNode：后支配树节点，其中Block为空表示分析层虚拟根
class PostDominatorNode {
public:
    //当前节点对应的真实基本块，虚拟根对应nullptr
    BasicBlock* Block = nullptr;
    //当前节点在后支配树中的直接父节点
    PostDominatorNode* IDom = nullptr;
    //当前节点的第一个子节点
    PostDominatorNode* FirstChild = nullptr;
    //当前节点在父节点子链表中的下一个兄弟节点
    PostDominatorNode* NextSibling = nullptr;
    //当前节点在后支配树DFS中的进入时间
    unsigned DFSNumIn  = 0;
    //当前节点在后支配树DFS中的离开时间
    unsigned DFSNumOut = 0;
    //判断当前节点是否为分析层虚拟根
    bool isVirtualRoot() const;
    //判断当前节点是否为后支配树叶节点
    bool isLeaf() const;
};

//PostDominatorTree：函数级后支配分析结果，保存后支配树和可达出口集合
class PostDominatorTree {
    //允许后支配构建器直接填充树节点和可达出口信息
    friend class PostSemiNCAInfo;
public:
    //后支配树的虚拟根节点
    PostDominatorNode* Root = nullptr;
    //按照反向CFG的DFS编号持有全部后支配树节点
    std::vector<PostDominatorNode> Nodes;
    //将真实基本块映射到Nodes中的节点下标
    std::unordered_map<BasicBlock*, unsigned> BlockToNode;
    //保存所有从函数入口可达且能够到达真实出口的基本块
    std::unordered_set<BasicBlock*> ExitReachable;
    //清空后支配树节点、映射和可达出口信息
    void clear();
    //构造空的后支配树分析结果
    PostDominatorTree();
    //禁止拷贝后支配树，避免内部节点指针失效
    PostDominatorTree(const PostDominatorTree&) = delete;
    //禁止拷贝赋值后支配树，避免内部节点指针失效
    PostDominatorTree& operator=(const PostDominatorTree&) = delete;
    //移动构造后支配树分析结果
    PostDominatorTree(PostDominatorTree&& other) noexcept;
    //移动赋值后支配树分析结果
    PostDominatorTree& operator=(PostDominatorTree&& other) noexcept;
    //返回指定基本块的直接后支配块，直接后支配者为虚拟根时返回nullptr
    BasicBlock* getIPostDom(BasicBlock* BB) const;
    //返回指定基本块对应的后支配树节点，传入nullptr时返回虚拟根
    PostDominatorNode* getNode(BasicBlock* BB) const;
    //判断基本块A是否后支配基本块B，其中nullptr表示虚拟根
    bool postDominates(BasicBlock* A, BasicBlock* B) const;
    //判断指定基本块是否能够到达函数的某个真实出口
    bool isReachableToExit(BasicBlock* BB) const;
    //根据Pass保留状态判断当前分析结果是否失效
    bool invalidate(Function &F, const PreservationStatus &PA);
};

//PostDominantAnalysis：函数级后支配树分析Pass
class PostDominantAnalysis : public AnalysisDescriptor<PostDominantAnalysis> {
public:
    //定义该分析Pass返回的结果类型
    using Result = PostDominatorTree;
    //运行函数级后支配分析并返回后支配树
    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;
private:
    //允许AnalysisDescriptor访问当前分析Pass的唯一标识
    friend AnalysisDescriptor<PostDominantAnalysis>;
    //当前分析Pass的唯一标识
    static PassID Key;
};
