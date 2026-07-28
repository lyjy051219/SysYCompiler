#pragma once

#include "ir/opt/NewPassManager.hpp"

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

// 流程：1. 在 LCSSA/LoopSimplify 循环中证明 trip count 有限；2. 验证循环无可观察行为；
// 3. 规划并全量覆盖 exit PHI 与循环外 use；4. preheader 绕过循环后分两阶段删块。
class LoopDeletionPass : public PassDescriptor<LoopDeletionPass> {
public:
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
	using PhiReplacementMap = std::unordered_map<PhiInst *, Value *>;

	static bool isSafeToSkipOnZeroTrip(Instruction *I);
	static bool hasNoObservableLoopInstruction(Loop *L);
	static bool isProvablyFiniteStrictUnitStepLoop(
		SCEVResult &SE, const LoopTripCountInfo &TCI);
	static Value *resolveZeroTripValue(
		Value *V, Loop *L, BasicBlock *PreHeader,
		std::unordered_set<Value *> &Active);
	static bool planZeroTripPhiReplacements(
		Loop *L, const LoopTripCountInfo &TCI, PhiReplacementMap &Plan);
	static bool planDeadLoopPhiReplacements(
		Loop *L, BasicBlock *Exit, PhiReplacementMap &Plan);
	static bool outsideUsesAreCovered(
		Loop *L, const std::vector<BasicBlock *> &Blocks,
		const PhiReplacementMap &Plan);
	static void applyPhiReplacements(const PhiReplacementMap &Plan);
	static bool deleteLoopCFG(
		Loop *L, Function &F, BasicBlock *PreHeader, BasicBlock *Exit,
		const std::vector<BasicBlock *> &LoopBlocks,
		const PhiReplacementMap &PhiPlan);
};
