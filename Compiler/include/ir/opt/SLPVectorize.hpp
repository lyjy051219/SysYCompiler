#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <unordered_set>
#include <vector>

class BasicAAResult;
class DominatorTree;
class LoopInfo;
class RangeInfo;
class SCEVResult;
class TargetCostInfo;

// SLPAddressPack 保存一组连续标量访问的共同地址证明。
struct SLPAddressPack {
    Value *BaseObject = nullptr;
    Value *FirstPointer = nullptr;
    std::optional<long long> FirstByteOffset;
    std::size_t ElementBytes = 0;
    std::size_t KnownAlignment = 1;
};

// SLPNodeKind 描述从同构标量 lane 构造向量值的方式。
enum class SLPNodeKind {
    Constant,
    ReuseVector,
    Gather,
    Load,
    Binary,
    Cast,
};

// SLPVectorizeNode 是一次候选计划中的临时打包节点，不进入最终 IR。
struct SLPVectorizeNode {
    SLPNodeKind Kind = SLPNodeKind::Gather;
    std::vector<Value *> Scalars;
    std::shared_ptr<VECTORType> VectorType;
    std::vector<SLPVectorizeNode *> Operands;
    SLPAddressPack Address;
    BinaryInst::Operation BinaryOperation = BinaryInst::ADD;
    Instruction::InstType Opcode = Instruction::BinaryUnknown;
    Value *ReusedVector = nullptr;
    Value *EmittedValue = nullptr;
};

// SLPVectorizePlan 把完整合法性、依赖和成本判断与 IR 提交分离。
struct SLPVectorizePlan {
    BasicBlock *Block = nullptr;
    std::vector<StoreInst *> Stores;
    std::shared_ptr<VECTORType> StoreVectorType;
    SLPAddressPack StoreAddress;
    std::vector<std::unique_ptr<SLPVectorizeNode>> Nodes;
    SLPVectorizeNode *StoredValues = nullptr;
    std::unordered_set<Instruction *> ScalarInstructions;
    std::vector<LoadInst *> ScalarLoads;
    // 提交完成前记录所有新插入指令，失败时按逆序完整回滚。
    std::vector<Instruction *> EmittedInstructions;
    bool HasGather = false;
    int ScalarCost = 0;
    int VectorCost = 0;
};

// SLPVectorizePass 从同块连续地址 store 根自底向上打包同构标量表达式。
// 生产模式仅在目标原生支持相应固定向量且成本严格下降时改写。
class SLPVectorizePass : public PassDescriptor<SLPVectorizePass> {
public:
    explicit SLPVectorizePass(bool ForceForTesting = false,
                              unsigned ForcedVectorBitWidth = 0);

    PassResult run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    static constexpr std::size_t MaxVectorLanes = 8;
    static constexpr unsigned MaxTreeDepth = 12;
    // 限制同一 seed 组的调度窗口，避免在超大基本块中反复做二次别名查询。
    static constexpr std::size_t MaxScheduleSpan = 256;

    bool ForceForTesting;
    unsigned ForcedVectorBitWidth;
    const TargetCostInfo *CostInfo = nullptr;
    BasicAAResult *AA = nullptr;
    DominatorTree *DT = nullptr;
    LoopInfo *LI = nullptr;
    RangeInfo *RI = nullptr;
    SCEVResult *SE = nullptr;

    bool tryVectorizeBlock(BasicBlock *Block);
    bool tryBuildPlan(const std::vector<StoreInst *> &Stores,
                      SLPVectorizePlan &Plan);
    SLPVectorizeNode *buildPack(const std::vector<Value *> &Scalars,
                                SLPVectorizePlan &Plan,
                                unsigned Depth);
    SLPVectorizeNode *findExistingNode(
        const std::vector<Value *> &Scalars,
        SLPVectorizePlan &Plan) const;

    bool analyzeConsecutivePointers(
        const std::vector<Value *> &Pointers,
        const std::shared_ptr<VECTORType> &VectorType,
        Instruction *Context, SLPAddressPack &Address) const;
    bool validatePlanUses(const SLPVectorizePlan &Plan) const;
    bool validatePlanSchedule(const SLPVectorizePlan &Plan) const;
    bool computePlanCost(SLPVectorizePlan &Plan) const;
    bool applyPlan(SLPVectorizePlan &Plan) const;

    Value *emitNode(SLPVectorizeNode *Node,
                    SLPVectorizePlan &Plan) const;
    Value *emitGather(SLPVectorizeNode *Node,
                      SLPVectorizePlan &Plan) const;
    Instruction *createVectorCast(SLPVectorizeNode *Node,
                                  Value *Operand) const;
    bool insertBeforeRoot(SLPVectorizePlan &Plan,
                          Instruction *InstructionToInsert) const;
    void rollbackEmittedInstructions(SLPVectorizePlan &Plan) const;
    void eraseScalarPlan(SLPVectorizePlan &Plan) const;

    std::vector<std::size_t> getCandidateFactors(
        const std::shared_ptr<IRType> &ScalarType,
        std::size_t AvailableStores) const;
    bool isLegalVectorType(
        const std::shared_ptr<VECTORType> &Type) const;
    bool isValueAvailableAtRoot(Value *V, StoreInst *Root) const;
    bool valueDependsOn(Value *V, Value *Needle,
                        std::unordered_set<Value *> &Visited,
                        unsigned Depth) const;
    bool hasCrossLaneDependence(
        const std::vector<Instruction *> &Instructions) const;

    static bool isVectorizableScalarType(
        const std::shared_ptr<IRType> &Type);
    static bool isSupportedBinaryOperation(BinaryInst::Operation Op);
    static bool isSupportedCastOpcode(Instruction::InstType Opcode);
    static bool haveSameScalarType(const std::vector<Value *> &Values);
    static bool haveDistinctValues(const std::vector<Value *> &Values);
    static bool addFiniteCost(int Cost, int &Total);
};
