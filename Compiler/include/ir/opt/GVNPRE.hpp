// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

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

class PostDominatorTree;
 
//InstRecord保存同一值号对应的一条候选定义记录
struct InstRecord {
	//Num记录候选定义所属的全局值号
	int Num = 0;
	//Inst指向产生该值号的IR指令
	Instruction *Inst = nullptr;
	//BB指向候选定义所在的基本块
	BasicBlock *BB = nullptr;
};

//ExprKey使用操作码、结果类型和操作数值号标识全局表达式
struct ExprKey {
	//Opcode记录表达式对应的主操作码和子操作码编码
	unsigned Opcode = 0;
	//ResultType记录表达式结果的精确IR类型
	const IRType *ResultType = nullptr;
	//Args按照规范化顺序保存操作数值号
	std::vector<int> Args;

	//判断两个全局表达式键是否完全相同
	bool operator==(const ExprKey &RHS) const;
};

//ExprKeyHash为全局表达式键提供组合哈希
struct ExprKeyHash {
	//计算给定全局表达式键的哈希值
	std::size_t operator()(const ExprKey &K) const;
};

//PhiTranslateKey标识一个值号沿指定CFG入边的PHI翻译查询
struct PhiTranslateKey {
	//Num记录需要翻译的原始值号
	int Num = 0;
	//From记录表达式当前所在的汇合基本块
	BasicBlock *From = nullptr;
	//ToPred记录要翻译到的目标前驱基本块
	BasicBlock *ToPred = nullptr;

	//判断两个PHI翻译查询键是否完全相同
	bool operator==(const PhiTranslateKey &RHS) const;
};

//PhiTranslateKeyHash为PHI翻译查询键提供组合哈希
struct PhiTranslateKeyHash {
	//计算给定PHI翻译查询键的哈希值
	std::size_t operator()(const PhiTranslateKey &K) const;
};

//AvailEntry保存块出口可用值的代表指令及其定义块
struct AvailEntry {
	//Inst指向当前值号在块出口可用的代表指令
	Instruction *Inst = nullptr;
	//DefBB指向代表指令的定义基本块
	BasicBlock  *DefBB = nullptr;
};
//AvailSet把值号映射到块出口的可用定义
using AvailSet = std::unordered_map<int, AvailEntry>;

//PhiCongruenceMatcher共递归判断循环PHI及其纯表达式操作数是否等价
class PhiCongruenceMatcher {
public:
	//判断两个IR值是否在支持的纯表达式图中同余
	bool equivalent(Value *A, Value *B);

private:
	//ValuePair保存一次递归等价查询的两个IR值
	struct ValuePair {
		//A指向规范化后地址较小的第一个值
		Value *A = nullptr;
		//B指向规范化后地址较大的第二个值
		Value *B = nullptr;

		//判断两个规范化值对是否完全相同
		bool operator==(const ValuePair &Other) const;
	};

	//ValuePairHash为规范化IR值对提供组合哈希
	struct ValuePairHash {
		//计算给定规范化值对的哈希值
		std::size_t operator()(const ValuePair &P) const;
	};

	//State记录共递归等价查询的三态求解状态
	enum class State : unsigned char {
		//Visiting表示当前值对仍在递归栈中
		Visiting,
		//Equal表示当前值对已经证明等价
		Equal,
		//Different表示当前值对已经证明不等价
		Different,
	};

	//States缓存值对的三态结论以终止循环表达式图
	std::unordered_map<ValuePair, State, ValuePairHash> States;

	//按照稳定指针顺序规范化两个待比较值
	static ValuePair canonicalPair(Value *A, Value *B);
	//判断指令是否属于同余匹配支持的纯表达式集合
	static bool isSupportedPureInstruction(Instruction *I);
	//递归比较两个PHI的入边及对应操作数
	bool equivalentPhis(PhiInst *A, PhiInst *B, unsigned Depth);
	//递归比较两个IR值并维护三态缓存和深度预算
	bool equivalentImpl(Value *A, Value *B, unsigned Depth);
};

//GVNPRESolver在规模预算内执行全局值编号和部分冗余消除
class GVNPRESolver {
public:
	//构造绑定函数及全部依赖分析的GVN/PRE求解器
	GVNPRESolver(Function &F,
				 FunctionAnalysisPassManager &FAM,
				 DominatorTree &DT,
				 LoopInfo &LI,
				 BasicAAResult &AA,
				 MemorySSAResult &MSSA,
				 bool EnablePRE = true);

	//运行有界GVN固定点及可选PRE固定点并返回IR是否变化
	bool run();

private:
	//F引用当前正在优化的函数
	Function &F;
	//FAM引用当前函数的分析结果管理器
	FunctionAnalysisPassManager &FAM;
	//DT引用当前函数的支配树
	DominatorTree &DT;
	//LI引用当前函数的循环信息
	LoopInfo &LI;
	//AA引用当前函数的基础别名分析结果
	BasicAAResult &AA;
	//MSSA引用当前函数的MemorySSA结果
	MemorySSAResult &MSSA;
	//EnablePRE控制是否在GVN之后执行部分冗余消除
	bool EnablePRE = true;

	//NextValueNum记录下一个尚未分配的正值号
	int NextValueNum = 1;
	//ValueNum把IR值映射到当前迭代的值号
	std::unordered_map<Value *, int> ValueNum;
	//ExpressionTable把规范化表达式键映射到值号
	std::unordered_map<ExprKey, int, ExprKeyHash> ExpressionTable;
	//LeaderTable按值号保存全部候选代表定义
	std::unordered_map<int, std::vector<InstRecord>> LeaderTable;
	//NumToExpr保存每个表达式值号对应的规范化表达式键
	std::unordered_map<int, ExprKey> NumToExpr;
	//NumToValue保存非表达式值号对应的原始IR值
	std::unordered_map<int, Value *> NumToValue;
	//DeadBlocks记录常量分支证明不可达的基本块
	std::unordered_set<BasicBlock *> DeadBlocks;
	//ReachableBlocks记录从函数入口可达的基本块
	std::unordered_set<BasicBlock *> ReachableBlocks;
	//RPO按照反向后序保存当前可达CFG基本块
	std::vector<BasicBlock *> RPO;
	//PhiTranslateTable缓存值号沿CFG入边的PHI翻译结果
	std::unordered_map<PhiTranslateKey, int, PhiTranslateKeyHash> PhiTranslateTable;

	//AvailOut保存每个基本块出口处可用的值号及代表定义
	std::unordered_map<BasicBlock *, AvailSet> AvailOut;

	//MemoryLoadRecord保存一次可复用load及其内存版本签名
	struct MemoryLoadRecord {
		//Clobbers保存各条MemorySSA路径上的首个潜在写
		std::vector<MemoryAccess *> Clobbers;
		//Location保存load读取的精确内存位置
		MemoryLocation Location;
		//Load指向作为候选代表的读取指令
		LoadInst *Load = nullptr;
		//Block指向候选load所在的基本块
		BasicBlock *Block = nullptr;
	};

	//MemoryReadClobber保存只读调用单个读取位置的MemorySSA签名
	struct MemoryReadClobber {
		//Location保存调用可能读取的一个内存位置
		MemoryLocation Location;
		//Clobbers保存该位置各条路径上的首个潜在写
		std::vector<MemoryAccess *> Clobbers;

		//判断两个调用读取位置及其内存版本签名是否相同
		bool operator==(const MemoryReadClobber &Other) const;
	};

	//MemoryCallRecord保存一次可复用只读调用及其完整内存签名
	struct MemoryCallRecord {
		//Call指向作为候选代表的只读调用
		CallInst *Call = nullptr;
		//Block指向候选调用所在的基本块
		BasicBlock *Block = nullptr;
		//Signature按读取位置保存完整MemorySSA签名
		std::vector<MemoryReadClobber> Signature;
	};

	//AvailableMemoryLoads保存当前扫描中仍可复用的load记录
	std::vector<MemoryLoadRecord> AvailableMemoryLoads;
	//AvailableMemoryCalls保存当前扫描中仍可复用的只读调用记录
	std::vector<MemoryCallRecord> AvailableMemoryCalls;
	//MemorySSAQueries记录当前迭代已经执行的MemorySSA查询数
	std::size_t MemorySSAQueries = 0;
	//MemorySSAValid标记当前迭代是否仍允许依赖MemorySSA结论
	bool MemorySSAValid = true;

	//kBackwardScanLimit限制局部反向扫描的指令数量
	static constexpr int kBackwardScanLimit = 64;
	//kMaxFunctionInsts限制允许执行GVN/PRE的函数指令总数
	static constexpr int kMaxFunctionInsts  = 1500;
	//kMaxBlockInsts限制允许执行GVN/PRE的单块指令数
	static constexpr int kMaxBlockInsts     = 400;
	//kMaxGVNRounds限制GVN固定点迭代轮数
	static constexpr int kMaxGVNRounds      = 12;
	//kMaxPRERounds限制PRE固定点迭代轮数
	static constexpr int kMaxPRERounds      = 12;
	//kMaxMemorySSAQueries限制单轮MemorySSA查询数量
	static constexpr std::size_t kMaxMemorySSAQueries = 512;
	//kMaxMemoryLoadRecords限制缓存的可用load记录数量
	static constexpr std::size_t kMaxMemoryLoadRecords = 512;
	//kMaxMemoryLoadComparisons限制单次load查询比较的候选数量
	static constexpr std::size_t kMaxMemoryLoadComparisons = 64;
	//kMaxMemoryCallRecords限制缓存的可用只读调用记录数量
	static constexpr std::size_t kMaxMemoryCallRecords = 256;
	//kMaxMemoryCallComparisons限制单次调用查询比较的候选数量
	static constexpr std::size_t kMaxMemoryCallComparisons = 64;

	//有界迭代GVN直到不动点或达到轮数预算
	bool runGVNFixedPoint();
	//有界迭代PRE直到不动点或达到轮数预算
	bool runPREFixedPoint();
	//判断函数规模是否超过GVN/PRE编译时间预算
	bool isFunctionTooLargeForGVNPRE() const;

	//清空一轮值编号和内存可用性求解状态
	void clearIterationState();
	//重建可达基本块集合及其反向后序
	void rebuildRPOAndReachability();
	//在不改写IR的情况下重建PRE所需值号表
	void buildTablesForPRE();
	//沿RPO计算每个基本块出口的可用值集合
	void buildAvailOut();

	//判断指令是否产生可以参与值编号的SSA结果
	static bool hasResultValue(Instruction *I);
	//判断基本块是否已被证明不可达
	bool isDeadBlock(BasicBlock *BB) const;

	//为给定值号登记一条候选代表定义
	void addRecord(int Num, Instruction *I, BasicBlock *BB);
	//从值号表和代表表中遗忘已删除指令
	void forgetInstruction(Instruction *I);
	//判断同一基本块中的指令A是否位于指令B之前
	bool isBeforeInSameBlock(Instruction *A, Instruction *B) const;
	//查找在使用位置可支配的值号代表
	Instruction *findLeader(int Num, BasicBlock *UseBB, Instruction *UseI) const;

	//查找在指定前驱块出口可用的值号代表
	Instruction *findAvailLeader(int Num, BasicBlock *PredBB) const;

	//为给定IR值分配新的唯一值号
	int allocateFreshNum(Value *V);
	//返回值的现有值号或为其分配新值号
	int getOrAssignValueNum(Value *V, bool &IsFresh);
	//为支持的纯指令构造规范化全局表达式键
	bool buildExpressionKey(Instruction *I, ExprKey &K);

	//查询调用的ModRef行为并消费副作用分析结果
	ModRefInfo getCallModRef(CallInst *CI);
	//判断调用是否满足GVN公共子表达式消除条件
	bool isCallCSEEligible(CallInst *CI);
	//收集只读调用实际读取的指针实参
	std::vector<Value *> collectCallReadPtrs(CallInst *CI);
	//判断两个调用的被调函数、类型和实参是否完全相同
	static bool identicalCall(CallInst *A, CallInst *B);
	//构建只读调用所有读取位置的MemorySSA签名
	bool buildReadOnlyCallSignature(
		CallInst *CI, std::vector<MemoryReadClobber> &Signature);

	//从常量分支的死后继开始标记只由死边可达的基本块
	void markDeadBranchBlocks(BasicBlock *BrBB, BasicBlock *DeadSucc);
	//识别常量条件分支并更新不可达基本块集合
	void processBranchForDeadBlocks(Instruction *I);

	//合并支配等价的循环PHI并维护SSA使用关系
	bool eliminateCongruentPhis();
	//尝试使用等价内存版本的支配load替换当前load
	bool processLoad(Instruction *I);
	//尝试使用等价实参和内存签名的支配调用替换当前调用
	bool processCall(Instruction *I);
	//执行一轮全局值编号和冗余替换
	bool runGVN();

	//判断给定CFG边是否为支配树意义上的循环回边
	static bool isBackedge(BasicBlock *Pred, BasicBlock *BB, const DominatorTree &DT);
	//判断指令是否不适合作为PRE候选表达式
	bool shouldSkipPREInst(Instruction *I);

	//把表达式值号从汇合块翻译到指定前驱并缓存结果
	int phiTranslate(int Num, BasicBlock *FromBB, BasicBlock *ToPred);

	//把原始操作数解析为指定前驱出口可用的SSA值
	Value *resolveAvailOperand(Value *OrigV, BasicBlock *SrcBB,
	                           BasicBlock *PredBB);

	//把死块中的定义播种到代表表以维持死代码内部编号稳定
	void seedDeadBlocksToLeaderTable();
	//执行一轮PRE插入、合流PHI构造和冗余表达式替换
	bool runPRE();
};

//GVNPREPass获取依赖分析并运行可选择关闭PRE的全局值编号优化
class GVNPREPass : public PassDescriptor<GVNPREPass> {
public:
	//构造并设置是否启用部分冗余消除和保守路径下沉
	explicit GVNPREPass(bool EnablePRE = true,
		bool EnableCodeSink = false);
	//对给定函数运行GVN及可选PRE并返回分析保留状态
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
	//EnablePRE记录当前Pass是否执行部分冗余消除阶段
	bool EnablePRE = true;
	//EnableCodeSink记录当前Pass是否执行不增加代码量的路径下沉
	bool EnableCodeSink = false;

	//判断指令是否可无条件推迟执行且不引入故障或poison。
	static bool isConservativeSinkCandidate(Instruction *I);
	//把单用户纯表达式下沉到条件分支的独占后继中。
	static bool runConservativeCodeSink(
		Function &F, FunctionAnalysisPassManager &FAM,
		LoopInfo &LI, PostDominatorTree &PDT);
};

//LocalGVNPass在单个基本块内线性消除重复纯表达式
class LocalGVNPass : public PassDescriptor<LocalGVNPass> {
public:
	//对给定函数逐块运行局部值编号并返回分析保留状态
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
	//LocalExprKey使用真实操作数标识单块内的纯表达式
	struct LocalExprKey {
		//Opcode记录指令主操作码
		unsigned Opcode = 0;
		//SubOpcode记录二元运算或转换等细分操作码
		unsigned SubOpcode = 0;
		//ResultType记录表达式结果的精确IR类型
		const IRType *ResultType = nullptr;
		//Operands按照规范化顺序保存真实SSA操作数
		std::vector<Value *> Operands;

		//判断两个局部表达式键是否完全相同
		bool operator==(const LocalExprKey &Other) const;
	};

	//LocalExprKeyHash为局部表达式键提供组合哈希
	struct LocalExprKeyHash {
		//计算给定局部表达式键的哈希值
		std::size_t operator()(const LocalExprKey &K) const;
	};

	//为支持的纯指令构造单块局部表达式键
	static bool buildLocalExprKey(Instruction *I, LocalExprKey &K);
};
