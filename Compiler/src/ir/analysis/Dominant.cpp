#include "ir/analysis/Dominant.hpp"

#include <queue>

bool DominatorNode::isLeaf() const {
    return FirstChild == nullptr;
}

DominatorTree::DominatorTree() = default;

DominatorTree::DominatorTree(DominatorTree &&other) noexcept = default;

DominatorTree &DominatorTree::operator=(DominatorTree &&other) noexcept = default;

void DominatorTree::clear() {
    Nodes.clear();
    BlockToNode.clear();
    Root = nullptr;
}

BasicBlock* DominatorTree::getIDom(BasicBlock* BB) const {
    auto it = BlockToNode.find(BB);
    if (it == BlockToNode.end()) return nullptr;
    const DominatorNode* node = &Nodes[it->second];
    return node->IDom ? node->IDom->Block : nullptr;
}

// 查询辅助：以显式栈稳定地产生支配树后序，供循环发现等消费者使用。
std::vector<DominatorNode *> DominatorTree::getNodesInPostOrder() const {
    std::vector<DominatorNode *> Order;
    if (!Root) return Order;

    std::vector<std::pair<DominatorNode *, bool>> Stack;
    Stack.push_back({Root, false});
    while (!Stack.empty()) {
        auto [Node, Expanded] = Stack.back();
        Stack.pop_back();
        if (!Node) continue;
        if (Expanded) {
            Order.push_back(Node);
            continue;
        }
        Stack.push_back({Node, true});
        std::vector<DominatorNode *> Children;
        for (DominatorNode *Child = Node->FirstChild; Child;
             Child = Child->NextSibling)
            Children.push_back(Child);
        for (auto It = Children.rbegin(); It != Children.rend(); ++It)
            Stack.push_back({*It, false});
    }
    return Order;
}

std::vector<DominatorNode *> DominatorTree::getChildNodes(
    DominatorNode *Node) const {
    std::vector<DominatorNode *> Children;
    if (!Node) return Children;
    for (DominatorNode *Child = Node->FirstChild; Child;
         Child = Child->NextSibling)
        Children.push_back(Child);
    return Children;
}

std::vector<BasicBlock *> DominatorTree::getBlocksInPreOrder() const {
    std::vector<BasicBlock *> Order;
    if (!Root) return Order;
    std::vector<DominatorNode *> Stack{Root};
    while (!Stack.empty()) {
        DominatorNode *Node = Stack.back();
        Stack.pop_back();
        if (!Node) continue;
        if (Node->Block) Order.push_back(Node->Block);
        std::vector<DominatorNode *> Children;
        for (DominatorNode *Child = Node->FirstChild; Child;
             Child = Child->NextSibling)
            Children.push_back(Child);
        for (auto It = Children.rbegin(); It != Children.rend(); ++It)
            Stack.push_back(*It);
    }
    return Order;
}

std::vector<BasicBlock *> DominatorTree::getChildren(BasicBlock *BB) const {
    std::vector<BasicBlock *> Children;
    DominatorNode *Node = getNode(BB);
    if (!Node) return Children;
    for (DominatorNode *Child = Node->FirstChild; Child;
         Child = Child->NextSibling) {
        if (Child->Block) Children.push_back(Child->Block);
    }
    return Children;
}

std::set<BasicBlock *> DominatorTree::getIteratedDominanceFrontier(
    const std::set<BasicBlock *> &DefBlocks,
    const std::set<BasicBlock *> *LiveInBlocks) const {
    // 1. 按支配树深度从内向外处理定义根；不可达定义块没有支配树节点，
    // 不参与可达 CFG 的 PHI 放置。
    std::priority_queue<std::pair<unsigned, DominatorNode *>> WorkQueue;
    std::set<DominatorNode *> QueuedRoots;
    std::set<DominatorNode *> PlacedPhis;
    std::set<BasicBlock *> PhiBlocks;

    for (BasicBlock *BB : DefBlocks) {
        DominatorNode *Node = getNode(BB);
        if (!Node || !QueuedRoots.insert(Node).second) continue;
        WorkQueue.push({Node->Level, Node});
    }

    // 2. 对每个定义根遍历其支配子树。离开该子树且支配层级不更深的 CFG
    // 后继就是当前根的支配边界；新 PHI 也视作定义，继续求 IDF 闭包。
    while (!WorkQueue.empty()) {
        unsigned RootLevel = WorkQueue.top().first;
        DominatorNode *RootNode = WorkQueue.top().second;
        WorkQueue.pop();

        std::vector<DominatorNode *> NodeWorklist{RootNode};
        std::set<DominatorNode *> LocalVisited{RootNode};
        while (!NodeWorklist.empty()) {
            DominatorNode *Node = NodeWorklist.back();
            NodeWorklist.pop_back();

            for (BasicBlock *Succ : Node->Block->getSuccessors()) {
                DominatorNode *SuccNode = getNode(Succ);
                if (!SuccNode || SuccNode->Level > RootLevel)
                    continue;
                if (LiveInBlocks && !LiveInBlocks->count(Succ))
                    continue;

                if (!PlacedPhis.insert(SuccNode).second)
                    continue;
                PhiBlocks.insert(Succ);

                if (!DefBlocks.count(Succ) &&
                    QueuedRoots.insert(SuccNode).second)
                    WorkQueue.push({SuccNode->Level, SuccNode});
            }

            for (DominatorNode *Child = Node->FirstChild; Child;
                 Child = Child->NextSibling) {
                if (LocalVisited.insert(Child).second)
                    NodeWorklist.push_back(Child);
            }
        }
    }

    return PhiBlocks;
}

DominatorNode* DominatorTree::getNode(BasicBlock* BB) const {
    auto it = BlockToNode.find(BB);
    if (it == BlockToNode.end()) return nullptr;
    return const_cast<DominatorNode*>(&Nodes[it->second]);
}

bool DominatorTree::dominates(BasicBlock* A, BasicBlock* B) const {
    if (A == B) return true;
    DominatorNode* NodeA = getNode(A);
    DominatorNode* NodeB = getNode(B);

    if (!NodeA || !NodeB) return false;

    return NodeA->DFSNumIn <= NodeB->DFSNumIn &&
           NodeA->DFSNumOut >= NodeB->DFSNumOut;
}

// 值级查询先处理常量和形参，再按跨块支配或同块指令次序完成判定。
bool DominatorTree::dominatesValue(Value* DefVal, Value* UseVal) const {
    if (!DefVal || !UseVal) return false;

    if (DefVal->isConst()) return true;

    if (DefVal->isParam()) return true;

    Instruction* DefInst = DefVal->as<Instruction>();
    Instruction* UseInst = UseVal->as<Instruction>();

    if (!DefInst) return false;

    if (!UseInst) return false;

    BasicBlock* DefBB = DefInst->getParent();
    BasicBlock* UseBB = UseInst->getParent();

    if (DefBB != UseBB) {
        return dominates(DefBB, UseBB);
    }

    for (auto inst = DefBB->begin(); inst != DefBB->end(); ++inst) {
        Instruction* currInst = *inst;
        if (currInst == DefInst) {

            return true;
        }
        if (currInst == UseInst) {

            return false;
        }
    }

    return false;
}

bool DominatorTree::invalidate(Function &F, const PreservationStatus &PA) {
        if (PA.isKept(DominantAnalysis::ID())) {
            return false;
        }
        return true;
}

// 求解辅助：路径压缩始终保留并查集路径上半支配编号最小的 Label。
void SemiNCAInfo::compress(unsigned v) {
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

unsigned SemiNCAInfo::eval(unsigned v) {
    if (Infos[v].Father == 0) return v;
    compress(v);
    return Infos[v].Label;
}

void SemiNCAInfo::link(unsigned parent, unsigned child) {
    Infos[child].Father = parent;
}

// 1. 从入口迭代遍历可达 CFG，为块分配一基 DFS 编号并记录编号化前驱边。
void SemiNCAInfo::runDFS(BasicBlock* entryBlock) {

    std::stack<std::pair<BasicBlock*, unsigned>> st;
    st.push({entryBlock, 0});

    while (!st.empty()) {
        auto [currBlock, parentDFSNum] = st.top();
        st.pop();

        unsigned currNum = 0;
        auto it = nodeToDFSNum.find(currBlock);

        if (it == nodeToDFSNum.end()) {

            currNum = ++lastNum;
            nodeToDFSNum[currBlock] = currNum;
            numToNode.push_back(currBlock);

            Infos.emplace_back();
            Infos[currNum].DFSNum = currNum;
            Infos[currNum].SdomNum = currNum;
            Infos[currNum].Label = currNum;
            Infos[currNum].Parent = parentDFSNum;

            auto succs = currBlock->getSuccessors();
            for (auto sit = succs.rbegin(); sit != succs.rend(); ++sit) {
                if (*sit) st.push({*sit, currNum});
            }
        } else {
            currNum = it->second;
        }

        if (parentDFSNum != 0) {
            Infos[currNum].Predecessors.push_back(parentDFSNum);
        }
    }
}

// 2. 在编号图上运行 Semi-NCA：先逆序求半支配者，再正序收敛直接支配者。
void SemiNCAInfo::runSemiNCA() {
    // 2.1 逆 DFS 序通过 eval/link 合并前驱信息，得到每个节点的半支配者。
    for (unsigned i = lastNum; i >= 2; --i) {
        for (unsigned pre : Infos[i].Predecessors) {
            if(pre == 0) continue;
            unsigned u = eval(pre);
            if (Infos[u].SdomNum < Infos[i].SdomNum) {
                Infos[i].SdomNum = Infos[u].SdomNum;
            }
        }
        link(Infos[i].Parent, i);
    }

    // 2.2 正 DFS 序沿已完成的 IDom 链回溯到半支配边界。
    for (unsigned i = 2; i <= lastNum; ++i) {
        unsigned runner = Infos[i].Parent;
        unsigned sdom = Infos[i].SdomNum;
        while (runner > sdom) {
            runner = Infos[runner].IDomNum;
        }
        Infos[i].IDomNum = runner;
    }
}

// 3. 将编号结果封装为父子树，并计算欧拉区间以支持 O(1) 支配判定。
void SemiNCAInfo::buildDominatorTree(DominatorTree& DT) {
    DT.clear();
    if (lastNum == 0) return;

    DT.Nodes.resize(lastNum + 1);

    for (unsigned i = 1; i <= lastNum; ++i) {
        BasicBlock* BB = numToNode[i];
        DT.Nodes[i].Block = BB;
        DT.BlockToNode[BB] = i;
    }

    DT.Root = &DT.Nodes[1];

    for (unsigned i = 2; i <= lastNum; ++i) {
        unsigned idomIndex = Infos[i].IDomNum;
        DominatorNode* currNode = &DT.Nodes[i];
        DominatorNode* idomNode = &DT.Nodes[idomIndex];

        currNode->IDom = idomNode;
        currNode->NextSibling = idomNode->FirstChild;
        idomNode->FirstChild = currNode;
    }

    unsigned time = 0;
    std::vector<DominatorNode*> stack;
    stack.push_back(DT.Root);

    while (!stack.empty()) {
        DominatorNode* node = stack.back();
        if (node->DFSNumIn == 0) {
            node->DFSNumIn = ++time;
            for (DominatorNode* child = node->FirstChild; child; child = child->NextSibling) {
                child->Level = node->Level + 1;
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

// 支配分析主流程：清空一次性状态，完成 CFG 编号、Semi-NCA 求解和结果封装。
DominatorTree SemiNCAInfo::calculate(Function* func) {
    // 1. 重置编号表并保留 0 号哨兵；空函数直接返回空树。
    DominatorTree DT;

    lastNum = 0;
    Infos.clear();
    numToNode.clear();
    nodeToDFSNum.clear();

    Infos.emplace_back();
    numToNode.push_back(nullptr);

    if (func->size() == 0) return DT;
    BasicBlock* entryBlock = func->front();

    // 2. 只从入口编号可达 CFG，不可达块不会进入支配树。
    runDFS(entryBlock);

    // 3. 基于编号化前驱求出半支配者和直接支配者。
    runSemiNCA();

    // 4. 建立树链接和查询时间戳，作为最终分析结果返回。
    buildDominatorTree(DT);

    return DT;
}

PassID DominantAnalysis::Key;

DominantAnalysis::Result DominantAnalysis::run(Function &F, FunctionAnalysisPassManager &FAM) const {
    // 1. 支配树没有其他分析依赖，创建一次性 Semi-NCA 求解器。
    SemiNCAInfo info;
    // 2. 从函数入口完成可达 CFG 求解并直接返回封装后的树。
    return info.calculate(&F);
}
