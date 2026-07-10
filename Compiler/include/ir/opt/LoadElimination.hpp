#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class BasicAAResult;
class DominatorTree;
class LoopInfo;

struct LoadElimQueryResult {
    enum Kind {
        KNOWN,
        PHI_NEEDED,
        CLOBBERED,
        UNKNOWN,
        NEED_PREDECESSORS
    };

    Kind kind;
    Value *value = nullptr;
    std::vector<std::pair<BasicBlock*, Value*>> predMap;

    static LoadElimQueryResult known(Value *V) { return {KNOWN, V, {}}; }
    static LoadElimQueryResult clobbered() { return {CLOBBERED, nullptr, {}}; }
    static LoadElimQueryResult unknown() { return {UNKNOWN, nullptr, {}}; }
    static LoadElimQueryResult needPredecessors() { return {NEED_PREDECESSORS, nullptr, {}}; }
    static LoadElimQueryResult phiNeeded(std::vector<std::pair<BasicBlock*, Value*>> M) {
        return {PHI_NEEDED, nullptr, std::move(M)};
    }
};

class LoadEliminationPass : public PassDescriptor<LoadEliminationPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    static constexpr int MaxQueryDepth = 8;
    static bool isTypeCompatible(Value *Source, LoadInst *TargetLoad);
    static LoadElimQueryResult scanBlockBackward(
        BasicBlock *BB, Value *P, Instruction *StopBefore,
        LoadInst *TargetLoad, BasicAAResult &AA, const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM);
    static LoadElimQueryResult queryAvailableValue(
        Value *P, BasicBlock *BB, LoadInst *TargetLoad, int Depth,
        std::unordered_set<BasicBlock *> &OnStack,
        std::unordered_map<BasicBlock *, LoadElimQueryResult> &Memo,
        BasicAAResult &AA, const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM);
    static bool processLoad(
        LoadInst *Load, BasicBlock *BB, BasicAAResult &AA,
        const LoopInfo &LI, FunctionAnalysisPassManager &FAM);
    static bool processFunction(
        Function &F, DominatorTree &DT, BasicAAResult &AA,
        const LoopInfo &LI, FunctionAnalysisPassManager &FAM);
};
