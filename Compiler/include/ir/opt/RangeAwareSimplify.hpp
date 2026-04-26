#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

// 基于整数范围分析的指令化简：
// 强度削减（除法→移位、取模→位与）、常量折叠、比较折叠、死分支消除
class RangeAwareSimplifyPass : public PassDescriptor<RangeAwareSimplifyPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);
};
