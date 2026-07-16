#pragma once

#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include <cstdint>
#include <unordered_set>
#include <utility>
#include <vector>

class MemorySSAResult;

// LICM (Loop Invariant Code Motion) - 循环不变量外提
// 依赖：DominatorTree, LoopInfo, BasicAliasAnalysis,SideEffect

using ExitValueKey = std::pair<BasicBlock *, Value *>;

struct ExitValueKeyHash {
    std::size_t operator()(const ExitValueKey &K) const;
};

class LICMPass : public PassDescriptor<LICMPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    static bool hasSafeExecutionPrefix(const Instruction *Candidate);
    static bool hasPreheaderDereferenceProof(
        LoadInst *Load, const Loop &L, BasicAAResult &AA,
        const LoopInfo &LI);
    static bool isSpeculatableDivisionOrRemainder(Instruction *I);
    static bool isNonFaultingStoreObject(Value *Pointer, Value *StoredValue);
    static bool isReusableLoopInvariantCall(
        CallInst *CI, const std::vector<Instruction *> &LoopInstructions,
        BasicAAResult &AA, const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM);
    static bool cacheLoopInvariantCall(
        Function &F, CallInst *CI, Loop &L, BasicBlock *PreHeader,
        BasicBlock *Latch, const DominatorTree &DT);
    static bool areAllUsersOutsideLoop(Instruction *I, const Loop &L);
    static bool isInvariant(
        Instruction *I, const Loop &L, const DominatorTree &DT,
        const std::unordered_set<Instruction *> &InvariantSet);
    static bool isSafeToMove(
        Instruction *I, const Loop &L, const DominatorTree &DT,
        const std::vector<BasicBlock *> &ExitBlocks,
        const std::vector<BasicBlock *> &Latches,
        const std::vector<Instruction *> &LoopInstructions,
        BasicAAResult &AA, MemorySSAResult &MSSA, bool MemorySSAUsable,
        const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM, bool ForSink,
        bool AggressiveHoist);
};
