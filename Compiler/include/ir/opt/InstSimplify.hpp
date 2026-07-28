#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

class BasicAAResult;
class DominatorTree;
class LoopInfo;

 
// 局部求解器通过去重工作表反复消费替换后暴露的用户；规则分为二元代数、
// compare/select 规范化与 PHI 跨臂合并，并显式保护浮点、undef、内存和计时语义。
class InstSimplifier {
public:
	InstSimplifier(DominatorTree *DT, Function *F,
		FunctionAnalysisPassManager *FAM, BasicAAResult *AA, LoopInfo *LI);

	bool run(Function &F);

private:
	 
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

 
 
 
 
// 流程：1. 获取支配/AA/循环信息；2. 初始化所有有用户指令；3. 生成等价值或新指令；
// 4. 替换、删除并重新排队用户，直至工作表清空。
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
