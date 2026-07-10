#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <vector>

struct GepCombineGroup {
    std::vector<GetElementPtrInst *> Geps;
};

// GEP 地址计算求值：单条 GEP 指令化简
class GepEvaluatePass : public PassDescriptor<GepEvaluatePass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    static bool evaluateGEP(Function &F);
};

// GEP 地址计算合并：链式 GEP 合并 + PHI 中 GEP 提取 + 循环内相邻 GEP 合并
class GepCombinePass : public PassDescriptor<GepCombinePass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    static bool combineChainedGEPs(Function &F);
    static bool foldGEPOfPhi(Function &F);
    static bool combineLoopGEPs(Function &F);
};
