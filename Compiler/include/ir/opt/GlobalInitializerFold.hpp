#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

// Fold a full-object memcpy at the non-recursive main entry into the
// destination global's static initializer.  The matcher deliberately accepts
// only direct module globals and a side-effect-free entry prefix; it therefore
// never needs to guess about aliases, constructors, or path coverage.
class GlobalInitializerFoldPass
    : public PassDescriptor<GlobalInitializerFoldPass> {
public:
    PreservationStatus run(Module &M, ModuleAnalysisPassManager &MAM);

private:
    static bool isRecursiveMain(Module &M, Function *Main);
    static Variable *getWholeObjectGlobal(Value *Pointer);
    static bool isFullGlobalCopy(CallInst *CI, Variable *&Destination,
                                 Variable *&Source);
    static void copyInitializer(Variable *Destination, Variable *Source);
};
