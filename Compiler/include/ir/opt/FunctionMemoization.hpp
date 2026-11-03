// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

struct SideEffectInfo;
 
//候选快照保存目标、自递归调用点和所有返回点，使模块扫描与 CFG 提交阶段分离。
struct FunctionMemoizationCandidate {
    //记录通过全部纯度和签名检查的递归目标函数。
    Function *Target = nullptr;
    //快照目标函数中的全部直接自递归调用点。
    std::vector<CallInst *> RecursiveCalls;
    //快照目标函数的全部原始返回点，供普通发布与严格路径划分共同使用。
    std::vector<RetInst *> Returns;
    //快照函数外的全部直接调用点；读取可变全局时在这些入口前使缓存失效。
    std::vector<CallInst *> ExternalCalls;
    //记录目标函数读取的可变全局对象；非空表示缓存不能跨顶层调用复用。
    std::vector<Variable *> MutableReadGlobals;
    //记录已证明便宜基例之后、进入递归核心前的唯一查找插入块。
    BasicBlock *DeferredLookupPredecessor = nullptr;
    //记录递归核心入口；缓存未命中时从下沉后的查找块进入这里。
    BasicBlock *DeferredLookupMissEntry = nullptr;
    //记录只有经过下沉查找才可达、因而允许发布缓存的递归核心返回点。
    std::vector<RetInst *> DeferredCachedReturns;
};

//流程：1. 验证纯标量、无写入、多分支递归函数及全部只读全局来源；
//2. 在模块缓存预算内创建键/结果数组；3. 读取可变全局时用代际隔离顶层调用；
//4. 插入 lookup/hit CFG；5. 普通模式发布全部返回，严格守卫模式只发布递归核心返回。
class FunctionMemoizationPass
    : public PassDescriptor<FunctionMemoizationPass> {
public:
    //绑定函数级分析管理器并规范化直接映射缓存容量。
    explicit FunctionMemoizationPass(
        FunctionAnalysisPassManager &FAM,
        unsigned CacheEntries = 4096);

    //扫描模块候选并在总缓存预算内为确定性递归函数插入记忆化缓存。
    PreservationStatus run(Module &M, ModuleAnalysisPassManager &MAM);

private:
    //限制缓存键允许包含的标量参数数量。
    static constexpr unsigned MaxScalarParameters = 4;
    //限制发布缓存的返回点数量，避免在分支繁多的函数中复制大量store。
    static constexpr unsigned MaxReturnSites = 8;
    //限制读取可变全局时需要插入失效操作的函数外静态调用点数量。
    static constexpr unsigned MaxExternalCallSites = 16;
    //限制单个候选预计新增的查找、发布和失效指令数量。
    static constexpr std::size_t MaxAddedInstructions = 128;
    //限制缓存查找前允许保留的便宜守卫层数。
    static constexpr unsigned MaxDeferredGuardDepth = 4;
    //限制绕过缓存的比较与基例结果计算总指令数。
    static constexpr std::size_t MaxCheapBypassInstructions = 16;
    //给出非法配置回退采用的默认缓存槽数量。
    static constexpr unsigned DefaultCacheEntries = 4096;
    //限制单个模块为所有记忆化函数创建的全局缓存总字节数。
    static constexpr std::size_t MaxModuleCacheBytes = 256 * 1024;

    //判断无符号整数是否为非零二次幂。
    static bool isPowerOfTwo(unsigned Value);
    //判断类型是否为缓存键和值支持的32位整数或单精度浮点标量。
    static bool isSupportedScalarType(
        const std::shared_ptr<IRType> &Type);
    //检查指令是否包含会破坏缓存确定性的未定义或不安全运算。
    static bool hasUnsafeOrUndefinedOperation(Instruction *Inst);
    //判断指针来源是否完整且仅包含候选允许读取的全局对象。
    static bool isTrackedReadGlobalPointer(
        Value *Pointer, const SideEffectInfo &Effects,
        const std::unordered_set<Value *> &AllowedReadGlobals);
    //验证函数的纯度、递归形态和标量语义并建立候选快照。
    static bool collectCandidate(
        Function &F, FunctionAnalysisPassManager &FAM,
        FunctionMemoizationCandidate &Candidate);
    //识别便宜直接返回守卫链，并为递归核心建立缓存查找下沉计划。
    static bool collectDeferredLookupPlan(
        Function &F, FunctionMemoizationCandidate &Candidate);
    //生成同时避开模块函数与全局变量名称的唯一缓存符号。
    static std::string makeUniqueGlobalName(
        Module &M, const std::string &Stem);
    //在基本块末尾生成指向缓存数组指定槽位的GEP。
    static GetElementPtrInst *appendSlotPointer(
        BasicBlock &Block, Variable *Cache, Value *Index);
    //把整数或浮点参数规范化为32位缓存键比特。
    static Value *appendKeyBits(BasicBlock &Block, Value *Argument);
    //把一个参数键混入当前32位哈希值。
    static Value *appendHashMix(
        BasicBlock &Block, Value *Hash, Value *KeyBits);
    //在读取可变全局的候选的每个函数外入口推进代际，回绕时清空槽位代际。
    static bool insertExternalCacheInvalidation(
        Module &M, const FunctionMemoizationCandidate &Candidate,
        Variable *SlotEpochCache, Variable *CurrentEpoch,
        unsigned CacheEntries);
    //为单个候选创建缓存全局量、命中入口和返回点发布逻辑。
    bool transformCandidate(
        Module &M, const FunctionMemoizationCandidate &Candidate,
        unsigned CacheOrdinal) const;

    //引用候选纯度查询所需的函数级分析管理器。
    FunctionAnalysisPassManager &FAM;
    //记录规范化后的直接映射缓存槽数量。
    unsigned CacheEntries;
};
