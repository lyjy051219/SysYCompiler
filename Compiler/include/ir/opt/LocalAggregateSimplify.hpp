#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <unordered_set>
#include <vector>

struct LocalAggregatePointerPath {
    AllocaInst *Root = nullptr;
    std::vector<int> Indices;
    bool IsConstant = true;
};

struct LocalAggregateReadOnlyConstantArrayPlan {
    Variable *Source = nullptr;
    CallInst *InitCopy = nullptr;
    std::vector<GetElementPtrInst *> ElementPtrs;
    std::vector<LoadInst *> Loads;
};

struct LocalAggregateDeadUses {
    bool IsDead = true;
    std::vector<Instruction *> Writes;
    std::vector<Instruction *> DerivedPointers;
};

enum class LocalAggregateSynthExpressionKind {
    Index,
    Constant,
    Binary
};

// 通用常量表表达式的语法树。节点只描述 IR 语义，不保存源码名称、
// 基本块名称或运行时输入特征。
struct LocalAggregateSynthExpression {
    LocalAggregateSynthExpressionKind Kind =
        LocalAggregateSynthExpressionKind::Index;
    BinaryInst::Operation Op = BinaryInst::ADD;
    int32_t Constant = 0;
    int Cost = 0;
    std::shared_ptr<LocalAggregateSynthExpression> Left;
    std::shared_ptr<LocalAggregateSynthExpression> Right;
};

// 化简较小且地址不逃逸的局部聚合对象：
//   1. 转发同一基本块内从只读常量源复制而来的元素；
//   2. 对完整定义域等价且有收益的常量表生成通用整数表达式，
//      其余极小数组仍可生成 select 链；
//   3. 将常量下标的聚合 alloca 拆成标量槽，交给 Mem2Reg 提升；
//   4. 删除只有写入而从未被观察的聚合 alloca。
class LocalAggregateSimplifyPass
    : public PassDescriptor<LocalAggregateSimplifyPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    // 表达式枚举的界限属于统一的编译时间预算；每个候选仍需在完整数组
    // 定义域逐点验证，达到预算只会放弃优化，不会放宽正确性条件。
    static constexpr int MaxSynthArrayElements = 32;
    static constexpr int MaxSynthConstants = 12;
    static constexpr int MaxSynthExpressionCost = 3;
    static constexpr int MaxSynthStatesPerCost = 128;
    static constexpr int MaxSynthPendingStates = 4096;
    // 保守地把一次标量内存读取计作四个简单整数运算；只有严格更便宜的
    // 表达式才替换查表，GEP 成本不计入收益，避免高估。
    static constexpr int GenericScalarLoadCost = 4;

    static bool isWholeProgramReadOnlyVariable(Variable *Source);
    static bool traceConstantGlobalPointer(
        Value *Pointer, Variable *&Root, std::vector<int> &Path,
        std::shared_ptr<IRType> &PointeeTy);
    static bool collectInt32ConstantTable(
        Variable *Source, int Count, std::vector<int32_t> &Values);
    static bool evaluateSynthExpression(
        const std::shared_ptr<LocalAggregateSynthExpression> &Expression,
        int32_t Index, int32_t &Result);
    static std::shared_ptr<LocalAggregateSynthExpression>
    synthesizeInt32TableExpression(const std::vector<int32_t> &Values);
    static Value *materializeSynthExpression(
        const std::shared_ptr<LocalAggregateSynthExpression> &Expression,
        Value *Index, Instruction *InsertBefore,
        std::vector<Instruction *> &Created);
    static bool isSynthExpressionProfitable(
        const std::shared_ptr<LocalAggregateSynthExpression> &Expression);
    static bool foldReadOnlyGlobalLoads(
        Function &F, FunctionAnalysisPassManager &FAM);
    static void deleteDeadPointerChain(Value *Pointer);
    static LocalAggregatePointerPath tracePointer(
        Value *V, std::unordered_set<Value *> &Visiting);
    static LocalAggregatePointerPath tracePointer(Value *V);
    static bool getDirectConstantPath(
        AllocaInst *AI, GetElementPtrInst *GEP, std::vector<int> &Path,
        std::shared_ptr<IRType> &PointeeTy);
    static bool getDirectConstantScalarPath(
        AllocaInst *AI, GetElementPtrInst *GEP, std::vector<int> &Path,
        std::shared_ptr<IRType> &ElementTy);
    static bool appendConstantAggregateGEPPath(
        Value *Base, GetElementPtrInst *GEP,
        const std::vector<int> &BasePath, std::vector<int> &NewPath,
        std::shared_ptr<IRType> &PointeeTy);
    static bool getDirectOneDimElementIndex(
        AllocaInst *AI, GetElementPtrInst *GEP, Value *&Index);
    static bool getConstantOffsetScalarGEP(
        Value *Base, GetElementPtrInst *GEP, int &Offset);
    static bool isProvablyReadOnlySource(Variable *Source);
    static bool isWholeConstantCopy(
        CallInst *CI, AllocaInst *Destination, Variable *Source);
    static bool hasSupportedSameBlockUses(AllocaInst *AI, BasicBlock *BB);
    static bool forwardConstantLoads(AllocaInst *AI);
    static int getOneDimScalarArrayCount(AllocaInst *AI);
    static bool indexTypeCanSelectAll(Value *Index, int Count);
    static bool collectReadOnlyConstantArrayUses(
        Function &F, AllocaInst *AI,
        LocalAggregateReadOnlyConstantArrayPlan &Plan, int Count);
    static Value *buildConstantArrayLookup(
        Variable *Source, int Count, Value *Index,
        Instruction *InsertBefore,
        std::vector<Instruction *> &Created);
    static bool expandReadOnlyConstantArrayLoads(
        Function &F, AllocaInst *AI);
    static bool addScalarSlot(
        std::map<std::vector<int>, std::shared_ptr<IRType>> &SlotTypes,
        const std::vector<int> &Path,
        const std::shared_ptr<IRType> &ElementTy,
        int MaxScalarizedElements);
    static bool collectScalarizedPointerUses(
        AllocaInst *AI, Value *Pointer, const std::vector<int> &PointerPath,
        std::vector<GetElementPtrInst *> &TransparentPointers,
        std::map<GetElementPtrInst *, std::vector<int>> &TransparentPaths,
        std::set<GetElementPtrInst *> &SeenTransparent,
        std::vector<Instruction *> &MemoryAccesses,
        std::map<std::vector<int>, std::shared_ptr<IRType>> &SlotTypes,
        int MaxScalarizedElements);
    static bool scalarizeConstantIndexedAggregate(AllocaInst *AI);
    static LocalAggregateDeadUses collectDeadAggregateUses(AllocaInst *AI);
    static bool deleteDeadAggregate(AllocaInst *AI);
    static std::vector<AllocaInst *> collectArrayAllocas(Function &F);
};
