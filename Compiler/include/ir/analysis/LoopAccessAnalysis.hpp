#pragma once

#include "ir/analysis/LoopDependence.hpp"
#include "ir/opt/NewPassManager.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

class BasicAAResult;
class Loop;
class LoopInfo;
class SCEVResult;

// 访问对分类区分无冲突、仅同逻辑迭代冲突和证据不足三种结果。
enum class LoopAccessPairKind {
    NoConflict,
    SameIterationOnly,
    Unknown
};

// 变换摘要引用 LoopDependence 中的稳定访问记录，并汇总未知内存与写入状态。
struct LoopAccessSummary {
    Loop *Scope = nullptr;
    std::vector<const LoopMemoryAccess *> Accesses;
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
                   BasicAAResult *AA, LoopDependenceInfo *LD);
    LoopAccessInfo(LoopAccessInfo &&) noexcept;
    LoopAccessInfo &operator=(LoopAccessInfo &&) noexcept;
    LoopAccessInfo(const LoopAccessInfo &) = delete;
    LoopAccessInfo &operator=(const LoopAccessInfo &) = delete;

    const LoopAccessSummary &getSummary(Loop *Scope);

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

    Function *Func = nullptr;
    LoopInfo *LI = nullptr;
    SCEVResult *SE = nullptr;
    BasicAAResult *AA = nullptr;
    LoopDependenceInfo *LD = nullptr;
    std::unordered_map<Loop *, std::unique_ptr<LoopAccessSummary>> Cache;

    std::unique_ptr<LoopAccessSummary> buildSummary(Loop *Scope);
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

// Pass 入口聚合 LoopInfo、SCEV、BasicAA 和 LoopDependence，并返回惰性缓存结果。
class LoopAccessAnalysis : public AnalysisDescriptor<LoopAccessAnalysis> {
public:
    using Result = LoopAccessInfo;
    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;

private:
    friend AnalysisDescriptor<LoopAccessAnalysis>;
    inline static PassID Key;
};
