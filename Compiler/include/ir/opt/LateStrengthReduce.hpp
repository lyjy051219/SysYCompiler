#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstdint>
#include <unordered_set>

//流程：1. 快照残留整数除余；2. 无符号 2 次幂直接降为移位/掩码；
//3. 有符号路径先证明非负，否则仅对除以 2 构造符号偏置序列；4. 失效值分析。
class LateStrengthReducePass : public PassDescriptor<LateStrengthReducePass> {
public:
    //在后端前把安全的常量二次幂整数除余降低为移位和掩码序列。
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    //为可能为负的有符号二次幂除法插入向零舍入的偏置移位序列。
    static bool insertSignedDivPow2Sequence(
        BinaryInst *BI, Value *LHS, int32_t Divisor,
        Instruction *&Quotient);
    //有界递归证明整数值在所有已建模路径上均非负。
    static bool isGuaranteedNonNegativeImpl(
        Value *V, int Depth, std::unordered_set<Value *> &Visiting);
    //以固定深度预算判断整数值是否能够证明非负。
    static bool isGuaranteedNonNegative(Value *V);
    //快照并降低函数中全部满足语义门禁的常量二次幂除余。
    static bool reduceFunction(Function &F);
};
