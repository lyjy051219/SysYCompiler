#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

// 候选快照只保存原函数中的节点，统一扫描结束后才提交改写。
struct FunctionMemoizationCandidate {
    Function *Target = nullptr;
    std::vector<CallInst *> RecursiveCalls;
    std::vector<RetInst *> Returns;
};

// 为满足严格条件的纯递归函数插入有界、精确键的直接映射缓存。
class FunctionMemoizationPass
    : public PassDescriptor<FunctionMemoizationPass> {
public:
    explicit FunctionMemoizationPass(
        FunctionAnalysisPassManager &FAM,
        unsigned CacheEntries = 4096);

    PreservationStatus run(Module &M, ModuleAnalysisPassManager &MAM);

private:
    static constexpr unsigned MaxScalarParameters = 4;
    static constexpr unsigned DefaultCacheEntries = 4096;
    static constexpr std::size_t MaxModuleCacheBytes = 256 * 1024;

    static bool isPowerOfTwo(unsigned Value);
    static bool isSupportedScalarType(
        const std::shared_ptr<IRType> &Type);
    static bool hasUnsafeOrUndefinedOperation(Instruction *Inst);
    static bool collectCandidate(
        Function &F, FunctionAnalysisPassManager &FAM,
        FunctionMemoizationCandidate &Candidate);
    static std::string makeUniqueGlobalName(
        Module &M, const std::string &Stem);
    static GetElementPtrInst *appendSlotPointer(
        BasicBlock &Block, Variable *Cache, Value *Index);
    static Value *appendKeyBits(BasicBlock &Block, Value *Argument);
    static Value *appendHashMix(
        BasicBlock &Block, Value *Hash, Value *KeyBits);
    bool transformCandidate(
        Module &M, const FunctionMemoizationCandidate &Candidate,
        unsigned CacheOrdinal) const;

    FunctionAnalysisPassManager &FAM;
    unsigned CacheEntries;
};
