#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

class TargetCostInfo;

//完整保存 triangle/diamond 的控制块、两臂、汇合块及条件方向；只有 matchPattern
//全量验证成功后，改写阶段才读取该结构。
struct IfConvMatch {
	//记录产生条件分支的控制前驱块。
	BasicBlock *Pred = nullptr;
	//记录条件为指定方向时执行的真实计算臂。
	BasicBlock *TrueBB = nullptr;
	//记录diamond模式的另一计算臂。
	BasicBlock *ElseBB = nullptr;
	//记录两条路径重新汇合的后继块。
	BasicBlock *Succ = nullptr;
	//记录控制前驱块末尾的原条件分支。
	BrInst *PredBr = nullptr;
	//记录用于生成select的分支条件值。
	Value *Cond = nullptr;
	//标记候选是否为具有两条计算臂的diamond。
	bool HasElse = false;
	//记录条件为真是否进入TrueBB以恢复select两臂方向。
	bool CondTrueToTrueBB = false;
};

//流程：1. 匹配单块 triangle/diamond；2. 验证推测安全、SSA 用户与成本预算；
//3. 上提两臂计算；4. 以 select 替换汇合 PHI并删除臂块；5. 迭代到无候选。
class IfConversionPass : public PassDescriptor<IfConversionPass> {
public:
	//迭代把安全且成本受限的triangle或diamond控制流改写为select。
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
	//判断指令被无条件执行时是否仍无副作用、陷阱和poison风险。
	static bool isSafeToSpeculativelyExecute(const Instruction *I);
	//估算单条候选指令被无条件执行的相对成本。
	static int instructionCost(const Instruction *I,
		const TargetCostInfo &TargetCost);
	//统计基本块中除PHI和终结指令外的候选指令数量。
	static int countCandidateInsts(BasicBlock *BB);
	//累计基本块中所有候选指令的推测执行成本。
	static int candidateCost(BasicBlock *BB,
		const TargetCostInfo &TargetCost);
	//检查基本块只包含可推测普通指令和唯一无条件出口。
	static bool checkBlockConvertible(BasicBlock *BB);
	//检查臂内定义的用户仅位于本臂或汇合PHI中。
	static bool usersAreSafeForHoist(BasicBlock *BB, BasicBlock *Succ);
	//检查基本块恰好由给定两个不同前驱到达。
	static bool hasExactPreds(BasicBlock *BB, BasicBlock *A, BasicBlock *B);
	//完整识别并验证以给定计算臂为入口的triangle或diamond模式。
	static bool matchPattern(BasicBlock *TrueBB, IfConvMatch &M,
		const TargetCostInfo &TargetCost);
	//上提候选计算、以select替换PHI并删除原计算臂。
	bool tryConvertOne(Function &F, BasicBlock *TrueBB,
		const TargetCostInfo &TargetCost);
};
