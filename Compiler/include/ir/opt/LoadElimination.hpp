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
struct MemoryLocation;

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

    static LoadElimQueryResult known(Value *V);
    static LoadElimQueryResult clobbered();
    static LoadElimQueryResult unknown();
    static LoadElimQueryResult needPredecessors();
    static LoadElimQueryResult phiNeeded(
        std::vector<std::pair<BasicBlock *, Value *>> M);
};

class LoadEliminationPass : public PassDescriptor<LoadEliminationPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    // 反向路径扫描同时记录当前块是否只扫描目标 load 之前的前缀。
    struct PathScanState {
        BasicBlock *BB = nullptr;
        bool TargetPrefixOnly = false;
    };

    static constexpr int MaxQueryDepth = 8;

    static constexpr int MaxDominatorSearchDepth = 16;
    static constexpr int MaxPathScanBlocks = 64;
    static constexpr int MaxPathScanInstructions = 2048;
    static constexpr int MaxDominatingQueriesPerFunction = 64;
    static bool isTypeCompatible(Value *Source, LoadInst *TargetLoad);
    static bool mayModifyPointer(
        const MemoryLocation &Location, Instruction *I, BasicAAResult &AA,
        const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM);
    static bool isUnmodifiedOnAllPaths(
        LoadInst *Candidate, LoadInst *Target, Value *P,
        DominatorTree &DT, BasicAAResult &AA, const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM);
    static LoadInst *findAvailableDominatingLoad(
        LoadInst *Target, BasicBlock *BB, DominatorTree &DT,
        BasicAAResult &AA, const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM);
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
        LoadInst *Load, BasicBlock *BB, DominatorTree &DT,
        BasicAAResult &AA, const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM, int &DominatingQueryBudget);
    static bool processFunction(
        Function &F, DominatorTree &DT, BasicAAResult &AA,
        const LoopInfo &LI, FunctionAnalysisPassManager &FAM);
};
