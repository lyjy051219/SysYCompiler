#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include "lib/Tool.hpp"
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <stack>
#include <algorithm>

class PostDominatorTree;
class PostDominatorNode;

// 后支配树 Semi-NCA 算法实现类
// 分析层虚拟化，不修改真实 CFG
// 在反向 CFG 视图上运行 Semi-NCA，虚拟根为 analysis-layer 的 nullptr 节点
class PostSemiNCAInfo {
public:
    // 和 SemiNCAInfo::InterInfo 完全一致
    struct InterInfo {
        unsigned DFSNum  = 0;    // DFS 序号 (1-based, 1=虚拟根)
        unsigned Parent  = 0;    // DFS树中的父节点序号
        unsigned SdomNum = 0;    // 半支配者的序号
        unsigned IDomNum = 0;    // 直接支配者序号
        unsigned Label   = 0;    // 并查集路径上sdom值最小的那个编号
        unsigned Father  = 0;    // 并查集里的父节点 (0表示它是根)
        std::vector<unsigned> Predecessors; // 反向图中的前驱 (DFS编号)
    };

private:
    std::vector<InterInfo> Infos;                             // DFS序 -> InterInfo, [0]占位
    std::unordered_map<BasicBlock*, unsigned> nodeToDFSNum;   // Block* -> DFS序
    std::vector<BasicBlock*> numToNode;                       // DFS序 -> Block*, [0]=nullptr占位, [1]=nullptr(虚拟根)
    unsigned lastNum = 0;

    // 预计算的反向CFG: reverseCFG[B] = B 在原 CFG 中的所有 reachable 前驱
    // 在 postdom DFS 中作为"前向邻居"使用
    std::unordered_map<BasicBlock*, std::vector<BasicBlock*>> reverseCFG;

    // 步骤5中被反向DFS访问的块 = 能到达出口的块
    std::unordered_set<BasicBlock*> exitReachableSet;

    void compress(unsigned v);
    unsigned eval(unsigned v);
    void link(unsigned parent, unsigned child);
    void runSemiNCA();

    // === PostDom 特有 ===
    void precomputeReverseCFG(Function* func,
                              const std::unordered_set<BasicBlock*>& reachable);
    void addVirtualRoot();
    void runDFS(BasicBlock* startBlock, unsigned parentDFSNum);
    void buildPostDominatorTree(PostDominatorTree& PDT);

    std::unordered_set<BasicBlock*> collectForwardReachable(Function* func);
    std::vector<BasicBlock*> findExitBlocks(
        Function* func, const std::unordered_set<BasicBlock*>& reachable);

public:
    PostDominatorTree calculate(Function* func);
};

// 后支配树节点
class PostDominatorNode {
public:
    BasicBlock* Block = nullptr;          // nullptr 仅用于虚拟根
    PostDominatorNode* IDom = nullptr;    // 直接后支配者 (父节点)

    // 树结构指针
    PostDominatorNode* FirstChild = nullptr;
    PostDominatorNode* NextSibling = nullptr;

    unsigned DFSNumIn  = 0;   // 欧拉序进入时间戳 (用于O(1)查询)
    unsigned DFSNumOut = 0;   // 欧拉序离开时间戳

    bool isVirtualRoot() const { return Block == nullptr; }
    bool isLeaf() const { return FirstChild == nullptr; }
};

// 后支配树
class PostDominatorTree {
    friend class PostSemiNCAInfo;
public:
    PostDominatorNode* Root = nullptr;    // 永远指向虚拟根 (Block==nullptr)
    std::vector<PostDominatorNode> Nodes; // Nodes[1] = 虚拟根
    std::unordered_map<BasicBlock*, unsigned> BlockToNode;

    // 记录哪些块能到达出口 (即在 postdom 树中被访问到)，供 DSE 查询使用
    std::unordered_set<BasicBlock*> ExitReachable;

    void clear() {
        Nodes.clear();
        BlockToNode.clear();
        ExitReachable.clear();
        Root = nullptr;
    }

    PostDominatorTree() = default;
    // 禁用拷贝
    PostDominatorTree(const PostDominatorTree&) = delete;
    PostDominatorTree& operator=(const PostDominatorTree&) = delete;
    // 允许移动
    PostDominatorTree(PostDominatorTree&& other) noexcept = default;
    PostDominatorTree& operator=(PostDominatorTree&& other) noexcept = default;

    // ========== 对外查询接口 ==========

    // 返回 BB 的直接后支配 若 ipdom 是虚拟根则返回 nullptr。
    BasicBlock* getIPostDom(BasicBlock* BB) const;

    // 返回 BB 对应的树节点。BB==nullptr 返回虚拟根。entry不可达块返回 nullptr。
    PostDominatorNode* getNode(BasicBlock* BB) const;

    // A 是否后支配 B。A==nullptr(虚拟根)时对任何树中块返回 true。
    bool postDominates(BasicBlock* A, BasicBlock* B) const;

    // BB 是否能从 entry 经某条路径到达至少一个 exit block。
    // 对 infinite loop 中的块返回 false。DSE 必须检查此条件。
    bool isReachableToExit(BasicBlock* BB) const;

    // 分析缓存失效回调
    bool invalidate(Function &F, const PreservationStatus &PA);
};

// 后支配树分析 Pass
class PostDominantAnalysis : public AnalysisDescriptor<PostDominantAnalysis> {
public:
    using Result = PostDominatorTree;
    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;

private:
    friend AnalysisDescriptor<PostDominantAnalysis>;
    static PassID Key;
};
