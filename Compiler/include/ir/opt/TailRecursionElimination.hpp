#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "lib/CFG.hpp"
#include <vector>
#include <unordered_map>
#include <unordered_set>

//TailRecursionEliminationPass标记普通尾调用并把安全的自尾递归改写为循环
class TailRecursionEliminationPass : public PassDescriptor<TailRecursionEliminationPass> {
public:
    //处理函数中的普通尾调用和全部可消除自尾递归站点
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    //把共享返回块的PHI或BitCast返回值解析为指定前驱可用的SSA值
    static Value *resolveRetValueForPred(
        Value *V, BasicBlock *Pred, BasicBlock *TargetBB,
        BasicBlock *InsertBB, Instruction *InsertBefore,
        std::vector<Instruction *> &ClonedInsts,
        std::unordered_set<Value *> &Visiting);
    //按指定前驱重写克隆返回指令的返回值操作数
    static bool rewriteClonedRetOperandForPred(
        RetInst *ClonedRet, BasicBlock *Pred, BasicBlock *TargetBB,
        BasicBlock *InsertBB, Instruction *InsertBefore,
        std::vector<Instruction *> &ClonedInsts);
    //判断指令是否不是调用或是只读型直接调用
    static bool isReadonlyLikeCallInst(
        Instruction *I, FunctionAnalysisPassManager &FAM, Function &F);
    //证明只读调用可跨自调用前移且不改变内存可观察次序
    static bool canMoveReadonlyCallBeforeSelfCall(
        CallInst *MovedCall, CallInst *SelfCall,
        FunctionAnalysisPassManager &FAM, Function &F);
    //证明调用必定返回且执行过程中不会故障或产生poison
    static bool isCallGuaranteedToReturnWithoutFault(
        CallInst *CI, std::unordered_set<Function *> &ActiveFunctions);
    //递归证明函数全部路径必定返回且不会故障或产生poison
    static bool isFunctionGuaranteedToReturnWithoutFault(
        Function &Callee, std::unordered_set<Function *> &ActiveFunctions);
    //判断load是否具有无需额外路径条件即可安全解引用的地址
    static bool isSafelyDereferenceableLoad(LoadInst *LI);
    //判断候选之前是否存在阻止改写的更早自调用
    static bool hasInterferingEarlierSelfCallInBlock(
        BasicBlock *BB, CallInst *StopCI, Function &F,
        FunctionAnalysisPassManager &FAM);
    //返回仅含PHI和唯一ret的基本块终结返回指令
    static RetInst *getRetOnlyTerminatorBlockRet(BasicBlock *BB);

    //判断函数中是否存在直接自递归调用
    bool hasSelfRecursion(Function &F);

    //判断调用是否直接调用当前函数
    bool isSelfCall(CallInst *CI, Function &F);

    //沿地址传播链判断入口alloca地址是否逃逸当前栈帧
    bool allocaAddressEscapes(AllocaInst *AI);
    //判断函数入口是否包含地址会逃逸的alloca
    bool hasEscapingEntryAlloca(Function &F);

    //标记紧邻返回且不泄漏当前栈地址的普通尾调用
    bool markTailCalls(Function &F);

    //从候选块尾部反向查找最后一个直接自调用
    CallInst *findSelfCall(BasicBlock *BB, Function &F);

    //证明调用至返回区间可前移或匹配唯一整数加乘累加器
    bool analyzeCallRetRange(CallInst *CI, RetInst *RI, Function &F,
                             FunctionAnalysisPassManager &FAM,
                             BinaryInst *&AccInst);

    //把单个自尾递归站点接入共享循环头并修复参数和累加器SSA
    bool eliminateTailRecursion(BasicBlock *BB, CallInst *CI, RetInst *RI,
                                BinaryInst *AccInst, Function &F,
                                FunctionAnalysisPassManager &FAM,
                                BasicBlock *&LoopHeader,
                                std::vector<PhiInst *> &ArgPhis,
                                PhiInst *&AccPhi,
                                std::vector<BasicBlock *> &TailBackedgeBlocks);
};
