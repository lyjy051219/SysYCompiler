#pragma once

#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

struct InstRecord {
	int Num = 0;
	Instruction *Inst = nullptr;
	BasicBlock *BB = nullptr;
};

struct ExprKey {
	unsigned Opcode = 0;
	std::vector<int> Args;

	bool operator==(const ExprKey &RHS) const;
};

struct ExprKeyHash {
	std::size_t operator()(const ExprKey &K) const;
};

struct PhiTranslateKey {
	int Num = 0;
	BasicBlock *From = nullptr;
	BasicBlock *ToPred = nullptr;

	bool operator==(const PhiTranslateKey &RHS) const;
};

struct PhiTranslateKeyHash {
	std::size_t operator()(const PhiTranslateKey &K) const;
};

// Per-block set: ValueNum -> (Instruction* leader, BasicBlock* defBB)
// Used for AVAIL_OUT in PRE phase.
struct AvailEntry {
	Instruction *Inst = nullptr;
	BasicBlock  *DefBB = nullptr;
};
using AvailSet = std::unordered_map<int, AvailEntry>;

class GVNPRESolver {
public:
	GVNPRESolver(Function &F,
				 FunctionAnalysisPassManager &FAM,
				 DominatorTree &DT,
				 LoopInfo &LI,
				 BasicAAResult &AA);

	bool run();

private:
	Function &F;
	FunctionAnalysisPassManager &FAM;
	DominatorTree &DT;
	LoopInfo &LI;
	BasicAAResult &AA;

	int NextValueNum = 1;
	std::unordered_map<Value *, int> ValueNum;
	std::unordered_map<ExprKey, int, ExprKeyHash> ExpressionTable;
	std::unordered_map<int, std::vector<InstRecord>> LeaderTable;
	std::unordered_map<int, ExprKey> NumToExpr;
	std::unordered_map<int, Value *> NumToValue;
	std::unordered_set<BasicBlock *> DeadBlocks;
	std::unordered_set<BasicBlock *> ReachableBlocks;
	std::vector<BasicBlock *> RPO;
	std::unordered_map<PhiTranslateKey, int, PhiTranslateKeyHash> PhiTranslateTable;

	// AVAIL_OUT[BB]: num -> {inst, defBB}  (built top-down on domtree)
	std::unordered_map<BasicBlock *, AvailSet> AvailOut;

	static constexpr int kBackwardScanLimit = 64;
	static constexpr int kMaxFunctionInsts  = 1500;
	static constexpr int kMaxBlockInsts     = 400;
	static constexpr int kMaxGVNRounds      = 12;
	static constexpr int kMaxPRERounds      = 12;

	bool runGVNFixedPoint();
	bool runPREFixedPoint();
	bool isFunctionTooLargeForGVNPRE() const;

	void clearIterationState();
	void rebuildRPOAndReachability();
	void buildTablesForPRE();
	void buildAvailOut();

	static bool hasResultValue(Instruction *I);
	bool isDeadBlock(BasicBlock *BB) const;

	void addRecord(int Num, Instruction *I, BasicBlock *BB);
	void forgetInstruction(Instruction *I);
	bool isBeforeInSameBlock(Instruction *A, Instruction *B) const;
	Instruction *findLeader(int Num, BasicBlock *UseBB, Instruction *UseI) const;
	// PRE-specific: find leader using AVAIL_OUT (O(1), domtree-propagated)
	Instruction *findAvailLeader(int Num, BasicBlock *PredBB) const;

	int allocateFreshNum(Value *V);
	int getOrAssignValueNum(Value *V, bool &IsFresh);
	bool buildExpressionKey(Instruction *I, ExprKey &K);

	ModRefInfo getCallModRef(CallInst *CI);
	std::vector<Value *> collectCallReadPtrs(CallInst *CI);
	static bool identicalCall(CallInst *A, CallInst *B);

	void markDeadBranchBlocks(BasicBlock *BrBB, BasicBlock *DeadSucc);
	void processBranchForDeadBlocks(Instruction *I);

	bool processLoad(Instruction *I);
	bool processCall(Instruction *I);
	bool runGVN();

	static bool isBackedge(BasicBlock *Pred, BasicBlock *BB, const DominatorTree &DT);
	bool shouldSkipPREInst(Instruction *I);
	// Improved phiTranslate: returns translated ExprKey and resolves to existing num
	int phiTranslate(int Num, BasicBlock *FromBB, BasicBlock *ToPred);
	// Resolve translated operand value available in PredBB
	Value *resolveAvailOperand(Value *OrigV, BasicBlock *SrcBB,
	                           BasicBlock *PredBB);

	void seedDeadBlocksToLeaderTable();
	bool runPRE();

	// Domtree DFS helper: visits BB and all domtree children top-down
	void domTreeDFS(BasicBlock *BB,
	                std::function<void(BasicBlock *)> Visit);
};

// GVN + PRE 组合优化
// - GVN：同值冗余消除（包含局部 LOAD/CALL 前扫）
// - PRE：部分冗余消除（基于 PHITranslate + AVAIL_OUT 集合）
class GVNPREPass : public PassDescriptor<GVNPREPass> {
public:
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);
};
