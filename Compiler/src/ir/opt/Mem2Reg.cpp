#include "ir/opt/Mem2Reg.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "lib/Tool.hpp"
#include "lib/TraverseTools.hpp"
#include <queue>
#include <set>
#include <stack>

void AllocaInfo::clean() {
    StoreBlocks.clear();
    LoadBlocks.clear();
    OnlyBlock = nullptr;
    OnlyStore = nullptr;
    OnlyUsedOneBlock = true;
}

unsigned BlockInfo::getInstIndex(const Instruction* I){
    auto it = InstNum.find(I);
    if(it != InstNum.end()) return it->second;
    unsigned bbnum = 0;
    const BasicBlock* bb = I->getParent();
    for(auto inst : *const_cast<BasicBlock*>(bb)){
        auto *LI = dynamic_cast<LoadInst *>(inst);
        auto *SI = dynamic_cast<StoreInst *>(inst);
        if ((LI && dynamic_cast<AllocaInst *>(LI->getPointerOperand())) ||
            (SI && dynamic_cast<AllocaInst *>(SI->getPointerOperand()))) {
            InstNum[inst] = bbnum++;
        }
    }
    return InstNum[I];
}

void BlockInfo::deleteIndex(const Instruction* I){InstNum.erase(I);}

bool PromotePass::isAllocaPromotable(const AllocaInst* AI) {
    auto ptrType = std::dynamic_pointer_cast<POINTERType>(
        const_cast<AllocaInst*>(AI)->getIRType());
    if (!ptrType) return false;

    auto pointeeType = ptrType->getPointerType();
    if (!pointeeType) return false;

    // 只提升标量局部对象；数组和 void 对象不进入 Mem2Reg。
    auto pointeeEnum = pointeeType->getEnum();
    if (pointeeEnum == IR_ARRAY || pointeeEnum == IR_VOID) {
        return false;
    }

    for (Use *use : AI->getUses()) {
        User* user = use->getUser();
        if (dynamic_cast<LoadInst*>(user)) {
            continue;
        }
        else if (auto storeInst = dynamic_cast<StoreInst*>(user)) {
            Value* storedVal = storeInst->getValueOperand();
            if (use->getOperandIndex() == 0) {
                return false;
            }

            // 指针逃逸：将 alloca 地址作为值存入其它内存。
            if (dynamic_cast<AllocaInst*>(storedVal)) {
                return false;
            }

            // 类型不匹配：store 值类型必须与 alloca 的被分配类型一致。
            if (storedVal->getIRType() != pointeeType) {
                return false;
            }
            continue;
        }
        else return false;
    }
    return true;
}

void AllocaInfo::AnalyzeAlloca(AllocaInst* AI) {
    clean();
    for (Use *use : AI->getUses()) {
        User* user = use->getUser();
        Instruction* inst = dynamic_cast<Instruction*>(user);
        if (!inst) continue;
        BasicBlock* parentBlock = inst->getParent();
        if(StoreInst *SI = dynamic_cast<StoreInst*>(inst)){
            StoreBlocks.push_back(SI->getParent());
            OnlyStore = SI;
        }else{
            LoadInst* LI = dynamic_cast<LoadInst*>(inst);
            LoadBlocks.push_back(LI->getParent());
        }
        if (OnlyUsedOneBlock) {
            if (OnlyBlock == nullptr) {
                OnlyBlock = parentBlock;
            } else if (OnlyBlock != parentBlock) {
                OnlyUsedOneBlock = false;
                OnlyBlock = nullptr;
            }
        }
    }
}

bool PromotePass::rewriteSingleStoreAlloca(AllocaInfo &info,AllocaInst* AI,DominatorTree& DT,BlockInfo &blockInfo){
    StoreInst* onlyStore = info.OnlyStore;
    Value* val = onlyStore->getValueOperand();
    BasicBlock* BB = onlyStore->getParent();
    unsigned index = -1;
    info.LoadBlocks.clear();

    std::vector<LoadInst*> loadsToReplace;
    std::vector<LoadInst*> loadsToKeep;

    for (Use *use : AI->getUses()) {
        User* user = use->getUser();
        if(dynamic_cast<StoreInst*>(user) == onlyStore) continue;
        LoadInst* LI = dynamic_cast<LoadInst*>(user);
        if(!LI) continue;

        bool canReplace = true;
        if(!val->isGlobal() && !val->isConst()){
            if(LI->getParent() == BB){
                if(index == (unsigned)-1) index = blockInfo.getInstIndex(onlyStore);
                unsigned loadIndex = blockInfo.getInstIndex(LI);
                if(loadIndex < index){
                    canReplace = false;
                }
            }else if(!DT.dominates(BB, LI->getParent())){
                canReplace = false;
            }
        }

        if(canReplace){
            loadsToReplace.push_back(LI);
        } else {
            loadsToKeep.push_back(LI);
        }
    }

    if(!loadsToKeep.empty()){
        for(auto* LI : loadsToKeep)
            info.LoadBlocks.push_back(LI->getParent());
        return false;
    }

    for(auto* LI : loadsToReplace){
        LI->replaceAllUsesWith(val);
        blockInfo.deleteIndex(LI);
        delete LI;
    }

    blockInfo.deleteIndex(onlyStore);
    delete onlyStore;
    blockInfo.deleteIndex(AI);
    delete AI;
    return true;
}

bool PromotePass::rewriteSingleBlockAlloca(AllocaInfo &info,AllocaInst* AI,DominatorTree& DT,BlockInfo &blockInfo){
    std::vector<std::pair<unsigned,StoreInst*>> stores;
    for (Use *use : AI->getUses()) {
        User* user = use->getUser();
        if(StoreInst* SI = dynamic_cast<StoreInst*>(user))
            stores.push_back(std::make_pair(blockInfo.getInstIndex(SI),SI));
    }

    std::sort(stores.begin(), stores.end(),
    [](const std::pair<unsigned, StoreInst*>& a, const std::pair<unsigned, StoreInst*>& b) {
        return a.first < b.first;
    });

    std::vector<std::pair<LoadInst*, Value*>> replacements;

    for (Use *use : AI->getUses()) {
        User* user = use->getUser();
        LoadInst* LI = dynamic_cast<LoadInst*>(user);
        if(!LI) continue;

        unsigned loadIndex = blockInfo.getInstIndex(LI);

        auto it = std::lower_bound(
            stores.begin(), stores.end(),
            std::make_pair(loadIndex, static_cast<StoreInst*>(nullptr)),
            [](const std::pair<unsigned, StoreInst*>& a, const std::pair<unsigned, StoreInst*>& b) {
                return a.first < b.first;
            }
        );

        Value* retVal;
        if(it == stores.begin()){
            if(stores.empty()) retVal = UndefValue::NewUndefValue(LI->getIRType());
            else return false;
        }else{
            retVal = std::prev(it)->second->getValueOperand();
        }

        replacements.push_back({LI, retVal});
    }

    for(auto& [LI, retVal] : replacements){
        LI->replaceAllUsesWith(retVal);
        blockInfo.deleteIndex(LI);
        delete LI;
    }

    std::vector<StoreInst*> storesToDelete;
    for (Use *use : AI->getUses()) {
        if(StoreInst* SI = dynamic_cast<StoreInst*>(use->getUser()))
            storesToDelete.push_back(SI);
    }
    for(auto* SI : storesToDelete){
        blockInfo.deleteIndex(SI);
        delete SI;
    }

    blockInfo.deleteIndex(AI);
    delete AI;
    return true;
}

// computeIDF:
//   使用优先队列 (深度最大的先处理) + 支配树子树 DFS
//   直接在子树 CFG 后继中发现 DF 边，无需预计算全局 DF
void PromotePass::computeIDF(
    const std::set<BasicBlock*>& DefBlocks,
    const std::set<BasicBlock*>& LiveInBlocks,
    std::set<BasicBlock*>& PhiBlocks,
    DominatorTree& DT,
    const std::unordered_map<DominatorNode*, unsigned>& DomLevels)
{
    // {level, node} 大顶堆，level 最大(最深)的先处理
    using DTNPair = std::pair<unsigned, DominatorNode*>;
    std::priority_queue<DTNPair> PQ;

    // 已进入扫描逻辑（对应算法中的 VisitedWorkList）
    std::set<DominatorNode*> VisitedWorkList;
    // 已决定插入 Phi（对应算法中的 VisitedPHI）
    std::set<DominatorNode*> VisitedPHI;

    for (BasicBlock* BB : DefBlocks) {
        DominatorNode* node = DT.getNode(BB);
        if (!node) continue;
        auto it = DomLevels.find(node);
        if (it != DomLevels.end()) {
            PQ.push({it->second, node});
            VisitedWorkList.insert(node);
        }
    }

    while (!PQ.empty()) {
        auto [rootLevel, root] = PQ.top();
        PQ.pop();

        // WorkList DFS：遍历 root 支配树子孙
        std::stack<DominatorNode*> WorkList;
        std::set<DominatorNode*> LocalVisited;
        WorkList.push(root);
        LocalVisited.insert(root);

        while (!WorkList.empty()) {
            DominatorNode* node = WorkList.top();
            WorkList.pop();

            // 检查 node 的 CFG 后继
            for (BasicBlock* succ : node->Block->getSuccessors()) {
                DominatorNode* succNode = DT.getNode(succ);
                if (!succNode) continue;

                auto succLevelIt = DomLevels.find(succNode);
                if (succLevelIt == DomLevels.end()) continue;

                // succ 比本轮根更深，仍被严格支配，不是本轮 IDF 候选
                if (succLevelIt->second > rootLevel) continue;

                // Pruned: 只在 live-in 块放 Phi
                if (!LiveInBlocks.count(succ)) continue;

                if (VisitedPHI.insert(succNode).second) {
                    PhiBlocks.insert(succ);

                    // 新产生的定义块继续参与 IDF 迭代
                    if (!DefBlocks.count(succ) && VisitedWorkList.insert(succNode).second) {
                        PQ.push({succLevelIt->second, succNode});
                    }
                }
            }

            // 将支配树子节点加入 WorkList
            for (auto* child = node->FirstChild; child; child = child->NextSibling) {
                if (LocalVisited.insert(child).second)
                    WorkList.push(child);
            }
        }
    }
}

bool PromotePass::promoteMemoryToRegister(Function& F, DominatorTree& DT){
    bool changed = false;
    BasicBlock* EntryBB = F.front();
    if (!EntryBB) return false;

    // 外层不动点循环：直到本轮找不到可提升/可消除的 alloca
    while (true) {
    std::vector<AllocaInst*> Allocas;
    AllocaInfo info;
    BlockInfo blockInfo;
    bool roundChanged = false;

    for (AllocaInst *AI : collectInstructions<AllocaInst>(*EntryBB))
        if (isAllocaPromotable(AI)) Allocas.push_back(AI);

    if(Allocas.empty()) {
        // Earlier rounds may already have promoted or deleted allocas.  Do
        // not lose that mutation merely because the final discovery round is
        // empty: fixed-point convergence and analysis invalidation consume
        // the cumulative answer.
        return changed;
    }

    // 简单优化: 删除无用 alloca / 单 store / 单 block
    for(unsigned i = 0; i != Allocas.size(); ++i){
        auto AI = Allocas[i];
        if (!AI->hasUses()) {
            delete AI;
            Allocas[i] = Allocas.back();
            Allocas.pop_back();
            i--;
            changed = true;
            roundChanged = true;
            continue;
        }
        info.AnalyzeAlloca(AI);
        if(info.StoreBlocks.size() == 1){
            if(rewriteSingleStoreAlloca(info, AI, DT, blockInfo)){
                Allocas[i] = Allocas.back();
                Allocas.pop_back();
                i--;
                changed = true;
                roundChanged = true;
                continue;
            }
        }
        if(info.OnlyUsedOneBlock && rewriteSingleBlockAlloca(info, AI, DT, blockInfo)){
            Allocas[i] = Allocas.back();
            Allocas.pop_back();
            i--;
            changed = true;
            roundChanged = true;
            continue;
        }
    }

    if(Allocas.empty()) {
        if (!roundChanged) break;
        continue;
    }

    std::unordered_map<BasicBlock*, std::vector<BasicBlock*>> PredMap;
    for (auto bb : F) {
        for (BasicBlock* succ : bb->getSuccessors()) {
            PredMap[succ].push_back(bb);
        }
    }

    //4.计算Pruned SSA
    std::unordered_map<DominatorNode*, unsigned> DomLevels;
    {
        DominatorNode* root = DT.getNode(F.front());
        if (root) {
            std::queue<DominatorNode*> q;
            q.push(root);
            DomLevels[root] = 0;
            while (!q.empty()) {
                auto* nd = q.front(); q.pop();
                unsigned lvl = DomLevels[nd];
                for (auto* ch = nd->FirstChild; ch; ch = ch->NextSibling) {
                    DomLevels[ch] = lvl + 1;
                    q.push(ch);
                }
            }
        }
    }

    // ========== 5. 计算 IDF + Pruned Phi 放置 ==========

    std::map<AllocaInst*, unsigned> AllocaIndex;
    for (unsigned i = 0; i < Allocas.size(); ++i)
        AllocaIndex[Allocas[i]] = i;

    std::vector<std::set<BasicBlock*>> DefBlocks(Allocas.size());
    for (unsigned i = 0; i < Allocas.size(); ++i) {
        for (Use *use : Allocas[i]->getUses()) {
            if (StoreInst* SI = dynamic_cast<StoreInst*>(use->getUser()))
                DefBlocks[i].insert(SI->getParent());
        }
    }

    std::map<BasicBlock*, std::map<unsigned, PhiInst*>> PhiNodes;

    for (unsigned i = 0; i < Allocas.size(); ++i) {
        AllocaInst* AI = Allocas[i];
        auto ptrType = std::dynamic_pointer_cast<POINTERType>(AI->getIRType());
        std::shared_ptr<IRType> allocType = ptrType->getPointerType();

        // 计算 live-in blocks
        std::set<BasicBlock*> LiveInBlocks;
        computeLiveInBlocks(AI, DefBlocks[i], LiveInBlocks,PredMap);

        //计算IDF
        std::set<BasicBlock*> phiBlocks;
        computeIDF(DefBlocks[i], LiveInBlocks, phiBlocks, DT, DomLevels);

        for (BasicBlock* Y : phiBlocks) {
            PhiInst* phi = new PhiInst(allocType);
            Y->pushFront(phi);
            PhiNodes[Y][i] = phi;
        }
    }

    // ========== 6. 变量重命名 ==========
    unsigned numAllocas = Allocas.size();
    std::vector<std::stack<Value*>> IncomingVals(numAllocas);

    for(unsigned i = 0; i < numAllocas; ++i){
        auto ptrType = std::dynamic_pointer_cast<POINTERType>(Allocas[i]->getIRType());
        IncomingVals[i].push(UndefValue::NewUndefValue(ptrType->getPointerType()));
    }

    std::stack<Mem2RegRenameFrame> renameStack;
    DominatorNode* rootNode = DT.getNode(F.front());
    if(!rootNode) return changed;

    renameStack.push({rootNode, rootNode->FirstChild, std::vector<unsigned>(numAllocas, 0), false});

    while(!renameStack.empty()){
        auto& frame = renameStack.top();
        BasicBlock* BB = frame.Node->Block;

        if(!frame.Processed){
            frame.Processed = true;

            // 6.1 处理该块中的 Phi 结点
            auto phiIt = PhiNodes.find(BB);
            if(phiIt != PhiNodes.end()){
                for(auto& [allocIdx, phi] : phiIt->second){
                    IncomingVals[allocIdx].push(phi);
                    frame.PushCounts[allocIdx]++;
                }
            }

            // 6.2 遍历块中的指令，处理 load 和 store
            std::vector<Instruction*> instsToDelete;

            for(auto inst : *BB){
                if(dynamic_cast<PhiInst*>(inst)) continue;

                if(LoadInst* LI = dynamic_cast<LoadInst*>(inst)){
                    Value* ptr = LI->getPointerOperand();
                    AllocaInst* AI = dynamic_cast<AllocaInst*>(ptr);
                    if(AI){
                        auto idxIt = AllocaIndex.find(AI);
                        if(idxIt != AllocaIndex.end()){
                            unsigned allocIdx = idxIt->second;
                            Value* currVal = IncomingVals[allocIdx].top();
                            LI->replaceAllUsesWith(currVal);
                            instsToDelete.push_back(LI);
                        }
                    }
                }
                else if(StoreInst* SI = dynamic_cast<StoreInst*>(inst)){
                    Value* ptr = SI->getPointerOperand();
                    AllocaInst* AI = dynamic_cast<AllocaInst*>(ptr);
                    if(AI){
                        auto idxIt = AllocaIndex.find(AI);
                        if(idxIt != AllocaIndex.end()){
                            unsigned allocIdx = idxIt->second;
                            Value* storedVal = SI->getValueOperand();
                            IncomingVals[allocIdx].push(storedVal);
                            frame.PushCounts[allocIdx]++;
                            instsToDelete.push_back(SI);
                        }
                    }
                }
            }

            for(auto* inst : instsToDelete){
                blockInfo.deleteIndex(inst);
                delete inst;
            }

            // 6.3 填充后继块的 Phi 结点
            auto succs = BB->getSuccessors();
            for(BasicBlock* succBB : succs){
                auto succPhiIt = PhiNodes.find(succBB);
                if(succPhiIt == PhiNodes.end()) continue;
                for(auto& [allocIdx, phi] : succPhiIt->second){
                    Value* currVal = IncomingVals[allocIdx].top();
                    phi->addIncoming(currVal, BB);
                }
            }
        }

        // 6.4 DFS 遍历子节点
        if(frame.ChildIter != nullptr){
            DominatorNode* child = frame.ChildIter;
            frame.ChildIter = child->NextSibling;
            renameStack.push({child, child->FirstChild, std::vector<unsigned>(numAllocas, 0), false});
        } else {
            for(unsigned i = 0; i < numAllocas; ++i){
                for(unsigned j = 0; j < frame.PushCounts[i]; ++j){
                    IncomingVals[i].pop();
                }
            }
            renameStack.pop();
        }
    }

    // ========== 7. 清除 Alloca 指令 ==========
    for (AllocaInst* AI : Allocas) {
        blockInfo.deleteIndex(AI);
        delete AI;
    }
    roundChanged = true;

    // ========== 8. Trivial Phi Simplification ==========
    bool phiChanged = true;
    while (phiChanged) {
        phiChanged = false;
        for (auto it = PhiNodes.begin(); it != PhiNodes.end(); ) {
            auto& [BB, phiMap] = *it;
            for (auto pit = phiMap.begin(); pit != phiMap.end(); ) {
                PhiInst* phi = pit->second;
                if (simplifyPhiNode(phi)) {
                    pit = phiMap.erase(pit);
                    phiChanged = true;
                } else {
                    ++pit;
                }
            }
            if (phiMap.empty())
                it = PhiNodes.erase(it);
            else
                ++it;
        }
    }

    changed = true;
    if (!roundChanged) break;
    }

    return changed;
}

PassResult PromotePass::run(Function &F, FunctionAnalysisPassManager &FAM){
    auto &dominant = FAM.getResult<DominantAnalysis>(F);

    if(!promoteMemoryToRegister(F, dominant)) {
        return PassResult::unchanged();
    }

    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<PostDominantAnalysis>();
    PA.keep<LoopAnalysis>();
    PA.discard<BasicAliasAnalysis>();
    PA.discard<SideEffectAnalysis>();
    return PassResult::changed(std::move(PA));
}

void PromotePass::computeLiveInBlocks(
    AllocaInst* AI,
    const std::set<BasicBlock*>& DefBlocks,
    std::set<BasicBlock*>& LiveInBlocks,
    const std::unordered_map<BasicBlock*, std::vector<BasicBlock*>>& PredMap)
{
    std::vector<BasicBlock*> UseBlocksToPropagate;

    for (Use *use : AI->getUses()) {
        LoadInst* LI = dynamic_cast<LoadInst*>(use->getUser());
        if (!LI) continue;

        BasicBlock* BB = LI->getParent();

        if (!DefBlocks.count(BB)) {
            UseBlocksToPropagate.push_back(BB);
            continue;
        }

        // 该块既有 load 又有 store，检查指令顺序
        bool foundStoreFirst = false;
        bool foundLoadBeforeStore = false;
        for (auto inst : *BB) {
            if (auto* SI = dynamic_cast<StoreInst*>(inst)) {
                if (dynamic_cast<AllocaInst*>(SI->getPointerOperand()) == AI) {
                    foundStoreFirst = true;
                    break;
                }
            }
            if (auto* loadI = dynamic_cast<LoadInst*>(inst)) {
                if (dynamic_cast<AllocaInst*>(loadI->getPointerOperand()) == AI) {
                    foundLoadBeforeStore = true;
                    break;
                }
            }
        }

        if (foundLoadBeforeStore) {
            UseBlocksToPropagate.push_back(BB);
        }
    }

    // Seed blocks are live-in and propagate through predecessors even when
    // they also define the alloca.  A DefBlock reached later through an edge
    // remains a barrier, matching the original two-phase seed/BFS behavior.
    std::set<BasicBlock *> SeedBlocks(
        UseBlocksToPropagate.begin(), UseBlocksToPropagate.end());
    forEachReachablePreOrderCustomSucc<BasicBlock>(
        UseBlocksToPropagate,
        [&PredMap](BasicBlock *BB) {
            auto It = PredMap.find(BB);
            return It == PredMap.end() ? std::vector<BasicBlock *>{}
                                       : It->second;
        },
        [&DefBlocks, &LiveInBlocks,
         &SeedBlocks](BasicBlock *BB) {
            if (LiveInBlocks.count(BB))
                return TraversalControl::SkipSuccessors;
            if (!SeedBlocks.count(BB) && DefBlocks.count(BB))
                return TraversalControl::SkipSuccessors;

            LiveInBlocks.insert(BB);
            return TraversalControl::Continue;
        });
}

bool PromotePass::simplifyPhiNode(PhiInst* Phi) {
    Value* singleVal = nullptr;

    bool AllSame = forEachPhiIncoming(
        Phi, [&singleVal, Phi](unsigned, Value *V, BasicBlock *) {
            // 只跳过 phi 自引用（循环回边），不跳过 undef
            if (V == Phi) return true;
            if (singleVal == nullptr) {
                singleVal = V;
                return true;
            }
            return singleVal == V;
        });
    if (!AllSame) return false;

    if (singleVal == nullptr) {
        // 所有 incoming 都是自引用（不应发生），替换为 undef
        singleVal = UndefValue::NewUndefValue(Phi->getIRType());
    }

    Phi->replaceAllUsesWith(singleVal);
    delete Phi;
    return true;
}
