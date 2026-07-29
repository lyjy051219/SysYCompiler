#pragma once

#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/MemorySSA.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// 同一值号可有多个定义记录，leader 查询再按支配、块内顺序与计时屏障筛选。
struct InstRecord {
	int Num = 0;
	Instruction *Inst = nullptr;
	BasicBlock *BB = nullptr;
};

// 全局表达式键由 opcode、精确结果类型与规范化操作数值号组成。
struct ExprKey {
	unsigned Opcode = 0;
	const IRType *ResultType = nullptr;
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

// PRE 的块出口可用项同时保存 leader 和其定义块。
struct AvailEntry {
	Instruction *Inst = nullptr;
	BasicBlock  *DefBB = nullptr;
};
using AvailSet = std::unordered_map<int, AvailEntry>;

// 对循环 PHI 和其纯表达式操作数做共递归等价判断，三态 memo 负责终止循环图。
class PhiCongruenceMatcher {
public:
	bool equivalent(Value *A, Value *B);

private:

	struct ValuePair {
		Value *A = nullptr;
		Value *B = nullptr;

		bool operator==(const ValuePair &Other) const;
	};

	struct ValuePairHash {
		std::size_t operator()(const ValuePair &P) const;
	};

	enum class State : unsigned char {
		Visiting,
		Equal,
		Different,
	};

	std::unordered_map<ValuePair, State, ValuePairHash> States;

	static ValuePair canonicalPair(Value *A, Value *B);
	static bool isSupportedPureInstruction(Instruction *I);
	bool equivalentPhis(PhiInst *A, PhiInst *B, unsigned Depth);
	bool equivalentImpl(Value *A, Value *B, unsigned Depth);
};

// 流程：1. 在规模预算内重建 RPO/值号；2. GVN 合并支配等价表达式、load 和只读调用；
// 3. PRE 建立 AvailOut 并经 PHI translate 定位恰缺一条前驱的表达式；
// 4. 在缺失边克隆并以新 PHI 合流；5. 两阶段分别有界迭代到不动点。
class GVNPRESolver {
public:
	GVNPRESolver(Function &F,
				 FunctionAnalysisPassManager &FAM,
				 DominatorTree &DT,
				 LoopInfo &LI,
				 BasicAAResult &AA,
				 MemorySSAResult &MSSA,
				 bool EnablePRE = true);

	bool run();

private:
	Function &F;
	FunctionAnalysisPassManager &FAM;
	DominatorTree &DT;
	LoopInfo &LI;
	BasicAAResult &AA;
	MemorySSAResult &MSSA;
	bool EnablePRE = true;

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

	std::unordered_map<BasicBlock *, AvailSet> AvailOut;

	struct MemoryLoadRecord {
		std::vector<MemoryAccess *> Clobbers;
		MemoryLocation Location;
		LoadInst *Load = nullptr;
		BasicBlock *Block = nullptr;
	};

	// 只读调用签名按每个实际读取位置记录全部路径上的首个潜在写。
	struct MemoryReadClobber {
		MemoryLocation Location;
		std::vector<MemoryAccess *> Clobbers;

		bool operator==(const MemoryReadClobber &Other) const;
	};

	struct MemoryCallRecord {
		CallInst *Call = nullptr;
		BasicBlock *Block = nullptr;
		std::vector<MemoryReadClobber> Signature;
	};

	std::vector<MemoryLoadRecord> AvailableMemoryLoads;
	std::vector<MemoryCallRecord> AvailableMemoryCalls;
	std::size_t MemorySSAQueries = 0;
	bool MemorySSAValid = true;

	static constexpr int kBackwardScanLimit = 64;
	static constexpr int kMaxFunctionInsts  = 1500;
	static constexpr int kMaxBlockInsts     = 400;
	static constexpr int kMaxGVNRounds      = 12;
	static constexpr int kMaxPRERounds      = 12;
	static constexpr std::size_t kMaxMemorySSAQueries = 512;
	static constexpr std::size_t kMaxMemoryLoadRecords = 512;
	static constexpr std::size_t kMaxMemoryLoadComparisons = 64;
	static constexpr std::size_t kMaxMemoryCallRecords = 256;
	static constexpr std::size_t kMaxMemoryCallComparisons = 64;

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

	Instruction *findAvailLeader(int Num, BasicBlock *PredBB) const;

	int allocateFreshNum(Value *V);
	int getOrAssignValueNum(Value *V, bool &IsFresh);
	bool buildExpressionKey(Instruction *I, ExprKey &K);

	ModRefInfo getCallModRef(CallInst *CI);
	bool isCallCSEEligible(CallInst *CI);
	std::vector<Value *> collectCallReadPtrs(CallInst *CI);
	static bool identicalCall(CallInst *A, CallInst *B);
	bool buildReadOnlyCallSignature(
		CallInst *CI, std::vector<MemoryReadClobber> &Signature);

	void markDeadBranchBlocks(BasicBlock *BrBB, BasicBlock *DeadSucc);
	void processBranchForDeadBlocks(Instruction *I);

	bool eliminateCongruentPhis();
	bool processLoad(Instruction *I);
	bool processCall(Instruction *I);
	bool runGVN();

	static bool isBackedge(BasicBlock *Pred, BasicBlock *BB, const DominatorTree &DT);
	bool shouldSkipPREInst(Instruction *I);

	int phiTranslate(int Num, BasicBlock *FromBB, BasicBlock *ToPred);

	Value *resolveAvailOperand(Value *OrigV, BasicBlock *SrcBB,
	                           BasicBlock *PredBB);

	void seedDeadBlocksToLeaderTable();
	bool runPRE();

};

// 函数 pass 负责取得支配、循环、AA、MemorySSA 并运行 GVNPRESolver，
// 可通过构造参数关闭 PRE、只执行全局值编号。
class GVNPREPass : public PassDescriptor<GVNPREPass> {
public:
	explicit GVNPREPass(bool EnablePRE = true);
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
	bool EnablePRE = true;
};

// 线性低成本层：每块独立按真实操作数建纯表达式键，顺序复用首个 leader；
// 计时调用清空本块表，不做跨块数据流或内存值编号。
class LocalGVNPass : public PassDescriptor<LocalGVNPass> {
public:
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:

	struct LocalExprKey {
		unsigned Opcode = 0;
		unsigned SubOpcode = 0;
		const IRType *ResultType = nullptr;
		std::vector<Value *> Operands;

		bool operator==(const LocalExprKey &Other) const;
	};

	struct LocalExprKeyHash {
		std::size_t operator()(const LocalExprKey &K) const;
	};

	static bool buildLocalExprKey(Instruction *I, LocalExprKey &K);
};
