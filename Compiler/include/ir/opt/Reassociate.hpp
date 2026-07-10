#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <vector>

class ReassociateEngine;

// 代数重结合优化
class ReassociatePass : public PassDescriptor<ReassociatePass> {
public:
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
	friend class ReassociateEngine;
	static bool isDeadInstruction(Instruction *I,
		FunctionAnalysisPassManager &FAM);
	static bool isAlreadyBuilt(Instruction *I, BinaryInst::Operation Op,
		const std::vector<Value *> &Operands);
};
