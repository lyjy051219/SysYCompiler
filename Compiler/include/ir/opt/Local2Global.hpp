#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstddef>

//流程：1. 排除递归 main；2. 从入口 alloca 中筛选大对象；
//3. 创建同型全局、替换全部 use 并删除栈分配。
//timer 不读取对象存储位置，因此不限制局部与全局表示转换。
class Local2GlobalPass : public PassDescriptor<Local2GlobalPass> {
public:
	//在非递归main中把大型入口alloca转换为同型全局对象。
	PreservationStatus run(Module &M, ModuleAnalysisPassManager &MAM);

private:
	//限定值得从栈迁移到全局区的最小对象字节数。
	static constexpr size_t RISCVThreshold = 1024;

	//沿直接调用图判断函数是否可能递归回到自身。
	static bool isRecursiveFrom(
		Function *F, const FunctionCallGraph &CallGraph);
};
