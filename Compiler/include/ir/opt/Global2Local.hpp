#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstddef>
#include <memory>
#include <vector>

struct Global2LocalCandidateInfo {
	Variable *GV = nullptr;
	std::shared_ptr<IRType> ElemTy;
	std::vector<Use *> MainUses;
};

class Global2LocalPass : public PassDescriptor<Global2LocalPass> {
public:
	explicit Global2LocalPass(bool RelaxScalarUses = false)
		: RelaxScalarUses(RelaxScalarUses) {}

	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
	static constexpr size_t RISCVThreshold = 32;
	bool RelaxScalarUses = false;

	static bool isSafeScalarUseInMain(Use *U, Variable *GV);
};
