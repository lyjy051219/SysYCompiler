#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include "lib/IRUtils.hpp"

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

class ReassociateEngine;

// 对整数可重结合表达式执行确定性的代数规范化：
// 1. 为参数、基本块和非重结合定义建立稳定秩序；
// 2. 从表达式根展开单用途同操作树，折叠常量并做重复项合并、相反项消去、
//    公因式提取和重复乘因子压缩；
// 3. 按秩排序后在原根之前重建表达式，并用待办队列处理新产生的机会；
// 4. 迭代至不动点，仅使依赖 SSA 形状的分析失效。
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

// 函数级求解状态将排名、稳定次序和重访队列贯穿全部局部改写，保证多轮
// 重建得到同一个规范形状，不因对象地址或遍历偶然性发生振荡。
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
	UniqueWorkList<
		Instruction *, WorkListOrder::LIFO> Todo;

	// 服务步骤 1：StableOrder 提供确定的全序，Ranks 让常量和高延迟定义位于
	// 重建树的预定层次；新指令通过 stampRank 延续同一排序规则。
	void initRankMap();
	uint32_t getRank(Value *V) const;
	bool rankComesBefore(Value *A, Value *B) const;
	void stampRank(Instruction *I);
	void pushTodo(Instruction *I);
	bool processTodo();
	bool feedsAddressComputation(Instruction *Root) const;
	// 服务步骤 3：所有新二元指令都插在旧根之前，并立即登记排名和待办项，
	// 因而旧定义支配关系与计时分段保持不变。
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
	// 服务步骤 2：在已展平的同操作数列上执行代数化简；直接得到替换值时
	// 返回该值，否则原地收缩并规范化 Ops，供主流程重建。
	Value *optExp(Instruction *Root, BinaryInst::Operation Op,
		std::vector<Value *> &Ops, bool &Changed);
	bool optimizeInst(Instruction *I);
};
