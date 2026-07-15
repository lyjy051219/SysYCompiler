#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

class BasicAAResult;
class DominatorTree;
class LoopInfo;

// 单条指令化简器仅保存当前函数的分析上下文；算法实现统一留在 cpp。
class InstSimplifier {
public:
	InstSimplifier(DominatorTree *DT, Function *F,
		FunctionAnalysisPassManager *FAM, BasicAAResult *AA, LoopInfo *LI);

	bool run(Function &F);

private:
	// 整数 min/max 选择模式的匹配结果。
	enum class ExtremumKind {
		None,
		Minimum,
		Maximum,
	};

	DominatorTree *DT = nullptr;
	Function *F = nullptr;
	FunctionAnalysisPassManager *FAM = nullptr;
	BasicAAResult *AA = nullptr;
	LoopInfo *LI = nullptr;

	bool isLoadSuitableForSinking(LoadInst *L) const;
	Value *foldPhiOfBinary(PhiInst *Phi);
	Value *foldPhiOfGEP(PhiInst *Phi);
	Value *foldPhiOfLoad(PhiInst *Phi);
	Value *simplifyInstruction(Instruction *I, bool &Changed);
	static bool maybeSwapCommutative(
		BinaryInst *BI, Value *&LHS, Value *&RHS, bool &Changed);
	Value *simplifyAdd(
		BinaryInst *BI, Value *LHS, Value *RHS, bool &Changed);
	Value *simplifySub(BinaryInst *BI, Value *LHS, Value *RHS);
	Value *simplifyMul(
		BinaryInst *BI, Value *LHS, Value *RHS, bool &Changed);
	Value *simplifyDiv(BinaryInst *BI, Value *LHS, Value *RHS);
	Value *simplifyRem(BinaryInst *BI, Value *LHS, Value *RHS);
	Value *simplifyAnd(
		BinaryInst *BI, Value *LHS, Value *RHS, bool &Changed);
	Value *simplifyOr(
		BinaryInst *BI, Value *LHS, Value *RHS, bool &Changed);
	Value *simplifyXor(
		BinaryInst *BI, Value *LHS, Value *RHS, bool &Changed);
	Value *simplifyCmp(BinaryInst *BI, Value *LHS, Value *RHS);
	Value *simplifyBinary(BinaryInst *BI, bool &Changed);
	Value *simplifySelect(SelectInst *SI);
	Value *simplifyPhi(PhiInst *Phi);
};

// InstSimplifyPass:
// 1) 以工作队列方式在函数内做局部指令化简
// 2) 支持 Binary / Select / PHI 的常见等式和常量折叠
// 3) 通过 RAUW 传播化简结果，并级联触发其用户重试
class InstSimplifyPass : public PassDescriptor<InstSimplifyPass> {
public:
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
	friend class InstSimplifier;
	static bool getBinaryOperandsForOp(
		Value *V, BinaryInst::Operation Op, Value *&LHS, Value *&RHS);
	static bool matchNegation(Value *V, Value *&Operand);
	static bool matchSub(Value *V, Value *&LHS, Value *&RHS);
	static bool isNegationPair(Value *A, Value *B);
	static bool matchYMinusX(Value *MaybeSub, Value *X, Value *&Y);
};
