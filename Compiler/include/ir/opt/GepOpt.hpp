#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <vector>

class DominatorTree;
class RangeInfo;

// 用于“把地址计算移到原执行域之外”时的通用对象边界证明：地址必须
// 追溯到已知 global/alloca，沿途 GEP 逐维在界且 bitcast 不改变形状。
bool pointerIsProvablyWithinKnownObject(
    Value *Pointer, RangeInfo &RI, const DominatorTree &DT);
bool gepIsProvablyWithinKnownObject(
    GetElementPtrInst *GEP, RangeInfo &RI, const DominatorTree &DT);

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
    static bool isKnownNoWrapAdd(
        BinaryInst *Inst, BasicBlock *ContextBlock, RangeInfo &RI,
        const DominatorTree &DT);
    static bool combineLoopGEPs(
        Function &F, RangeInfo &RI, const DominatorTree &DT);
};
