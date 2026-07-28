#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

 
 
 
 
// 流程：1. 证明标量全局的所有 use 都是直接 store；2. 删除这些纯写；
// 3. 清扫所有无 use 全局；4. 仅失效依赖内存与全局副作用的分析。
class LateDeadGlobalCleanupPass
    : public PassDescriptor<LateDeadGlobalCleanupPass> {
public:
    PreservationStatus run(Module &M, ModuleAnalysisPassManager &MAM);

private:
    static bool collectDeadStoresForGlobal(
        Variable *G, std::vector<StoreInst *> &Stores);
};
