#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

// 死函数消除：保留 main，以及从 main 通过 CallInst 可达的函数。
// 当前编译器生成的 IR 中，Function 只作为 CallInst 的直接被调目标出现，
// 因此无需处理间接调用或函数地址逃逸。
class DeadFunctionEliminationPass : public PassDescriptor<DeadFunctionEliminationPass> {
public:
	explicit DeadFunctionEliminationPass(FunctionAnalysisPassManager &FAM);

	PreservationStatus run(Module &M, ModuleAnalysisPassManager &MAM);

private:
	FunctionAnalysisPassManager &FAM;
};
