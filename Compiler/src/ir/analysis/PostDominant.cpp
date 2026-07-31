#include "ir/analysis/PostDominant.hpp"
PostDominatorTree::PostDominatorTree() = default;
PostDominatorTree::PostDominatorTree(PostDominatorTree &&other) noexcept = default;
PostDominatorTree &PostDominatorTree::operator=(
    PostDominatorTree &&other) noexcept = default;
bool PostDominatorNode::isVirtualRoot() const {return Block == nullptr;}
bool PostDominatorNode::isLeaf() const {return FirstChild == nullptr;}
//返回指定基本块的直接后支配块，直接后支配者为虚拟根时返回nullptr
BasicBlock* PostDominatorTree::getIPostDom(BasicBlock* BB) const {
    auto it = BlockToNode.find(BB);
    if (it == BlockToNode.end()) return nullptr;
    const PostDominatorNode* node = &Nodes[it->second];
    if (!node->IDom) return nullptr;
    return node->IDom->Block;
}
//返回指定基本块对应的后支配树节点，传入nullptr时返回虚拟根
PostDominatorNode* PostDominatorTree::getNode(BasicBlock* BB) const {
    if (BB == nullptr) {
        return Root ? const_cast<PostDominatorNode*>(Root) : nullptr;
    }
    auto it = BlockToNode.find(BB);
    if (it == BlockToNode.end()) return nullptr;
    return const_cast<PostDominatorNode*>(&Nodes[it->second]);
}
//判断基本块A是否后支配基本块B，其中nullptr表示虚拟根
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
//判断指定基本块是否能够到达函数的某个真实出口
bool PostDominatorTree::isReachableToExit(BasicBlock* BB) const {
    return BB != nullptr && ExitReachable.count(BB) > 0;
}
void PostDominatorTree::clear() {
    Nodes.clear();
    BlockToNode.clear();
    ExitReachable.clear();
    Root = nullptr;
}
bool PostDominatorTree::invalidate(Function &F, const PreservationStatus &PA) {
    if (PA.isKept(PostDominantAnalysis::ID())) {
        return false;
    }
    return true;
}
//构建算法1：创建虚拟根结点
void PostSemiNCAInfo::addVirtualRoot() {
    ++lastNum;
    numToNode.push_back(nullptr);
    Infos.emplace_back();
    Infos[lastNum].DFSNum  = lastNum;
    Infos[lastNum].SdomNum = lastNum;
    Infos[lastNum].Label   = lastNum;
    Infos[lastNum].Parent  = 0;
}
//压缩并查集路径并维护路径上的最优半支配候选
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
//返回指定DFS编号对应的最优半支配候选编号
unsigned PostSemiNCAInfo::eval(unsigned v) {
    if (Infos[v].Father == 0) return v;
    compress(v);
    return Infos[v].Label;
}
//在并查集中连接父节点编号和子节点编号
void PostSemiNCAInfo::link(unsigned parent, unsigned child) {
    Infos[child].Father = parent;
}
//构建算法2：从所有真实出口反向DFS，编DFS序
void PostSemiNCAInfo::runDFS(BasicBlock* startBlock, unsigned parentDFSNum) {
    //1.使用显式栈从当前真实出口开始遍历反向CFG
    std::stack<std::pair<BasicBlock*, unsigned>> st;
    st.push({startBlock, parentDFSNum});
    while (!st.empty()) {
        auto [currBlock, pDFS] = st.top();
        st.pop();
        unsigned currNum = 0;
        auto it = nodeToDFSNum.find(currBlock);
        //2.首次访问基本块时分配DFS编号，并将反向后继压入工作栈
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
        //3.重复访问不创建节点，只补充该反向CFG入边的编号
        if (pDFS != 0) {
            Infos[currNum].Predecessors.push_back(pDFS);
        }
    }
}
//构建算法5：Semi-NCA求直接后支配节点，这一步和支配树算法一样
void PostSemiNCAInfo::runSemiNCA() {
    //1.逆DFS序通过eval和link汇合反向图前驱，得到半支配节点
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
    //2.正DFS序沿已求出的直接后支配链回溯，得到直接后支配节点
    for (unsigned i = 2; i <= lastNum; ++i) {
        unsigned runner = Infos[i].Parent;
        unsigned sdom = Infos[i].SdomNum;
        while (runner > sdom) {
            runner = Infos[runner].IDomNum;
        }
        Infos[i].IDomNum = runner;
    }
}
//根据求解完成的直接后支配编号构建后支配树
void PostSemiNCAInfo::buildPostDominatorTree(PostDominatorTree& PDT) {
    //1.重置分析结果并按照DFS编号创建全部树节点
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
    //2.编号1固定为虚拟根，按照直接后支配关系连接父子节点
    PDT.Root = &PDT.Nodes[1];
    for (unsigned i = 2; i <= lastNum; ++i) {
        unsigned idomIndex = Infos[i].IDomNum;
        PostDominatorNode* currNode = &PDT.Nodes[i];
        PostDominatorNode* idomNode = &PDT.Nodes[idomIndex];
        currNode->IDom = idomNode;
        currNode->NextSibling = idomNode->FirstChild;
        idomNode->FirstChild = currNode;
    }
    //3.在后支配树上生成欧拉区间，支持常数时间后支配查询
    unsigned time = 0;
    std::vector<PostDominatorNode*> stack;
    stack.push_back(PDT.Root);
    while (!stack.empty()) {
        PostDominatorNode* node = stack.back();
        if (node->DFSNumIn == 0) {
            node->DFSNumIn = ++time;
            for (PostDominatorNode* child = node->FirstChild;
                 child; child = child->NextSibling) {
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
//后支配分析主流程
PostDominatorTree PostSemiNCAInfo::calculate(Function* func) {
    //1.初始化后支配树和PostSemiNCA类
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
    //2.获取真实可达出口块，以及反向CFG(边反转存储)
    std::unordered_set<BasicBlock*> reachable;
    func->collectReachableBlocks(reachable);
    if (reachable.empty()) return PDT;
    auto exits = func->findExitBlocks(reachable);
    reverseCFG = func->buildReverseCFG(reachable);
    //3.创建虚拟根
    addVirtualRoot();
    //4.从所有真实出口反向DFS，编DFS序
    for (BasicBlock* exitBB : exits) {
        runDFS(exitBB, 1);
    }
    //5.把反向 DFS 访问到的节点记为出口可达
    for (auto& kv : nodeToDFSNum) {
        if (kv.first != nullptr) {
            exitReachableSet.insert(kv.first);
        }
    }
    //6.将无法到达真实出口的块直接挂到虚拟根，并排除在ExitReachable之外
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
    //7.Semi-NCA求直接后支配节点
    runSemiNCA();
    //8.构建后支配树
    buildPostDominatorTree(PDT);
    PDT.ExitReachable = std::move(exitReachableSet);
    return PDT;
}
PassID PostDominantAnalysis::Key;
PostDominantAnalysis::Result PostDominantAnalysis::run(
    Function &F, FunctionAnalysisPassManager &FAM) const {
    //1.创建一次性反向Semi-NCA求解器
    PostSemiNCAInfo info;
    //2.计算后支配树
    return info.calculate(&F);
}
