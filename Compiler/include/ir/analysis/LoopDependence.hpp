#pragma once

#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/opt/NewPassManager.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class BasicAAResult;
class Loop;
class LoopInfo;
class SCEVResult;
struct SCEVExpr;

// A deliberately small affine vocabulary for loop-memory reasoning.  Loop
// coefficients are expressed in iteration coordinates (rather than raw PHI
// values): an AddRec {Start,+,Step}<L> contributes Step to L and recursively
// contributes Start to the remaining terms.
struct LoopAffineExpr {
    bool Exact = false;
    int64_t Constant = 0;
    std::unordered_map<const Loop *, int64_t> LoopCoefficients;
    std::unordered_map<const SCEVExpr *, int64_t> InvariantTerms;
};

// One scalar load/store together with its typed GEP fingerprint.  Subscripts
// and ByteStrides are parallel arrays.  Equal byte strides from adjacent GEPs
// are folded into one typed coordinate, which preserves common split GEP
// shapes such as row(A, i) followed by element(row, j).
struct LoopMemoryAccess {
    Instruction *Inst = nullptr;
    Value *Pointer = nullptr;
    MemoryLocation Location;
    Value *Root = nullptr;
    bool Reads = false;
    bool Writes = false;
    bool AddressExact = false;
    std::vector<LoopAffineExpr> Subscripts;
    std::vector<int64_t> ByteStrides;
    std::size_t Order = 0;
};

enum class LoopDependenceKind {
    Flow,
    Anti,
    Output,
    Unknown
};

// Direction bits describe SinkIteration - SourceIteration.  An exact equality
// therefore has only LoopDepSameIteration set, while an unresolved component
// uses LoopDepAnyDirection.
enum LoopDependenceDirection : uint8_t {
    LoopDepSinkEarlier = 1u << 0,
    LoopDepSameIteration = 1u << 1,
    LoopDepSinkLater = 1u << 2,
    LoopDepAnyDirection = LoopDepSinkEarlier | LoopDepSameIteration |
                          LoopDepSinkLater
};

struct LoopDependenceRecord {
    const LoopMemoryAccess *Source = nullptr;
    const LoopMemoryAccess *Sink = nullptr;
    LoopDependenceKind Kind = LoopDependenceKind::Unknown;
    // Loops and DirectionMasks are parallel arrays, ordered outer-to-inner
    // (preorder for an imperfect loop tree).
    std::vector<Loop *> Loops;
    std::vector<uint8_t> DirectionMasks;
    bool Exact = false;
};

// Function-scoped dependence information used by loop distribution and loop
// interchange.  The analysis intentionally proves only a conservative core:
// distinct locations through BasicAA, affine typed GEP coordinates, and axes
// that address equality forces to the same iteration.  Unsupported pointer
// arithmetic and calls become barriers instead of speculative dependences.
class LoopDependenceInfo {
public:
    using InstructionSet = std::unordered_set<Instruction *>;

    LoopDependenceInfo();
    LoopDependenceInfo(Function *F, LoopInfo *LI, SCEVResult *SE,
                       BasicAAResult *AA);
    ~LoopDependenceInfo();
    LoopDependenceInfo(LoopDependenceInfo &&) noexcept;
    LoopDependenceInfo &operator=(LoopDependenceInfo &&) noexcept;
    LoopDependenceInfo(const LoopDependenceInfo &) = delete;
    LoopDependenceInfo &operator=(const LoopDependenceInfo &) = delete;

    const std::vector<LoopMemoryAccess> &getMemoryAccesses(Loop *L);
    const std::vector<LoopDependenceRecord> &getDependences(Loop *L);

    // This compact legality rule accepts an interchange only when every
    // potentially conflicting access pair is forced equal on at least one of
    // the two exchanged axes.
    bool isInterchangeLegal(Loop *Outer, Loop *Inner);

    // Splitting First before Second reverses only Second(i) -> First(j)
    // relationships.  Such a conflict is safe here only when L is forced
    // equal, so no cross-iteration relationship can be reversed.
    bool isDistributionLegal(Loop *L, const InstructionSet &First,
                             const InstructionSet &Second);

    // 判断两个可能冲突的访问是否只能发生在 L 的同一次迭代中。
    // 无写冲突或已经证明 NoAlias 时也返回 true。
    bool isConflictLocalToIteration(Instruction *A, Instruction *B,
                                    Loop *L);

    // 仅供循环调度：在固定同一 Scope 外层实例时，证明不随
    // VaryingLoop 变化的访问与该内层循环写访问始终不相交。
    bool isInvariantAccessDisjointFromLoopWrite(
        Instruction *Invariant, Instruction *Write,
        Loop *Scope, Loop *VaryingLoop);

    // Signed byte displacement of I's pointer for one iteration of L.  Zero
    // means loop-invariant; nullopt means non-affine or arithmetic overflow.
    std::optional<int64_t> getByteStride(Instruction *I, Loop *L);

    bool hasUnknownMemory(Loop *L);
    bool invalidate(Function &F, const PreservationStatus &PA);

private:
    struct LoopSummary {
        std::vector<LoopMemoryAccess> Accesses;
        std::vector<LoopDependenceRecord> Dependences;
        std::vector<Loop *> Loops;
        bool UnknownMemory = false;
    };

    static constexpr std::size_t MaxMemoryAccesses = 512;
    static constexpr std::size_t MaxDependencePairs = 8192;
    static constexpr unsigned MaxPointerChainDepth = 64;
    static constexpr unsigned MaxGEPIndexTerms = 128;
    inline static const std::vector<LoopMemoryAccess> EmptyMemoryAccesses;
    inline static const std::vector<LoopDependenceRecord>
        EmptyDependenceRecords;

    Function *Func = nullptr;
    LoopInfo *LI = nullptr;
    SCEVResult *SE = nullptr;
    BasicAAResult *AA = nullptr;
    std::unordered_map<Loop *, std::unique_ptr<LoopSummary>> Cache;

    static bool checkedAdd(int64_t A, int64_t B, int64_t &Result);
    static bool checkedMul(int64_t A, int64_t B, int64_t &Result);
    static void eraseZeroTerms(LoopAffineExpr &Expression);
    static bool addScaledAffine(LoopAffineExpr &Destination,
                                const LoopAffineExpr &Source, int64_t Scale);
    static bool loopIsInScope(const Loop *Candidate, const Loop *Scope);
    static void collectLoopsPreorder(Loop *L, std::vector<Loop *> &Loops);
    static bool isPureAxisCoordinate(const LoopAffineExpr &Expression,
                                     Loop *Axis, int64_t &Coefficient);

    LoopSummary &getOrBuildSummary(Loop *L);
    std::unique_ptr<LoopSummary> buildSummary(Loop *L);
    LoopAffineExpr buildAffine(const SCEVExpr *E, Loop *Scope) const;
    bool buildMemoryAccess(Instruction *I, Loop *Scope, std::size_t Order,
                           LoopMemoryAccess &Access) const;
    bool areForcedEqual(const LoopMemoryAccess &A,
                        const LoopMemoryAccess &B, Loop *Axis) const;
    bool areNoAlias(const LoopMemoryAccess &A,
                    const LoopMemoryAccess &B) const;
    static bool isWriteConflict(const LoopMemoryAccess &A,
                                const LoopMemoryAccess &B);
};

class LoopDependenceAnalysis
    : public AnalysisDescriptor<LoopDependenceAnalysis> {
public:
    using Result = LoopDependenceInfo;
    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;

private:
    friend AnalysisDescriptor<LoopDependenceAnalysis>;
    inline static PassID Key;
};
