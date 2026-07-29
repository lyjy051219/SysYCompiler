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
class MemoryAccess;
struct MemoryLocation;

// 前驱数据流的五态结果：已知值、需合成 PHI、确定被破坏、预算/环导致未知，
// 以及块内无定义需继续查询前驱。
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

// 流程：1. 用 MemorySSA 汇总各路径最近 MustAlias store，并筛选clobber集合相同的
// 支配load；后者还需通过CFG全路径无写证明；
// 2. 解析计划依赖后批量删除；
// 3. 按支配序对剩余 load 做块内逆扫、前驱递归和跨回边全路径证明；
// 4. 各路径值不同但均已知时在汇合块合成 PHI。
class LoadEliminationPass : public PassDescriptor<LoadEliminationPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:

    struct MSSALoadRecord {
        LoadInst *Load = nullptr;
        BasicBlock *Block = nullptr;
        std::vector<MemoryAccess *> Clobbers;
    };

    struct PathScanState {
        BasicBlock *BB = nullptr;
        bool TargetPrefixOnly = false;
    };

    static constexpr int MaxQueryDepth = 8;

    static constexpr int MaxDominatorSearchDepth = 16;
    static constexpr int MaxPathScanBlocks = 64;
    static constexpr int MaxPathScanInstructions = 2048;
    static constexpr int MaxDominatingQueriesPerFunction = 64;
    static constexpr std::size_t MaxMSSALoadComparisons = 128;
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
