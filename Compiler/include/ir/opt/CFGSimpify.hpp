#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <unordered_map>
#include <unordered_set>
#include <vector>

// CFGSimplifyPass:
// 1) 消除死块
// 2) 分支终结指令常量折叠
// 3) 折叠已知分支条件
// 4) 消除重复 PHI
// 5）穿透布尔 PHI 节点的条件分支
// 6) 规范化布尔 PHI 节点
// 7) 在循环外小块上特化可证明的入边
// 8) 合并基本块
// 9) 简化条件分支
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

	//入边特化中的PHI替换项，Incoming是指定前驱边上的真实输入。
	struct EdgeSpecializationPhiInput {
		PhiInst *Phi = nullptr;
		Value *Incoming = nullptr;
	};

	//克隆块连到Taken后，需为其后继PHI追加一条翻译后的输入。
	struct EdgeSpecializationSuccessorInput {
		PhiInst *Phi = nullptr;
		Value *Incoming = nullptr;
	};

	//入边特化先保存全部可行性、收益和PHI修复信息，提交阶段不再做投机判断。
	struct EdgeSpecializationPlan {
		BasicBlock *Block = nullptr;
		BasicBlock *Predecessor = nullptr;
		BasicBlock *Taken = nullptr;
		BrInst *Branch = nullptr;
		unsigned CloneInstructionCount = 0;
		std::vector<EdgeSpecializationPhiInput> PhiInputs;
		std::vector<EdgeSpecializationSuccessorInput> SuccessorInputs;
		std::vector<Instruction *> DeadConditionChain;
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
	//检查指令是否属于入边特化允许的纯标量子集。
	static bool isEdgeSpecializableInstruction(
		Instruction *I, BrInst *Branch);
	//在指定前驱边上有界解释PHI、纯二元运算、Select和类型转换。
	static Value *evaluateValueOnIncomingEdge(
		Value *V, BasicBlock *Block,
		const std::unordered_map<PhiInst *, Value *> &PhiInputs,
		unsigned Depth, std::unordered_map<Value *, Value *> &Memo,
		std::unordered_set<Value *> &Visiting,
		unsigned &FoldedInstructionCount, bool &UsedIncomingPhi);
	//收集删除条件分支后必然变死的单用户纯标量依赖链。
	static void collectDeadConditionChain(
		Value *V, Instruction *ExpectedUser, BasicBlock *Block,
		std::unordered_set<Instruction *> &Visited,
		std::vector<Instruction *> &Result);
	//为一个循环外小块完整规划最有收益的一条可特化入边。
	static bool buildEdgeSpecializationPlan(
		Function &F, BasicBlock *Block,
		const std::unordered_set<BasicBlock *> &LoopBlocks,
		EdgeSpecializationPlan &Plan);
	//按已验证的计划克隆小块、固化PHI/分支并修复后继PHI。
	static bool commitEdgeSpecialization(
		Function &F, const EdgeSpecializationPlan &Plan);

	bool removeDeadBlocks(Function &F);
	bool foldTerminatorBranches(Function &F);
	bool eliminateDuplicatePhis(Function &F);
	bool normalizeBooleanPhis(Function &F);
	bool foldKnownBranchConditions(Function &F);
	bool threadBranchOnPhi(Function &F, BasicBlock *BB);
	bool specializeBranchesOnIncomingEdges(
		Function &F, const std::unordered_set<BasicBlock *> &LoopBlocks);
	bool mergeBlocks(Function &F);
	bool simplifyUncondBranch(Function &F, BasicBlock *BB);
	bool simplifyBranches(Function &F);
};
