#include "ir/analysis/Dominant.hpp"

// DominatorTree Implementation

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

DominatorNode* DominatorTree::getNode(BasicBlock* BB) const {
    auto it = BlockToNode.find(BB);
    if (it == BlockToNode.end()) return nullptr;
    return const_cast<DominatorNode*>(&Nodes[it->second]);
}

bool DominatorTree::dominates(BasicBlock* A, BasicBlock* B) const {
    if (A == B) return true;
    DominatorNode* NodeA = getNode(A);
    DominatorNode* NodeB = getNode(B);

    if (!NodeA || !NodeB) return false; // 不可达节点处理

    // 核心优化：利用 DFS 时间戳进行 O(1) 判断
    return NodeA->DFSNumIn <= NodeB->DFSNumIn &&
           NodeA->DFSNumOut >= NodeB->DFSNumOut;
}

bool DominatorTree::dominatesValue(Value* DefVal, Value* UseVal) const {
    if (!DefVal || !UseVal) return false;

    // 情况 1：如果 DefVal 是常量，认为它支配一切
    if (DefVal->isConst()) return true;

    // 情况 2：如果 DefVal 是形参，认为它支配一切
    Variable* DefVar = DefVal->as<Variable>();
    if (DefVar && DefVar->isParam()) return true;

    // 情况 3：将两个 Value 转换为 Instruction
    Instruction* DefInst = DefVal->as<Instruction>();
    Instruction* UseInst = UseVal->as<Instruction>();

    // 如果 DefVal 不是 Instruction，返回 false
    if (!DefInst) return false;

    // UseVal 可能不是 Instruction（比如是形参），如果是则无法支配关系
    if (!UseInst) return false;

    // 获取两条指令所在的基本块
    BasicBlock* DefBB = DefInst->getParent();
    BasicBlock* UseBB = UseInst->getParent();

    // 情况 4：不同基本块，使用基本块级的支配关系
    if (DefBB != UseBB) {
        return dominates(DefBB, UseBB);
    }

    // 情况 5：同一基本块，比较指令在链表中的顺序
    // 遍历 DefBB 中的指令，看 DefInst 是否在 UseInst 前面
    for (auto inst = DefBB->begin(); inst != DefBB->end(); ++inst) {
        Instruction* currInst = *inst;
        if (currInst == DefInst) {
            // 找到 DefInst，说明它在 UseInst 前面
            return true;
        }
        if (currInst == UseInst) {
            // 先找到 UseInst，说明 DefInst 在它后面
            return false;
        }
    }

    // 不应该到这里，但以防万一
    return false;
}

bool DominatorTree::invalidate(Function &F, const PreservationStatus &PA) {
        if (PA.isKept(DominantAnalysis::ID())) {
            return false;
        }
        return true;
}



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

void SemiNCAInfo::runDFS(BasicBlock* entryBlock) {
    //迭代法前序遍历
    //{BasicBlock*,parentDFSNum}
    std::stack<std::pair<BasicBlock*, unsigned>> st;
    st.push({entryBlock, 0});

    while (!st.empty()) {
        auto [currBlock, parentDFSNum] = st.top();
        st.pop();

        unsigned currNum = 0;
        auto it = nodeToDFSNum.find(currBlock);

        if (it == nodeToDFSNum.end()) {
            // 第一次访问：分配 DFS 序号
            currNum = ++lastNum;
            nodeToDFSNum[currBlock] = currNum;
            numToNode.push_back(currBlock);

            // 扩展 Infos 数组
            Infos.emplace_back();
            Infos[currNum].DFSNum = currNum;
            Infos[currNum].SdomNum = currNum;
            Infos[currNum].Label = currNum;
            Infos[currNum].Parent = parentDFSNum;

            // 将后继加入栈
            auto succs = currBlock->getSuccessors();
            for (auto sit = succs.rbegin(); sit != succs.rend(); ++sit) {
                if (*sit) st.push({*sit, currNum});
            }
        } else {
            currNum = it->second;
        }

        // 处理前驱关系 (CFG edges)
        if (parentDFSNum != 0) {
            Infos[currNum].Predecessors.push_back(parentDFSNum);
        }
    }
}

void SemiNCAInfo::runSemiNCA() {
    // 1. 计算半支配节点
    // 逆DFS序遍历
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

    // 2. 计算支配结点 -- SemiNCA
    // 正DFS序遍历 -- 除了根节点
    for (unsigned i = 2; i <= lastNum; ++i) {
        unsigned runner = Infos[i].Parent;
        unsigned sdom = Infos[i].SdomNum;
        while (runner > sdom) {
            runner = Infos[runner].IDomNum;
        }
        Infos[i].IDomNum = runner;
    }
}

void SemiNCAInfo::buildDominatorTree(DominatorTree& DT) {
    DT.clear();
    if (lastNum == 0) return;

    // 1. 调整 DT 内部数组大小
    DT.Nodes.resize(lastNum + 1);

    // 2. 填充基本信息 & 建立映射
    for (unsigned i = 1; i <= lastNum; ++i) {
        BasicBlock* BB = numToNode[i];
        DT.Nodes[i].Block = BB;
        DT.BlockToNode[BB] = i;
    }

    // 设置根节点
    DT.Root = &DT.Nodes[1];

    // 3. 构建树结构 (IDom 指针 和 Child 链表)
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

DominatorTree SemiNCAInfo::calculate(Function* func) {
    DominatorTree DT;

    //0.初始化
    lastNum = 0;
    Infos.clear();
    numToNode.clear();
    nodeToDFSNum.clear();
    // 0号占位 (DFS序从1开始)
    Infos.emplace_back();
    numToNode.push_back(nullptr);

    if (func->Size() == 0) return DT;
    BasicBlock* entryBlock = func->front();
    //1.DFS遍历，初始化数据+计算DFS序
    runDFS(entryBlock);
    //2.计算sdom->计算idom
    runSemiNCA();
    //3.构建支配树
    buildDominatorTree(DT);

    return DT;
}


PassID DominantAnalysis::Key;

DominantAnalysis::Result DominantAnalysis::run(Function &F, FunctionAnalysisPassManager &FAM) const {
    SemiNCAInfo info;
    return info.calculate(&F);
}
