#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

// Late, direct-use-only cleanup for globals that became store-only after
// inlining/constant folding. This intentionally does not do alias or call
// analysis: a global is removed only when every remaining use is a direct
// `store value, @G` address use.
class LateDeadGlobalCleanupPass
    : public PassDescriptor<LateDeadGlobalCleanupPass> {
public:
    PreservationStatus run(Module &M, ModuleAnalysisPassManager &MAM);

private:
    static bool collectDeadStoresForGlobal(
        Variable *G, std::vector<StoreInst *> &Stores);
};
