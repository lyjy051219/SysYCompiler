#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

// 死函数消除（模块级 GlobalDCE 的函数子集）。
// 只保留从 main / 外部 ABI 根可达的函数，以及在可达代码中被直接调用或地址逃逸的函数。
// 未被标记的用户函数会在 sweep 阶段从 Module 函数表中删除。
class DeadFunctionEliminationPass : public PassDescriptor<DeadFunctionEliminationPass> {
public:
	explicit DeadFunctionEliminationPass(FunctionAnalysisPassManager &FAM) : FAM(FAM) {}

	PreservationStatus run(Module &M, ModuleAnalysisPassManager &MAM);

private:
	FunctionAnalysisPassManager &FAM;
};
