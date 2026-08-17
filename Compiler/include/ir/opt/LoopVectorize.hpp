#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Loop;
class LoopAccessInfo;
class BasicAAResult;
class SCEVResult;
class TargetCostInfo;
struct InductionDescriptor;
struct LoopTripCountInfo;

// LoopVectorizeConfig 集中保存固定宽度循环向量化的规模和收益约束。
struct LoopVectorizeConfig {
    // VectorWidth 表示一轮向量循环覆盖的原标量迭代数。
    // 0 表示按目标原生位宽自动选择；例如 64/128 位目标上的 i32/float
    // 分别得到 VF=2/VF=4。显式非零值主要用于目标适配和回归测试。
    unsigned VectorWidth = 0;
    // MaxBodyInstructions 限制单次变换检查和生成的循环体规模。
    std::size_t MaxBodyInstructions = 64;
    // MaxInvariantSplats 限制循环不变量广播带来的额外指令数量。
    std::size_t MaxInvariantSplats = 8;
    // AllowUnsupportedTargetForTesting 只供 IR 回归测试模拟尚未公布的 SIMD。
    // 生产流水线保持默认 false，目标未声明原生向量时绝不自动改写。
    bool AllowUnsupportedTargetForTesting = false;
};

// LoopVectorizePass 把可证明独立的连续标量迭代压缩为固定长度向量迭代。
// 当前版本只接受无需余数路径的 Header+Body 两块规范循环；
// 未知依赖、归约、调用、
// 非单位正步长和活跃输出都会使候选保守失败。
class LoopVectorizePass : public PassDescriptor<LoopVectorizePass> {
public:
    explicit LoopVectorizePass(
        LoopVectorizeConfig Config = LoopVectorizeConfig());

    PassResult run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    struct VectorizationShape {
        Loop *L = nullptr;
        BasicBlock *Header = nullptr;
        BasicBlock *Body = nullptr;
        BasicBlock *PreHeader = nullptr;
        BasicBlock *Exit = nullptr;
        PhiInst *InductionPhi = nullptr;
        BinaryInst *InductionUpdate = nullptr;
        BinaryInst *ExitCompare = nullptr;
        BrInst *ExitBranch = nullptr;
        BrInst *LatchBranch = nullptr;
        std::vector<GetElementPtrInst *> AddressInstructions;
        std::vector<Instruction *> ScalarInstructions;
        std::unordered_set<Instruction *> ScalarInstructionSet;
    };

    struct VectorizationPlan {
        VectorizationShape Shape;
        BinaryInst *NewInductionUpdate = nullptr;
        std::vector<Instruction *> NewInstructions;
        std::vector<Instruction *> OldInstructions;
        std::unordered_map<Value *, Value *> WidenedValues;
        std::unordered_map<Value *, Value *> InvariantSplats;
        std::size_t DynamicSplatCount = 0;
    };

    LoopVectorizeConfig Config;

    bool matchShape(Loop *L, SCEVResult &SE, LoopAccessInfo &LA,
                    BasicAAResult &AA,
                    VectorizationShape &Shape) const;
    bool validateControl(Loop *L, const LoopTripCountInfo &Trip,
                         const InductionDescriptor &Induction,
                         VectorizationShape &Shape) const;
    bool validateBody(VectorizationShape &Shape, LoopAccessInfo &LA,
                      BasicAAResult &AA) const;
    bool buildPlan(const VectorizationShape &Shape,
                   VectorizationPlan &Plan) const;
    bool isProfitable(const VectorizationPlan &Plan,
                      const TargetCostInfo &Target) const;
    bool commitPlan(VectorizationPlan &Plan) const;

    Value *getWidenedValue(Value *V, VectorizationPlan &Plan) const;
    Value *createVectorLoad(LoadInst *Load,
                            VectorizationPlan &Plan) const;
    bool createVectorStore(StoreInst *Store,
                           VectorizationPlan &Plan) const;
    Value *createVectorBinary(BinaryInst *Binary,
                              VectorizationPlan &Plan) const;
    Value *createVectorCast(Instruction *Cast,
                            VectorizationPlan &Plan) const;
    Value *createVectorSelect(SelectInst *Select,
                              VectorizationPlan &Plan) const;

    static bool isSupportedScalarType(
        const std::shared_ptr<IRType> &Type);
    static bool isSupportedMemoryElementType(
        const std::shared_ptr<IRType> &Type);
    static bool isSupportedBodyInstruction(Instruction *I);
    static bool isSupportedCast(Instruction *I);
    static bool isInstructionInLoop(Value *V, Loop *L);
    static bool hasUseOutsideSet(
        Instruction *I,
        const std::unordered_set<Instruction *> &AllowedUsers);
    static std::shared_ptr<VECTORType> getVectorType(
        const std::shared_ptr<IRType> &ElementType, unsigned Width);
    static void destroyPlan(VectorizationPlan &Plan);
};
