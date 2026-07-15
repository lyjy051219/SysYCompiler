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

// Loop::contains --外界使用这个AnalysisPass的接口
bool Loop::contains(BasicBlock* BB) const {
    for (BasicBlock* b : Blocks)
        if (b == BB) return true;
    for (Loop* sub : SubLoops)
        if (sub->contains(BB)) return true;
    return false;
}

std::vector<BasicBlock*> Loop::getExitBlocks() const {
    std::vector<BasicBlock*> exitBlocks;
    std::unordered_set<BasicBlock*> allInternalBlocks; // 存放该循环及其所有子循环的块
    std::unordered_set<BasicBlock*> seenExits;        // 用于出口块去重

    // 步骤 1：收集所有内部块 (Standard Tree Traversal)
    // 复杂度：O(TotalBlocksInLoop)
    std::vector<const Loop*> loopStack = { this };
    while (!loopStack.empty()) {
        const Loop* curr = loopStack.back();
        loopStack.pop_back();

        // 将当前层级的块加入集合
        for (BasicBlock* bb : curr->getBlocks()) {
            if (bb) allInternalBlocks.insert(bb);
        }
        // 将子循环压栈
        for (Loop* sub : curr->getSubLoops()) {
            if (sub) loopStack.push_back(sub);
        }
    }

    // 步骤 2：识别出口边 (Identify Exit Edges)
    // 复杂度：O(TotalEdgesInLoop)
    // 逻辑：遍历所有内部块，如果它的后继不在 allInternalBlocks 集合中，则该后继是出口块
    for (BasicBlock* bb : allInternalBlocks) {
        for (BasicBlock* succ : bb->getSuccessors()) {
            if (succ && allInternalBlocks.find(succ) == allInternalBlocks.end()) {
                // 如果该出口块还没被记录过，则加入结果集
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

    // Dedicated Exits: 所有 exit block 的前驱都必须来自循环内部
    auto Exits = getExitBlocks();
    for (BasicBlock* Exit : Exits) {
        if (!Exit) continue;
        for (BasicBlock* Pred : Exit->getPredecessors()) {
            if (!contains(Pred)) return false;
        }
    }

    return true;
}

// LoopInfo::invalidate
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

// 步骤1：后序遍历支配树，识别回边 & 创建 Loop 对象
// RPOLoops 按拓扑（外层先入）顺序收集，便于步骤3使用
void LoopInfoBuilder::discoverLoopHeaders(Function &F, const DominatorTree &DT,
                                          LoopInfo &LI,
                                          std::vector<Loop*>& RPOLoops) {
    // 1a. 后序遍历支配树，得到后序序列
    std::vector<DominatorNode*> postorder = DT.getNodesInPostOrder();

    // 1b. 对后序序列中的每个节点，检查是否有回边指向它（即是否是循环头）
    for (DominatorNode* dtNode : postorder) {
        //先假设目前的是循环头
        BasicBlock* header = dtNode->Block;

        //收集回边：某个节点被循环头支配，同时这个结点在BasicBlock中又是循环头的前驱
        std::vector<BasicBlock*> backedges;
        auto predecessors = header->getPredecessors();
        for (BasicBlock* pred : predecessors) {
            if (DT.dominates(header,pred)) {
                backedges.push_back(pred);
            }
        }

        if (backedges.empty()) continue; // 不是循环头

        // 创建 Loop 对象
        auto loop = std::make_unique<Loop>();
        loop->Header = header;
        loop->BackedgeSrcs = std::move(backedges);

        Loop* loopPtr = loop.get();
        LI.Loops.push_back(std::move(loop));
        RPOLoops.push_back(loopPtr); // 后序 => 内层优先
    }
}

// 步骤2：反向遍历 CFG，一次性完成基本块归属与父子循环树的建立
void LoopInfoBuilder::LoopsCFG(LoopInfo &LI, const DominatorTree &DT) {
    /*
    核心思想：
    1. 初始化工作队列为所有回边的起点（也叫Latch节点）
    2. 对于工作队列中的每个块：
    如果该块没有被分配：
       a) 如果块是新的，将其分配给循环L
       b) 如果块是循环头，停止向前遍历（否则会跑到循环外面）
       c) 否则，将块的所有前驱加入工作队列
    如果该块已经被分配了，说明我们遇到了一个内层循环：
    （因为是后序遍历的原因，我们总是先处理内部循环再处理外部循环，所以只能是遇到了更内层的循环）
       - 建立父子关系
       - 跳过内层循环体（因为我们已经将整个内循环确认为当前L的子循环了），从内层循环头的前驱继续
    */
    for (auto& loopUPtr : LI.Loops) {
        Loop* L = loopUPtr.get();
        BasicBlock* header = L->Header;
        std::stack<BasicBlock*> worklist;

        // 1. 将回边起点加入工作队列
        for (BasicBlock* src : L->BackedgeSrcs) {
            worklist.push(src);
        }

        // 2. 开始逆向遍历
        while (!worklist.empty()) {
            BasicBlock* bb = worklist.top();
            worklist.pop();

            Loop* subloop = LI.getLoopFor(bb);

            if (subloop == nullptr) {
                // 情况 A：发现一个新块，它专属于当前循环 L
                LI.BBToLoop[bb] = L;

                // 到达循环头就停止向上探索
                if (bb == header) continue;

                // 将前驱加入队列，继续逆向
                for (BasicBlock* pred : bb->getPredecessors()) {
                    worklist.push(pred);
                }
            } else {
                // 情况 B：遇到一个已经属于其他循环的块，说明我们撞上了一个内层循环

                // 找到这个内层循环目前已知的“最外层”祖先
                while (subloop->ParentLoop != nullptr && subloop->ParentLoop != L) {
                    subloop = subloop->ParentLoop;
                }

                // 如果它已经是 L 的子循环了，说明这条路径处理过，直接跳过
                if (subloop == L) continue;

                subloop->ParentLoop = L;
                L->SubLoops.push_back(subloop);

                // 跳过整个内层循环体，直接从内层循环 Header 的前驱继续逆向探索
                BasicBlock* innerHeader = subloop->Header;
                for (BasicBlock* pred : innerHeader->getPredecessors()) {
                    // 修正：只有不被 innerHeader 支配的前驱，才是从外部进入内层循环的边
                    if (!DT.dominates(innerHeader, pred)) {
                        worklist.push(pred);
                    }
                }
            }
        }
    }
}

// 步骤3：RPO 遍历（后序结果反转），在父循环中建立与子循环的关系
void LoopInfoBuilder::LoopsDFS(LoopInfo &LI, const std::vector<Loop*>& RPOLoops) {
    /*
    优化后的步骤3逻辑：
    父子关系已在步骤2建立。这里利用 RPO (外层->内层) 的顺序：
    1. 收集所有的 TopLevelLoops。
    2. 可以做一些基于深度的初始化（如果需要）。
    3. 将 BBToLoop 中的基本块塞入它们对应的 L->Blocks 中。
    */

    // 1. RPO 遍历循环 (从 RPOLoops 的末尾向前遍历即为 RPO)
    for (int i = (int)RPOLoops.size() - 1; i >= 0; --i) {
        Loop* L = RPOLoops[i];

        // 如果没有父节点，它就是顶层循环 (Top-Level Loop)
        if (L->ParentLoop == nullptr) {
            LI.TopLevelLoops.push_back(L);
        }

        // 确保 Header 也被记录在当前循环中 (在步骤2中可能没填进去)
        // 仅当 Header 真的属于当前循环 (而不是更内层的循环) 时才归属
        if (LI.getLoopFor(L->Header) == nullptr) {
            LI.BBToLoop[L->Header] = L;
        }
    }

    // 2. 填充每个循环直接包含的 BasicBlock (过滤掉子循环的块)
    for (auto& pair : LI.BBToLoop) {
        BasicBlock* bb = pair.first;
        Loop* ownerLoop = pair.second;
        if (ownerLoop != nullptr) {
            ownerLoop->Blocks.push_back(bb);
        }
    }
}

//自然循环识别算法主流程：
LoopInfo LoopInfoBuilder::build(Function &F, const DominatorTree &DT) {
    /*
    1. 获取支配树分析结果（必须依赖DominatorTree）
    2. Postorder 遍历支配树：
       - 对每个块，检查它的所有前驱
       - 如果某个前驱被该块支配，则该边是回边（backedge）
       - 回边指向的块就是潜在的循环头
    3. 对于每个有回边的块（循环头），创建一个Loop对象
    4. 通过反向CFG遍历填补这个Loop对象的BasicBlock从属关系，各个Loop之间从属关系
    5. 最后通过正向全局 RPO 遍历天然地填充块向量和建立嵌套关系 (原创优化方案)
    */
    LoopInfo LI;

    if (F.Size() == 0 || DT.Root == nullptr)
        return LI;

    //步骤1：后序遍历支配树，识别循环头，创建 Loop 对象
    //RPO顺序数组--用于步骤3
    std::vector<Loop*> RPOLoops;
    discoverLoopHeaders(F, DT, LI, RPOLoops);

    if (LI.Loops.empty())
        return LI;

    //步骤2：反向遍历 CFG，在子循环中建立与父循环的关系
    LoopsCFG(LI,DT);

    //步骤3：RPO 遍历，在父循环中建立与子循环的关系
    LoopsDFS(LI, RPOLoops);

    return LI;
}

// LoopAnalysis::run
PassID LoopAnalysis::Key;

LoopAnalysis::Result LoopAnalysis::run(Function &F, FunctionAnalysisPassManager &FAM) const {
    const DominatorTree &DT = FAM.getResult<DominantAnalysis>(F);
    //我们使用自然循环识别算法来构建LoopInfo
    LoopInfoBuilder builder;
    return builder.build(F, DT);
}
