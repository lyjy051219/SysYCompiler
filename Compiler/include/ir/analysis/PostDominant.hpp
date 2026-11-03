// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

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
class PostDominatorTree;
class PostDominatorNode;
 
//PostSemiNCAInfo：后支配树Build类，使用Semi-NCA算法构建
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
        //并查集：半支配最小的候选节点编号
        unsigned Label   = 0;
        //并查集：当前节点所在的并查集父结点编号
        unsigned Father  = 0;
        //反向CFG中所有指向当前节点的DFS编号
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
/*和支配树不同的地方*/
    //反向CFG，存储为BasicBlock* --> 反向后继BasicBlock*列表（原来的前驱）
    std::unordered_map<BasicBlock*, std::vector<BasicBlock*>> reverseCFG;
    //函数真实出口基本块
    std::unordered_set<BasicBlock*> exitReachableSet;

    //压缩并查集路径并维护路径上的最优半支配候选
    void compress(unsigned v);
    //返回指定DFS编号对应的最优半支配候选编号
    unsigned eval(unsigned v);
    //在并查集中连接父节点编号和子节点编号
    void link(unsigned parent, unsigned child);
    //在反向CFG的编号空间中求解半支配节点和直接后支配节点
    void runSemiNCA();
    //创建连接所有真实出口的虚拟根节点
    void addVirtualRoot();
    //从指定出口沿反向CFG进行DFS编号并记录前驱关系
    void runDFS(BasicBlock* startBlock, unsigned parentDFSNum);
    //根据求解完成的直接后支配编号构建后支配树
    void buildPostDominatorTree(PostDominatorTree& PDT);
public:
    //接口函数：供给run函数调用
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

//PostDominatorTree：后支配树
class PostDominatorTree {
    friend class PostSemiNCAInfo;
public:
    //后支配树的虚拟根节点
    PostDominatorNode* Root = nullptr;
    //按照反向CFG的DFS编号持有全部后支配树节点
    std::vector<PostDominatorNode> Nodes;
    //基本块->后支配树节点编号的索引
    std::unordered_map<BasicBlock*, unsigned> BlockToNode;
    //保存所有从函数入口可达且能够到达真实出口的基本块
    std::unordered_set<BasicBlock*> ExitReachable;
    void clear();
    PostDominatorTree();
    PostDominatorTree(const PostDominatorTree&) = delete;
    PostDominatorTree& operator=(const PostDominatorTree&) = delete;
    PostDominatorTree(PostDominatorTree&& other) noexcept;
    PostDominatorTree& operator=(PostDominatorTree&& other) noexcept;
    //返回指定基本块的直接后支配块，直接后支配者为虚拟根时返回nullptr
    BasicBlock* getIPostDom(BasicBlock* BB) const;
    //返回指定基本块对应的后支配树节点，传入nullptr时返回虚拟根
    PostDominatorNode* getNode(BasicBlock* BB) const;
    //判断基本块A是否后支配基本块B，其中nullptr表示虚拟根
    bool postDominates(BasicBlock* A, BasicBlock* B) const;
    //判断指定基本块是否能够到达函数的某个真实出口
    bool isReachableToExit(BasicBlock* BB) const;
    bool invalidate(Function &F, const PreservationStatus &PA);
};

//PostDominantAnalysis：函数级后支配树分析Pass
class PostDominantAnalysis : public AnalysisDescriptor<PostDominantAnalysis> {
public:
    using Result = PostDominatorTree;
    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;
private:
    friend AnalysisDescriptor<PostDominantAnalysis>;
    static PassID Key;
};
