#pragma once

#include "ir/analysis/LoopDependence.hpp"
#include "ir/opt/NewPassManager.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class BasicAAResult;
class DominatorTree;
class Loop;
class LoopInfo;
class RangeInfo;
class SCEVResult;

// 访问对分类区分无冲突、仅同逻辑迭代冲突和证据不足三种结果。
enum class LoopAccessPairKind {
    NoConflict,
    SameIterationOnly,
    Unknown
};

// 单个循环维度记录迭代次数和该轴每前进一步产生的字节位移。
struct LoopAccessDimension {
    Loop *Axis = nullptr;
    const SCEVExpr *TripCount = nullptr;
    std::optional<std::int64_t> ConstantTripCount;
    std::optional<std::int64_t> ByteStride;
    bool IsFinite = false;
    bool IsUnitStride = false;
};

// 访问足迹保留运行时次数的符号表达，常量上下界只有在全部维度均可安全
// 枚举时才发布；MaxByteOffsetExclusive 表示半开区间的右端点。
struct LoopAccessFootprint {
    const LoopMemoryAccess *Access = nullptr;
    Value *Base = nullptr;
    std::optional<std::int64_t> StartByteOffset;
    std::optional<std::int64_t> MinByteOffset;
    std::optional<std::int64_t> MaxByteOffsetExclusive;
    std::optional<std::size_t> ObjectSize;
    std::size_t ElementSize = 0;
    std::size_t Alignment = 1;
    std::vector<LoopAccessDimension> Dimensions;
    bool IsConsecutive = false;
    // 表示整个已证明迭代域在该足迹内无洞连续，并不声称覆盖完整分配对象。
    bool IsFullAccess = false;
    // 常量零次迭代用独立标志表示，避免把不存在的访问误报为完整区间。
    bool IsEmpty = false;
    bool Exact = false;
};

// 变换摘要引用 LoopDependence 中的稳定访问记录，并汇总未知内存与写入状态。
struct LoopAccessSummary {
    Loop *Scope = nullptr;
    std::vector<const LoopMemoryAccess *> Accesses;
    std::vector<LoopAccessFootprint> Footprints;
    bool HasUnknownMemory = true;
    bool HasWrite = false;
};

struct LoopFusionAccessResult {
    bool Legal = false;
    bool Unknown = false;
    unsigned SameIterationFlowPairs = 0;
    unsigned SameIterationConflictPairs = 0;
};

// LoopAccessInfo 把标量地址证明转换成融合、分块和循环交换可直接消费的判定。
class LoopAccessInfo {
public:
    LoopAccessInfo();
    LoopAccessInfo(Function *F, LoopInfo *LI, SCEVResult *SE,
                   BasicAAResult *AA, LoopDependenceInfo *LD,
                   RangeInfo *RI, DominatorTree *DT);
    LoopAccessInfo(LoopAccessInfo &&) noexcept;
    LoopAccessInfo &operator=(LoopAccessInfo &&) noexcept;
    LoopAccessInfo(const LoopAccessInfo &) = delete;
    LoopAccessInfo &operator=(const LoopAccessInfo &) = delete;

    const LoopAccessSummary &getSummary(Loop *Scope);
    const LoopAccessFootprint *getFootprint(
        Loop *Scope, const LoopMemoryAccess &Access);
    std::vector<const LoopAccessFootprint *> getFootprints(
        Loop *Scope, Instruction *I);
    bool isConsecutiveAccess(Loop *Scope, Instruction *I, Loop *Axis);

    // 查询先用不同根的 NoAlias 排除冲突，再尝试证明两个循环轴的逻辑迭代对齐。
    LoopAccessPairKind classifyPair(
        Loop *FirstScope, Loop *FirstAxis,
        const LoopMemoryAccess &First,
        Loop *SecondScope, Loop *SecondAxis,
        const LoopMemoryAccess &Second) const;

    bool areIterationsIndependent(Loop *Scope, Loop *Axis);

    LoopFusionAccessResult analyzeFusion(Loop *First, Loop *Second);
    bool isFusionLegal(Loop *First, Loop *Second);

    bool isRectangularNest(Loop *Outer, Loop *Inner);

    bool hasCrossAxisReuse(Loop *Scope, Loop *ReusedAxis,
                           Loop *VaryingAxis);
    std::optional<std::int64_t> getByteStride(Instruction *I, Loop *Scope,
                                              Loop *Axis);

    bool invalidate(Function &F, const PreservationStatus &PA);

private:
    static constexpr std::size_t MaxTransformAccessPairs = 8192;
    static constexpr std::size_t MaxAddressNoWrapQueryNodes = 4096;

    struct AddressNoWrapQueryState {
        std::unordered_set<Value *> Active;
        std::unordered_map<Value *, bool> Completed;
        std::size_t VisitedNodes = 0;
    };

    Function *Func = nullptr;
    LoopInfo *LI = nullptr;
    SCEVResult *SE = nullptr;
    BasicAAResult *AA = nullptr;
    LoopDependenceInfo *LD = nullptr;
    RangeInfo *RI = nullptr;
    DominatorTree *DT = nullptr;
    std::unordered_map<Loop *, std::unique_ptr<LoopAccessSummary>> Cache;

    std::unique_ptr<LoopAccessSummary> buildSummary(Loop *Scope);
    LoopAccessFootprint buildFootprint(
        Loop *Scope, const LoopMemoryAccess &Access);
    static void collectLoopAxes(Loop *Scope, std::vector<Loop *> &Axes);
    static std::optional<std::int64_t> computeAccessByteStride(
        const LoopMemoryAccess &Access, Loop *Axis);
    static std::optional<std::int64_t> computeConstantStartOffset(
        const LoopMemoryAccess &Access);
    static bool checkedAdd(std::int64_t First, std::int64_t Second,
                           std::int64_t &Result);
    static bool checkedMul(std::int64_t First, std::int64_t Second,
                           std::int64_t &Result);
    static std::uint64_t absoluteMagnitude(std::int64_t Value);
    bool isKnownFinite(Loop *Axis) const;
    bool dominatesIterationPaths(BasicBlock *Gate, Loop *Axis) const;
    bool isRectangularAxisPair(Loop *Outer, Loop *Inner) const;
    bool isMustExecuteInIterationDomain(
        const LoopMemoryAccess &Access,
        const std::vector<LoopAccessDimension> &Dimensions) const;
    bool isIntegerAddressExpressionNoWrap(
        Value *Expression, Instruction *Context,
        AddressNoWrapQueryState &State, unsigned Depth) const;
    bool isAddressArithmeticKnownNoWrap(
        const LoopMemoryAccess &Access) const;
    static bool isWriteConflict(const LoopMemoryAccess &First,
                                const LoopMemoryAccess &Second);
    static bool sameInvariantTerms(const LoopAffineExpr &First,
                                   const LoopAffineExpr &Second);
    static bool sameLoopTermsWithAxisMapping(
        const LoopAffineExpr &First, Loop *FirstAxis,
        const LoopAffineExpr &Second, Loop *SecondAxis);
    static bool isMixedRadixAccess(const LoopMemoryAccess &Access);
    static bool isolateAxis(const LoopAffineExpr &Expression, Loop *Axis,
                            std::int64_t &Coefficient);
    static bool equalityForcesAlignedIteration(
        const LoopMemoryAccess &First, Loop *FirstAxis,
        const LoopMemoryAccess &Second, Loop *SecondAxis);
};

// Pass 入口聚合循环、范围、别名和依赖事实，并返回惰性缓存结果。
class LoopAccessAnalysis : public AnalysisDescriptor<LoopAccessAnalysis> {
public:
    using Result = LoopAccessInfo;
    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;

private:
    friend AnalysisDescriptor<LoopAccessAnalysis>;
    inline static PassID Key;
};
