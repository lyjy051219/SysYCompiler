#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

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

// 将简单 if/if-else 控制流改写为 select，减少分支跳转。
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
