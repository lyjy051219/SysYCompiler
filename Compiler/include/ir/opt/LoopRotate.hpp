#pragma once

#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include <unordered_map>
#include <unordered_set>
#include <vector>

// LoopRotatePass:
// 依赖 LoopSimplify + LCSSA 形态，尽量将 while 形态旋转为 do-while 形态。
class LoopRotatePass : public PassDescriptor<LoopRotatePass> {
public:
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
	bool runLoopRotate(Function &F, Loop &L, const DominatorTree &DT, const LoopInfo &LI);

	static bool isLCSSAPhi(PhiInst *Phi);
	static Instruction *cloneInstructionWithMap(
		Instruction *I,
		const std::unordered_map<Value *, Value *> &VMap);
};
