#pragma once

#include "ir/opt/NewPassManager.hpp"

class Loop;
class PhiInst;
class SCEVResult;
class Value;

//规范化后续循环 pass 观察到的归纳变量：
//1. 用 SCEV 键发现可能重复的整数递推，再逐边核验完全相同的 SSA 形状；
//2. 合并通过核验的重复 PHI，删除一套冗余的每轮更新；
//3. 将退出比较统一为“归纳变量在左”，不展开迭代次数也不修改 CFG；
//4. 每次只提交一次改写，使下一轮用重建后的 SCEV 继续处理。
class IndVarSimplifyPass : public PassDescriptor<IndVarSimplifyPass> {
public:
    //合并重复归纳递推或规范化一次循环退出比较。
    PassResult run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    //服务步骤 1：精确比较入边、更新操作码、类型和递推 PHI 的对应关系；
    //SCEV 数学等价只用于分桶，不能单独作为合并依据。
    static bool correspondingRecurrenceValue(
        Value *A, Value *B, PhiInst *APhi, PhiInst *BPhi);
    //核验两个回边更新具有相同类型、操作码和对应操作数。
    static bool haveExactUpdateShape(
        Value *A, Value *B, PhiInst *APhi, PhiInst *BPhi);
    //按前驱核验两个循环头PHI具有完全相同的起点和递推形状。
    static bool haveExactRecurrenceShape(PhiInst *A, PhiInst *B, Loop *L);
    //以SCEV键分桶并合并通过精确SSA核验的重复递推。
    static bool mergeExactRecurrences(Loop *L, SCEVResult &SE);
    //把退出比较规范为归纳变量位于左操作数且保持谓词语义。
    static bool orientExitComparison(Loop *L, SCEVResult &SE);
};
