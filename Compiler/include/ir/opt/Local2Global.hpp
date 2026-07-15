#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstddef>

// 将超大局部栈分配(alloca)提升为全局变量，避免超大栈对象。
class Local2GlobalPass : public PassDescriptor<Local2GlobalPass> {
public:
	PreservationStatus run(Module &M, ModuleAnalysisPassManager &MAM);

private:
	static constexpr size_t RISCVThreshold = 1024;

	// 递归入口的栈对象不能提升成所有栈帧共享的全局对象。
	static bool isRecursiveFrom(
		Function *F, const FunctionCallGraph &CallGraph);
};
