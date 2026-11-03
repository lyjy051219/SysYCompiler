// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

//DeadGlobalCleanup：删除只存在直接store的global，以及store一并删除
//只用在中端快结束的位置
class DeadGlobalCleanupPass: public PassDescriptor<DeadGlobalCleanupPass> {
public:
    PreservationStatus run(Module &M, ModuleAnalysisPassManager &MAM);
private:
    //验证全局的每条use都是直接store地址并收集这些死写
    static bool collectDeadStoresForGlobal(Variable *G, std::vector<StoreInst *> &Stores);
};
 