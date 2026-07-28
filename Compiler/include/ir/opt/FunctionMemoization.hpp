#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

// 候选快照保存目标、自递归调用点和所有返回点，使模块扫描与 CFG 提交阶段分离。
struct FunctionMemoizationCandidate {
    Function *Target = nullptr;
    std::vector<CallInst *> RecursiveCalls;
    std::vector<RetInst *> Returns;
};

// 流程：1. 验证纯标量、NoModRef、多分支递归函数；2. 在模块缓存预算内创建键/结果数组；
// 3. 插入 lookup/hit 入口 CFG；4. 在每个原返回点按发布顺序写入直接映射缓存。
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
