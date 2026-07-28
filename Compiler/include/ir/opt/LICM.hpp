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

 
 

using ExitValueKey = std::pair<BasicBlock *, Value *>;

struct ExitValueKeyHash {
    std::size_t operator()(const ExitValueKey &K) const;
};

// 流程：1. 按内到外处理 LoopSimplify/LCSSA 循环；2. 结合支配、AA、MemorySSA
// 验证不变量与推测安全；3. 将安全 store 克隆到全部出口；4. 将必执行计算提升到
// preheader，非必执行纯调用改为首轮计算/后续复用 CFG；5. 按是否改 CFG 失效分析。
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
