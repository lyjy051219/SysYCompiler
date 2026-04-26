#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include <unordered_map>
#include <unordered_set>
#include <cstdint>

class LoopInfo;

//别名分析：采用即时计算两个指针的关系
//如果实现了select指令，那么需要补全aliasSelect函数

// Alias 查询结果：两个指针之间的别名关系
enum class AliasInfo {
    NoAlias,    // 一定不同
    MayAlias,   // 可能相同，也可能没关系
    MustAlias   // 一定相同
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

    //即时查询指针两者之间的别名关系
    AliasInfo alias(Value* V1, Value* V2, const LoopInfo& LI);

private:
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

    Value* stripBaseInst(Value* V) const;
    Value* getBaseAddress(Value* V) const;
    bool isEqualAlias(Value* V1, Value* V2, const LoopInfo& LI) const;

    AliasInfo aliasPHI(PhiInst* Phi, Value* Other, const LoopInfo& LI);
    AliasInfo mergePhiAlias(AliasInfo A, AliasInfo B) const;
    std::pair<Value*, Value*> makeCanonicalPair(Value* A, Value* B) const;

    bool accumulateGEPIndices(GetElementPtrInst* GEP, LinearGEP& Expr) const;
    LinearGEP fillLinearGEP(Value* V) const;

    //分析两个GEP的偏移量关系
    AliasInfo aliasGEP(GetElementPtrInst* GEP1, GetElementPtrInst* GEP2);
};


// 分析 Pass
class BasicAliasAnalysis : public AnalysisDescriptor<BasicAliasAnalysis> {
public:
    using Result = BasicAAResult;
    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;

private:
    friend AnalysisDescriptor<BasicAliasAnalysis>;
    static PassID Key;
};
