#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

// 死代码消除 (Dead Code Elimination)
//函数内级联删除死代码
class DCEPass : public PassDescriptor<DCEPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);
private:
	static bool isDeletableInstruction(
		Instruction *I, FunctionAnalysisPassManager &FAM);
};

// Reusable helper: classify whether a call has observable side effects.
bool dceCallHasSideEffect(CallInst* CI, FunctionAnalysisPassManager& FAM);
