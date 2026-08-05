#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

//单地址追踪结果同时携带全局根、根到叶常量索引路径及变量索引/解析失败标志。
struct StoreOnlyGlobalTraceResult {
    //记录追踪得到的全局变量根。
    Value *base = nullptr;
    //按根到叶顺序记录每层GEP中能够确定的常量索引。
    std::vector<int64_t> constPath;
    //标记地址路径中是否出现变量索引。
    bool hasVarIdx = false;
    //标记地址生成链是否包含无法保守解析的节点。
    bool unresolved = false;
};

//每个全局的读取摘要：精确第一维索引集合，或“任意索引可能被读”的保守状态。
struct StoreOnlyGlobalReadInfo {
    //标记该全局任意索引位置都可能被读取。
    bool anyIndexRead = false;
    //记录能够精确证明被读取的第一维常量索引。
    std::unordered_set<int64_t> constIdx;
};

//流程：1. 全模块收集 load/call 对全局及索引的读取摘要；2. 删除从未被读的写入；
//3. 对无写标量传播静态初值；4. 删除失去全部 use 的全局对象。
class StoreOnlyGlobalEliminationPass : public PassDescriptor<StoreOnlyGlobalEliminationPass> {
public:
    //绑定模块变换期间查询函数级分析所需的分析管理器。
    explicit StoreOnlyGlobalEliminationPass(
        FunctionAnalysisPassManager &FAM);
    //收集全模块全局读取摘要并删除不可观察的写入和全局对象。
    PreservationStatus run(Module &M, ModuleAnalysisPassManager &MAM);
private:
    //递归收集一个指针可能来自的全部全局根并报告无法解析的逃逸。
    static void collectReachableBases(
        Value *Pointer, std::unordered_set<Value *> &Bases,
        std::unordered_set<Value *> &Visited, bool &Escapes,
        int Depth = 0);
    //沿单一地址链追踪全局根和根到叶的常量索引路径。
    static StoreOnlyGlobalTraceResult traceBaseAndIndex(Value *Pointer);
    //把一次精确或保守读取合并到对应全局变量的读取摘要。
    static void markGlobalRead(
        Variable *Global, const StoreOnlyGlobalTraceResult *Trace,
        std::unordered_map<Variable *, StoreOnlyGlobalReadInfo> &ReadInfo);
    //引用模块变换共享的函数级分析管理器。
    FunctionAnalysisPassManager &FAM;
};
