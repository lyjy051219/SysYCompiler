// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include <cstdint>
#include <memory>
#include <unordered_set>
#include <utility>
#include <vector>

//MemorySSAResult提供循环内存读写干扰的稀疏证明
class MemorySSAResult;
//RangeInfo提供带控制流上下文的整数范围证明
class RangeInfo;
//SCEVResult提供变量起点归纳范围和循环次数证明
class SCEVResult;

//ExitValueKey使用循环出口块和循环内SSA值标识LCSSA桥接值
using ExitValueKey = std::pair<BasicBlock *, Value *>;

//ExitValueKeyHash为循环出口桥接值键提供组合哈希
struct ExitValueKeyHash {
    //计算出口基本块和SSA值组合键的哈希值
    std::size_t operator()(const ExitValueKey &K) const;
};

//LoopMemoryPromotionPlan记录一个可由循环内存状态提升为标量SSA的精确位置
struct LoopMemoryPromotionPlan {
    //被提升内存位置所属的规范循环
    Loop *L = nullptr;
    //循环外可用且代表该精确位置的规范指针
    Value *Pointer = nullptr;
    //该位置存放的标量类型
    std::shared_ptr<IRType> ValueType;
    //循环唯一预头块
    BasicBlock *PreHeader = nullptr;
    //循环唯一回边块
    BasicBlock *Latch = nullptr;
    //循环全部专用出口块
    std::vector<BasicBlock *> ExitBlocks;
    //所有读写该精确位置且将被统一标量化的Load
    std::vector<LoadInst *> Loads;
    //所有完整写入该精确位置且将被改写或同步到缓存的Store
    std::vector<StoreInst *> Stores;
    //为真时保留原Store，仅用紧随其后的私有写同步循环携带缓存
    bool PreserveOriginalStores = false;
    //每次完成迭代必经的内存访问数量
    unsigned MandatoryAccesses = 0;
    //完整提升记录严格净收益下界，读取缓存记录受压力门约束的收益评分
    std::int64_t EstimatedNetBenefit = 0;
};

//LICMMode区分完整循环不变量优化和流水线末尾的纯地址提升模式
enum class LICMMode {
    //Full启用标量提升、内存提升、store下沉和不变量调用缓存
    Full,
    //AddressOnly只移动循环不变GEP，不改写任何内存访问或控制流
    AddressOnly,
}; 

//LICMPass提升或下沉经过支配、内存和可执行性证明的循环不变量
class LICMPass : public PassDescriptor<LICMPass> {
public:
    //选择完整LICM或仅消费晚期地址规范化暴露的不变GEP
    explicit LICMPass(LICMMode Mode = LICMMode::Full);
    //按由内到外的循环顺序运行LICM并返回分析保留状态
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    //Mode记录本次运行允许处理的候选类别
    LICMMode Mode = LICMMode::Full;
    //判断候选之前的块内前缀是否可安全提前执行
    static bool hasSafeExecutionPrefix(const Instruction *Candidate);
    //判断preheader是否已有同址内存访问证明Load地址可解引用
    static bool hasPreheaderDereferenceProof(
        LoadInst *Load, const Loop &L, BasicAAResult &AA,
        const LoopInfo &LI);
    //证明内存位置的完整地址区间始终落在已知栈或全局对象内
    static bool isMemoryLocationProvablyInBounds(
        const MemoryLocation &Location, Instruction *Access,
        BasicAAResult &AA, SCEVResult &SE,
        RangeInfo &RI, const DominatorTree &DT);
    //判断除法或余数指令能否在不引入异常的情况下推测执行
    static bool isSpeculatableDivisionOrRemainder(Instruction *I);
    //判断Store目标是否为可在循环出口安全写入的在界对象
    static bool isNonFaultingStoreObject(Value *Pointer, Value *StoredValue);
    //判断只读调用结果是否在整个循环中保持可复用
    static bool isReusableLoopInvariantCall(
        CallInst *CI, const std::vector<Instruction *> &LoopInstructions,
        BasicAAResult &AA, const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM);
    //把非必执行循环不变调用改写为首轮计算后续复用的缓存CFG
    static bool cacheLoopInvariantCall(
        Function &F, CallInst *CI, Loop &L, BasicBlock *PreHeader,
        BasicBlock *Latch, const DominatorTree &DT,
        const std::vector<Instruction *> &LoopInstructions,
        BasicAAResult &AA, const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM,
        const std::unordered_set<Instruction *> &KnownInvariant);
    //判断值能否在循环预头终结指令之前直接使用
    static bool isAvailableAtPreheader(
        Value *V, BasicBlock *PreHeader, const DominatorTree &DT);
    //为一个规范循环选择严格有收益的精确内存位置提升计划
    static bool planLoopMemoryPromotion(
        Function &F, Loop *L, BasicAAResult &AA, const LoopInfo &LI,
        const DominatorTree &DT, FunctionAnalysisPassManager &FAM,
        LoopMemoryPromotionPlan &Plan);
    //为条件更新位置选择保留真实Store的循环携带读取缓存计划
    static bool planStoreSynchronizedLoadCache(
        Function &F, Loop *L, BasicAAResult &AA, const LoopInfo &LI,
        const DominatorTree &DT, FunctionAnalysisPassManager &FAM,
        LoopMemoryPromotionPlan &Plan);
    //通过临时标量槽复用Mem2Reg，按计划同步真实Store或在出口写回
    static bool applyLoopMemoryPromotion(
        Function &F, LoopMemoryPromotionPlan &Plan,
        FunctionAnalysisPassManager &FAM);
    //判断指令的全部IR用户是否都位于给定循环之外
    static bool areAllUsersOutsideLoop(Instruction *I, const Loop &L);
    //判断指令全部操作数是否来自循环外或已证明的不变量定义
    static bool isInvariant(
        Instruction *I, const Loop &L, const DominatorTree &DT,
        const std::unordered_set<Instruction *> &InvariantSet);
    //综合验证候选能否安全提升到preheader或下沉到全部出口
    static bool isSafeToMove(
        Instruction *I, const Loop &L, const DominatorTree &DT,
        const std::vector<BasicBlock *> &ExitBlocks,
        const std::vector<BasicBlock *> &Latches,
        const std::vector<Instruction *> &LoopInstructions,
        BasicAAResult &AA, MemorySSAResult &MSSA, bool MemorySSAUsable,
        const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM, bool ForSink,
        bool AggressiveHoist);
};
