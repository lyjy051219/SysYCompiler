// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
 
// BreakCriticalEdgesPass:
// 将关键边 A->B（A 有多个后继且 B 有多个前驱）拆分为 A->N->B
//GVN-PRE的前置pass
class BreakCriticalEdgesPass : public PassDescriptor<BreakCriticalEdgesPass> {
private:
	static bool isCriticalEdge(Function &F, BasicBlock *From, BasicBlock *To);
	static bool splitCriticalEdge(Function &F, BasicBlock *From, BasicBlock *To);

public:
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);
};
