#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

// CFGSimplifyPass:
// 1) 消除死块
// 2) 分支终结指令常量折叠
// 3) 消除重复 PHI
// 4) 基本块合并
// 5) 分支结构化简（无条件/条件）
class CFGSimplifyPass : public PassDescriptor<CFGSimplifyPass> {
public:
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
	bool removeDeadBlocks(Function &F);
	bool foldTerminatorBranches(Function &F);
	bool eliminateDuplicatePhis(Function &F);
	bool normalizeBooleanPhis(Function &F);
	bool foldKnownBranchConditions(Function &F);
	bool mergeBlocks(Function &F);
	bool simplifyUncondBranch(Function &F, BasicBlock *BB);
	bool simplifyCondBranch(Function &F, BasicBlock *BB);
	bool simplifyBranches(Function &F);
};
