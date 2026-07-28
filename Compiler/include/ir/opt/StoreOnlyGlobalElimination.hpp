#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// 单地址追踪结果同时携带全局根、根到叶常量索引路径及变量索引/解析失败标志。
struct StoreOnlyGlobalTraceResult {
    Value *base = nullptr;
    std::vector<int64_t> constPath;
    bool hasVarIdx = false;
    bool unresolved = false;
};

// 每个全局的读取摘要：精确第一维索引集合，或“任意索引可能被读”的保守状态。
struct StoreOnlyGlobalReadInfo {
    bool anyIndexRead = false;
    std::unordered_set<int64_t> constIdx;
};

// 流程：1. 全模块收集 load/call 对全局及索引的读取摘要；2. 删除从未被读的写入；
// 3. 对无写标量传播静态初值；4. 删除失去全部 use 的全局对象。
class StoreOnlyGlobalEliminationPass : public PassDescriptor<StoreOnlyGlobalEliminationPass> {
public:
    explicit StoreOnlyGlobalEliminationPass(
        FunctionAnalysisPassManager &FAM);
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
