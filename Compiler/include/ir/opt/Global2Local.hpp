#pragma once

#include "ir/opt/NewPassManager.hpp"

#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Global2LocalPass : public PassDescriptor<Global2LocalPass> {
public:
	explicit Global2LocalPass(bool RelaxScalarUses = false)
		: RelaxScalarUses(RelaxScalarUses) {}

	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
	static constexpr size_t RISCVThreshold = 32;
	bool RelaxScalarUses = false;

	static bool isMemcpyLikeCall(CallInst *CI);
	static bool isMemcpyArgUse(Use *U);
	static std::unordered_map<Function *, std::vector<Function *>> buildCallGraph(Module &M);
	static std::unordered_set<Function *> computeReachableFrom(
		Function *Entry,
		const std::unordered_map<Function *, std::vector<Function *>> &CallGraph);
};
