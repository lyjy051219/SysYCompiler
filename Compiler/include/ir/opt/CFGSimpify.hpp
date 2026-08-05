#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <unordered_map>
#include <vector>

// CFGSimplifyPass:
// 1) 消除死块
// 2) 分支终结指令常量折叠
// 3) 折叠已知分支条件
// 4) 消除重复 PHI
// 5）穿透布尔 PHI 节点的条件分支
// 6) 规范化布尔 PHI 节点
// 7) 合并基本块
// 8) 简化条件分支
class CFGSimplifyPass : public PassDescriptor<CFGSimplifyPass> {
public:
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
	using PredecessorMap =
		std::unordered_map<BasicBlock *, std::vector<BasicBlock *>>;

	// 分支穿透前先完整规划后继 PHI 的新输入，避免半途修改 CFG。
	struct BranchPhiRewrite {
		PhiInst *Phi = nullptr;
		std::vector<PhiInst::IncomingEdge> Incoming;
	};

	// CFG 局部匹配与前驱查询只服务于本 Pass。
	static PredecessorMap buildPredecessorMap(Function &F);
	static const std::vector<BasicBlock *> &predecessorsOf(
		BasicBlock *BB, const PredecessorMap &Preds);
	static bool matchBooleanCondition(Value *A, Value *B, bool &Negated);
	static BasicBlock *resolveArmIncomingPred(
		BasicBlock *Controller, BasicBlock *Arm, BasicBlock *Merge,
		const PredecessorMap &Preds);
	static std::vector<BasicBlock *> collectControllerCandidates(
		BasicBlock *IncomingPred, const PredecessorMap &Preds);

	bool removeDeadBlocks(Function &F);
	bool foldTerminatorBranches(Function &F);
	bool eliminateDuplicatePhis(Function &F);
	bool normalizeBooleanPhis(Function &F);
	bool foldKnownBranchConditions(Function &F);
	bool threadBranchOnPhi(Function &F, BasicBlock *BB);
	bool mergeBlocks(Function &F);
	bool simplifyUncondBranch(Function &F, BasicBlock *BB);
	bool simplifyBranches(Function &F);
};
