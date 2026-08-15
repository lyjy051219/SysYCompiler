#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

//BasicAAResult提供底层对象、对象大小和对齐事实。
class BasicAAResult;
//DominatorTree提供完整覆盖阶段及受控访存的支配证明。
class DominatorTree;
//LoopInfo提供父子循环和访存所属循环查询。
class LoopInfo;
//LoopAccessInfo提供矩形循环巢结构证明。
class LoopAccessInfo;
//LoopDependenceInfo提供完整访存集合及未知副作用摘要。
class LoopDependenceInfo;
//RangeInfo提供动态起点的非负范围事实。
class RangeInfo;
//SCEVResult提供规范归纳、循环次数、有限性和无回绕证明。
class SCEVResult;
//LoopMemoryAccess是循环依赖分析发布的稳定访存记录。
struct LoopMemoryAccess;
//LoopTripCountInfo描述规范循环退出比较。
struct LoopTripCountInfo;
//SCEVExpr表示共享动态边界的符号表达式。
struct SCEVExpr;

//IdempotentAddressBand保存同一 typed GEP 字节步长上的常量和动态坐标。
struct IdempotentAddressBand {
    std::int64_t ByteStride = 0;
    std::int64_t ConstantIndex = 0;
    Value *DynamicIndex = nullptr;
};

//IdempotentTypedAddress保存一次标量访存的完整类型化地址摘要。
struct IdempotentTypedAddress {
    Instruction *Inst = nullptr;
    Value *Pointer = nullptr;
    Value *Root = nullptr;
    std::size_t AccessBytes = 0;
    std::vector<IdempotentAddressBand> Bands;
};

//IdempotentAxisMatch保存一个受严格共同上界约束的动态坐标证明。
struct IdempotentAxisMatch {
    Loop *Axis = nullptr;
    PhiInst *Phi = nullptr;
    Value *Update = nullptr;
    Value *Index = nullptr;
    Value *Start = nullptr;
    Value *Bound = nullptr;
    const SCEVExpr *BoundExpr = nullptr;
    BasicBlock *ContinueSuccessor = nullptr;
    bool UsesUpdate = false;
};

//IdempotentIterationPlan保存分析完成后一次性提交的安全回边收缩计划。
struct IdempotentIterationPlan {
    Loop *Parent = nullptr;
    BasicBlock *PreHeader = nullptr;
    BasicBlock *Header = nullptr;
    BasicBlock *Latch = nullptr;
    BasicBlock *Exit = nullptr;
    BrInst *LatchBranch = nullptr;
    //尾测循环保存原继续条件；头测循环的无条件回边在此保持为空。
    Value *OriginalCondition = nullptr;
    bool ContinueOnTrue = true;
    Value *SharedBound = nullptr;
    std::int64_t SafeUpperBound = 0;
};

//IdempotentIterationCollapsePass把已证明达到同一内存不动点的重复迭代保留一次。
class IdempotentIterationCollapsePass
    : public PassDescriptor<IdempotentIterationCollapsePass> {
public:
    //每次运行至多提交一个候选，使所有分析在下一轮重新建立。
    PreservationStatus run(
        Function &F, FunctionAnalysisPassManager &FAM);

private:
    //限制单个候选扫描的访存和指令数量，预算耗尽按未知拒绝。
    static constexpr std::size_t MaxMemoryAccesses = 192;
    static constexpr std::size_t MaxInstructions = 768;
    static constexpr std::size_t MaxPointerDepth = 16;

    //把分段 typed GEP 链归一化为按字节步长降序排列的坐标带。
    static bool normalizeTypedAddress(
        const LoopMemoryAccess &Access, BasicAAResult &AA,
        IdempotentTypedAddress &Address);
    //判断两个目标地址是否具有同根、同宽度和同矩形布局。
    static bool hasSameRectangleLayout(
        const IdempotentTypedAddress &First,
        const IdempotentTypedAddress &Second);
    //识别严格小于运行时上界的正单位规范归纳轴。
    static bool matchAxisPredicate(
        Loop *Axis, Value *Index, Instruction *Context,
        Loop *Parent, SCEVResult &SE, RangeInfo &RI,
        DominatorTree &DT, const LoopInfo &LI,
        IdempotentAxisMatch &Match);
    //证明动态坐标的起点在所有可达路径上非负。
    static bool isKnownNonNegativeStart(
        Value *Start, Instruction *Context, Loop *Parent,
        SCEVResult &SE, RangeInfo &RI,
        const DominatorTree &DT, const LoopInfo &LI,
        std::size_t Depth = 0);
    //证明受回边比较控制的 PHI 首次进入循环前也满足严格上界。
    static bool hasMatchingEntryGuard(
        const IdempotentAxisMatch &Match,
        const DominatorTree &DT);
    //证明地址中的每个动态坐标都包含于共同的半开矩形边界。
    static bool matchContainedAddress(
        const IdempotentTypedAddress &Address, Loop *Parent,
        Value *&SharedBound, const SCEVExpr *&SharedBoundExpr,
        SCEVResult &SE, RangeInfo &RI,
        DominatorTree &DT, const LoopInfo &LI);
    //由对象大小和 typed stride 推导当前地址允许的最大共同边界。
    static bool accumulateSafeUpperBound(
        const IdempotentTypedAddress &Address, BasicAAResult &AA,
        std::int64_t &SafeUpperBound);
    //检查父归纳值只参与回边控制，不改变本轮计算或循环外标量状态。
    static bool hasOnlyControlUses(
        const LoopTripCountInfo &Trip, PhiInst *Induction,
        Value *Update, BrInst *LatchBranch);
    //为单个父循环建立完整覆盖、不动点和对象安全计划。
    static bool buildPlan(
        Loop *Parent, LoopInfo &LI,
        SCEVResult &SE, RangeInfo &RI, BasicAAResult &AA,
        LoopDependenceInfo &LD, LoopAccessInfo &LA,
        DominatorTree &DT, IdempotentIterationPlan &Plan);
    //在预头生成对象安全门禁，并只在安全域截断父循环回边。
    static bool applyPlan(IdempotentIterationPlan &Plan);
};
