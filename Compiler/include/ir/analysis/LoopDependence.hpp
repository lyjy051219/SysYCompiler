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

// LoopAffineExpr 以迭代坐标表示常量、循环轴系数和循环不变量项；Exact 为假即不可用于证明。
struct LoopAffineExpr {
    bool Exact = false;
    int64_t Constant = 0;
    std::unordered_map<const Loop *, int64_t> LoopCoefficients;
    std::unordered_map<const SCEVExpr *, int64_t> InvariantTerms;
};

// LoopMemoryAccess 保存一次标量访存、底层对象以及并行的类型化下标和字节步长。
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

enum LoopDependenceDirection : uint8_t {
    LoopDepSinkEarlier = 1u << 0,
    LoopDepSameIteration = 1u << 1,
    LoopDepSinkLater = 1u << 2,
    LoopDepAnyDirection = LoopDepSinkEarlier | LoopDepSameIteration |
                          LoopDepSinkLater
};

// 依赖记录按外到内循环轴保存方向掩码，Unknown 方向不会被合法性查询忽略。
struct LoopDependenceRecord {
    const LoopMemoryAccess *Source = nullptr;
    const LoopMemoryAccess *Sink = nullptr;
    LoopDependenceKind Kind = LoopDependenceKind::Unknown;

    std::vector<Loop *> Loops;
    std::vector<uint8_t> DirectionMasks;
    bool Exact = false;
};

// 函数级结果按循环惰性缓存摘要，并为交换、分布和调度提供保守合法性证明。
class LoopDependenceInfo {
public:
    using InstructionSet = std::unordered_set<Instruction *>;

    LoopDependenceInfo();
    LoopDependenceInfo(Function *F, LoopInfo *LI, SCEVResult *SE,
                       BasicAAResult *AA,
                       FunctionAnalysisPassManager *FAM);
    ~LoopDependenceInfo();
    LoopDependenceInfo(LoopDependenceInfo &&) noexcept;
    LoopDependenceInfo &operator=(LoopDependenceInfo &&) noexcept;
    LoopDependenceInfo(const LoopDependenceInfo &) = delete;
    LoopDependenceInfo &operator=(const LoopDependenceInfo &) = delete;

    const std::vector<LoopMemoryAccess> &getMemoryAccesses(Loop *L);
    const std::vector<LoopDependenceRecord> &getDependences(Loop *L);

    bool isInterchangeLegal(Loop *Outer, Loop *Inner);

    bool isDistributionLegal(Loop *L, const InstructionSet &First,
                             const InstructionSet &Second);

    bool isConflictLocalToIteration(Instruction *A, Instruction *B,
                                    Loop *L);

    bool isInvariantAccessDisjointFromLoopWrite(
        Instruction *Invariant, Instruction *Write,
        Loop *Scope, Loop *VaryingLoop);

    std::optional<int64_t> getByteStride(Instruction *I, Loop *L);

    bool hasUnknownMemory(Loop *L);
    bool hasObservableEffect(Loop *L);
    bool hasOrderingBarrier(Loop *L);
    bool invalidate(Function &F, const PreservationStatus &PA);

private:

    // 单循环摘要同时保存访问、访问对依赖和无法精确建模的内存屏障标志。
    struct LoopSummary {
        std::vector<LoopMemoryAccess> Accesses;
        std::vector<LoopDependenceRecord> Dependences;
        std::vector<Loop *> Loops;
        bool UnknownMemory = false;
        bool ObservableEffect = false;
        bool OrderingBarrier = false;
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
    FunctionAnalysisPassManager *FAM = nullptr;
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
    bool buildPointerAccess(Instruction *I, Value *Pointer,
                            const MemoryLocation &Location,
                            bool Reads, bool Writes, Loop *Scope,
                            std::size_t Order,
                            LoopMemoryAccess &Access) const;
    bool areForcedEqual(const LoopMemoryAccess &A,
                        const LoopMemoryAccess &B, Loop *Axis) const;
    bool areNoAlias(const LoopMemoryAccess &A,
                    const LoopMemoryAccess &B) const;
    static bool isWriteConflict(const LoopMemoryAccess &A,
                                const LoopMemoryAccess &B);
};

// Pass 入口组合 LoopInfo、SCEV 与 BasicAA，具体循环摘要在查询时构建。
class LoopDependenceAnalysis
    : public AnalysisDescriptor<LoopDependenceAnalysis> {
public:
    using Result = LoopDependenceInfo;
    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;

private:
    friend AnalysisDescriptor<LoopDependenceAnalysis>;
    inline static PassID Key;
};
