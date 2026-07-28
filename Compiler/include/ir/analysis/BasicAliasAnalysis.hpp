#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include <optional>
#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>

class LoopInfo;
class Loop;
class SCEVResult;

// AliasInfo 表示查询可证明的最强关系；任何证据不足都必须返回 MayAlias。
enum class AliasInfo {
    NoAlias,
    MayAlias,
    MustAlias
};

// MemoryLocation 把指针与实际访问字节数绑定，避免把指针相等误当成区间重叠。
struct MemoryLocation {
    Value *Pointer = nullptr;
    std::optional<std::size_t> Size;

    MemoryLocation();
    MemoryLocation(Value *P, std::optional<std::size_t> S);

    static MemoryLocation get(const LoadInst *Load);
    static MemoryLocation get(const StoreInst *Store);
};

// BasicAAResult 按需分解指针并缓存查询；可选 SCEV 只用于已证明不回绕的下标距离。
class BasicAAResult {
public:
    BasicAAResult();
    explicit BasicAAResult(SCEVResult *SE);
    BasicAAResult(BasicAAResult&&);
    BasicAAResult& operator=(BasicAAResult&&);
    BasicAAResult(const BasicAAResult&) = delete;
    BasicAAResult& operator=(const BasicAAResult&) = delete;
    bool invalidate(Function &F, const PreservationStatus &PA);

    void addObjectNoAliasFact(Value* V1, Value* V2);

    // 查询接口分为指针关系与显式内存区间关系，IndexRange 版本允许消费循环距离证明。
    AliasInfo alias(Value* V1, Value* V2, const LoopInfo& LI);
    AliasInfo aliasWithIndexRange(Value* V1, Value* V2, const LoopInfo& LI);
    AliasInfo alias(const MemoryLocation &L1, const MemoryLocation &L2,
                    const LoopInfo &LI);
    AliasInfo aliasWithIndexRange(const MemoryLocation &L1,
                                  const MemoryLocation &L2,
                                  const LoopInfo &LI);

    bool haveKnownDistinctUnderlyingObjects(Value *V1, Value *V2) const;
    bool callMayAliasPointer(CallInst *CI, Value *P, const LoopInfo &LI);

private:
    friend class BasicAliasAnalysis;

    static constexpr long long I32Modulus = 1LL << 32;

    static bool checkedAdd(long long A, long long B, long long &Result);
    static bool checkedSub(long long A, long long B, long long &Result);
    static bool checkedMul(long long A, long long B, long long &Result);
    static bool hasUndefPointerOrigin(
        Value *V, std::unordered_set<Value *> &Active, unsigned Depth = 0);

    struct ValuePairHash {
        std::size_t operator()(const std::pair<Value*, Value*>& P) const;
    };

    // 内部规范形把 GEP 链拆为底层对象、常量字节偏移和 SSA 系数表。
    struct LinearGEP {
        Value* Base = nullptr;
        long long ConstantOffset = 0;
        std::unordered_map<Value*, long long> VariableOffsets;
        bool Valid = true;
    };

    std::unordered_map<std::pair<Value*, Value*>, AliasInfo, ValuePairHash> AliasCache;
    std::unordered_set<std::pair<Value*, Value*>, ValuePairHash> ObjectNoAlias;
    std::unordered_set<std::pair<Value*, Value*>, ValuePairHash> ActiveSelectQueries;

    struct AdditiveDecomposition {
        Value* Base = nullptr;
        long long Offset = 0;
        bool Valid = true;
    };

    struct PositivePhiRecurrence {
        Value* Start = nullptr;
        Value* Update = nullptr;
        long long Step = 0;
        const Loop* L = nullptr;
    };

    SCEVResult *SE = nullptr;

    Value* stripBaseInst(Value* V) const;
    Value* getBaseAddress(Value* V) const;

    bool isEqualAlias(Value* V1, Value* V2, const LoopInfo& LI) const;
    bool hasObjectNoAlias(Value* V1, Value* V2) const;

    AliasInfo aliasImpl(Value* V1, Value* V2, const LoopInfo& LI,
                        bool UseIndexRange);
    AliasInfo aliasLocationImpl(
        const MemoryLocation &L1, const MemoryLocation &L2,
        const LoopInfo &LI, bool UseIndexRange, unsigned Depth,
        std::unordered_set<std::pair<Value *, Value *>, ValuePairHash> &Active);
    AliasInfo aliasPHI(PhiInst* Phi, Value* Other, const LoopInfo& LI,
                       bool UseIndexRange);
    AliasInfo aliasSelect(SelectInst *Select, Value *Other,
                          const LoopInfo &LI, bool UseIndexRange);
    AliasInfo mergePhiAlias(AliasInfo A, AliasInfo B) const;
    std::pair<Value*, Value*> makeCanonicalPair(Value* A, Value* B) const;

    bool accumulateGEPIndices(GetElementPtrInst* GEP, LinearGEP& Expr) const;
    LinearGEP fillLinearGEP(Value* V) const;

    AliasInfo aliasGEP(Value* GEP1, Value* GEP2,
                       const LoopInfo& LI, bool UseIndexRange,
                       std::optional<std::size_t> AccessSize1 = std::nullopt,
                       std::optional<std::size_t> AccessSize2 = std::nullopt);

    std::size_t getPointeeAccessSize(Value* V) const;
    AdditiveDecomposition decomposeAdditive(Value* V) const;
    std::optional<long long> getAdditiveOffsetFrom(Value* V, Value* Base) const;
    std::optional<PositivePhiRecurrence> getPositivePhiRecurrence(PhiInst* Phi,
                                                                  const LoopInfo& LI) const;
    bool isLoopInvariant(Value* V, const Loop* L) const;
    std::optional<long long> lowerBoundDiff(Value* Greater, Value* Lesser,
                                            const LoopInfo& LI) const;
    AliasInfo aliasGEPByIndexDistance(
        const std::unordered_map<Value*, long long>& DiffVars,
        long long ConstDiff, std::size_t Threshold, const LoopInfo& LI) const;
};

// Pass 入口构造即时查询器，并从全部直接调用点提炼形参对象互斥事实。
class BasicAliasAnalysis : public AnalysisDescriptor<BasicAliasAnalysis> {
public:
    using Result = BasicAAResult;

    static constexpr bool InvalidateAcrossIRUnits = true;
    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;

private:
    friend AnalysisDescriptor<BasicAliasAnalysis>;
    inline static PassID Key;
};
