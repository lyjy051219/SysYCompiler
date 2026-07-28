#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "lib/CFG.hpp"
#include <unordered_map>

struct CalleeInlineProfile {
    bool HasBody = false;
    bool HasOtherCalls = false;
    bool SideEffectFreeLocalOnly = false;
    bool SafeToMemoize = false;
    int TotalInstCount = 0;
    int SelfRecursiveCallSites = 0;
};

// 对原始调用点执行受预算约束的函数内联：
// 1. 汇总被调函数调用频次，并分析函数体、递归形态和副作用；
// 2. 拒绝计时边界、可记忆化递归及代码增长过大的候选；
// 3. 绑定实参与形参后克隆被调 CFG，把返回值合并到调用后续块；
// 4. 用克隆入口和返回边替换原调用边界，并使受影响分析失效。
class FuncInlinePass : public PassDescriptor<FuncInlinePass> {
public:
	explicit FuncInlinePass(bool enableRecursiveInline = true);

	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
	static CalleeInlineProfile analyzeCallee(
		Function *Callee, FunctionAnalysisPassManager &FAM);
	static bool isInlineSite(
		Function &Caller, CallInst *CI, Function *Callee,
		const std::unordered_map<Function *, int> &CalleeFrequency,
		bool EnableRecursiveInline, FunctionAnalysisPassManager &FAM,
		CalleeInlineProfile &OutProfile);
	bool EnableRecursiveInline = true;
};
