// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
class DominatorTree;

//晚期PHI函数清理：清除无Use的PHI，把所有incoming value相同的PHI退化成value
class LatePhiCleanupPass : public PassDescriptor<LatePhiCleanupPass> {
public:
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
	//判断PHI是否可以退化成某个Value
	static Value *safeReplacement(PhiInst *Phi, const DominatorTree &DT);
	//交替执行死PHI删除和安全替换直到函数达到不动点
	static bool cleanupFunction(Function &F, DominatorTree &DT);
};
 