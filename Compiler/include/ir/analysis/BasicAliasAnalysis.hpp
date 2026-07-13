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

//别名分析：采用即时计算两个指针的关系
//如果实现了select指令，那么需要补全aliasSelect函数

// Alias 查询结果：两个指针之间的别名关系
enum class AliasInfo {
    NoAlias,    // 一定不同
    MayAlias,   // 可能相同，也可能没关系
    MustAlias   // 一定相同
};

// A pointer together with the byte range touched through it.  Keeping the
// access width in the query avoids conflating pointer identity with memory
// overlap (for example, an i8 store next to an i32 load).  Unknown widths are
// represented explicitly and simply fall back to the conservative pointer
// query.
struct MemoryLocation {
    Value *Pointer = nullptr;
    std::optional<std::size_t> Size;

    MemoryLocation() = default;
    MemoryLocation(Value *P, std::optional<std::size_t> S)
        : Pointer(P), Size(S) {}

    static MemoryLocation get(const LoadInst *Load);
    static MemoryLocation get(const StoreInst *Store);
};


// BasicAAResult：别名分析结果
class BasicAAResult {
public:
    BasicAAResult() = default;
    BasicAAResult(BasicAAResult&&) = default;
    BasicAAResult& operator=(BasicAAResult&&) = default;
    BasicAAResult(const BasicAAResult&) = delete;
    BasicAAResult& operator=(const BasicAAResult&) = delete;
    bool invalidate(Function &F, const PreservationStatus &PA);

    void addObjectNoAliasFact(Value* V1, Value* V2);

    //即时查询指针两者之间的别名关系
    AliasInfo alias(Value* V1, Value* V2, const LoopInfo& LI);
    AliasInfo aliasWithIndexRange(Value* V1, Value* V2, const LoopInfo& LI);
    AliasInfo alias(const MemoryLocation &L1, const MemoryLocation &L2,
                    const LoopInfo &LI);
    AliasInfo aliasWithIndexRange(const MemoryLocation &L1,
                                  const MemoryLocation &L2,
                                  const LoopInfo &LI);
    // Allocation-object disjointness deliberately ignores the immediate
    // pointee-sized offsets used by alias().  It is the safe fallback for a
    // dynamic-length memory intrinsic: two different elements of one array
    // are not disjoint objects because an unknown byte range may span both.
    bool haveKnownDistinctUnderlyingObjects(Value *V1, Value *V2) const;
    bool callMayAliasPointer(CallInst *CI, Value *P, const LoopInfo &LI);

private:
    friend class BasicAliasAnalysis;

    struct ValuePairHash {
        std::size_t operator()(const std::pair<Value*, Value*>& P) const {
            auto H1 = static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(P.first));
            auto H2 = static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(P.second));
            return H1 ^ (H2 + 0x9e3779b97f4a7c15ULL + (H1 << 6) + (H1 >> 2));
        }
    };

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
        long long Step = 0;
        const Loop* L = nullptr;
    };

    Value* stripBaseInst(Value* V) const;
    Value* getBaseAddress(Value* V) const;
    // Unlike alias(), this answers an allocation-object question rather than
    // whether the two immediate pointee-sized access ranges overlap.  Only
    // facts proved here may be promoted to a callee-wide ObjectNoAlias fact.
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

    //分析两个GEP的偏移量关系
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


// 分析 Pass
class BasicAliasAnalysis : public AnalysisDescriptor<BasicAliasAnalysis> {
public:
    using Result = BasicAAResult;
    // The result may contain facts inferred from every direct call site of
    // the analysed function.  A caller mutation can therefore invalidate a
    // cached callee result even though the callee body itself did not change.
    static constexpr bool InvalidateAcrossIRUnits = true;
    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;

private:
    friend AnalysisDescriptor<BasicAliasAnalysis>;
    static PassID Key;
};
