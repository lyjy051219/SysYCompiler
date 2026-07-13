#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

// 死代码消除 (Dead Code Elimination)
//函数内级联删除死代码
class DCEPass : public PassDescriptor<DCEPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);
private:
	static bool isDeletableInstruction(
		Instruction *I, FunctionAnalysisPassManager &FAM);
};

// Reusable helper: classify whether a call has observable side effects.
bool dceCallHasSideEffect(CallInst* CI, FunctionAnalysisPassManager& FAM);

// Return true only when I is an integer shift whose constant amount is in the
// half-open range [0, bitwidth(lhs)).  A dynamic, ill-typed or out-of-range
// shift is deliberately not classified as safe to speculate/remove.
//
// This legality predicate is shared by DCE's whole-function scan,
// IfConversion and LICM so they cannot silently disagree for non-i32 values.
bool hasDefinedConstantShiftAmount(const Instruction *I);

// Instructions whose mere evaluation may fault or rely on an undefined input
// must remain live even when their SSA result is unused.  Shared with ADCE so
// the aggressive sweep cannot undo DCE's legality policy.
bool dceInstructionMustBePreserved(Instruction *I);
