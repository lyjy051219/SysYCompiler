#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "lib/CFG.hpp"
#include <unordered_map>

//CalleeInlineProfile汇总被调函数的体积、调用形态和副作用特征。
struct CalleeInlineProfile {
    //标记被调函数是否具有可克隆的函数体。
    bool HasBody = false;
    //标记被调函数体中是否存在非自递归调用。
    bool HasOtherCalls = false;
    //标记函数副作用是否只涉及不会逃逸的局部对象。
    bool SideEffectFreeLocalOnly = false;
    //标记递归函数是否更适合由记忆化Pass处理。
    bool SafeToMemoize = false;
    //记录被调函数体的总指令数量。
    int TotalInstCount = 0;
    //记录被调函数中的直接自递归调用点数量。
    int SelfRecursiveCallSites = 0;
};

//对原始调用点执行受预算约束的函数内联：
//1. 汇总被调函数调用频次，并分析函数体、递归形态和副作用；
//2. 拒绝计时边界、可记忆化递归及代码增长过大的候选；
//3. 绑定实参与形参后克隆被调 CFG，把返回值合并到调用后续块；
//4. 用克隆入口和返回边替换原调用边界，并使受影响分析失效。
class FuncInlinePass : public PassDescriptor<FuncInlinePass> {
public:
	//构造内联Pass并选择是否允许受限递归内联。
	explicit FuncInlinePass(bool enableRecursiveInline = true);

	//筛选并内联当前调用者中的合法调用点。
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
	//扫描被调函数并汇总体积、调用形态、副作用和记忆化适合度。
	static CalleeInlineProfile analyzeCallee(
		Function *Callee, FunctionAnalysisPassManager &FAM);
	//综合语义边界、调用频次和代码增长预算判断调用点是否可内联。
	static bool isInlineSite(
		Function &Caller, CallInst *CI, Function *Callee,
		const std::unordered_map<Function *, int> &CalleeFrequency,
		bool EnableRecursiveInline, FunctionAnalysisPassManager &FAM,
		CalleeInlineProfile &OutProfile);
	//记录是否允许满足严格预算的递归调用点参与内联。
	bool EnableRecursiveInline = true;
};
