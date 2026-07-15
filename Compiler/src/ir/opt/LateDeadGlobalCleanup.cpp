#include "ir/opt/LateDeadGlobalCleanup.hpp"

#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/analysis/SideEffect.hpp"

#include <unordered_set>
#include <vector>

bool LateDeadGlobalCleanupPass::collectDeadStoresForGlobal(
    Variable *G, std::vector<StoreInst *> &Stores) {
    if (!G || !G->isScalarGlobal()) return false;
    if (!G->hasUses()) return false;

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

PreservationStatus LateDeadGlobalCleanupPass::run(
    Module &M, ModuleAnalysisPassManager &MAM) {
    (void)MAM;

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

    std::unordered_set<StoreInst *> SeenStores;
    bool Changed = false;
    for (StoreInst *SI : StoresToDelete) {
        if (!SI || !SeenStores.insert(SI).second) continue;
        delete SI;
        Changed = true;
    }

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
