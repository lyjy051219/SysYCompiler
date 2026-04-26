#include "ir/opt/PassBuilder.hpp"
#include "ir/opt/Mem2Reg.hpp"
#include "ir/opt/DAE.hpp"
#include "ir/opt/DCE.hpp"
#include "ir/opt/DSE.hpp"
#include "ir/opt/LoadElimination.hpp"
#include "ir/opt/CFGSimpify.hpp"
#include "ir/opt/SCCP.hpp"
#include "ir/opt/InstSimplify.hpp"
#include "ir/opt/FuncInline.hpp"
#include "ir/opt/LoopSimplify.hpp"
#include "ir/opt/LCSSA.hpp"
#include "ir/opt/LoopRotate.hpp"
#include "ir/opt/LICM.hpp"
#include "ir/opt/LoopDeletion.hpp"
#include "ir/opt/TailRecursionElimination.hpp"
#include "ir/opt/Reassociate.hpp"
#include "ir/opt/Local2Global.hpp"
#include "ir/opt/Global2Local.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "ir/opt/UnifyFuncExits.hpp"
#include "ir/opt/BreakCriticalEdges.hpp"
#include "ir/opt/Select2Branch.hpp"
#include "ir/opt/IfConversion.hpp"
#include "ir/opt/GVNPRE.hpp"
#include "ir/opt/ADCE.hpp"
#include "ir/opt/RangeAwareSimplify.hpp"
#include "ir/opt/ConstraintElimination.hpp"
#include "ir/opt/GepOpt.hpp"
#include "ir/opt/StoreOnlyGlobalElimination.hpp"
#include "ir/opt/ConstantHoist.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/RangeAnalysis.hpp"

// Pipeline 阶段构建器 (Phase Builders)
// 1. 轻量级清理 Pipeline
static FunctionPassManager buildLightCleanupPipeline() {
    FunctionPassManager FPM;
    FPM.addPass(CFGSimplifyPass());
    FPM.addPass(PromotePass()); // Mem2Reg (将局部变量提升为寄存器)
    FPM.addPass(SCCPPass());    // 稀疏条件常量传播 (快速清理常数和不可达分支)
    FPM.addPass(InstSimplifyPass());
    FPM.addPass(ReassociatePass());

    // 约束/范围/GEP 清理链：按指定顺序插入
    FPM.addPass(LoopSimplifyPass());
    FPM.addPass(RangeAwareSimplifyPass());
    FPM.addPass(LoopSimplifyPass());
    FPM.addPass(ConstraintEliminationPass());
    FPM.addPass(GepCombinePass());
    FPM.addPass(GepEvaluatePass());

    FPM.addPass(DCEPass());     // 局部死代码消除 (快速)
    FPM.addPass(CFGSimplifyPass());
    return FPM;
}

// 2. 重量级内存与冗余优化 Pipeline
static FunctionPassManager buildHeavyMemoryPipeline() {
    FunctionPassManager FPM;
    FPM.addPass(BreakCriticalEdgesPass());

    // Step 1: 标量冗余消除 (GVN-PRE)
    FPM.addPass(GVNPREPass());

    // Step 2: 常量折叠与死分支消除
    FPM.addPass(SCCPPass());
    FPM.addPass(CFGSimplifyPass());

    // Step 3: 内存冗余消除
    FPM.addPass(LoadEliminationPass());

    // Step 4: 消除因为 LoadElim 暴露出来的无用 Store
    FPM.addPass(DSEPass());

    // Step 5: 全局死代码消除
    FPM.addPass(ADCEPass());
    FPM.addPass(CFGSimplifyPass());

    return FPM;
}

// 3. 循环优化 Pipeline
// 针对循环结构的深度优化，限制迭代次数，防止编译挂起
static FunctionPassManager buildLoopPipeline() {
    FunctionPassManager FPM;
    FPM.addPass(BreakCriticalEdgesPass());
    FPM.addPass(LoopSimplifyPass());
    FPM.addPass(LCSSAPass());
    FPM.addPass(LoopRotatePass());
    FPM.addPass(LICMPass());         // 循环不变量外提
    FPM.addPass(LoopDeletionPass()); // 消除死循环或无用循环
    FPM.addPass(CFGSimplifyPass());
    FPM.addPass(DCEPass());
    return FPM;
}

// 4. 晚期清理与后端准备 Pipeline
// 将 IR 规范化为后端代码生成器(CodeGen)最喜欢的形态
static FunctionPassManager buildLatePipeline() {
    FunctionPassManager LateFPM;
    LateFPM.addPass(CFGSimplifyPass());
    LateFPM.addPass(InstSimplifyPass());
    LateFPM.addPass(DCEPass());
    // 大整数常量提升（放在 InstSimplify 和 DCE 之后，免得 add 0,c 被折掉）
    LateFPM.addPass(ConstantHoistPass());
    // LateFPM.addPass(Select2BranchPass());
    LateFPM.addPass(UnifyFuncExitsPass());
    return LateFPM;
}


// PassBuilder 核心实现
void PassBuilder::registerModuleAnalyses(ModuleAnalysisPassManager &MAM){
    #define MODULE_ANALYSIS(NAME,CREATE_PASS)\
        MAM.registerPass([&] {return CREATE_PASS;});
    #include "./ModuleAnalysisPass.def"
    #undef MODULE_ANALYSIS
}

void PassBuilder::registerFunctionAnalyses(FunctionAnalysisPassManager &FAM){
    #define FUNCTION_ANALYSIS(NAME,CREATE_PASS)\
        FAM.registerPass([&] {return CREATE_PASS;});
    #include "./FunctionAnalysisPass.def"
    #undef FUNCTION_ANALYSIS
}

ModulePassManager PassBuilder::buildModulePipeline(OptimizationLevel Level, FunctionAnalysisPassManager &FAM) {
    ModulePassManager MPM;

    // O0 直接返回，不进行任何优化
    if (Level == OptimizationLevel::O0) {
        return MPM;
    }


    // Phase 1: Early Simplification (早期清理)
    // 目标：清理前端生成的冗余 Alloca、控制流，为 Inline 做准备
    // 限制迭代次数为 2 次，避免浪费时间
    MPM.addPass(createFixedPointPass<Module, ModuleAnalysisPassManager>(
        createModuleFunctionBridge(buildLightCleanupPipeline(), FAM), 2));

    // 全局化大数组
    MPM.addPass(Local2GlobalPass());

    // Phase 2: Interprocedural Optimization (IPO，过程间优化)
    MPM.addPass(createModuleFunctionBridge(TailRecursionEliminationPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(FuncInlinePass(/*enableRecursiveInline=*/true), FAM));
    MPM.addPass(createModuleFunctionBridge(Global2LocalPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(CFGSimplifyPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(PromotePass(), FAM));
    MPM.addPass(createModuleFunctionBridge(CFGSimplifyPass(), FAM));

    // 死参数消除 (Dead Argument Elimination)
    MPM.addPass(DAEPass());

    // 内联后会暴露出大量新的常数折叠、死代码和 Alloca 机会，再做一次轻量清理
    MPM.addPass(createFixedPointPass<Module, ModuleAnalysisPassManager>(
        createModuleFunctionBridge(buildLightCleanupPipeline(), FAM), 2));

    // 清理只写不读的全局变量 store，给后续内存优化减负
    MPM.addPass(StoreOnlyGlobalEliminationPass(FAM));

    // 提前做控制流转 select，便于后续 LICM 外提循环不变量
    MPM.addPass(createModuleFunctionBridge(IfConversionPass(), FAM));

    // Phase 3: Heavy Memory & Redundancy Elim (核心内存与冗余优化)
    // 目标：执行 GVN / LoadElim / DSE。绝对不放入 FixedPoint！顺序执行一次。
    MPM.addPass(createModuleFunctionBridge(buildHeavyMemoryPipeline(), FAM));

    // Phase 4: Loop Optimizations (循环优化)
    MPM.addPass(createFixedPointPass<Module, ModuleAnalysisPassManager>(
        createModuleFunctionBridge(buildLoopPipeline(), FAM), 3));

    // O2 及以上可以跑更多激进的后端准备优化，目前 O1 和 O2 复用流水线
    if (Level == OptimizationLevel::O2) {
        // 保留给未来更加激进的优化 (如 Auto-Vectorization 等)
    }

    // Phase 5: Late CFG / Backend Prep (晚期清理与后端准备)
    MPM.addPass(createModuleFunctionBridge(buildLatePipeline(), FAM));

    return MPM;
}