#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

class DominatorTree;

// 流程：1. 汇总 PHI 的非自引用、非 undef incoming；2. 用支配关系验证公共值；
// 3. 交替删除无用户 PHI 与折叠退化 PHI，直到不动点。
class LatePhiCleanupPass : public PassDescriptor<LatePhiCleanupPass> {
public:
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
	static Value *safeReplacement(PhiInst *Phi, const DominatorTree &DT);
	static bool cleanupFunction(Function &F, DominatorTree &DT);
};
