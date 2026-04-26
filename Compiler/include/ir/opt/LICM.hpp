#pragma once

#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include <unordered_set>
#include <vector>

// LICM (Loop Invariant Code Motion) - 循环不变量外提
// 依赖：DominatorTree, LoopInfo, BasicAliasAnalysis,SideEffect

class LICMPass : public PassDescriptor<LICMPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);
};
