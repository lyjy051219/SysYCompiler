#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <vector>

class DominatorTree;
class RangeInfo;

// 沿 GEP/同形 bitcast 链验证根对象、每维索引范围与最终类型，供会改变地址分组
// 的变换共同证明指针始终位于已知 alloca/global 对象内。
bool pointerIsProvablyWithinKnownObject(
    Value *Pointer, RangeInfo &RI, const DominatorTree &DT);
bool gepIsProvablyWithinKnownObject(
    GetElementPtrInst *GEP, RangeInfo &RI, const DominatorTree &DT);

struct GepCombineGroup {
    std::vector<GetElementPtrInst *> Geps;
};

// 流程：1. 折叠空索引与 undef 基址；2. 在第二个非零索引处分割复杂 GEP；
// 3. 保留 CFG 分析并让后续地址优化消费较简单的寻址形态。
class GepEvaluatePass : public PassDescriptor<GepEvaluatePass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    static bool evaluateGEP(Function &F);
};

// 流程：1. 有界迭代合并单用户链式 GEP；2. 将同形 incoming GEP 从 PHI 外提；
// 3. 用范围证明把块内相邻循环 GEP 重基址为常量步进。
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
