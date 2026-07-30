#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

class DominatorTree;

//流程：1. 汇总 PHI 的非自引用、非 undef incoming；2. 用支配关系验证公共值；
//3. 交替删除无用户 PHI 与折叠退化 PHI，直到不动点。
class LatePhiCleanupPass : public PassDescriptor<LatePhiCleanupPass> {
public:
	//迭代删除无用户PHI并折叠具有支配安全公共入值的退化PHI。
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
	//寻找可支配PHI全部外部使用的唯一有效公共入值。
	static Value *safeReplacement(PhiInst *Phi, const DominatorTree &DT);
	//交替执行死PHI删除和安全替换直到函数达到不动点。
	static bool cleanupFunction(Function &F, DominatorTree &DT);
};
