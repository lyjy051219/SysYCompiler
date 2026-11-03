// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#include "ir/opt/DeadGlobalCleanup.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/analysis/SideEffect.hpp"
#include <unordered_set>
#include <vector> 
//验证全局的每条use都是直接store地址并收集这些死写。
bool DeadGlobalCleanupPass::collectDeadStoresForGlobal(
    Variable *G, std::vector<StoreInst *> &Stores) {
    //1.1 候选全局量必须是仍有引用的标量，不处理数组
    if (!G || !G->isScalarGlobal()) return false;
    if (!G->hasUses()) return false;
    //1.2 遍历每一条Use，收集store，并且每条Use必须是store
    std::vector<StoreInst *> LocalStores;
    for (Use *U : G->getUses()) {
        if (!U || U->getValue() != G) return false;
        auto *SI = dynamic_cast<StoreInst *>(U->getUser());
        if (!SI || SI->getPointerOperand() != G) return false;
        LocalStores.push_back(SI);
    }
    if (LocalStores.empty()) return false;
    Stores.insert(Stores.end(), LocalStores.begin(), LocalStores.end());
    return true;
}
PreservationStatus DeadGlobalCleanupPass::run(Module &M, ModuleAnalysisPassManager &MAM) {
    (void)MAM;
    //可以删除的store集合
    std::vector<StoreInst *> StoresToDelete;
    //只store的全局变量
    std::unordered_set<Variable *> CandidateGlobals;
    bool HasUnusedGlobal = false;
    //1.遍历所有全局变量
    for (auto &GPtr : M.getGlobalVar()) {
        Variable *G = GPtr.get();
        if (G && !G->hasUses()) {
            HasUnusedGlobal = true;
        }
        const size_t Before = StoresToDelete.size();
        if (collectDeadStoresForGlobal(G, StoresToDelete)) {
            CandidateGlobals.insert(G);
        } else {
            StoresToDelete.resize(Before);
        }
    }
    if (StoresToDelete.empty() && CandidateGlobals.empty() &&
        !HasUnusedGlobal) {
        return PreservationStatus::all();
    }
    //2.删除store
    std::unordered_set<StoreInst *> SeenStores;
    bool Changed = false;
    for (StoreInst *SI : StoresToDelete) {
        if (!SI || !SeenStores.insert(SI).second) continue;
        delete SI;
        Changed = true;
    }
    //3.统一删除无Use的全局变量
    auto &Globals = M.getGlobalVar();
    for (auto It = Globals.begin(); It != Globals.end();) {
        Variable *G = It->get();
        if (G && !G->hasUses()) {
            It = Globals.erase(It);
            Changed = true;
            continue;
        }
        ++It;
    }
    if (!Changed) return PreservationStatus::all();
    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<PostDominantAnalysis>();
    PA.keep<LoopAnalysis>();
    PA.discard<BasicAliasAnalysis>();
    PA.discard<RangeAnalysis>();
    PA.discard<SideEffectAnalysis>();
    return PA;
}
