#include "ir/opt/Mem2Reg.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "lib/IRUtils.hpp"
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
    // 1.1 仅提升非数组标量，且 alloca 的每个 use 必须是类型精确匹配的直接 load/store；
    // 地址被存储或交给其他指令都意味着逃逸。
    auto pointeeType =
        IRType::getPointeeType(AI ? AI->getIRType() : nullptr);
    if (!pointeeType) return false;

    if (pointeeType->isArray() || pointeeType->isVoid()) {
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

            if (dynamic_cast<AllocaInst*>(storedVal)) {
                return false;
            }

            if (storedVal->getIRType() != pointeeType) {
                return false;
            }
            continue;
        }
        else return false;
    }
    return true;
}

bool PromotePass::areAllocaUsersReachable(const AllocaInst* AI,
                                          DominatorTree& DT) {
    // 1.2 所有访问块都必须存在于入口支配树；不可达 use 无法参与统一重命名。
    if (!AI) return false;

    for (Use *UseEdge : AI->getUses()) {
        auto *UserInst =
            dynamic_cast<Instruction *>(UseEdge ? UseEdge->getUser() : nullptr);
        if (!UserInst || !UserInst->getParent() ||
            !DT.getNode(UserInst->getParent())) {
            return false;
        }
    }
    return true;
}

void AllocaInfo::AnalyzeAlloca(AllocaInst* AI) {
    // 2. 汇总定义块、使用块、唯一 store 与是否仅在一个块使用，为快速路径分类。
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
    // 2.1 唯一 store 快速路径：store 值必须在每个 load 处可用；同块按指令序检查，
    // 跨块用支配关系检查。先验证全部 load，避免只替换一部分后失败。
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
    // 2.2 单块快速路径：按缓存的指令序排序 store，每个 load 取最近前置 store；
    // 无 store 时读 undef，有 store 却在首个 store 前读取则退回通用 SSA 构造。
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

bool PromotePass::promoteMemoryToRegister(Function& F, DominatorTree& DT){
    // 1. 每轮重新收集入口 alloca，过滤不可提升和不可达对象；timer 不访问程序内存，
    // 不阻止 SSA 表示提升。循环执行让本轮删除和 PHI 化简暴露的新快速路径继续处理。
    bool changed = false;
    BasicBlock* EntryBB = F.front();
    if (!EntryBB) return false;

    while (true) {
    std::vector<AllocaInst*> Allocas;
    AllocaInfo info;
    BlockInfo blockInfo;
    bool roundChanged = false;

    for (AllocaInst *AI : collectInstructions<AllocaInst>(*EntryBB)) {

        if (isAllocaPromotable(AI) && areAllocaUsersReachable(AI, DT))
            Allocas.push_back(AI);
    }

    if(Allocas.empty()) {

        return changed;
    }

    // 2. 先删除无 use alloca，并尝试唯一 store、单块两条无需 PHI 的快速路径。
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

    // 3. 为剩余对象一次性建立前驱表，供活跃性计算共享；pruned IDF
    // 直接复用 DominatorTree 的通用迭代支配边界接口。
    std::unordered_map<BasicBlock*, std::vector<BasicBlock*>> PredMap;
    for (auto bb : F) {
        for (BasicBlock* succ : bb->getSuccessors()) {
            PredMap[succ].push_back(bb);
        }
    }

    std::map<AllocaInst*, unsigned> AllocaIndex;
    for (unsigned i = 0; i < Allocas.size(); ++i)
        AllocaIndex[Allocas[i]] = i;

    std::vector<std::set<BasicBlock*>> DefBlocks(Allocas.size());
    // 3.1 分别收集每个对象的 store 定义块，反向求 live-in，再在 pruned IDF 中插入 PHI。
    for (unsigned i = 0; i < Allocas.size(); ++i) {
        for (Use *use : Allocas[i]->getUses()) {
            if (StoreInst* SI = dynamic_cast<StoreInst*>(use->getUser()))
                DefBlocks[i].insert(SI->getParent());
        }
    }

    std::map<BasicBlock*, std::map<unsigned, PhiInst*>> PhiNodes;

    for (unsigned i = 0; i < Allocas.size(); ++i) {
        AllocaInst* AI = Allocas[i];
        std::shared_ptr<IRType> allocType =
            IRType::getPointeeType(AI->getIRType());

        std::set<BasicBlock*> LiveInBlocks;
        computeLiveInBlocks(AI, DefBlocks[i], LiveInBlocks,PredMap);

        std::set<BasicBlock*> phiBlocks =
            DT.getIteratedDominanceFrontier(
                DefBlocks[i], &LiveInBlocks);

        for (BasicBlock* Y : phiBlocks) {
            PhiInst* phi = new PhiInst(allocType);
            Y->pushFront(phi);
            PhiNodes[Y][i] = phi;
        }
    }

    // 4. 为每个 alloca 建当前 SSA 值栈，以 undef 作为入口初值；随后显式 DFS 支配树
    // 完成重命名，frame 记录本块压栈次数以便离开子树时精确恢复。
    unsigned numAllocas = Allocas.size();
    std::vector<std::stack<Value*>> IncomingVals(numAllocas);

    for(unsigned i = 0; i < numAllocas; ++i){
        auto AllocType =
            IRType::getPointeeType(Allocas[i]->getIRType());
        IncomingVals[i].push(UndefValue::NewUndefValue(AllocType));
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

            // 4.1 进入块先压入该块新 PHI，再顺序把 load 替换为栈顶、把 store 值压栈，
            // 最后为每个 CFG 后继的 PHI 填入当前出块值。
            auto phiIt = PhiNodes.find(BB);
            if(phiIt != PhiNodes.end()){
                for(auto& [allocIdx, phi] : phiIt->second){
                    IncomingVals[allocIdx].push(phi);
                    frame.PushCounts[allocIdx]++;
                }
            }

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

        // 4.2 子节点处理完毕后按 PushCounts 回退各变量值栈，保持兄弟支配子树隔离。
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

    // 5. 删除已无访问的 alloca；若异常残留 use 则保守保留对象。
    for (AllocaInst* AI : Allocas) {

        if (AI->hasUses()) continue;
        blockInfo.deleteIndex(AI);
        delete AI;
    }
    roundChanged = true;

    // 6. 反复折叠自引用以外 incoming 全相同的 PHI，直到本轮 SSA 图不再退化。
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
    // 1. 获取支配树并执行快速路径、pruned PHI 放置和 SSA 重命名完整流程。
    auto &dominant = FAM.getResult<DominantAnalysis>(F);

    if(!promoteMemoryToRegister(F, dominant)) {
        return PassResult::unchanged();
    }

    // 7. CFG 未改，结构分析保持；内存对象转为 SSA 值后 AA 与副作用摘要失效。
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
    // 3.1 以可能在本块首个定义前读取的块为种子反向传播；传播遇到其他定义块停止，
    // 得到真正需要 incoming 值的区域，避免在完整 DF 上过量插 PHI。
    std::vector<BasicBlock*> UseBlocksToPropagate;

    for (Use *use : AI->getUses()) {
        LoadInst* LI = dynamic_cast<LoadInst*>(use->getUser());
        if (!LI) continue;

        BasicBlock* BB = LI->getParent();

        if (!DefBlocks.count(BB)) {
            UseBlocksToPropagate.push_back(BB);
            continue;
        }

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
    // 6.1 忽略 PHI 自引用后要求所有 incoming 相同；没有其他值时退化为同型 undef。
    Value* singleVal = nullptr;

    bool AllSame = forEachPhiIncoming(
        Phi, [&singleVal, Phi](unsigned, Value *V, BasicBlock *) {

            if (V == Phi) return true;
            if (singleVal == nullptr) {
                singleVal = V;
                return true;
            }
            return singleVal == V;
        });
    if (!AllSame) return false;

    if (singleVal == nullptr) {

        singleVal = UndefValue::NewUndefValue(Phi->getIRType());
    }

    Phi->replaceAllUsesWith(singleVal);
    delete Phi;
    return true;
}
