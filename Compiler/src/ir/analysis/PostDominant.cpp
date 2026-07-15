#include "ir/analysis/PostDominant.hpp"

bool PostDominatorNode::isVirtualRoot() const {
    return Block == nullptr;
}

bool PostDominatorNode::isLeaf() const {
    return FirstChild == nullptr;
}

PostDominatorTree::PostDominatorTree() = default;

PostDominatorTree::PostDominatorTree(PostDominatorTree &&other) noexcept = default;

PostDominatorTree &PostDominatorTree::operator=(
    PostDominatorTree &&other) noexcept = default;

void PostDominatorTree::clear() {
    Nodes.clear();
    BlockToNode.clear();
    ExitReachable.clear();
    Root = nullptr;
}

// PostDominatorTree 查询接口实现
BasicBlock* PostDominatorTree::getIPostDom(BasicBlock* BB) const {
    auto it = BlockToNode.find(BB);
    if (it == BlockToNode.end()) return nullptr;
    const PostDominatorNode* node = &Nodes[it->second];
    if (!node->IDom) return nullptr;
    return node->IDom->Block;   // 若 ipdom 是虚拟根，Block==nullptr，返回 nullptr
}

PostDominatorNode* PostDominatorTree::getNode(BasicBlock* BB) const {
    if (BB == nullptr) {
        // 返回虚拟根
        return Root ? const_cast<PostDominatorNode*>(Root) : nullptr;
    }
    auto it = BlockToNode.find(BB);
    if (it == BlockToNode.end()) return nullptr;
    return const_cast<PostDominatorNode*>(&Nodes[it->second]);
}

bool PostDominatorTree::postDominates(BasicBlock* A, BasicBlock* B) const {
    if (A == B) return true;
    if (B == nullptr) return false;  // 真实块不后支配虚拟根

    // A==nullptr 表示虚拟根，但必须确认 B 在树中才能返回 true
    if (A == nullptr) {
        return getNode(B) != nullptr;
    }

    PostDominatorNode* NodeA = getNode(A);
    PostDominatorNode* NodeB = getNode(B);
    if (!NodeA || !NodeB) return false;

    // O(1) 时间戳区间判断
    return NodeA->DFSNumIn <= NodeB->DFSNumIn &&
           NodeA->DFSNumOut >= NodeB->DFSNumOut;
}

bool PostDominatorTree::isReachableToExit(BasicBlock* BB) const {
    return BB != nullptr && ExitReachable.count(BB) > 0;
}

bool PostDominatorTree::invalidate(Function &F, const PreservationStatus &PA) {
    if (PA.isKept(PostDominantAnalysis::ID())) {
        return false;
    }
    return true;
}

// PostSemiNCAInfo: 四个核心函数 (从 SemiNCAInfo 复制)
// compress / eval / link / runSemiNCA
// 这四个函数只操作 DFS 编号数组 Infos，完全不依赖 BasicBlock* 的具体接口
void PostSemiNCAInfo::compress(unsigned v) {
    unsigned father = Infos[v].Father;
    if (Infos[father].Father != 0) {
        compress(father);
        InterInfo& vLabelInfo = Infos[Infos[v].Label];
        InterInfo& aLabelInfo = Infos[Infos[father].Label];
        if (aLabelInfo.SdomNum < vLabelInfo.SdomNum) {
            Infos[v].Label = Infos[father].Label;
        }
        Infos[v].Father = Infos[father].Father;
    }
}

unsigned PostSemiNCAInfo::eval(unsigned v) {
    if (Infos[v].Father == 0) return v;
    compress(v);
    return Infos[v].Label;
}

void PostSemiNCAInfo::link(unsigned parent, unsigned child) {
    Infos[child].Father = parent;
}

void PostSemiNCAInfo::runSemiNCA() {
    // 1. 计算半支配节点 (逆DFS序遍历)
    for (unsigned i = lastNum; i >= 2; --i) {
        for (unsigned pre : Infos[i].Predecessors) {
            if (pre == 0) continue;
            unsigned u = eval(pre);
            if (Infos[u].SdomNum < Infos[i].SdomNum) {
                Infos[i].SdomNum = Infos[u].SdomNum;
            }
        }
        link(Infos[i].Parent, i);
    }

    // 2. 计算支配节点 -- SemiNCA (正DFS序遍历)
    for (unsigned i = 2; i <= lastNum; ++i) {
        unsigned runner = Infos[i].Parent;
        unsigned sdom = Infos[i].SdomNum;
        while (runner > sdom) {
            runner = Infos[runner].IDomNum;
        }
        Infos[i].IDomNum = runner;
    }
}

// 在 reachable 集合中，找出所有 getSuccessors() 为空的块 (即以 RetInst 结尾)
std::vector<BasicBlock*> PostSemiNCAInfo::findExitBlocks(
    Function* func, const std::unordered_set<BasicBlock*>& reachable)
{
    std::vector<BasicBlock*> exits;
    for (BasicBlock* BB : *func) {
        if (!reachable.count(BB)) continue;
        if (BB->Size() == 0) continue;  // 跳过空块
        auto succs = BB->getSuccessors();
        if (succs.empty()) {
            exits.push_back(BB);
        }
    }
    return exits;
}

// 一次性扫描所有 reachable 块的 successor 边，建反向邻接表
// reverseCFG[B] = B 在原 CFG 中的所有 reachable 前驱
void PostSemiNCAInfo::precomputeReverseCFG(Function* func,
    const std::unordered_set<BasicBlock*>& reachable)
{
    for (BasicBlock* BB : *func) {
        if (!reachable.count(BB)) continue;
        for (BasicBlock* succ : BB->getSuccessors()) {
            if (succ && reachable.count(succ)) {
                reverseCFG[succ].push_back(BB);
            }
        }
    }
}

// 为虚拟根分配 DFS #1, Block = nullptr
void PostSemiNCAInfo::addVirtualRoot() {
    ++lastNum;  // lastNum 变为 1
    numToNode.push_back(nullptr);   // numToNode[1] = nullptr (虚拟根)
    Infos.emplace_back();
    Infos[lastNum].DFSNum  = lastNum;
    Infos[lastNum].SdomNum = lastNum;
    Infos[lastNum].Label   = lastNum;
    Infos[lastNum].Parent  = 0;     // 根没有父节点
}

// 从 startBlock 出发，沿 reverseCFG 做迭代 DFS
// parentDFSNum: 起始块的父节点 DFS 编号 (通常是 1 = 虚拟根)
void PostSemiNCAInfo::runDFS(BasicBlock* startBlock, unsigned parentDFSNum) {
    std::stack<std::pair<BasicBlock*, unsigned>> st;
    st.push({startBlock, parentDFSNum});

    while (!st.empty()) {
        auto [currBlock, pDFS] = st.top();
        st.pop();

        unsigned currNum = 0;
        auto it = nodeToDFSNum.find(currBlock);

        if (it == nodeToDFSNum.end()) {
            // 首次访问：分配 DFS 序号
            currNum = ++lastNum;
            nodeToDFSNum[currBlock] = currNum;
            numToNode.push_back(currBlock);

            Infos.emplace_back();
            Infos[currNum].DFSNum  = currNum;
            Infos[currNum].SdomNum = currNum;
            Infos[currNum].Label   = currNum;
            Infos[currNum].Parent  = pDFS;

            // 沿反向 CFG 推入邻居 (即原 CFG 前驱)
            auto revIt = reverseCFG.find(currBlock);
            if (revIt != reverseCFG.end()) {
                const auto& neighbors = revIt->second;
                for (auto sit = neighbors.rbegin(); sit != neighbors.rend(); ++sit) {
                    st.push({*sit, currNum});
                }
            }
        } else {
            currNum = it->second;
        }

        // 记录反向图中的"前驱"关系
        if (pDFS != 0) {
            Infos[currNum].Predecessors.push_back(pDFS);
        }
    }
}

// 从 Infos 数组构建 PostDominatorTree
void PostSemiNCAInfo::buildPostDominatorTree(PostDominatorTree& PDT) {
    PDT.clear();
    if (lastNum == 0) return;

    // 1. 分配节点数组
    PDT.Nodes.resize(lastNum + 1);

    // 2. 填充基本信息 & 建立映射
    for (unsigned i = 1; i <= lastNum; ++i) {
        BasicBlock* BB = numToNode[i];
        PDT.Nodes[i].Block = BB;    // i==1 时 BB==nullptr (虚拟根)
        if (BB) {
            PDT.BlockToNode[BB] = i;
        }
    }

    // 设置根节点 (虚拟根)
    PDT.Root = &PDT.Nodes[1];

    // 3. 构建树结构 (IDom 指针和 Child 链表)
    for (unsigned i = 2; i <= lastNum; ++i) {
        unsigned idomIndex = Infos[i].IDomNum;
        PostDominatorNode* currNode = &PDT.Nodes[i];
        PostDominatorNode* idomNode = &PDT.Nodes[idomIndex];

        currNode->IDom = idomNode;
        currNode->NextSibling = idomNode->FirstChild;
        idomNode->FirstChild = currNode;
    }

    // 4. 计算 DFS in/out 时间戳 (用于 O(1) postDominates 查询)
    unsigned time = 0;
    std::vector<PostDominatorNode*> stack;
    stack.push_back(PDT.Root);

    while (!stack.empty()) {
        PostDominatorNode* node = stack.back();
        if (node->DFSNumIn == 0) {
            node->DFSNumIn = ++time;
            for (PostDominatorNode* child = node->FirstChild; child; child = child->NextSibling) {
                stack.push_back(child);
            }
        } else {
            if (node->DFSNumOut == 0) {
                node->DFSNumOut = time;
                stack.pop_back();
            } else {
                stack.pop_back();
            }
        }
    }
}

// PostSemiNCAInfo::calculate -- 主入口
PostDominatorTree PostSemiNCAInfo::calculate(Function* func) {
    PostDominatorTree PDT;

    // 0. 初始化
    lastNum = 0;
    Infos.clear();
    numToNode.clear();
    nodeToDFSNum.clear();
    reverseCFG.clear();
    exitReachableSet.clear();
    // [0]号占位 (DFS序从1开始)
    Infos.emplace_back();
    numToNode.push_back(nullptr);

    if (func->Size() == 0) return PDT;

    // 1. 收集 entry 可达块
    std::unordered_set<BasicBlock*> reachable;
    func->collectReachableBlocks(reachable);
    if (reachable.empty()) return PDT;

    // 2. 找出口块 (getSuccessors() 为空的 reachable 块)
    auto exits = findExitBlocks(func, reachable);
    // exits 可能为空 (纯 infinite loop 函数)

    // 3. 预计算反向 CFG
    precomputeReverseCFG(func, reachable);

    // 4. 添加虚拟根节点，DFS 序号为 1，Block = nullptr
    addVirtualRoot();

    // 5. 从每个出口块做反向 DFS，以虚拟根为父
    for (BasicBlock* exitBB : exits) {
        runDFS(exitBB, 1);  // parentDFSNum=1 = 虚拟根
    }

    // 记录步骤5中访问到的块 = 能到达出口的块
    for (auto& kv : nodeToDFSNum) {
        if (kv.first != nullptr) {  // 排除虚拟根
            exitReachableSet.insert(kv.first);
        }
    }

    // 6. 处理 reverse-unreachable 块 (entry 可达但无法到达任何 exit)
    //    这些块直接作为虚拟根的子节点，不继续沿反向图 DFS
    //    DSE 等消费者必须先用 isReachableToExit() 过滤，不应依赖这些块的 postdom 关系
    for (BasicBlock* BB : reachable) {
        if (nodeToDFSNum.find(BB) == nodeToDFSNum.end()) {
            // 直接分配 DFS 序号，父节点设为虚拟根 (DFS #1)
            unsigned currNum = ++lastNum;
            nodeToDFSNum[BB] = currNum;
            numToNode.push_back(BB);
            Infos.emplace_back();
            Infos[currNum].DFSNum  = currNum;
            Infos[currNum].SdomNum = 1;      // sdom = 虚拟根
            Infos[currNum].Label   = currNum;
            Infos[currNum].Parent  = 1;       // 父 = 虚拟根
            Infos[currNum].Father  = 0;
            // Predecessors 留空 → runSemiNCA 中此节点 sdom 不会被进一步更新
            // IDomNum 将在 runSemiNCA 中被设为 1 (虚拟根)
        }
    }

    // 7. 运行 Semi-NCA
    runSemiNCA();

    // 8. 构建后支配树
    buildPostDominatorTree(PDT);
    PDT.ExitReachable = std::move(exitReachableSet);

    return PDT;
}

// PostDominantAnalysis Pass
PassID PostDominantAnalysis::Key;

PostDominantAnalysis::Result PostDominantAnalysis::run(
    Function &F, FunctionAnalysisPassManager &FAM) const
{
    PostSemiNCAInfo info;
    return info.calculate(&F);
}
