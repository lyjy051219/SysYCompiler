#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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

	struct PointerRecipeStep {
		enum class Kind : unsigned char { GEP, BitCast } kind;
		std::vector<Value *> indices;
		std::shared_ptr<IRType> targetType;
	};

	struct SpecializationValue {
		Value *directValue = nullptr;
		Variable *pointerBase = nullptr;
		std::vector<PointerRecipeStep> pointerSteps;
		std::string key;
	};

	struct ParameterEquality {
		unsigned representativeIndex = 0;
		unsigned redundantIndex = 0;
	};

	struct SpecializationGroup {
		std::vector<std::pair<unsigned, SpecializationValue>> specializedArgs;
		std::vector<ParameterEquality> equalArguments;
		std::vector<CallInst *> calls;
	};

	static constexpr std::size_t MaxPointerRecipeDepth = 8;
	static constexpr std::size_t MaxSpecializationsPerFunction = 4;
	static constexpr std::size_t MaxSpecializableFunctionInstructions = 192;
	static constexpr std::size_t MaxClonedInstructionsPerFunction = 384;
	static constexpr std::size_t MaxSingleCallCloneInstructions = 48;
	static constexpr std::size_t MaxSingleCallEqualityCloneInstructions = 96;
	//DeadReturn值流遍历同时限制节点数和链深，超限时保守放弃。
	class DeadReturnTraversalBudget final {
	public:
		static constexpr std::size_t MaxNodes = 4096;
		static constexpr unsigned MaxDepth = 128;

		bool tryVisit(unsigned Depth);

	private:
		std::size_t VisitedNodes = 0;
	};

	static const std::vector<bool> *getLiveMask(
		const ParamMaskMap &Masks, Function *F);
	static bool collectPointerRecipe(
		Value *V, SpecializationValue &Result, unsigned Depth = 0);
	static std::optional<SpecializationValue> describeSpecializationValue(
		Value *Argument, Value *Parameter);
	static Value *materializeSpecializationValue(
		const SpecializationValue &Spec, BasicBlock *InsertionBlock);
	static void moveMaterializedValuesToEntry(
		BasicBlock *Staging, BasicBlock *Entry);
	static bool hasFullyReachableBody(Function &F);
	static std::size_t countInstructions(Function &F);
	static std::string makeUniqueSpecializedName(
		Module &M, Function &F, std::size_t &Ordinal);
	static std::string makeGroupKey(
		const std::vector<std::pair<unsigned, SpecializationValue>> &Args,
		const std::vector<ParameterEquality> &EqualArguments);
	static std::unique_ptr<Function> cloneSpecializedFunction(
		Function &Source, const std::string &Name,
		const std::vector<std::pair<unsigned, SpecializationValue>>
			&Specialized,
		const std::vector<ParameterEquality> &EqualArguments);
	static bool tryConstantSpecialization(
		Function *F, const std::vector<CallInst *> &CallSites);
	static bool tryGroupedSpecialization(Module &M, Function *F);
	static bool tryRedundantArgMerge(
		Function *F, const std::vector<CallInst *> &CallSites);
	bool eliminateDeadArguments(Module &M);
	// 当全部直接调用均忽略结果时，事务式把函数、call与ret统一改成void。
	bool eliminateDeadReturns(Module &M);
	static bool collectDeadReturnFlow(
		Value *Root, Function *Owner,
		std::unordered_set<Instruction *> &DeadFlow,
		std::unordered_set<Value *> &Visiting);
	static std::unique_ptr<Function> cloneFunctionWithVoidReturn(
		Function &Source,
		const std::unordered_set<Instruction *> &DeadReturnFlow);
	static bool rewriteFunctionWithDeadReturn(
		Module &M, std::size_t FunctionIndex);

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
