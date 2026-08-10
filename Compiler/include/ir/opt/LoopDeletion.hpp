#pragma once

#include "ir/opt/NewPassManager.hpp"

#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class BasicBlock;
class Instruction;
class Loop;
class PhiInst;
class SCEVResult;
class Value;
struct LoopTripCountInfo;

//流程：1. 在 LCSSA/LoopSimplify 循环中证明 trip count 有限；2. 验证循环无可观察行为；
//3. 规划并全量覆盖 exit PHI 与循环外 use；4. preheader 绕过循环后分两阶段删块。
class LoopDeletionPass : public PassDescriptor<LoopDeletionPass> {
public:
	//删除能够证明有限且无可观察行为的循环并修复外部SSA使用。
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
	//PhiReplacementMap记录退出PHI及删除循环后使用的循环外替代值。
	using PhiReplacementMap = std::unordered_map<PhiInst *, Value *>;

	//判断零次迭代路径是否可安全跳过给定循环头指令。
	static bool isSafeToSkipOnZeroTrip(Instruction *I);
	//检查循环及其子循环中的全部指令是否均无可观察行为。
	static bool hasNoObservableLoopInstruction(Loop *L);
	//证明运行期trip count对应严格比较的无回绕i32单位步长有限循环。
	static bool isProvablyFiniteStrictUnitStepLoop(
		SCEVResult &SE, const LoopTripCountInfo &TCI);
	//识别由前置guard保护的单块旋转单位步长有限循环。
	static std::optional<LoopTripCountInfo>
	matchProvablyFiniteRotatedUnitStepLoop(
		Loop *L, BasicBlock *PreHeader, BasicBlock *Latch,
		SCEVResult &SE);
	//沿循环头PHI的预头入值递归解析零次迭代路径上的替代值。
	static Value *resolveZeroTripValue(
		Value *V, Loop *L, BasicBlock *PreHeader,
		std::unordered_set<Value *> &Active);
	//为零次循环的全部退出PHI规划类型一致的预头路径替代值。
	static bool planZeroTripPhiReplacements(
		Loop *L, const LoopTripCountInfo &TCI, PhiReplacementMap &Plan);
	//为会执行但无观察的死循环规划循环外公共退出值。
	static bool planDeadLoopPhiReplacements(
		Loop *L, BasicBlock *Exit, PhiReplacementMap &Plan);
	//检查所有循环外SSA使用是否均由退出PHI替换计划覆盖。
	static bool outsideUsesAreCovered(
		Loop *L, const std::vector<BasicBlock *> &Blocks,
		const PhiReplacementMap &Plan);
	//集中应用已验证的退出PHI替换并删除原PHI。
	static void applyPhiReplacements(const PhiReplacementMap &Plan);
	//让预头绕过循环后断开并删除整个循环CFG区域。
	static bool deleteLoopCFG(
		Loop *L, Function &F, BasicBlock *PreHeader, BasicBlock *Exit,
		const std::vector<BasicBlock *> &LoopBlocks,
		const PhiReplacementMap &PhiPlan);
};
