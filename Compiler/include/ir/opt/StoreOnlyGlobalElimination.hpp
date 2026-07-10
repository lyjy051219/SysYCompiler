#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct StoreOnlyGlobalTraceResult {
    Value *base = nullptr;
    std::vector<int64_t> constPath;
    bool hasVarIdx = false;
    bool unresolved = false;
};

struct StoreOnlyGlobalReadInfo {
    bool anyIndexRead = false;
    std::unordered_set<int64_t> constIdx;
};

// 只写全局变量消除（索引精确版）：
//   1) 对每个全局 G，跟踪是否被读，以及（对数组 G）被读的"常量索引集合"
//   2) 复用 SideEffectAnalysis：Call 的实参只在 callee 实际 Ref 时才算读
//   3) 删除：store 到从未被读的 global 全删；store 到数组常量索引 c 且 c
//      不在读集合且数组没有变量索引读 → 删
// 需要访问 FAM 以查 SideEffect/BasicAA/LoopInfo
class StoreOnlyGlobalEliminationPass : public PassDescriptor<StoreOnlyGlobalEliminationPass> {
public:
    explicit StoreOnlyGlobalEliminationPass(FunctionAnalysisPassManager &FAM) : FAM(FAM) {}
    PreservationStatus run(Module &M, ModuleAnalysisPassManager &MAM);
private:
    static void collectReachableBases(
        Value *Pointer, std::unordered_set<Value *> &Bases,
        std::unordered_set<Value *> &Visited, bool &Escapes,
        int Depth = 0);
    static StoreOnlyGlobalTraceResult traceBaseAndIndex(Value *Pointer);
    static void markGlobalRead(
        Variable *Global, const StoreOnlyGlobalTraceResult *Trace,
        std::unordered_map<Variable *, StoreOnlyGlobalReadInfo> &ReadInfo);
    FunctionAnalysisPassManager &FAM;
};
