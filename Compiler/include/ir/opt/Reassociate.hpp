#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class ReassociateEngine;

// 代数重结合优化
class ReassociatePass : public PassDescriptor<ReassociatePass> {
public:
	explicit ReassociatePass(bool PreserveAddressArithmetic = false);
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
	friend class ReassociateEngine;
	bool PreserveAddressArithmetic = false;
	static bool isDeadInstruction(Instruction *I,
		FunctionAnalysisPassManager &FAM);
	static bool isAlreadyBuilt(Instruction *I, BinaryInst::Operation Op,
		const std::vector<Value *> &Operands);
};

// 重结合算法的函数级求解状态。类体放在头文件中，算法实现统一留在 cpp。
class ReassociateEngine {
public:
	ReassociateEngine(Function &F, FunctionAnalysisPassManager &FAM,
		bool PreserveAddressArithmetic);
	bool run();

private:
	Function &F;
	FunctionAnalysisPassManager &FAM;
	bool PreserveAddressArithmetic = false;
	std::unordered_map<Value *, uint32_t> Ranks;
	std::unordered_map<Value *, uint64_t> StableOrder;
	uint64_t NextStableOrder = 0;
	std::unordered_map<BasicBlock *, uint32_t> BlockBase;
	std::unordered_map<BasicBlock *, uint32_t> BlockSeq;
	std::vector<Instruction *> Todo;
	std::unordered_set<Instruction *> InTodo;

	void initRankMap();
	uint32_t getRank(Value *V) const;
	bool rankComesBefore(Value *A, Value *B) const;
	void stampRank(Instruction *I);
	void pushTodo(Instruction *I);
	bool processTodo();
	bool feedsAddressComputation(Instruction *Root) const;
	Instruction *createBinaryBefore(Instruction *Pos,
		BinaryInst::Operation Op, Value *LHS, Value *RHS);
	bool flattenMulFactors(Value *V, std::vector<Value *> &Factors);
	bool buildBalancedProduct(Instruction *Root,
		std::vector<Value *> Terms, Value *&Out);
	Value *buildMulPowProduct(
		Instruction *Root,
		const std::vector<std::pair<Value *, int>> &PowList,
		bool &Changed);
	bool matchMulByMinusOne(Value *V, Value *&X);
	bool buildAddFromTerms(Instruction *Root,
		std::vector<Value *> Terms, Value *&Out);
	Value *optExp(Instruction *Root, BinaryInst::Operation Op,
		std::vector<Value *> &Ops, bool &Changed);
	bool optimizeInst(Instruction *I);
};
