#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "lib/CFG.hpp"
#include <unordered_map>

//函数内联：将Callee的函数体拼接到Caller的相应位置

//被调函数收益模型
struct CalleeInlineProfile {
    //是否有函数体
    bool HasBody = false;
    //是否调用其他函数
    bool HasOtherCalls = false;
    //该函数副作用是否是NoModRef，并且没有Builtin函数
    bool SideEffectFreeLocalOnly = false;
    //标记递归函数是否更适合由记忆化Pass处理。
    bool SafeToMemoize = false;
    //总指令数
    int TotalInstCount = 0;
    //自递归调用点数
    int SelfRecursiveCallSites = 0;
};

class FuncInlinePass : public PassDescriptor<FuncInlinePass> {
public:
	explicit FuncInlinePass(bool enableRecursiveInline = true);
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
	//计算Callee的收益模型
	static CalleeInlineProfile analyzeCallee(
		Function *Callee, FunctionAnalysisPassManager &FAM);
	//判断Callee是否符合收益模型
	static bool isInlineSite(
		Function &Caller, CallInst *CI, Function *Callee,
		const std::unordered_map<Function *, int> &CalleeFrequency,
		bool EnableRecursiveInline, FunctionAnalysisPassManager &FAM,
		CalleeInlineProfile &OutProfile);
	//是否允许递归内联
	bool EnableRecursiveInline = true;
};
