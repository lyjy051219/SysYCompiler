#include "ir/opt/LateDeadGlobalCleanup.hpp"

#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/analysis/SideEffect.hpp"

#include <unordered_set>
#include <vector>

namespace {

static bool isScalarGlobal(Variable *G) {
    if (!G || G->vartag != Variable::GlobalVariable) return false;
    auto PtrTy = std::dynamic_pointer_cast<POINTERType>(G->getIRType());
    if (!PtrTy) return false;
    auto ElemTy = PtrTy->getPointerType();
    if (!ElemTy) return false;
    return ElemTy->getEnum() == IR_INT || ElemTy->getEnum() == IR_FLOAT;
}

static bool getDirectStoreToGlobal(Use *U, Variable *G, StoreInst *&SI) {
    SI = nullptr;
    if (!U || !G || U->getValue() != G) return false;
    auto *Store = dynamic_cast<StoreInst *>(U->getUser());
    if (!Store) return false;

    auto &UL = Store->getUserList();
    if (UL.size() < 2 || !UL[1]) return false;
    if (UL[1]->getValue() != G) return false;

    SI = Store;
    return true;
}

static bool collectDeadStoresForGlobal(Variable *G,
                                       std::vector<StoreInst *> &Stores) {
    if (!isScalarGlobal(G)) return false;
    if (G->getUseList().is_empty()) return false;

    std::vector<StoreInst *> LocalStores;
    for (auto It = G->getUseList().begin(); It != G->getUseList().end(); ++It) {
        StoreInst *SI = nullptr;
        if (!getDirectStoreToGlobal(*It, G, SI)) return false;
        LocalStores.push_back(SI);
    }

    if (LocalStores.empty()) return false;
    Stores.insert(Stores.end(), LocalStores.begin(), LocalStores.end());
    return true;
}

} // namespace

PreservationStatus LateDeadGlobalCleanupPass::run(
    Module &M, ModuleAnalysisPassManager &MAM) {
    (void)MAM;

    std::vector<StoreInst *> StoresToDelete;
    std::unordered_set<Variable *> CandidateGlobals;
    bool HasUnusedGlobal = false;

    for (auto &GPtr : M.getGlobalVar()) {
        Variable *G = GPtr.get();
        if (G && G->getUseList().is_empty()) {
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
        if (G && G->getUseList().is_empty()) {
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
