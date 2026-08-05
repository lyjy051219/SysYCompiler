#pragma once

#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include <cstdint>
#include <unordered_set>
#include <utility>
#include <vector>

//MemorySSAResult提供循环内存读写干扰的稀疏证明
class MemorySSAResult;

//ExitValueKey使用循环出口块和循环内SSA值标识LCSSA桥接值
using ExitValueKey = std::pair<BasicBlock *, Value *>;

//ExitValueKeyHash为循环出口桥接值键提供组合哈希
struct ExitValueKeyHash {
    //计算出口基本块和SSA值组合键的哈希值
    std::size_t operator()(const ExitValueKey &K) const;
};

//LICMPass提升或下沉经过支配、内存和可执行性证明的循环不变量
class LICMPass : public PassDescriptor<LICMPass> {
public:
    //按由内到外的循环顺序运行LICM并返回分析保留状态
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    //判断候选之前的块内前缀是否可安全提前执行
    static bool hasSafeExecutionPrefix(const Instruction *Candidate);
    //判断preheader是否已有同址内存访问证明Load地址可解引用
    static bool hasPreheaderDereferenceProof(
        LoadInst *Load, const Loop &L, BasicAAResult &AA,
        const LoopInfo &LI);
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
        BasicBlock *Latch, const DominatorTree &DT);
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
