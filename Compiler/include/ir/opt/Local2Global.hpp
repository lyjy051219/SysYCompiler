#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstddef>

// 流程：1. 排除递归 main；2. 从入口 alloca 中筛选不跨计时屏障的大对象；
// 3. 创建同型全局、替换全部 use 并删除栈分配。
class Local2GlobalPass : public PassDescriptor<Local2GlobalPass> {
public:
	PreservationStatus run(Module &M, ModuleAnalysisPassManager &MAM);

private:
	static constexpr size_t RISCVThreshold = 1024;

	static bool isRecursiveFrom(
		Function *F, const FunctionCallGraph &CallGraph);
};
