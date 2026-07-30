#include "ir/analysis/LoopInfo.hpp"
#include <stack>
#include <queue>
#include <algorithm>
//一些基本接口
Loop::Loop() = default;
BasicBlock *Loop::getHeader() const {return Header;}
Loop *Loop::getParentLoop() const {return ParentLoop;}
const std::vector<Loop *> &Loop::getSubLoops() const {return SubLoops;}
const std::vector<BasicBlock *> &Loop::getBlocks() const {return Blocks;}
const std::vector<BasicBlock *> &Loop::getBackedgeSrcs() const {return BackedgeSrcs;}
LoopInfo::LoopInfo() = default;
LoopInfo::LoopInfo(LoopInfo &&) = default;
LoopInfo &LoopInfo::operator=(LoopInfo &&) = default;
void LoopInfo::clear() {
    Loops.clear();
    TopLevelLoops.clear();
    BBToLoop.clear();
}
//从当前循环开始，沿着父循环向上计算深度（表层循环深度为1）
unsigned Loop::getDepth() const {
    unsigned Depth = 1;
    for (Loop *Parent = ParentLoop; Parent; Parent = Parent->ParentLoop)
        ++Depth;
    return Depth;
}
//判断当前循环是否是最内层循环
bool Loop::isInnermost() const {return SubLoops.empty();}
//检查目标基本块是否存在于当前或者内层循环中
bool Loop::contains(BasicBlock* BB) const {
    for (BasicBlock* b : Blocks)
        if (b == BB) return true;
    for (Loop* sub : SubLoops)
        if (sub->contains(BB)) return true;
    return false;
}
//返回当前循环及其所有子循环所有基本块
std::vector<BasicBlock*> Loop::getBlocksIncludingSubLoops() const {
    std::vector<BasicBlock*> Result;
    std::unordered_set<BasicBlock*> Seen;
    std::vector<const Loop*> Stack{this};
    while (!Stack.empty()) {
        const Loop *Current = Stack.back();
        Stack.pop_back();
        if (!Current) continue;
        for (BasicBlock *BB : Current->getBlocks()) {
            if (BB && Seen.insert(BB).second) Result.push_back(BB);
        }
        for (Loop *Sub : Current->getSubLoops()) {
            if (Sub) Stack.push_back(Sub);
        }
    }
    return Result;
}
//获取该循环所有出口块：先获得当前循环及所有子循环基本块
//然后遍历这些块的后继，若后继不在当前循环及子循环中，则是出口块
std::vector<BasicBlock*> Loop::getExitBlocks() const {
    std::vector<BasicBlock*> exitBlocks;
    std::unordered_set<BasicBlock*> allInternalBlocks;
    std::unordered_set<BasicBlock*> seenExits;

    std::vector<const Loop*> loopStack = { this };
    while (!loopStack.empty()) {
        const Loop* curr = loopStack.back();
        loopStack.pop_back();

        for (BasicBlock* bb : curr->getBlocks()) {
            if (bb) allInternalBlocks.insert(bb);
        }

        for (Loop* sub : curr->getSubLoops()) {
            if (sub) loopStack.push_back(sub);
        }
    }

    for (BasicBlock* bb : allInternalBlocks) {
        for (BasicBlock* succ : bb->getSuccessors()) {
            if (succ && allInternalBlocks.find(succ) == allInternalBlocks.end()) {

                if (seenExits.insert(succ).second) {
                    exitBlocks.push_back(succ);
                }
            }
        }
    }

    return exitBlocks;
}
//按照所属函数中的基本块顺序返回当前循环所有基本块
std::vector<BasicBlock*> Loop::getBlocksInFunctionOrder() const {
    std::vector<BasicBlock*> Result;
    Function *F = Header ? Header->getParent() : nullptr;
    if (!F) return Result;
    for (BasicBlock *BB : *F) {
        if (BB && contains(BB)) Result.push_back(BB);
    }
    return Result;
}
//RPO：对CFG后序遍历，然后反转结果得到RPO，只获得当前循环层的基本块
std::vector<BasicBlock*> Loop::getBlocksInRPO() const {
    std::vector<BasicBlock*> PostOrder;
    std::unordered_set<BasicBlock*> InLoop;
    std::unordered_set<BasicBlock*> Visited;
    for (BasicBlock *BB : Blocks) {
        if (BB) InLoop.insert(BB);
    }

    auto AppendPostOrder = [&](BasicBlock *Start) {
        if (!Start || !InLoop.count(Start) || !Visited.insert(Start).second)
            return;
        std::vector<std::pair<BasicBlock *, bool>> Stack{{Start, false}};
        while (!Stack.empty()) {
            auto [BB, Expanded] = Stack.back();
            Stack.pop_back();
            if (Expanded) {
                PostOrder.push_back(BB);
                continue;
            }
            Stack.push_back({BB, true});
            auto Succs = BB->getSuccessors();
            for (auto It = Succs.rbegin(); It != Succs.rend(); ++It) {
                BasicBlock *Succ = *It;
                if (Succ && InLoop.count(Succ) && Visited.insert(Succ).second)
                    Stack.push_back({Succ, false});
            }
        }
    };

    AppendPostOrder(Header);
    for (BasicBlock *BB : Blocks) AppendPostOrder(BB);
    std::reverse(PostOrder.begin(), PostOrder.end());
    return PostOrder;
}
//后序遍历返回当前循环及其所有子循环
std::vector<Loop*> Loop::getLoopsInPostOrder(
    bool ReverseSiblingOrder) const {
    std::vector<Loop*> Result;
    std::vector<std::pair<Loop *, bool>> Stack{
        {const_cast<Loop *>(this), false}};
    while (!Stack.empty()) {
        auto [Current, Expanded] = Stack.back();
        Stack.pop_back();
        if (!Current) continue;
        if (Expanded) {
            Result.push_back(Current);
            continue;
        }
        Stack.push_back({Current, true});
        const auto &Subs = Current->getSubLoops();
        if (ReverseSiblingOrder) {
            for (Loop *Sub : Subs)
                if (Sub) Stack.push_back({Sub, false});
        } else {
            for (auto It = Subs.rbegin(); It != Subs.rend(); ++It)
                if (*It) Stack.push_back({*It, false});
        }
    }
    return Result;
}
//判断该基本块是否是该循环的退出块：即该基本块有后继不在该循环中
bool Loop::isExitingBlock(BasicBlock *BB) const {
    if (!BB || !contains(BB)) return false;
    for (BasicBlock *Succ : BB->getSuccessors()) {
        if (Succ && !contains(Succ)) return true;
    }
    return false;
}
//检查当前循环是否具有唯一 preheader、唯一 latch 和专用出口，并在成功时返回 preheader 与 latch
bool Loop::getLoopSimplifyForm(BasicBlock* &PreHeader, BasicBlock* &Latch) const {
    PreHeader = nullptr;
    Latch = nullptr;

    if (!Header) return false;

    std::vector<BasicBlock*> InPreds;
    std::vector<BasicBlock*> OutPreds;
    for (BasicBlock* pred : Header->getPredecessors()) {
        if (!pred) continue;
        if (contains(pred)) InPreds.push_back(pred);
        else OutPreds.push_back(pred);
    }

    if (InPreds.size() != 1 || OutPreds.size() != 1) return false;
    Latch = InPreds.front();
    PreHeader = OutPreds.front();
    if (!Latch || !PreHeader) return false;

    auto PreSuccs = PreHeader->getSuccessors();
    if (PreSuccs.size() != 1 || PreSuccs.front() != Header) return false;

    auto Exits = getExitBlocks();
    for (BasicBlock* Exit : Exits) {
        if (!Exit) continue;
        for (BasicBlock* Pred : Exit->getPredecessors()) {
            if (!contains(Pred)) return false;
        }
    }
    return true;
}
//返回给定基本块直接属于的最内层循环
Loop *LoopInfo::getLoopFor(BasicBlock *BB) const {
    auto It = BBToLoop.find(BB);
    return It != BBToLoop.end() ? It->second : nullptr;
}
//返回循环森林中所有顶层循环
const std::vector<Loop *> &LoopInfo::getTopLevelLoops() const {
    return TopLevelLoops;
}
//是否是循环不变值：常量、参数、全局值和定义在循环外的指令
bool LoopInfo::isLoopInvariant(Value *V, const Loop *L) {
    if (!V || !L) return true;
    if (V->isConst()) return true;
    if (V->isParam() || V->isGlobal()) return true;
    auto *I = dynamic_cast<Instruction *>(V);
    BasicBlock *BB = I ? I->getParent() : nullptr;
    return !BB || !L->contains(BB);
}
//后序遍历返回所有循环
std::vector<Loop*> LoopInfo::getLoopsInPostOrder(bool ReverseSiblingOrder) const {
    std::vector<Loop*> Result;
    if (ReverseSiblingOrder) {
        for (auto It = TopLevelLoops.rbegin(); It != TopLevelLoops.rend(); ++It) {
            if (!*It) continue;
            auto Nested = (*It)->getLoopsInPostOrder(true);
            Result.insert(Result.end(), Nested.begin(), Nested.end());
        }
    } else {
        for (Loop *Top : TopLevelLoops) {
            if (!Top) continue;
            auto Nested = Top->getLoopsInPostOrder(false);
            Result.insert(Result.end(), Nested.begin(), Nested.end());
        }
    }
    return Result;
}
bool LoopInfo::invalidate(Function &F, const PreservationStatus &PA) {
    if (PA.isKept(LoopAnalysis::ID()))
        return false;
    return true;
}

//1.获得循环头
void LoopInfoBuilder::discoverLoopHeaders(Function &F, const DominatorTree &DT,
                                          LoopInfo &LI,
                                          std::vector<Loop*>& RPOLoops) {
    //1.1使用支配树后序遍历
    std::vector<DominatorNode*> postorder = DT.getNodesInPostOrder();
    for (DominatorNode* dtNode : postorder) {
        //1.2检查每个基本块的前驱，若有前驱被该基本块支配，则是回边源，当前基本块是循环头
        BasicBlock* header = dtNode->Block;
        std::vector<BasicBlock*> backedges;
        auto predecessors = header->getPredecessors();
        for (BasicBlock* pred : predecessors) {
            if (DT.dominates(header,pred)) {
                backedges.push_back(pred);
            }
        }
        if (backedges.empty()) continue;
        //1.3发现循环头，创建 Loop 对象并记录循环头和回边源
        auto loop = std::make_unique<Loop>();
        loop->Header = header;
        loop->BackedgeSrcs = std::move(backedges);
        Loop* loopPtr = loop.get();
        LI.Loops.push_back(std::move(loop));
        //1.4记录RPO顺序，后序第三步使用
        RPOLoops.push_back(loopPtr);
    }
}

//2.从每个循环回边源按照CFG反向遍历到循环头，构建循环体和嵌套关系
void LoopInfoBuilder::LoopsCFG(LoopInfo &LI, const DominatorTree &DT) {
    //2.1先遍历内层循环后遍历外层循环
    for (auto& loopUPtr : LI.Loops) {
        Loop* L = loopUPtr.get();
        BasicBlock* header = L->Header;
        std::stack<BasicBlock*> worklist;
        for (BasicBlock* src : L->BackedgeSrcs) {
            worklist.push(src);
        }
        //2.2遍历所有回边源
        while (!worklist.empty()) {
            BasicBlock* bb = worklist.top();
            worklist.pop();
            Loop* subloop = LI.getLoopFor(bb);
            //2.3该块未归属任何循环，直接映射到当前循环，然后将前驱加入工作栈
            if (subloop == nullptr) {
                LI.BBToLoop[bb] = L;
                if (bb == header) continue;
                for (BasicBlock* pred : bb->getPredecessors()) {
                    worklist.push(pred);
                }
            //2.4该块已经归属某个循环，建立子循环与父循环嵌套关系
            } else {
                while (subloop->ParentLoop != nullptr && subloop->ParentLoop != L) {
                    subloop = subloop->ParentLoop;
                }
                if (subloop == L) continue;
                subloop->ParentLoop = L;
                L->SubLoops.push_back(subloop);
                BasicBlock* innerHeader = subloop->Header;
                for (BasicBlock* pred : innerHeader->getPredecessors()) {
                    if (!DT.dominates(innerHeader, pred)) {
                        worklist.push(pred);
                    }
                }
            }
        }
    }
}

//3.RPO
void LoopInfoBuilder::LoopsDFS(LoopInfo &LI, const std::vector<Loop*>& RPOLoops) {

    for (int i = (int)RPOLoops.size() - 1; i >= 0; --i) {
        Loop* L = RPOLoops[i];

        if (L->ParentLoop == nullptr) {
            LI.TopLevelLoops.push_back(L);
        }

        if (LI.getLoopFor(L->Header) == nullptr) {
            LI.BBToLoop[L->Header] = L;
        }
    }

    for (auto& pair : LI.BBToLoop) {
        BasicBlock* bb = pair.first;
        Loop* ownerLoop = pair.second;
        if (ownerLoop != nullptr) {
            ownerLoop->Blocks.push_back(bb);
        }
    }
}

//LoopInfo分析主流程:
LoopInfo LoopInfoBuilder::build(Function &F, const DominatorTree &DT) {
    //1.
    LoopInfo LI;
    if (F.size() == 0 || DT.Root == nullptr)
        return LI;

    //2.发现所有自然循环头，填充Header和BackedgeSrcs，并记录RPO顺序
    std::vector<Loop*> RPOLoops;
    discoverLoopHeaders(F, DT, LI, RPOLoops);
    if (LI.Loops.empty())
        return LI;

    //3.构建循环体和循环嵌套，填充ParentLoop、SubLoops
    LoopsCFG(LI,DT);

    //4.填充LoopInfo中TopLevelLoops，BBToLoop，以及Blocks直接块
    LoopsDFS(LI, RPOLoops);

    return LI;
}

PassID LoopAnalysis::Key;
LoopAnalysis::Result LoopAnalysis::run(Function &F, FunctionAnalysisPassManager &FAM) const {
    //1.依赖支配树
    const DominatorTree &DT = FAM.getResult<DominantAnalysis>(F);
    //2.构建循环森林
    LoopInfoBuilder builder;
    return builder.build(F, DT);
}
