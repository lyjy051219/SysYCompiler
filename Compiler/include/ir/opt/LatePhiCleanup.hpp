#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

class DominatorTree;

// A very small late pass for PHI nodes created or simplified after the main
// InstSimplify pipeline has already run.  It intentionally avoids general
// algebraic folding so ConstantHoist materializations are left intact.
class LatePhiCleanupPass : public PassDescriptor<LatePhiCleanupPass> {
public:
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
	static Value *safeReplacement(PhiInst *Phi, const DominatorTree &DT);
	static bool cleanupFunction(Function &F, DominatorTree &DT);
};
