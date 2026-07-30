#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

//流程：1. 证明标量全局的所有 use 都是直接 store；2. 删除这些纯写；
//3. 清扫所有无 use 全局；4. 仅失效依赖内存与全局副作用的分析。
class LateDeadGlobalCleanupPass
    : public PassDescriptor<LateDeadGlobalCleanupPass> {
public:
    //删除只被直接写入的标量全局及模块中全部无使用全局对象。
    PreservationStatus run(Module &M, ModuleAnalysisPassManager &MAM);

private:
    //验证全局的每条use都是直接store地址并收集这些死写。
    static bool collectDeadStoresForGlobal(
        Variable *G, std::vector<StoreInst *> &Stores);
};
