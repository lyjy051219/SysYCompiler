#pragma once

#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include <cstdint>
#include <unordered_set>
#include <utility>
#include <vector>

// LICM (Loop Invariant Code Motion) - 循环不变量外提
// 依赖：DominatorTree, LoopInfo, BasicAliasAnalysis,SideEffect

using ExitValueKey = std::pair<BasicBlock *, Value *>;

struct ExitValueKeyHash {
    std::size_t operator()(const ExitValueKey &K) const {
        auto A = reinterpret_cast<std::uintptr_t>(K.first);
        auto B = reinterpret_cast<std::uintptr_t>(K.second);
        return A ^ (B + 0x9e3779b97f4a7c15ULL + (A << 6) + (A >> 2));
    }
};

class LICMPass : public PassDescriptor<LICMPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);
};
