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

class LoopDeletionPass : public PassDescriptor<LoopDeletionPass> {
public:
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
	using PhiReplacementMap = std::unordered_map<PhiInst *, Value *>;

	// 删除 CFG 前先收集并验证全部替换计划，保证失败时 IR 不变。
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
