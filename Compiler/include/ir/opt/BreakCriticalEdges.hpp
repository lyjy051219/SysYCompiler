#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

// BreakCriticalEdgesPass:
// 将关键边 A->B（A 有多个后继且 B 有多个前驱）拆分为 A->N->B。
class BreakCriticalEdgesPass : public PassDescriptor<BreakCriticalEdgesPass> {
public:
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);
};
