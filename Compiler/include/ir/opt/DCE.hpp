#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <unordered_map>

// 死代码消除 (Dead Code Elimination)
//函数内级联删除死代码
class DCEPass : public PassDescriptor<DCEPass> {
public:
    PassResult run(Function &F, FunctionAnalysisPassManager &FAM);
private:
	// 递归函数可删除性证明的三色状态。
	enum class ReturnProofState {
		Visiting,
		Proven,
		Failed,
	};

	static bool isGuaranteedRemovableFunction(
		Function &F, FunctionAnalysisPassManager &FAM,
		std::unordered_map<Function *, ReturnProofState> &Memo);
	static bool isDeletableInstruction(
		Instruction *I, FunctionAnalysisPassManager &FAM);

	// 公共调用副作用查询复用上述私有证明过程。
	friend bool dceCallHasSideEffect(
		CallInst *CI, FunctionAnalysisPassManager &FAM);
};

// Reusable helper: classify whether a call has observable side effects.
bool dceCallHasSideEffect(CallInst* CI, FunctionAnalysisPassManager& FAM);

// 共享的“无环且所有 CFG 路径都返回”结构证明。
// 该接口只回答控制流一定到达 ret，不隐含无副作用或无 fault；
// 调用方仍需分别检查指令与内存行为。
bool hasAcyclicReturningCFG(Function &F);

// Return true only when I is an integer shift whose constant amount is in the
// half-open range [0, bitwidth(lhs)).  A dynamic, ill-typed or out-of-range
// shift is deliberately not classified as safe to speculate or reuse as a
// defined-value proof.  Dead-code removal itself follows poison semantics.
//
// This legality predicate is shared by DCE's whole-function scan,
// IfConversion and LICM so they cannot silently disagree for non-i32 values.
bool hasDefinedConstantShiftAmount(const Instruction *I);

// Return true for context-independent observable execution not already
// classified by each caller (stores, calls and terminators are handled there).
// At present the IR has no volatile, atomic, or constrained arithmetic
// instruction, so ordinary loads and poison-producing arithmetic are
// deliberately not preserved.
bool dceInstructionMustBePreserved(Instruction *I);

// A separate predicate for motion/speculation.  Pure operations may be legal
// to delete when dead but illegal to move onto paths where they did not execute
// originally.  Keeping this separate prevents DCE policy from silently
// weakening LICM or peeling legality.
bool instructionMayFaultOrProducePoison(const Instruction *I);
