#pragma once

#include "ir/opt/NewPassManager.hpp"

class Loop;
class PhiInst;
class SCEVResult;
class Value;






// 规范化后续循环 pass 观察到的归纳变量：
// 1. 用 SCEV 键发现可能重复的整数递推，再逐边核验完全相同的 SSA 形状；
// 2. 合并通过核验的重复 PHI，删除一套冗余的每轮更新；
// 3. 将退出比较统一为“归纳变量在左”，不展开迭代次数也不修改 CFG；
// 4. 每次只提交一次改写，使下一轮用重建后的 SCEV 继续处理。
class IndVarSimplifyPass : public PassDescriptor<IndVarSimplifyPass> {
public:
    PassResult run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    // 服务步骤 1：精确比较入边、更新操作码、类型和递推 PHI 的对应关系；
    // SCEV 数学等价只用于分桶，不能单独作为合并依据。
    static bool correspondingRecurrenceValue(
        Value *A, Value *B, PhiInst *APhi, PhiInst *BPhi);
    static bool haveExactUpdateShape(
        Value *A, Value *B, PhiInst *APhi, PhiInst *BPhi);
    static bool haveExactRecurrenceShape(PhiInst *A, PhiInst *B, Loop *L);
    static bool mergeExactRecurrences(Loop *L, SCEVResult &SE);
    static bool orientExitComparison(Loop *L, SCEVResult &SE);
};
