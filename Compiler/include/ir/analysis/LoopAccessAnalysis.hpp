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

// A transform-facing classification of two scalar memory operations.  The
// aligned case means that address equality is possible only when the two loop
// axes have the same logical iteration number.  It is deliberately stronger
// than an ordinary same-execution BasicAA query.
enum class LoopAccessPairKind {
    NoConflict,
    SameIterationOnly,
    Unknown
};

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

// LoopAccessInfo is the conservative bridge between scalar address analysis
// and schedule transforms.  It does not invent runtime alias checks: distinct
// objects use BasicAA, while accesses on one object must have typed affine
// coordinates which force equal logical iterations.
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

    LoopAccessPairKind classifyPair(
        Loop *FirstScope, Loop *FirstAxis,
        const LoopMemoryAccess &First,
        Loop *SecondScope, Loop *SecondAxis,
        const LoopMemoryAccess &Second) const;

    // True only when every conflicting pair from different Axis iterations is
    // disjoint.  This strong property is sufficient for tiling and jamming.
    bool areIterationsIndependent(Loop *Scope, Loop *Axis);

    LoopFusionAccessResult analyzeFusion(Loop *First, Loop *Second);
    bool isFusionLegal(Loop *First, Loop *Second);

    bool isRectangularNest(Loop *Outer, Loop *Inner);
    // True when a read is reused across ReusedAxis iterations but advances
    // along VaryingAxis.  This excludes small loop-invariant tables: keeping
    // those hot does not by itself justify changing the two-dimensional
    // execution order.
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

class LoopAccessAnalysis : public AnalysisDescriptor<LoopAccessAnalysis> {
public:
    using Result = LoopAccessInfo;
    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;

private:
    friend AnalysisDescriptor<LoopAccessAnalysis>;
    inline static PassID Key;
};
