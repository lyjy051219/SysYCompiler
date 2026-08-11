#pragma once

#include "ir/opt/NewPassManager.hpp"

#include <cstdint>
#include <vector>

class BasicBlock;
class DominatorTree;
class Instruction;
class Loop;
class PhiInst;
class RangeInfo;
struct SCEVExpr;
class SCEVResult;
class Value;
struct LoopTripCountInfo;

//LoopExitValuePlan记录一个退出PHI及其可在循环外物化的精确SCEV表达式。
struct LoopExitValuePlan {
	//需要消除的LCSSA退出PHI。
	PhiInst *Phi = nullptr;
	//原退出边上由循环产生的值。
	Value *OriginalValue = nullptr;
	//原值在真实退出迭代求值得到的循环不变表达式。
	const SCEVExpr *ExitExpression = nullptr;
	//物化该表达式需要新增的通用IR指令成本。
	int ExpansionCost = 0;
	//模递推专用计划的循环外初值；为空时使用普通SCEV物化。
	Value *ModuloBase = nullptr;
	//模递推闭式中需要加到初值上的已折叠常量。
	std::int64_t ModuloAddend = 0;
	//仍需执行一次有符号余数时保存正模数，零表示无需余数指令。
	std::int64_t ModuloDivisor = 0;
	//运行期模递推版本保存精确总迭代次数；非空时生成带收益门禁的闭式快路径。
	const SCEVExpr *ModuloTripCount = nullptr;
	//SCEV无法直接物化时保存规范零起点单位归纳循环的运行期边界值。
	Value *ModuloTripCountValue = nullptr;
	//运行期边界可能为负时，需要在预头以max(0,bound)恢复精确次数。
	bool ModuloTripCountNeedsClamp = false;
	//运行期模递推每轮增加的正常量。
	std::int64_t ModuloStep = 0;
	//一次规范循环迭代中按相同步长完成的模递推更新次数。
	std::int64_t ModuloUpdatesPerIteration = 1;
	//进入闭式快路径所需的最小迭代次数，由统一指令成本推导。
	std::int64_t ModuloFastTripThreshold = 0;
};

//LoopExitValueCandidate保存一次循环出口值综合计划。
struct LoopExitValueCandidate {
    //被优化的循环。
	Loop *L = nullptr;
	//接收闭式出口表达式的循环预头。
	BasicBlock *PreHeader = nullptr;
	//全部需要在提交阶段同步替换的退出PHI。
	std::vector<LoopExitValuePlan> Plans;
	//全部表达式的保守新增指令成本。
	unsigned TotalExpansionCost = 0;
	//替换出口值后可消除的静态通用指令成本。
	unsigned RemovableLoopCost = 0;
};

//流程：1.证明规范循环有限且无可观察行为；2.按退出相位计算精确迭代编号；
//3.用SCEV综合全部循环live-out；4.以统一成本模型筛选并替换LCSSA PHI。
class LoopExitValueSimplifyPass
	: public PassDescriptor<LoopExitValueSimplifyPass> {
public:
	//把可证明的无副作用循环出口递推值提前综合到循环预头。
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
	//限制单个循环出口闭式表达式允许新增的通用IR指令数量。
	static constexpr unsigned MaxExpansionCost = 16;
	//判断单条循环指令能否在删除整个有限循环时无观察地丢弃。
	static bool isSafePureInstruction(Instruction *I);
	//检查循环全部指令均无副作用、故障、poison和未知语义。
	static bool hasNoObservableInstruction(Loop *L);
	//估算删除循环能够消除的通用静态IR成本。
	static unsigned estimateRemovableLoopCost(Loop *L);
	//估算单条可删除指令的目标无关通用成本。
	static unsigned estimateInstructionCost(Instruction *I);
	//结合SCEV、Range与支配信息证明候选循环次数可靠且有限。
	static bool hasReliableTripCount(
		SCEVResult &SE, const LoopTripCountInfo &Trip,
		RangeInfo &RI, DominatorTree &DT);
	//根据退出块位于Header或Latch选择N或N-1迭代表达式。
	static const SCEVExpr *getExitIteration(
		const LoopTripCountInfo &Trip);
	//为单个循环建立全量live-out综合与收益计划。
	static bool planCandidate(
		Loop *L, SCEVResult &SE, RangeInfo &RI, DominatorTree &DT,
		LoopExitValueCandidate &Candidate);
	//为含副作用循环中独立的纯递推live-out建立保留循环的综合计划。
	static bool planPreservedLoopCandidate(
		Loop *L, SCEVResult &SE, RangeInfo &RI, DominatorTree &DT,
		FunctionAnalysisPassManager &FAM,
		LoopExitValueCandidate &Candidate);
	//识别并综合正常量步长、正常量模数的有界模递推。
	static bool planModuloLiveOut(
		Loop *L, PhiInst *ExitPhi, Value *OnExit,
		const LoopTripCountInfo &Trip, SCEVResult &SE, RangeInfo &RI,
		DominatorTree &DT, LoopExitValueCandidate &Candidate);
	//在预头物化表达式并替换已规划的退出PHI。
	static bool applyCandidate(
		LoopExitValueCandidate &Candidate, DominatorTree &DT);
};
