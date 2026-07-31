#include "ir/analysis/Dominant.hpp"
#include <queue>
bool DominatorNode::isLeaf() const {return FirstChild == nullptr;}
DominatorTree::DominatorTree() = default;
DominatorTree::DominatorTree(DominatorTree &&other) noexcept = default;
DominatorTree &DominatorTree::operator=(DominatorTree &&other) noexcept = default;
void DominatorTree::clear() {
    Nodes.clear();
    BlockToNode.clear();
    Root = nullptr;
}
//返回给定基本块的直接支配者基本块
BasicBlock* DominatorTree::getIDom(BasicBlock* BB) const {
    auto it = BlockToNode.find(BB);
    if (it == BlockToNode.end()) return nullptr;
    const DominatorNode* node = &Nodes[it->second];
    return node->IDom ? node->IDom->Block : nullptr;
}
//返回给定基本块对应的支配树节点
DominatorNode* DominatorTree::getNode(BasicBlock* BB) const {
    auto it = BlockToNode.find(BB);
    if (it == BlockToNode.end()) return nullptr;
    return const_cast<DominatorNode*>(&Nodes[it->second]);
}
//判断基本块A是否支配基本块B
bool DominatorTree::dominates(BasicBlock* A, BasicBlock* B) const {
    if (A == B) return true;
    DominatorNode* NodeA = getNode(A);
    DominatorNode* NodeB = getNode(B);
    if (!NodeA || !NodeB) return false;
    return NodeA->DFSNumIn <= NodeB->DFSNumIn &&
           NodeA->DFSNumOut >= NodeB->DFSNumOut;
}
//按照后序遍历返回所有支配树节点
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
//返回给定支配树节点的所有直接子节点
std::vector<DominatorNode *> DominatorTree::getChildNodes(
    DominatorNode *Node) const {
    std::vector<DominatorNode *> Children;
    if (!Node) return Children;
    for (DominatorNode *Child = Node->FirstChild; Child;
         Child = Child->NextSibling)
        Children.push_back(Child);
    return Children;
}
//按照前序遍历返回所有支配树节点对应的基本块
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
//返回给定基本块在支配树中的所有直接子基本块
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
//DefVal定义的位置是否支配UseVal使用的位置
bool DominatorTree::dominatesValue(Value* DefVal, Value* UseVal) const {
/*
dominatesValue(DefVal, UseVal)
│
├─ DefVal 是常量
│    └─ true
│
├─ DefVal 是函数参数
│    └─ true
│
├─ 定义和使用位于不同基本块
│    └─ 查询 DefBB 是否支配 UseBB
│
└─ 定义和使用位于同一基本块
     └─ 从块首向后扫描
          ├─ 先遇到 DefInst -> true
          ├─ 先遇到 UseInst -> false
          └─ 都找不到          -> false
*/
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
//压缩并查集路径并维护路径上半支配编号最小的Label
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
//返回节点所在并查集路径上半支配编号最小的候选节点
unsigned SemiNCAInfo::eval(unsigned v) {
    if (Infos[v].Father == 0) return v;
    compress(v);
    return Infos[v].Label;
}
//将子节点连接到父节点所在的并查集中
void SemiNCAInfo::link(unsigned parent, unsigned child) {
    Infos[child].Father = parent;
}
//计算定义块集合的迭代支配边界，提供LiveInBlocks时执行裁剪
std::set<BasicBlock *> DominatorTree::getIteratedDominanceFrontier(
    const std::set<BasicBlock *> &DefBlocks,
    const std::set<BasicBlock *> *LiveInBlocks) const {
    //1.按照支配树深度从内向外处理定义根，不可达定义块不参与可达CFG的PHI放置
    std::priority_queue<std::pair<unsigned, DominatorNode *>> WorkQueue;
    std::set<DominatorNode *> QueuedRoots;
    std::set<DominatorNode *> PlacedPhis;
    std::set<BasicBlock *> PhiBlocks;
    for (BasicBlock *BB : DefBlocks) {
        DominatorNode *Node = getNode(BB);
        if (!Node || !QueuedRoots.insert(Node).second) continue;
        WorkQueue.push({Node->Level, Node});
    }
    //2.遍历每个定义根的支配子树，收集离开子树且层级不更深的CFG后继
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
                if (!SuccNode || SuccNode->Level > RootLevel) continue;
                if (LiveInBlocks && !LiveInBlocks->count(Succ)) continue;
                if (!PlacedPhis.insert(SuccNode).second) continue;
                PhiBlocks.insert(Succ);
                //2.1新PHI块不是原始定义块时，将其作为新定义继续求IDF闭包
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
//构建第1步：从函数入口遍历可达CFG并记录DFS编号、父节点和前驱
void SemiNCAInfo::runDFS(BasicBlock* entryBlock) {
    //1.使用显式栈从入口开始遍历可达CFG
    std::stack<std::pair<BasicBlock*, unsigned>> st;
    st.push({entryBlock, 0});
    while (!st.empty()) {
        auto [currBlock, parentDFSNum] = st.top();
        st.pop();
        unsigned currNum = 0;
        auto it = nodeToDFSNum.find(currBlock);
        //2.首次到达基本块时分配编号并按照原后继顺序继续DFS
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
        //3.每次沿CFG边到达节点时记录来源节点编号
        if (parentDFSNum != 0) {
            Infos[currNum].Predecessors.push_back(parentDFSNum);
        }
    }
}
//构建第2步：根据编号化CFG计算每个节点的半支配者和直接支配者
void SemiNCAInfo::runSemiNCA() {
    //1.逆DFS序通过eval和link合并前驱信息，得到每个节点的半支配者
    for (unsigned i = lastNum; i >= 2; --i) {
        //遍历当前节点所有CFG前驱
/*
pre < i:前驱比当前节点更早被DFS访问,A(2) -> B(5),满足A是B的半支配者
pre > i：前驱比当前节点更晚被DFS访问,
        A(2) -> ... -> X(7) -> B(3)
        由于逆DFS序，所以X先于B被处理，但是这里不能简单将X作为最佳SDom
        所系调用eval(pre)沿着并查集路径向上找到SDom最小的节点
*/
        for (unsigned pre : Infos[i].Predecessors) {
            if (pre == 0) continue;
            unsigned u = eval(pre);
            if (Infos[u].SdomNum < Infos[i].SdomNum) {
                Infos[i].SdomNum = Infos[u].SdomNum;
            }
        }
        //处理完节点i之后将其连接到父节点所在的并查集中
        //表示：i的半支配者已经确定
        link(Infos[i].Parent, i);
    }
    //2.正DFS序沿已经完成的IDom链回溯到半支配边界
    for (unsigned i = 2; i <= lastNum; ++i) {
        //从i的DFS父结点开始
        unsigned runner = Infos[i].Parent;
        //i的半支配者编号
        unsigned sdom = Infos[i].SdomNum;
        //只要候选还在半支配边界之下，就沿着IDom向上跳
        while (runner > sdom) {
            runner = Infos[runner].IDomNum;
        }
        //直到找到第一个<= sdom的节点，作为i的直接支配者
        Infos[i].IDomNum = runner;
    }
}
//构建第3步：使用IDom建树
void SemiNCAInfo::buildDominatorTree(DominatorTree& DT) {
    //1.初始化
    DT.clear();
    if (lastNum == 0) return;
    DT.Nodes.resize(lastNum + 1);
    for (unsigned i = 1; i <= lastNum; ++i) {
        BasicBlock* BB = numToNode[i];
        DT.Nodes[i].Block = BB;
        DT.BlockToNode[BB] = i;
    }
    DT.Root = &DT.Nodes[1];
    //2.根据直接支配者编号建立父子链
    for (unsigned i = 2; i <= lastNum; ++i) {
        unsigned idomIndex = Infos[i].IDomNum;
        DominatorNode* currNode = &DT.Nodes[i];
        DominatorNode* idomNode = &DT.Nodes[idomIndex];
        currNode->IDom = idomNode;
        currNode->NextSibling = idomNode->FirstChild;
        idomNode->FirstChild = currNode;
    }
    //3.遍历支配树并计算层级和欧拉区间，支持常数时间支配查询
    unsigned time = 0;
    std::vector<DominatorNode*> stack;
    stack.push_back(DT.Root);
    while (!stack.empty()) {
        DominatorNode* node = stack.back();
        if (node->DFSNumIn == 0) {
            node->DFSNumIn = ++time;
            for (DominatorNode* child = node->FirstChild; child;
                 child = child->NextSibling) {
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
//计算给定函数的直接支配树
DominatorTree SemiNCAInfo::calculate(Function* func) {
    //0.初始化支配树和SemiNCAInfo类
    DominatorTree DT;
    lastNum = 0;
    Infos.clear();
    numToNode.clear();
    nodeToDFSNum.clear();
    Infos.emplace_back();
    numToNode.push_back(nullptr);
    if (func->size() == 0) return DT;
    //1.从入口开始编号所有可达基本块
    BasicBlock* entryBlock = func->front();
    runDFS(entryBlock);
    //2.基于编号化CFG求出半支配者和直接支配者
    runSemiNCA();
    //3.建立支配树父子关系和查询时间戳
    buildDominatorTree(DT);
    return DT;
}
PassID DominantAnalysis::Key;
DominantAnalysis::Result DominantAnalysis::run(
    Function &F, FunctionAnalysisPassManager &FAM) const {
    //1.创建支配树Build类
    SemiNCAInfo info;
    //2.调用Semi-NCA算法构建支配树
    return info.calculate(&F);
}
