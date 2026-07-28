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

// 常量表综合只保存与源码命名无关的整数表达式语法树；Cost 是生成该树所需的
// 二元 IR 指令数，候选仍须在完整表定义域逐点验证。


struct LocalAggregateSynthExpression {
    LocalAggregateSynthExpressionKind Kind =
        LocalAggregateSynthExpressionKind::Index;
    BinaryInst::Operation Op = BinaryInst::ADD;
    int32_t Constant = 0;
    int Cost = 0;
    std::shared_ptr<LocalAggregateSynthExpression> Left;
    std::shared_ptr<LocalAggregateSynthExpression> Right;
};

// 分阶段化简局部小聚合与只读常量表：
// 1. 折叠只读全局的常量下标 load，并把完整定义域可综合的动态查表替换为
//    更便宜的整数表达式；
// 2. 在单块内转发“整表常量 memcpy + 常量下标覆盖”后的 load；
// 3. 将只读常量 memcpy 初始化的小数组 load 展开为表达式或极小 select 链；
// 4. 把仅按常量路径访问的局部聚合拆成独立标量槽；
// 5. 删除只有写入、地址未逃逸且从未被观察的聚合对象。
// 每阶段都重新快照剩余 alloca，前一阶段的清理结果自然成为下一阶段输入。






class LocalAggregateSimplifyPass
    : public PassDescriptor<LocalAggregateSimplifyPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    // 表综合预算只限制编译时间，不降低正确性门槛；超出元素、常量、表达式
    // 深度或状态数限制时直接放弃，任何接受项都覆盖完整数组定义域。


    static constexpr int MaxSynthArrayElements = 32;
    static constexpr int MaxSynthConstants = 12;
    static constexpr int MaxSynthExpressionCost = 3;
    static constexpr int MaxSynthStatesPerCost = 128;
    static constexpr int MaxSynthPendingStates = 4096;
    // 只有严格少于一次标量 load 的保守成本估计时才接受综合表达式。


    static constexpr int GenericScalarLoadCost = 4;

    // 服务步骤 1：沿全局指针的 GEP/bitcast 闭包证明只读性，解析常量路径，
    // 或为一维 i32 表枚举并物化完整定义域等价表达式。
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
    // 服务步骤 2—5：把局部指针沿零基常量 GEP/形状保持 bitcast 追溯到根
    // alloca；IsConstant 为 false 时只保留根，不把未知路径误作精确元素。
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
    // 服务步骤 2—4：验证 memcpy 源只读、字节数覆盖整个同形对象，并分别
    // 收集单块转发或只读数组展开所允许的完整 use 闭包。
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
    // 服务步骤 4：递归收集所有常量标量路径与透明派生 GEP；同一路径类型必须
    // 一致，且总槽位数不超过固定代码增长预算。
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
    // 服务步骤 5：写入、memcpy 目的和透明派生指针是唯一允许用途；发现 load、
    // 指针保存或未知调用即把整个聚合标为可观察。
    static LocalAggregateDeadUses collectDeadAggregateUses(AllocaInst *AI);
    static bool deleteDeadAggregate(AllocaInst *AI);
    static std::vector<AllocaInst *> collectArrayAllocas(Function &F);
};
