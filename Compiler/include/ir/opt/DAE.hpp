#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <unordered_map>
#include <unordered_set>
#include <vector>

// 死参数消除 (Dead Argument Elimination)
// 跨函数分析参数是否仅被“转发调用”使用，并进行函数签名收缩重写。
class DAEPass : public PassDescriptor<DAEPass> {
public:
	PreservationStatus run(Module &M, ModuleAnalysisPassManager &MAM);

private:
	enum class ParamLiveness : unsigned char {
		Unknown,
		InProgress,
		Dead,
		Live,
	};

	using ParamMaskMap = std::unordered_map<Function *, std::vector<bool>>;

	static bool isExternelFunc(Function *F);
	static bool isDirectCallOperand(CallInst *CI, const Use *U);
	static Function *getDirectCallee(CallInst *CI);
	static const std::vector<bool> *getLiveMask(const ParamMaskMap &Masks, Function *F);

	bool isParamLive(
		Function *Owner,
		Value *Param,
		std::unordered_map<Value *, ParamLiveness> &Memo,
		std::unordered_set<Value *> &Visiting,
		const ParamMaskMap &ExistingMasks);

	bool rewriteFunctionWithDeadArgs(Module &M,
									 Function *OldF,
									 const std::vector<bool> &LiveMask);
};
