#include "ir/analysis/LoopInfo.hpp"
#include <stack>
#include <queue>
#include <algorithm>
Loop::Loop() = default;

BasicBlock *Loop::getHeader() const {
    return Header;
}

Loop *Loop::getParentLoop() const {
    return ParentLoop;
}

const std::vector<Loop *> &Loop::getSubLoops() const {
    return SubLoops;
}

const std::vector<BasicBlock *> &Loop::getBlocks() const {
    return Blocks;
}

const std::vector<BasicBlock *> &Loop::getBackedgeSrcs() const {
    return BackedgeSrcs;
}

unsigned Loop::getDepth() const {
    unsigned Depth = 1;
    for (Loop *Parent = ParentLoop; Parent; Parent = Parent->ParentLoop)
        ++Depth;
    return Depth;
}

bool Loop::isInnermost() const {
    return SubLoops.empty();
}

LoopInfo::LoopInfo() = default;

LoopInfo::LoopInfo(LoopInfo &&) = default;

LoopInfo &LoopInfo::operator=(LoopInfo &&) = default;

void LoopInfo::clear() {
    Loops.clear();
    TopLevelLoops.clear();
    BBToLoop.clear();
}

Loop *LoopInfo::getLoopFor(BasicBlock *BB) const {
    auto It = BBToLoop.find(BB);
    return It != BBToLoop.end() ? It->second : nullptr;
}

const std::vector<Loop *> &LoopInfo::getTopLevelLoops() const {
    return TopLevelLoops;
}

bool Loop::contains(BasicBlock* BB) const {
    for (BasicBlock* b : Blocks)
        if (b == BB) return true;
    for (Loop* sub : SubLoops)
        if (sub->contains(BB)) return true;
    return false;
}

// 查询流程：先展平当前循环树的内部块，再扫描离开该集合的 CFG 边并去重出口。
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

std::vector<BasicBlock*> Loop::getBlocksInFunctionOrder() const {
    std::vector<BasicBlock*> Result;
    Function *F = Header ? Header->getParent() : nullptr;
    if (!F) return Result;
    for (BasicBlock *BB : *F) {
        if (BB && contains(BB)) Result.push_back(BB);
    }
    return Result;
}

// 顺序查询只遍历当前循环直接拥有的块，子循环由循环树查询单独处理。
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

bool Loop::isExitingBlock(BasicBlock *BB) const {
    if (!BB || !contains(BB)) return false;
    for (BasicBlock *Succ : BB->getSuccessors()) {
        if (Succ && !contains(Succ)) return true;
    }
    return false;
}

// 形状查询依次验证唯一循环内前驱、唯一循环外前驱、专用预头和专用出口。
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

bool LoopInfo::invalidate(Function &F, const PreservationStatus &PA) {
    if (PA.isKept(LoopAnalysis::ID()))
        return false;
    return true;
}

std::vector<Loop*> LoopInfo::getLoopsInPostOrder(
    bool ReverseSiblingOrder) const {
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

// 1. 后序扫描支配树，把 Header 支配其前驱的边识别为回边并创建循环对象。
void LoopInfoBuilder::discoverLoopHeaders(Function &F, const DominatorTree &DT,
                                          LoopInfo &LI,
                                          std::vector<Loop*>& RPOLoops) {

    std::vector<DominatorNode*> postorder = DT.getNodesInPostOrder();

    for (DominatorNode* dtNode : postorder) {

        BasicBlock* header = dtNode->Block;

        std::vector<BasicBlock*> backedges;
        auto predecessors = header->getPredecessors();
        for (BasicBlock* pred : predecessors) {
            if (DT.dominates(header,pred)) {
                backedges.push_back(pred);
            }
        }

        if (backedges.empty()) continue;

        auto loop = std::make_unique<Loop>();
        loop->Header = header;
        loop->BackedgeSrcs = std::move(backedges);

        Loop* loopPtr = loop.get();
        LI.Loops.push_back(std::move(loop));
        RPOLoops.push_back(loopPtr);
    }
}

// 2. 按内层优先顺序从回边源逆向走 CFG，确定直接块归属并连接父子循环。
void LoopInfoBuilder::LoopsCFG(LoopInfo &LI, const DominatorTree &DT) {
    // 2.1 每个循环以全部回边源为种子，向前驱方向扩展到 Header。
    for (auto& loopUPtr : LI.Loops) {
        Loop* L = loopUPtr.get();
        BasicBlock* header = L->Header;
        std::stack<BasicBlock*> worklist;

        for (BasicBlock* src : L->BackedgeSrcs) {
            worklist.push(src);
        }

        while (!worklist.empty()) {
            BasicBlock* bb = worklist.top();
            worklist.pop();

            // 2.2 未归属块加入当前循环；已归属块代表内层循环，只越过其入口继续搜索。
            Loop* subloop = LI.getLoopFor(bb);

            if (subloop == nullptr) {

                LI.BBToLoop[bb] = L;

                if (bb == header) continue;

                for (BasicBlock* pred : bb->getPredecessors()) {
                    worklist.push(pred);
                }
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

// 3. 逆转发现顺序得到外层优先次序，收集顶层循环并填充每个循环的直接块列表。
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

// 自然循环分析主流程：以支配回边为入口，构建循环森林及块到最内层循环的映射。
LoopInfo LoopInfoBuilder::build(Function &F, const DominatorTree &DT) {
    // 1. 缺少可达入口或支配树时返回空结果。
    LoopInfo LI;

    if (F.size() == 0 || DT.Root == nullptr)
        return LI;

    // 2. 发现所有自然循环头，并保留内层优先的发现顺序。
    std::vector<Loop*> RPOLoops;
    discoverLoopHeaders(F, DT, LI, RPOLoops);

    if (LI.Loops.empty())
        return LI;

    // 3. 逆向 CFG 填充归属关系，同时建立循环嵌套。
    LoopsCFG(LI,DT);

    // 4. 整理顶层循环和直接块列表，完成结果封装。
    LoopsDFS(LI, RPOLoops);

    return LI;
}

PassID LoopAnalysis::Key;

LoopAnalysis::Result LoopAnalysis::run(Function &F, FunctionAnalysisPassManager &FAM) const {
    // 1. 自然循环的回边判定依赖支配关系，先取得当前函数的支配树。
    const DominatorTree &DT = FAM.getResult<DominantAnalysis>(F);

    // 2. 运行三阶段构建器，返回循环森林和最内层块归属映射。
    LoopInfoBuilder builder;
    return builder.build(F, DT);
}
