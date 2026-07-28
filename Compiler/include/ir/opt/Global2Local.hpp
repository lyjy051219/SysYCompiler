#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstddef>
#include <memory>
#include <vector>

// 候选保留全局对象、元素类型以及扫描阶段已验证的 main 内 use，提交阶段只改这些边。
struct Global2LocalCandidateInfo {
	Variable *GV = nullptr;
	std::shared_ptr<IRType> ElemTy;
	std::vector<Use *> MainUses;
};

// 流程：1. 仅在非递归 main 执行；2. 筛选小型、无跨函数可达引用且不跨计时屏障的全局；
// 3. 在入口创建 alloca 和标量 store/数组 memcpy 初始化；4. 重连 main use。
class Global2LocalPass : public PassDescriptor<Global2LocalPass> {
public:
	explicit Global2LocalPass(bool RelaxScalarUses = false);

	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
	static constexpr size_t RISCVThreshold = 32;
	bool RelaxScalarUses = false;

	 
	static bool isRecursiveFrom(
		Function *Root, const FunctionCallGraph &CallGraph);
	static bool isSafeScalarUseInMain(Use *U, Variable *GV);
};
