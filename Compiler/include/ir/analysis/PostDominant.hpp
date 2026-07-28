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

// 后支配构建器在反向 CFG 视图上复用 Semi-NCA，不改写真实控制流。
// 虚拟根统一连接所有真实出口，并隔离无法到达出口的区域。
class PostSemiNCAInfo {
public:

    // 编号状态与正向 Semi-NCA 同构，但 Predecessors 表示反向图的入边。
    struct InterInfo {
        unsigned DFSNum  = 0;
        unsigned Parent  = 0;
        unsigned SdomNum = 0;
        unsigned IDomNum = 0;
        unsigned Label   = 0;
        unsigned Father  = 0;
        std::vector<unsigned> Predecessors;
    };

private:
    std::vector<InterInfo> Infos;
    std::unordered_map<BasicBlock*, unsigned> nodeToDFSNum;
    std::vector<BasicBlock*> numToNode;
    unsigned lastNum = 0;

    std::unordered_map<BasicBlock*, std::vector<BasicBlock*>> reverseCFG;

    std::unordered_set<BasicBlock*> exitReachableSet;

    void compress(unsigned v);
    unsigned eval(unsigned v);
    void link(unsigned parent, unsigned child);
    void runSemiNCA();

    void precomputeReverseCFG(Function* func,
                              const std::unordered_set<BasicBlock*>& reachable);
    void addVirtualRoot();
    void runDFS(BasicBlock* startBlock, unsigned parentDFSNum);
    void buildPostDominatorTree(PostDominatorTree& PDT);

    std::vector<BasicBlock*> findExitBlocks(
        Function* func, const std::unordered_set<BasicBlock*>& reachable);

public:
    PostDominatorTree calculate(Function* func);
};

// 后支配节点的 Block 为空仅表示分析层虚拟根。
class PostDominatorNode {
public:
    BasicBlock* Block = nullptr;
    PostDominatorNode* IDom = nullptr;

    PostDominatorNode* FirstChild = nullptr;
    PostDominatorNode* NextSibling = nullptr;

    unsigned DFSNumIn  = 0;
    unsigned DFSNumOut = 0;

    bool isVirtualRoot() const;
    bool isLeaf() const;
};

// 结果同时保存后支配树和可达出口集合，避免无限循环区域被误用于变换证明。
class PostDominatorTree {
    friend class PostSemiNCAInfo;
public:
    PostDominatorNode* Root = nullptr;
    std::vector<PostDominatorNode> Nodes;
    std::unordered_map<BasicBlock*, unsigned> BlockToNode;

    std::unordered_set<BasicBlock*> ExitReachable;

    void clear();

    PostDominatorTree();

    PostDominatorTree(const PostDominatorTree&) = delete;
    PostDominatorTree& operator=(const PostDominatorTree&) = delete;

    PostDominatorTree(PostDominatorTree&& other) noexcept;
    PostDominatorTree& operator=(PostDominatorTree&& other) noexcept;

    // 直接后支配查询以 nullptr 表示虚拟根；变换还必须用 ExitReachable 排除无限区域。
    BasicBlock* getIPostDom(BasicBlock* BB) const;

    PostDominatorNode* getNode(BasicBlock* BB) const;

    bool postDominates(BasicBlock* A, BasicBlock* B) const;

    bool isReachableToExit(BasicBlock* BB) const;

    bool invalidate(Function &F, const PreservationStatus &PA);
};

// Pass 入口构造反向 CFG 视图并返回函数级后支配结果。
class PostDominantAnalysis : public AnalysisDescriptor<PostDominantAnalysis> {
public:
    using Result = PostDominatorTree;
    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;

private:
    friend AnalysisDescriptor<PostDominantAnalysis>;
    static PassID Key;
};
