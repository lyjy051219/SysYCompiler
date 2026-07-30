#include "ir/opt/LateDeadGlobalCleanup.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/analysis/SideEffect.hpp"
#include <unordered_set>
#include <vector>
//验证全局的每条use都是直接store地址并收集这些死写。
bool LateDeadGlobalCleanupPass::collectDeadStoresForGlobal(
    Variable *G, std::vector<StoreInst *> &Stores) {
    //1.1 候选全局量必须是仍有引用的标量；数组或已经无引用的对象交给最终清扫处理。
    if (!G || !G->isScalarGlobal()) return false;
    if (!G->hasUses()) return false;
    //1.2 只有“所有 use 都是直接写入该全局量”才能证明这些 store 永远不可观察。
    //先在局部容器中完成全量验证，避免失败时向总删除表写入半成品。
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
//删除只被直接写入的标量全局及模块中全部无使用全局对象。
PreservationStatus LateDeadGlobalCleanupPass::run(
    Module &M, ModuleAnalysisPassManager &MAM) {
    (void)MAM;
    //1.扫描全局表，同时收集纯写标量的 store，并记录原本就没有 use 的全局量。
    std::vector<StoreInst *> StoresToDelete;
    std::unordered_set<Variable *> CandidateGlobals;
    bool HasUnusedGlobal = false;
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
    //2.先删除已证明无读者的 store，使其指针 use 从对应全局量上消失。
    //去重集合防止异常 use 链让同一条指令进入删除表多次。
    std::unordered_set<StoreInst *> SeenStores;
    bool Changed = false;
    for (StoreInst *SI : StoresToDelete) {
        if (!SI || !SeenStores.insert(SI).second) continue;
        delete SI;
        Changed = true;
    }
    //3.再统一删除 use 已归零的全局量；这个顺序也覆盖被上一步刚刚释放的对象。
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
    //4.CFG、支配和循环结构没有变化；内存/范围/副作用结论会因全局写入消失而失效。
    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<PostDominantAnalysis>();
    PA.keep<LoopAnalysis>();
    PA.discard<BasicAliasAnalysis>();
    PA.discard<RangeAnalysis>();
    PA.discard<SideEffectAnalysis>();
    return PA;
}
