#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstddef>
#include <memory>
#include <vector>

class TargetCostInfo;

// VectorCombineStage 区分进入主要结构优化前的保守折叠和流水线末端的组合。
enum class VectorCombineStage {
    Early,
    Late,
};

// VectorCombinePass 只依据 IR 语义、use-def 和通用目标成本整理固定长度向量。
// 它不读取符号名称、测试输入或源码特征，也不利用越界、undef/poison 获利。
class VectorCombinePass : public PassDescriptor<VectorCombinePass> {
public:
    explicit VectorCombinePass(
        VectorCombineStage Stage = VectorCombineStage::Late);

    PassResult run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    VectorCombineStage Stage;
    const TargetCostInfo *CostInfo = nullptr;

    Value *simplifyInstruction(Instruction *I) const;
    Value *foldExtract(ExtractElementInst *Extract) const;
    Value *foldInsert(InsertElementInst *Insert) const;
    Value *foldShuffle(ShuffleVectorInst *Shuffle) const;
    Value *foldBinary(BinaryInst *Binary) const;
    Value *foldSelect(SelectInst *Select) const;
    Value *foldCast(Instruction *Cast) const;
    Value *scalarizeExtractedBinary(ExtractElementInst *Extract) const;

    static std::shared_ptr<VECTORType> getVectorType(const Value *V);
    static bool getValidConstantLane(
        Value *Index, const std::shared_ptr<VECTORType> &Type,
        std::size_t &Lane);
    static bool isConcreteScalarConstant(Value *V);
    static bool replacementHasExpectedType(
        const Instruction *Old, const Value *Replacement);
    static bool replaceInstruction(
        Instruction *Old, Value *Replacement);

    static Value *foldInsertChain(InsertElementInst *Root);
    static Value *foldConstantShuffle(ShuffleVectorInst *Shuffle);
    static ShuffleVectorInst *composeNestedShuffles(
        ShuffleVectorInst *Outer);
    static bool isScalarizationOpcodeAllowed(
        BinaryInst::Operation Operation);
    static bool addFiniteCost(int Cost, int &Total);
};
