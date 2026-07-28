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

BasicBlock* PostDominatorTree::getIPostDom(BasicBlock* BB) const {
    auto it = BlockToNode.find(BB);
    if (it == BlockToNode.end()) return nullptr;
    const PostDominatorNode* node = &Nodes[it->second];
    if (!node->IDom) return nullptr;
    return node->IDom->Block;
}

PostDominatorNode* PostDominatorTree::getNode(BasicBlock* BB) const {
    if (BB == nullptr) {

        return Root ? const_cast<PostDominatorNode*>(Root) : nullptr;
    }
    auto it = BlockToNode.find(BB);
    if (it == BlockToNode.end()) return nullptr;
    return const_cast<PostDominatorNode*>(&Nodes[it->second]);
}

bool PostDominatorTree::postDominates(BasicBlock* A, BasicBlock* B) const {
    if (A == B) return true;
    if (B == nullptr) return false;

    if (A == nullptr) {
        return getNode(B) != nullptr;
    }

    PostDominatorNode* NodeA = getNode(A);
    PostDominatorNode* NodeB = getNode(B);
    if (!NodeA || !NodeB) return false;

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

// 求解辅助：在反向图编号空间中维护半支配最小 Label。
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

// 5. 对虚拟根可达的反向图先求半支配者，再求直接后支配者。
void PostSemiNCAInfo::runSemiNCA() {

    // 5.1 逆 DFS 序通过 eval/link 汇合反向图前驱，得到半支配者。
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

    // 5.2 正 DFS 序沿已求出的 IDom 链回溯，得到直接后支配者。
    for (unsigned i = 2; i <= lastNum; ++i) {
        unsigned runner = Infos[i].Parent;
        unsigned sdom = Infos[i].SdomNum;
        while (runner > sdom) {
            runner = Infos[runner].IDomNum;
        }
        Infos[i].IDomNum = runner;
    }
}

// 收集入口可达且没有后继的真实出口；纯无限循环函数允许出口集合为空。
std::vector<BasicBlock*> PostSemiNCAInfo::findExitBlocks(
    Function* func, const std::unordered_set<BasicBlock*>& reachable)
{
    std::vector<BasicBlock*> exits;
    for (BasicBlock* BB : *func) {
        if (!reachable.count(BB)) continue;
        if (BB->size() == 0) continue;
        auto succs = BB->getSuccessors();
        if (succs.empty()) {
            exits.push_back(BB);
        }
    }
    return exits;
}

// 反向 CFG 仅包含入口可达块，后续 DFS 因而不会混入死控制流。
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

// 虚拟根占用 DFS 编号 1，使多出口函数退化为单根支配问题。
void PostSemiNCAInfo::addVirtualRoot() {
    ++lastNum;
    numToNode.push_back(nullptr);
    Infos.emplace_back();
    Infos[lastNum].DFSNum  = lastNum;
    Infos[lastNum].SdomNum = lastNum;
    Infos[lastNum].Label   = lastNum;
    Infos[lastNum].Parent  = 0;
}

// 从每个出口沿反向边编号；重复访问只补充前驱关系，不重复创建节点。
void PostSemiNCAInfo::runDFS(BasicBlock* startBlock, unsigned parentDFSNum) {
    std::stack<std::pair<BasicBlock*, unsigned>> st;
    st.push({startBlock, parentDFSNum});

    while (!st.empty()) {
        auto [currBlock, pDFS] = st.top();
        st.pop();

        unsigned currNum = 0;
        auto it = nodeToDFSNum.find(currBlock);

        if (it == nodeToDFSNum.end()) {

            currNum = ++lastNum;
            nodeToDFSNum[currBlock] = currNum;
            numToNode.push_back(currBlock);

            Infos.emplace_back();
            Infos[currNum].DFSNum  = currNum;
            Infos[currNum].SdomNum = currNum;
            Infos[currNum].Label   = currNum;
            Infos[currNum].Parent  = pDFS;

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

        if (pDFS != 0) {
            Infos[currNum].Predecessors.push_back(pDFS);
        }
    }
}

// 6. 将 IDom 编号封装为后支配树，并生成常数时间查询所需的欧拉区间。
void PostSemiNCAInfo::buildPostDominatorTree(PostDominatorTree& PDT) {
    PDT.clear();
    if (lastNum == 0) return;

    PDT.Nodes.resize(lastNum + 1);

    for (unsigned i = 1; i <= lastNum; ++i) {
        BasicBlock* BB = numToNode[i];
        PDT.Nodes[i].Block = BB;
        if (BB) {
            PDT.BlockToNode[BB] = i;
        }
    }

    PDT.Root = &PDT.Nodes[1];

    for (unsigned i = 2; i <= lastNum; ++i) {
        unsigned idomIndex = Infos[i].IDomNum;
        PostDominatorNode* currNode = &PDT.Nodes[i];
        PostDominatorNode* idomNode = &PDT.Nodes[idomIndex];

        currNode->IDom = idomNode;
        currNode->NextSibling = idomNode->FirstChild;
        idomNode->FirstChild = currNode;
    }

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

// 后支配分析主流程：建立反向图和虚拟根，求解可达出口区域，再保守挂接无限区域。
PostDominatorTree PostSemiNCAInfo::calculate(Function* func) {
    // 1. 清空一次性状态并建立 0 号编号哨兵；空函数直接返回。
    PostDominatorTree PDT;

    lastNum = 0;
    Infos.clear();
    numToNode.clear();
    nodeToDFSNum.clear();
    reverseCFG.clear();
    exitReachableSet.clear();

    Infos.emplace_back();
    numToNode.push_back(nullptr);

    if (func->size() == 0) return PDT;

    // 2. 先限定入口可达块，再识别真实出口并预计算反向邻接表。
    std::unordered_set<BasicBlock*> reachable;
    func->collectReachableBlocks(reachable);
    if (reachable.empty()) return PDT;

    auto exits = findExitBlocks(func, reachable);

    precomputeReverseCFG(func, reachable);

    // 3. 创建虚拟根，从所有出口反向 DFS，得到能够到达出口的区域。
    addVirtualRoot();

    for (BasicBlock* exitBB : exits) {
        runDFS(exitBB, 1);
    }

    for (auto& kv : nodeToDFSNum) {
        if (kv.first != nullptr) {
            exitReachableSet.insert(kv.first);
        }
    }

    // 4. 无法到达出口的块直接挂到虚拟根；消费者必须结合 ExitReachable 查询。
    for (BasicBlock* BB : reachable) {
        if (nodeToDFSNum.find(BB) == nodeToDFSNum.end()) {

            unsigned currNum = ++lastNum;
            nodeToDFSNum[BB] = currNum;
            numToNode.push_back(BB);
            Infos.emplace_back();
            Infos[currNum].DFSNum  = currNum;
            Infos[currNum].SdomNum = 1;
            Infos[currNum].Label   = currNum;
            Infos[currNum].Parent  = 1;
            Infos[currNum].Father  = 0;

        }
    }

    // 5. 在完整编号图上求解直接后支配关系。
    runSemiNCA();

    // 6. 封装树结构并发布可达出口集合。
    buildPostDominatorTree(PDT);
    PDT.ExitReachable = std::move(exitReachableSet);

    return PDT;
}

PassID PostDominantAnalysis::Key;

PostDominantAnalysis::Result PostDominantAnalysis::run(
    Function &F, FunctionAnalysisPassManager &FAM) const
{
    // 1. 后支配树不改写 CFG，创建一次性反向 Semi-NCA 求解器。
    PostSemiNCAInfo info;
    // 2. 在虚拟出口视图上完成求解并返回树与 ExitReachable 集合。
    return info.calculate(&F);
}
