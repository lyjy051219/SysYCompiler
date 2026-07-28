#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "lib/CFG.hpp"
#include <vector>
#include <unordered_map>
#include <unordered_set>

// TailRecursionElimination 的整体流程：
// 1. 先为紧邻 ret、返回值匹配且不泄漏当前栈地址的非自递归调用设置 tail 标志。
// 2. 自递归路径先排除计时调用和会逃逸的入口 alloca，避免转循环后改变栈帧可观察语义。
// 3. 逐块查找直接 ret 或经无条件边到 ret-only 块的自调用，证明 call..ret 区间可前移或匹配整数加/乘累加器。
// 4. 首个候选创建新入口和形参 PHI，之后每个候选把递归实参接入 PHI，并用回边替换 call+ret。
// 5. 累加器模式额外建立带单位元的 PHI，在所有剩余 ret 处合并最终结果。
// 6. 每成功改写一个站点就失效分析并重新扫描；仅 tail 标志变更时保留全部 CFG/内存分析。
class TailRecursionEliminationPass : public PassDescriptor<TailRecursionEliminationPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    // 3.2 将共享返回块的 PHI/BitCast 返回值解析为指定前驱上可用的 SSA 值。
    static Value *resolveRetValueForPred(
        Value *V, BasicBlock *Pred, BasicBlock *TargetBB,
        BasicBlock *InsertBB, Instruction *InsertBefore,
        std::vector<Instruction *> &ClonedInsts,
        std::unordered_set<Value *> &Visiting);
    static bool rewriteClonedRetOperandForPred(
        RetInst *ClonedRet, BasicBlock *Pred, BasicBlock *TargetBB,
        BasicBlock *InsertBB, Instruction *InsertBefore,
        std::vector<Instruction *> &ClonedInsts);
    static bool isReadonlyLikeCallInst(
        Instruction *I, FunctionAnalysisPassManager &FAM, Function &F);
    static bool canMoveReadonlyCallBeforeSelfCall(
        CallInst *MovedCall, CallInst *SelfCall,
        FunctionAnalysisPassManager &FAM, Function &F);
    static bool isCallGuaranteedToReturnWithoutFault(
        CallInst *CI, std::unordered_set<Function *> &ActiveFunctions);
    static bool isFunctionGuaranteedToReturnWithoutFault(
        Function &Callee, std::unordered_set<Function *> &ActiveFunctions);
    static bool isSafelyDereferenceableLoad(LoadInst *LI);
    static bool hasInterferingEarlierSelfCallInBlock(
        BasicBlock *BB, CallInst *StopCI, Function &F,
        FunctionAnalysisPassManager &FAM);
    static RetInst *getRetOnlyTerminatorBlockRet(BasicBlock *BB);

    // 2.1 识别函数内的直接自递归调用。
    bool hasSelfRecursion(Function &F);

    bool isSelfCall(CallInst *CI, Function &F);

    // 2.2 沿地址传播链追踪入口 alloca，只允许当帧内的直接访存。
    bool allocaAddressEscapes(AllocaInst *AI);
    bool hasEscapingEntryAlloca(Function &F);

    // 1. 标记不改 CFG 的普通尾调用候选。
    bool markTailCalls(Function &F);

    // 3.1 从候选块尾部反向查找最后一个自调用。
    CallInst *findSelfCall(BasicBlock *BB, Function &F);

    // 3.3 证明 call..ret 区间的指令可安全跨自调用前移，或识别唯一整数 ADD/MUL 累加器。
    bool analyzeCallRetRange(CallInst *CI, RetInst *RI, Function &F,
                             FunctionAnalysisPassManager &FAM,
                             BinaryInst *&AccInst);

    // 4. 把单个已证明站点接入共享循环头，重写形参/累加器 SSA 并用回边替换调用。
    bool eliminateTailRecursion(BasicBlock *BB, CallInst *CI, RetInst *RI,
                                BinaryInst *AccInst, Function &F,
                                FunctionAnalysisPassManager &FAM,
                                BasicBlock *&LoopHeader,
                                std::vector<PhiInst *> &ArgPhis,
                                PhiInst *&AccPhi,
                                std::vector<BasicBlock *> &TailBackedgeBlocks);
};
