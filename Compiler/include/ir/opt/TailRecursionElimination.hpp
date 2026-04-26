#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "lib/CFG.hpp"
#include <vector>
#include <unordered_map>
#include <unordered_set>

// 尾递归消除 (Tail Recursion Elimination) Pass
class TailRecursionEliminationPass : public PassDescriptor<TailRecursionEliminationPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:

    // 判断函数是否包含自递归调用
    bool hasSelfRecursion(Function &F);

    // 判断某个 CallInst 是否是对本函数的调用
    bool isSelfCall(CallInst *CI, Function &F);

    // 标记非自递归的尾调用 (call 紧接 ret, 但 call 目标 ≠ 当前函数)
    bool markTailCalls(Function &F);

    // ===== 自递归尾递归消除 =====

    // 在基本块 BB 中从末尾向前查找对自身的 CallInst
    CallInst *findSelfCall(BasicBlock *BB, Function &F);

    // 判断 call 与 ret 之间的指令能否移到 call 之前
    // 同时检测累加器模式 (ADD/MUL)
    // 返回 true 表示此尾递归可以消除
    // AccInst 输出检测到的累加器指令 (可为 nullptr)
    bool analyzeCallRetRange(CallInst *CI, RetInst *RI, Function &F,
                             FunctionAnalysisPassManager &FAM,
                             BinaryInst *&AccInst);

    // 执行尾递归 → 循环 变换 (单个基本块)
    // 返回 true 表示发生了变换
    bool eliminateTailRecursion(BasicBlock *BB, CallInst *CI, RetInst *RI,
                                BinaryInst *AccInst, Function &F,
                                FunctionAnalysisPassManager &FAM,
                                BasicBlock *&LoopHeader,
                                std::vector<PhiInst *> &ArgPhis,
                                PhiInst *&AccPhi);
};
