#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstdint>
#include <unordered_set>

 
 
 
 
 
 
 
// 流程：1. 快照残留整数除余；2. 无符号 2 次幂直接降为移位/掩码；
// 3. 有符号路径先证明非负，否则仅对除以 2 构造符号偏置序列；4. 失效值分析。
class LateStrengthReducePass : public PassDescriptor<LateStrengthReducePass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    static bool insertSignedDivPow2Sequence(
        BinaryInst *BI, Value *LHS, int32_t Divisor,
        Instruction *&Quotient);
    static bool isGuaranteedNonNegativeImpl(
        Value *V, int Depth, std::unordered_set<Value *> &Visiting);
    static bool isGuaranteedNonNegative(Value *V);
    static bool reduceFunction(Function &F);
};
