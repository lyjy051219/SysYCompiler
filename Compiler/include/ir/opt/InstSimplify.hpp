// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

//BasicAAResult表示函数级基础别名分析结果
class BasicAAResult;
//DominatorTree表示函数级支配关系分析结果
class DominatorTree;
//LoopInfo表示函数的循环森林分析结果
class LoopInfo;
//MemorySSAResult表示函数的内存SSA分析结果
class MemorySSAResult;

//InstSimplifier通过工作表反复应用局部代数化简、折叠和等价值替换
class InstSimplifier {
public:
    //构造绑定函数及支配、别名、循环和MemorySSA结果的局部化简器
    InstSimplifier(DominatorTree *DT, Function *F,
        FunctionAnalysisPassManager *FAM, BasicAAResult *AA, LoopInfo *LI,
        MemorySSAResult *MSSA);

    //运行局部指令化简工作表直到达到固定点
    bool run(Function &F);
 
private:

    //ExtremumKind枚举比较选择结构能够表示的极值种类
    enum class ExtremumKind {
        //当前结构不表示极值
        None,
        //当前结构表示最小值
        Minimum,
        //当前结构表示最大值
        Maximum,
    };

    //当前函数对应的支配树分析结果
    DominatorTree *DT = nullptr;
    //当前正在化简的函数
    Function *F = nullptr;
    //当前函数使用的分析管理器
    FunctionAnalysisPassManager *FAM = nullptr;
    //当前函数对应的基础别名分析结果
    BasicAAResult *AA = nullptr;
    //当前函数对应的循环分析结果
    LoopInfo *LI = nullptr;
    //当前函数对应的MemorySSA分析结果
    MemorySSAResult *MSSA = nullptr;
    //标记当前工作流中MemorySSA是否仍可安全增量维护
    bool MemorySSAUsable = true;

    //判断Load是否能够安全下沉到PHI各输入边
    bool isLoadSuitableForSinking(LoadInst *L) const;
    //尝试把二元指令跨PHI各输入臂分解并折叠
    Value *foldPhiOfBinary(PhiInst *Phi);
    //尝试把GEP跨PHI各输入臂分解并折叠
    Value *foldPhiOfGEP(PhiInst *Phi);
    //尝试把Load跨PHI各输入臂安全下沉
    Value *foldPhiOfLoad(PhiInst *Phi);
    //按照指令种类分派并返回等价的化简结果
    Value *simplifyInstruction(Instruction *I, bool &Changed);
    //按照规范顺序交换可交换二元操作数
    static bool maybeSwapCommutative(
        BinaryInst *BI, Value *&LHS, Value *&RHS, bool &Changed);
    //应用加法恒等式、结合律和常量折叠规则
    Value *simplifyAdd(
        BinaryInst *BI, Value *LHS, Value *RHS, bool &Changed);
    //应用减法恒等式和相消规则
    Value *simplifySub(BinaryInst *BI, Value *LHS, Value *RHS);
    //应用乘法恒等式、结合律和常量折叠规则
    Value *simplifyMul(
        BinaryInst *BI, Value *LHS, Value *RHS, bool &Changed);
    //应用有符号除法恒等式和安全相消规则
    Value *simplifyDiv(BinaryInst *BI, Value *LHS, Value *RHS);
    //应用有符号取余恒等式和安全相消规则
    Value *simplifyRem(BinaryInst *BI, Value *LHS, Value *RHS);
    //应用按位与恒等式、吸收律和常量折叠规则
    Value *simplifyAnd(
        BinaryInst *BI, Value *LHS, Value *RHS, bool &Changed);
    //应用按位或恒等式、吸收律和常量折叠规则
    Value *simplifyOr(
        BinaryInst *BI, Value *LHS, Value *RHS, bool &Changed);
    //应用按位异或恒等式和常量折叠规则
    Value *simplifyXor(
        BinaryInst *BI, Value *LHS, Value *RHS, bool &Changed);
    //化简整数或浮点比较并保持对应谓词语义
    Value *simplifyCmp(BinaryInst *BI, Value *LHS, Value *RHS);
    //规范化二元操作数并分派到具体代数化简规则
    Value *simplifyBinary(BinaryInst *BI, bool &Changed);
    //化简条件选择并识别布尔与极值模式
    Value *simplifySelect(SelectInst *SI);
    //折叠PHI公共输入并尝试跨臂合并等价操作
    Value *simplifyPhi(PhiInst *Phi);
};

//InstSimplifyPass是运行函数级局部指令化简的优化Pass
class InstSimplifyPass : public PassDescriptor<InstSimplifyPass> {
public:
    //运行指令化简并返回能够保留的分析集合
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    //允许InstSimplifier复用本Pass的代数模式匹配辅助函数
    friend class InstSimplifier;
    //匹配给定操作码的二元表达式并返回两个操作数
    static bool getBinaryOperandsForOp(
        Value *V, BinaryInst::Operation Op, Value *&LHS, Value *&RHS);
    //匹配零减某值表示的算术取负
    static bool matchNegation(Value *V, Value *&Operand);
    //匹配任意减法表达式并返回两个操作数
    static bool matchSub(Value *V, Value *&LHS, Value *&RHS);
    //判断两个值是否构成互为算术相反数的表达式
    static bool isNegationPair(Value *A, Value *B);
    //匹配包含指定X的Y-X表达式并返回Y
    static bool matchYMinusX(Value *MaybeSub, Value *X, Value *&Y);
};
