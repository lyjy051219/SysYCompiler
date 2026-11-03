// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "lib/CFG.hpp"
#include <unordered_map>
#include <vector>

//函数内联：将Callee的函数体拼接到Caller的相应位置

//被调函数收益模型
struct CalleeInlineProfile {
    //是否有函数体
    bool HasBody = false;
    //是否调用其他函数
    bool HasOtherCalls = false;
    //是否包含需要为每层调用保留独立身份的栈对象。
    bool HasStackAllocation = false;
    //该函数副作用是否是NoModRef，并且没有Builtin函数
    bool SideEffectFreeLocalOnly = false;
    //标记递归函数是否更适合由记忆化Pass处理。
    bool SafeToMemoize = false;
    //按当前目标静态指令单位估算的函数总代价。
    int TotalCodeSizeCost = 0;
    //全部返回指令对应的静态代价，用于得到可克隆函数体成本。
    int ReturnCodeSizeCost = 0;
    //基本块数量，用于估算内联后新增的控制流开销
    int BasicBlockCount = 0;
    //原始IR指令数量，用于约束目标代价为零的指令造成的代码膨胀
    int InstructionCount = 0;
    //自递归调用点数
    int SelfRecursiveCallSites = 0;
    //每个形参在函数体中的实际使用次数
    std::vector<int> ParameterUseCounts;
};

//线性自递归每个显式展开阶段使用的独立代码增长预算
struct LinearRecursiveInlineBudget {
	int MaxBodyCodeSizeCost = 100;
	int MaxBasicBlocks = 16;
	int MaxInstructions = 128;
};
 
class FuncInlinePass : public PassDescriptor<FuncInlinePass> {
public:
	explicit FuncInlinePass(bool enableRecursiveInline = true,
		bool lateCleanupMode = false);
	explicit FuncInlinePass(LinearRecursiveInlineBudget budget);
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
	//计算Callee的收益模型
	static CalleeInlineProfile analyzeCallee(
		Function *Callee, FunctionAnalysisPassManager &FAM);
	//判断一次自递归展开是否满足独立的线性、纯标量和规模门禁
	static bool isLinearRecursiveExpansionCandidate(
		Function &Caller, CallInst *CI, Function *Callee,
		int BodyCost, const CalleeInlineProfile &Profile,
		const LinearRecursiveInlineBudget &Budget,
		FunctionAnalysisPassManager &FAM);
	//判断Callee是否符合收益模型
	static bool isInlineSite(
		Function &Caller, CallInst *CI, Function *Callee,
		const std::unordered_map<Function *, int> &CalleeFrequency,
		bool EnableRecursiveInline, bool LateCleanupMode,
		bool LinearRecursiveOnly, bool HotLoopCall,
		bool CalleeIsRecursive,
		const LinearRecursiveInlineBudget &LinearBudget,
		FunctionAnalysisPassManager &FAM,
		CalleeInlineProfile &OutProfile);
	//是否允许递归内联
	bool EnableRecursiveInline = true;
	//是否使用晚期低增长收益模型
	bool LateCleanupMode = false;
	//是否只处理满足严格门禁的线性自递归调用点
	bool LinearRecursiveOnly = false;
	//当前显式线性递归展开阶段的代码增长预算
	LinearRecursiveInlineBudget LinearBudget;
};
