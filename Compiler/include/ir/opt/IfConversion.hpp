#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

// 完整保存 triangle/diamond 的控制块、两臂、汇合块及条件方向；只有 matchPattern
// 全量验证成功后，改写阶段才读取该结构。
struct IfConvMatch {
	BasicBlock *Pred = nullptr;
	BasicBlock *TrueBB = nullptr;
	BasicBlock *ElseBB = nullptr;
	BasicBlock *Succ = nullptr;
	BrInst *PredBr = nullptr;
	Value *Cond = nullptr;
	bool HasElse = false;
	bool CondTrueToTrueBB = false;
};

// 流程：1. 匹配单块 triangle/diamond；2. 验证推测安全、SSA 用户与成本预算；
// 3. 上提两臂计算；4. 以 select 替换汇合 PHI并删除臂块；5. 迭代到无候选。
class IfConversionPass : public PassDescriptor<IfConversionPass> {
public:
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
	static bool isSafeToSpeculativelyExecute(const Instruction *I);
	static int instructionCost(const Instruction *I);
	static int countCandidateInsts(BasicBlock *BB);
	static int candidateCost(BasicBlock *BB);
	static bool checkBlockConvertible(BasicBlock *BB);
	static bool usersAreSafeForHoist(BasicBlock *BB, BasicBlock *Succ);
	static bool hasExactPreds(BasicBlock *BB, BasicBlock *A, BasicBlock *B);
	static bool matchPattern(BasicBlock *TrueBB, IfConvMatch &M);
	bool tryConvertOne(Function &F, BasicBlock *TrueBB);
};
