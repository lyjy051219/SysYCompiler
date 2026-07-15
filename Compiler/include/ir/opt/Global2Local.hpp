#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstddef>
#include <memory>
#include <vector>

struct Global2LocalCandidateInfo {
	Variable *GV = nullptr;
	std::shared_ptr<IRType> ElemTy;
	std::vector<Use *> MainUses;
};

class Global2LocalPass : public PassDescriptor<Global2LocalPass> {
public:
	explicit Global2LocalPass(bool RelaxScalarUses = false);

	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
	static constexpr size_t RISCVThreshold = 32;
	bool RelaxScalarUses = false;

	// 递归入口不能把单实例全局对象改成逐栈帧局部对象。
	static bool isRecursiveFrom(
		Function *Root, const FunctionCallGraph &CallGraph);
	static bool isSafeScalarUseInMain(Use *U, Variable *GV);
};
