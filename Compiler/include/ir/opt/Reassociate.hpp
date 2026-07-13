#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <vector>

class ReassociateEngine;

// 代数重结合优化
class ReassociatePass : public PassDescriptor<ReassociatePass> {
public:
	explicit ReassociatePass(bool PreserveAddressArithmetic = false)
		: PreserveAddressArithmetic(PreserveAddressArithmetic) {}
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
	friend class ReassociateEngine;
	bool PreserveAddressArithmetic = false;
	static bool isDeadInstruction(Instruction *I,
		FunctionAnalysisPassManager &FAM);
	static bool isAlreadyBuilt(Instruction *I, BinaryInst::Operation Op,
		const std::vector<Value *> &Operands);
};
