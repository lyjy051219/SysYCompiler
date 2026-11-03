// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#include "ir/opt/PassBuilder.hpp"
#include "ir/opt/Mem2Reg.hpp"
#include "ir/opt/CodeMotion.hpp"
#include "ir/opt/IPSCCP.hpp"
#include "ir/opt/DAE.hpp"
#include "ir/opt/DeadFunctionElimination.hpp"
#include "ir/opt/DCE.hpp"
#include "ir/opt/DSE.hpp"
#include "ir/opt/StoreMerge.hpp"
#include "ir/opt/LoadElimination.hpp"
#include "ir/opt/LoopLoadElimination.hpp"
#include "ir/opt/CFGSimpify.hpp"
#include "ir/opt/SCCP.hpp"
#include "ir/opt/InstSimplify.hpp"
#include "ir/opt/FuncInline.hpp"
#include "ir/opt/FunctionMemoization.hpp"
#include "ir/opt/LoopSimplify.hpp"
#include "ir/opt/LCSSA.hpp"
#include "ir/opt/LoopRotate.hpp"
#include "ir/opt/LICM.hpp"
#include "ir/opt/LoopUnswitch.hpp"
#include "ir/opt/LoopDeletion.hpp" 
#include "ir/opt/LoopExitValueSimplify.hpp"
#include "ir/opt/LoopUnroll.hpp"
#include "ir/opt/TailRecursionElimination.hpp"
#include "ir/opt/Reassociate.hpp"
#include "ir/opt/Local2Global.hpp"
#include "ir/opt/Global2Local.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/MemorySSA.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "ir/opt/UnifyFuncExits.hpp"
#include "ir/opt/BreakCriticalEdges.hpp"
#include "ir/opt/IfConversion.hpp"
#include "ir/opt/GVNPRE.hpp"
#include "ir/opt/GuardedLoopCodeMotion.hpp"
#include "ir/opt/IdempotentIterationCollapse.hpp"
#include "ir/opt/InvariantPrivateStoreHoist.hpp"
#include "ir/opt/ADCE.hpp"
#include "ir/opt/RangeAwareSimplify.hpp"
#include "ir/opt/ConstraintElimination.hpp"
#include "ir/opt/GepCombine.hpp"
#include "ir/opt/GepEvaluate.hpp"
#include "ir/opt/StoreOnlyGlobalElimination.hpp"
#include "ir/opt/ConstantHoist.hpp"
#include "ir/opt/DeadGlobalCleanup.hpp"
#include "ir/opt/LatePhiCleanup.hpp"
#include "ir/opt/LateStrengthReduce.hpp"
#include "ir/opt/LoopStrengthReduction.hpp"
#include "ir/opt/IndVarSimplify.hpp"
#include "ir/opt/LocalAggregateSimplify.hpp"
#include "ir/opt/GlobalInitializerFold.hpp"
#include "ir/opt/LoopIdiomRecognize.hpp"
#include "ir/opt/LoopPhaseRelayout.hpp"
#include "ir/opt/LoopInterchange.hpp"
#include "ir/opt/LoopDistribution.hpp"
#include "ir/opt/LoopFusion.hpp"
#include "ir/opt/UnrollAndJam.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/analysis/LoopDependence.hpp"
#include "ir/analysis/LoopAccessAnalysis.hpp"
#include "ir/analysis/TargetCostAnalysis.hpp"
#include "ir/analysis/BlockFrequencyInfo.hpp"
PassBuilder::PassBuilder() = default;
//注册函数级AnalysisPass
void PassBuilder::registerFunctionAnalyses(FunctionAnalysisPassManager &FAM){
    //通过定义表逐项实例化并注册全部函数分析
    #define FUNCTION_ANALYSIS(NAME,CREATE_PASS)\
        FAM.registerPass([&] {return CREATE_PASS;});
    #include "./FunctionAnalysisPass.def"
    #undef FUNCTION_ANALYSIS
}
//辅助pipeline1：
/*
CFGSimplify->Promote->SCCP->InstSimplify->Reassociate->LoopSimplify->RangeAwareSimplify
->LoopSimplify->ConstraintElimination->GepCombine->GepEvaluate->DCE->CFGSimplify
*/
FunctionPassManager PassBuilder::buildLightCleanupPipeline() {
    FunctionPassManager FPM;
    FPM.addPass(CFGSimplifyPass());
    FPM.addPass(PromotePass());
    FPM.addPass(SCCPPass());
    FPM.addPass(InstSimplifyPass());
    FPM.addPass(ReassociatePass());
    FPM.addPass(LoopSimplifyPass());
    FPM.addPass(RangeAwareSimplifyPass());
    FPM.addPass(LoopSimplifyPass());
    FPM.addPass(ConstraintEliminationPass());
    FPM.addPass(GepCombinePass());
    FPM.addPass(GepEvaluatePass());
    FPM.addPass(DCEPass());
    FPM.addPass(CFGSimplifyPass());
    return FPM;
}
//辅助pipeline3：重型内存清理
/*
BreakCriticalEdges->LocalGVN->GVNPRE->SCCP->Simplify->StoreMerge->LoadElimination->DSE->ADCE->Simplify
*/
FunctionPassManager PassBuilder::buildHeavyMemoryPipeline() {
    FunctionPassManager FPM;
    FPM.addPass(BreakCriticalEdgesPass());
    FPM.addPass(LocalGVNPass());
    FPM.addPass(GVNPREPass());
    FPM.addPass(SCCPPass());
    FPM.addPass(CFGSimplifyPass());
    //先把各控制流路径末尾的store合流成统一内存定义，再转发冗余load并删除，因此变得不可观察或被覆盖的store
    FPM.addPass(StoreMergePass());
    FPM.addPass(LoadEliminationPass());
    FPM.addPass(DSEPass());
    FPM.addPass(ADCEPass());
    FPM.addPass(CFGSimplifyPass());
    return FPM;
}
//辅助pipeline2：简单标量优化
/*
InstSimplify->Reassociate->LocalGVN->GVNPRE->DCE->CFGSimplify
*/
FunctionPassManager PassBuilder::buildScalarCleanupPipeline() {
    FunctionPassManager FPM;
    FPM.addPass(InstSimplifyPass());
    FPM.addPass(ReassociatePass( true));
    FPM.addPass(LocalGVNPass());
    FPM.addPass(GVNPREPass( false));
    FPM.addPass(DCEPass());
    FPM.addPass(CFGSimplifyPass());
    return FPM;
}
//构造循环展开后恢复标量、内存和循环规范形态的清理流水线
FunctionPassManager PassBuilder::buildPostUnrollCleanupPipeline() {
    //1. 展开后先折叠克隆产生的退化 PHI、常量表达式和死分支，恢复基本 CFG 可读性。
    FunctionPassManager FPM;
    FPM.addPass(CFGSimplifyPass());
    FPM.addPass(InstSimplifyPass());
    FPM.addPass(ReassociatePass( true));
    FPM.addPass(SCCPPass());
    FPM.addPass(CFGSimplifyPass());
    //2. 动态索引常在展开后变成有限常量集合；立即拆分局部聚合并 Mem2Reg，
    //让后续标量数据流看见各元素。
    FPM.addPass(LocalAggregateSimplifyPass());
    FPM.addPass(PromotePass());
    FPM.addPass(InstSimplifyPass());
    //3. 消费展开暴露的范围与路径约束，然后再次规范 CFG/表达式。
    FPM.addPass(RangeAwareSimplifyPass());
    FPM.addPass(ConstraintEliminationPass());
    FPM.addPass(CFGSimplifyPass());
    FPM.addPass(InstSimplifyPass());
    FPM.addPass(ReassociatePass( true));
    //4. 用本地及无 PRE 的全局值编号合并克隆表达式，随后关闭标量死图。
    FPM.addPass(LocalGVNPass());
    FPM.addPass(GVNPREPass( false));
    FPM.addPass(DCEPass());
    FPM.addPass(CFGSimplifyPass());
    //5. 地址合并/拆分后做 load forwarding 与 DSE，再以 late PHI/指令/DCE 链收尾。
    FPM.addPass(GepCombinePass());
    FPM.addPass(GepEvaluatePass());
    FPM.addPass(LoadEliminationPass());
    FPM.addPass(DSEPass());
    FPM.addPass(DCEPass());
    FPM.addPass(LatePhiCleanupPass());
    FPM.addPass(InstSimplifyPass());
    FPM.addPass(DCEPass());
    FPM.addPass(CFGSimplifyPass());
    //6. 在最后重建LoopSimplify+LCSSA，规范归纳变量并综合无副作用循环的live-out；
    //随后删除死循环，并立即让LICM消费最后一轮展开暴露的不变量。
    FPM.addPass(createFixedPointPass<Function, FunctionAnalysisPassManager>(
        LoopSimplifyPass(), 4));
    FPM.addPass(LCSSAPass());
    FPM.addPass(IndVarSimplifyPass());
    FPM.addPass(LoopExitValueSimplifyPass());
    FPM.addPass(LoopDeletionPass());
    FPM.addPass(LICMPass());
    return FPM;
}
//辅助pipeline4：循环优化
/*
BreakCriticalEdges->LoopSimplify->LCSSA->IndVarSimplify
->LoopInterchange->LoopDistribution->LoopInterchange->LoopFusion
->IdempotentIterationCollapse识别内存不动点并保留->UnrollAndJam
->LoopRotate->GuardedLoopCodeMotion->CFGSimplify->LoopSimplify->LCSSA->IndVarSimplify
->LoopExitValueSimplify->LICM->InvariantPrivateStoreHoist->ADCE->CFGSimplify
->LoopSimplify->LCSSA->LoopLoadElimination->LoopUnswitch
*/
FunctionPassManager PassBuilder::buildLoopPipeline() {
    FunctionPassManager FPM;
    FPM.addPass(BreakCriticalEdgesPass());
    FPM.addPass(createFixedPointPass<Function, FunctionAnalysisPassManager>(
        LoopSimplifyPass(), 8));
    FPM.addPass(LCSSAPass());
    //先规范归纳变量，再依次尝试 interchange、distribution、再次 interchange、
    //fusion 与有界 unroll-and-jam；前一步改变嵌套形态后，后一步重新发现机会
    FPM.addPass(IndVarSimplifyPass());
    FPM.addPass(LoopInterchangePass());
    FPM.addPass(LoopDistributionPass());
    FPM.addPass(LoopInterchangePass());
    FPM.addPass(LoopFusionPass());
    //在展开复制循环体之前识别完整覆盖形成的内存不动点
    FPM.addPass(IdempotentIterationCollapsePass());
    FPM.addPass(createFixedPointPass<Function, FunctionAnalysisPassManager>(
        UnrollAndJamPass(), 6));
    FPM.addPass(LoopRotatePass());
    FPM.addPass(createFixedPointPass<Function, FunctionAnalysisPassManager>(
        GuardedLoopCodeMotionPass(), 3));
    FPM.addPass(CFGSimplifyPass());
    FPM.addPass(createFixedPointPass<Function, FunctionAnalysisPassManager>(
        LoopSimplifyPass(), 8));
    FPM.addPass(LCSSAPass());
    FPM.addPass(IndVarSimplifyPass());
    FPM.addPass(LoopExitValueSimplifyPass());
    FPM.addPass(LICMPass());
    FPM.addPass(createFixedPointPass<Function, FunctionAnalysisPassManager>(
        InvariantPrivateStoreHoistPass(), 4));
    FPM.addPass(ADCEPass());
    FPM.addPass(CFGSimplifyPass());
    FPM.addPass(createFixedPointPass<Function, FunctionAnalysisPassManager>(
        LoopSimplifyPass(), 4));
    FPM.addPass(LCSSAPass());
    FPM.addPass(createFixedPointPass<Function, FunctionAnalysisPassManager>(
        LoopLoadEliminationPass(), 4));
    //4. Unswitch克隆CFG后消费新产生的边约束，再次恢复LoopSimplify+LCSSA。
    FPM.addPass(LoopUnswitchPass());
    FPM.addPass(CFGSimplifyPass());
    FPM.addPass(createFixedPointPass<Function, FunctionAnalysisPassManager>(
        LoopSimplifyPass(), 4));
    FPM.addPass(LCSSAPass());
    FPM.addPass(RangeAwareSimplifyPass());
    FPM.addPass(ConstraintEliminationPass());
    FPM.addPass(CFGSimplifyPass());
    FPM.addPass(createFixedPointPass<Function, FunctionAnalysisPassManager>(
        LoopSimplifyPass(), 2));
    FPM.addPass(LCSSAPass());
    //5. 识别内存循环idiom，再用无代码增长的路径下沉缩短纯值活跃区间，并以
    //标量闭合链综合循环live-out；随后清理残骸、删空循环并规范剩余地址归纳式。
    FPM.addPass(LoopIdiomRecognizePass());
    FPM.addPass(InstSimplifyPass());
    FPM.addPass(ReassociatePass( true));
    FPM.addPass(LocalGVNPass());
    FPM.addPass(GVNPREPass( false, true));
    FPM.addPass(LoopExitValueSimplifyPass());
    FPM.addPass(ADCEPass());
    FPM.addPass(DCEPass());
    FPM.addPass(LoopDeletionPass());
    FPM.addPass(LoopStrengthReductionPass());
    //6. 前面的CFG清理可能把空预头重新并入条件入口；展开前再次恢复
    //LoopSimplify+LCSSA，保证运行时版本化产生的快慢循环也能独立参与成本模型。
    FPM.addPass(createFixedPointPass<Function, FunctionAnalysisPassManager>(
        LoopSimplifyPass(), 8));
    FPM.addPass(LCSSAPass());
    //8. 最后展开剩余适合循环，并对展开结果运行最多两轮专用清理。
    UnrollConfig LoopUnrollConfig;
    //竞赛O1就是完整优化级别；通用runtime主段和尾副本使用完整RV64预算。
    LoopUnrollConfig.RuntimeThreshold = 192;
    FPM.addPass(LoopUnrollPass(LoopUnrollConfig));
    FPM.addPass(createFixedPointPass<Function, FunctionAnalysisPassManager>(
        buildPostUnrollCleanupPipeline(), 2));
    FPM.addPass(CFGSimplifyPass());
    FPM.addPass(DCEPass());
    return FPM;
}
//构造循环主流水线结束后的地址、内存和CFG清理流水线
FunctionPassManager PassBuilder::buildPostLoopCleanupPipeline() {
    //1. 循环变换后先清 CFG/指令/GEP 与局部值冗余，再删死值并执行 late 强度削减。
    FunctionPassManager FPM;
    FPM.addPass(CFGSimplifyPass());
    FPM.addPass(InstSimplifyPass());
    FPM.addPass(GepCombinePass());
    FPM.addPass(GepEvaluatePass());
    FPM.addPass(LocalGVNPass());
    FPM.addPass(CodeMotionPass());
    FPM.addPass(DCEPass());
    FPM.addPass(LateStrengthReducePass());
    //2. 强度削减产生的新地址与内存机会再走一轮化简、GEP、store合流和load/DSE；
    //最后清退化 PHI 和 CFG。
    FPM.addPass(InstSimplifyPass());
    FPM.addPass(GepCombinePass());
    FPM.addPass(GepEvaluatePass());
    FPM.addPass(StoreMergePass());
    FPM.addPass(LoadEliminationPass());
    FPM.addPass(DSEPass());
    FPM.addPass(CFGSimplifyPass());
    FPM.addPass(DCEPass());
    FPM.addPass(LatePhiCleanupPass());
    FPM.addPass(CFGSimplifyPass());
    return FPM;
}
//构造晚期重新发现并消费循环优化机会的流水线
FunctionPassManager PassBuilder::buildLateLoopRediscoveryPipeline() {
    //1. Late 标量化可能重新暴露循环：恢复关键边、LoopSimplify、LCSSA 和归纳变量。
    FunctionPassManager FPM;
    FPM.addPass(BreakCriticalEdgesPass());
    FPM.addPass(createFixedPointPass<Function, FunctionAnalysisPassManager>(
        LoopSimplifyPass(), 4));
    FPM.addPass(LCSSAPass());
    FPM.addPass(createFixedPointPass<Function, FunctionAnalysisPassManager>(
        IndVarSimplifyPass(), 2));
    //2. 先消费晚期标量化暴露的范围与路径事实，再重新运行LICM/idiom。
    FPM.addPass(RangeAwareSimplifyPass());
    FPM.addPass(ConstraintEliminationPass());
    FPM.addPass(CFGSimplifyPass());
    FPM.addPass(createFixedPointPass<Function, FunctionAnalysisPassManager>(
        LoopSimplifyPass(), 2));
    FPM.addPass(LCSSAPass());
    FPM.addPass(LICMPass());
    FPM.addPass(createFixedPointPass<Function, FunctionAnalysisPassManager>(
        LoopLoadEliminationPass(), 4));
    FPM.addPass(LoopIdiomRecognizePass());
    FPM.addPass(InstSimplifyPass());
    FPM.addPass(ReassociatePass( true));
    FPM.addPass(LocalGVNPass());
    FPM.addPass(GVNPREPass( false));
    FPM.addPass(LoopExitValueSimplifyPass());
    FPM.addPass(ADCEPass());
    FPM.addPass(DCEPass());
    FPM.addPass(CFGSimplifyPass());
    //3. 删除新变空循环并做强度、store合流、内存与CFG最终闭合。
    FPM.addPass(LoopDeletionPass());
    FPM.addPass(LoopStrengthReductionPass());
    FPM.addPass(StoreMergePass());
    FPM.addPass(LoadEliminationPass());
    FPM.addPass(DSEPass());
    FPM.addPass(DCEPass());
    FPM.addPass(CFGSimplifyPass());
    return FPM;
}

//构造晚期控制流化简与IfConversion流水线
FunctionPassManager PassBuilder::buildLateControlFlowPipeline() {
    //1. 先交替化简指令与 CFG，给 IfConversion 提供小型规范 diamond/triangle。
    FunctionPassManager FPM;
    FPM.addPass(CFGSimplifyPass());
    FPM.addPass(InstSimplifyPass());
    FPM.addPass(CFGSimplifyPass());
    //2. IfConversion 生成 select 后再次化简、删死值并反复清 CFG。
    FPM.addPass(IfConversionPass());
    FPM.addPass(InstSimplifyPass());
    FPM.addPass(CFGSimplifyPass());
    FPM.addPass(DCEPass());
    FPM.addPass(CFGSimplifyPass());
    return FPM;
}
//构造最终控制流、常量外提和函数出口规范化流水线
FunctionPassManager PassBuilder::buildLatePipeline() {
    //1. 对晚期控制流清理做三轮上限固定点，再清除残余死代码。
    FunctionPassManager LateFPM;
    LateFPM.addPass(createFixedPointPass<Function, FunctionAnalysisPassManager>(
        buildLateControlFlowPipeline(), 3));
    LateFPM.addPass(DCEPass());
    //2. 晚期 CFG 清理可能重新形成多入口循环；先恢复专用预头与 LCSSA，
    //让常量外提把值放在目标循环附近而不是拉长到函数入口。
    LateFPM.addPass(createFixedPointPass<Function, FunctionAnalysisPassManager>(
        LoopSimplifyPass(), 4));
    LateFPM.addPass(LCSSAPass());
    //3. 常量外提后统一函数出口，连续两次 PHI/CFG 清理消化新汇合结构。
    LateFPM.addPass(ConstantHoistPass());
    LateFPM.addPass(UnifyFuncExitsPass());
    LateFPM.addPass(LatePhiCleanupPass());
    LateFPM.addPass(CFGSimplifyPass());
    LateFPM.addPass(LatePhiCleanupPass());
    return LateFPM;
}

ModulePassManager PassBuilder::buildModulePipeline(OptimizationLevel Level, FunctionAnalysisPassManager &FAM) {
    //O0优化层级默认无中端优化
    ModulePassManager MPM;
    MPM.setIRMutationCallback([&FAM] { FAM.clear(); });
    if (Level == OptimizationLevel::O0) {
        return MPM;
    }
//优化阶段1：GlobalInitializerFold()消除前端引入的一部分memcpy + LightCleanupPipeline轻度清理
    MPM.addPass(GlobalInitializerFoldPass());
    MPM.addPass(createFixedPointPass<Module, ModuleAnalysisPassManager>(
        createModuleFunctionBridge(buildLightCleanupPipeline(), FAM), 2));
//优化阶段2：Local2Global->GlobalInitializerFold->StoreOnlyGlobalElimination针对全局的优化
    MPM.addPass(Local2GlobalPass());
    MPM.addPass(GlobalInitializerFoldPass());
    MPM.addPass(StoreOnlyGlobalEliminationPass(FAM));
//优化阶段3：IPO1：IPSCCP->SCCP->CFGSimplify->DAE->LightCleanupPipeline轻度清理
    /*IPO只传播全部调用共同成立的常量形参与常量返回值,随后让函数内SCCP消费新常量，再交给已有DAE收缩签名*/
    MPM.addPass(IPSCCPPass());
    MPM.addPass(createModuleFunctionBridge(SCCPPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(CFGSimplifyPass(), FAM));
    MPM.addPass(DAEPass());
    MPM.addPass(createModuleFunctionBridge(buildLightCleanupPipeline(), FAM));
//优化阶段4:暴露优化机会
    //TRE尾递归消除->LocalGVN公共子表达式消除->GVNPRE(不开启PRE并且不CodeSink)->Funcinline递归版函数内联->DFE死函数消除->LightCleanupPipeline轻度清理
    MPM.addPass(createModuleFunctionBridge(TailRecursionEliminationPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(LocalGVNPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(GVNPREPass( false), FAM));
    MPM.addPass(createModuleFunctionBridge(FuncInlinePass( true), FAM));
    MPM.addPass(DeadFunctionEliminationPass(FAM));
    MPM.addPass(createModuleFunctionBridge(buildLightCleanupPipeline(), FAM));
    //Funcinline线性子递归第一次->ScalarCleanupPipeline简单标量清理
    MPM.addPass(createModuleFunctionBridge(FuncInlinePass(LinearRecursiveInlineBudget{100, 16, 128}), FAM));
    MPM.addPass(createFixedPointPass<Module, ModuleAnalysisPassManager>(
        createModuleFunctionBridge(buildScalarCleanupPipeline(), FAM), 2));
    //Funcinline线性子递归第二次(增大预算)->ScalarCleanupPipeline简单标量清理
    MPM.addPass(createModuleFunctionBridge(FuncInlinePass(LinearRecursiveInlineBudget{200, 32, 256}), FAM));
    MPM.addPass(createModuleFunctionBridge(buildScalarCleanupPipeline(), FAM));
//优化阶段5:Global2Local->CFGSimplify->Promote->CFGSimplify(保守Global2Local把小全局放回main栈，紧接CFG/Mem2Reg让标量进入 SSA)
    MPM.addPass(createModuleFunctionBridge(Global2LocalPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(CFGSimplifyPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(PromotePass(), FAM));
    MPM.addPass(createModuleFunctionBridge(CFGSimplifyPass(), FAM));
//优化阶段6：DAE->LightCleanupPipeline轻度清理->LocalAggregateSimplify/SROA聚合变量化
    //->Promote->SCCP->InstSimplify->DCE->CFGSimplify->ScalarCleanupPipeline简单标量清理
    //->DAE第二次->ScalarCleanupPipeline简单标量清理->DFE->StoreOnlyGlobalElimination
    MPM.addPass(DAEPass());
    MPM.addPass(createFixedPointPass<Module, ModuleAnalysisPassManager>(
        createModuleFunctionBridge(buildLightCleanupPipeline(), FAM), 2));
    MPM.addPass(createModuleFunctionBridge(LocalAggregateSimplifyPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(PromotePass(), FAM));
    MPM.addPass(createModuleFunctionBridge(SCCPPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(InstSimplifyPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(DCEPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(CFGSimplifyPass(), FAM));
    MPM.addPass(createFixedPointPass<Module, ModuleAnalysisPassManager>(
        createModuleFunctionBridge(buildScalarCleanupPipeline(), FAM), 2));
    MPM.addPass(DAEPass());
    MPM.addPass(createModuleFunctionBridge(buildScalarCleanupPipeline(), FAM));
    MPM.addPass(DeadFunctionEliminationPass(FAM));
    MPM.addPass(StoreOnlyGlobalEliminationPass(FAM));
//优化阶段7:针对内存优化
    //IfConversion引入Select线性化BasicBlock暴露优化机会
    //HeavyMemoryPipeline重型内存优化pipeline
    MPM.addPass(createModuleFunctionBridge(IfConversionPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(buildHeavyMemoryPipeline(), FAM));
    //BreakCriticalEdges->LoopSimplify->LCSSA->IndVarSimplify规范化归纳变量->GepCombine->GepEvaluate->LoopPhaseRelayout二维布局优化
    MPM.addPass(createModuleFunctionBridge(BreakCriticalEdgesPass(), FAM));
    MPM.addPass(createFixedPointPass<Module, ModuleAnalysisPassManager>(
        createModuleFunctionBridge(LoopSimplifyPass(), FAM), 4));
    MPM.addPass(createModuleFunctionBridge(LCSSAPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(IndVarSimplifyPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(GepCombinePass(), FAM));
    MPM.addPass(createModuleFunctionBridge(GepEvaluatePass(), FAM));
    MPM.addPass(createFixedPointPass<Module, ModuleAnalysisPassManager>(LoopPhaseRelayoutPass(FAM), 4));
//优化阶段8：循环优化
    MPM.addPass(createFixedPointPass<Module, ModuleAnalysisPassManager>(
        createModuleFunctionBridge(buildLoopPipeline(), FAM), 3));
    MPM.addPass(createFixedPointPass<Module, ModuleAnalysisPassManager>(
        createModuleFunctionBridge(buildPostLoopCleanupPipeline(), FAM), 1));
    //11. 循环变换改变调用参数与常量后再运行DAE/标量清理；随后用低增长模型
    //内联新生成的专用小函数，并立即删除旧函数体与清理克隆表达式。
    MPM.addPass(DAEPass());
    MPM.addPass(createModuleFunctionBridge(buildScalarCleanupPipeline(), FAM));
    MPM.addPass(DeadFunctionEliminationPass(FAM));
    MPM.addPass(createFixedPointPass<Module, ModuleAnalysisPassManager>(
        createModuleFunctionBridge(FuncInlinePass( false, true), FAM), 3));
    MPM.addPass(DeadFunctionEliminationPass(FAM));
    MPM.addPass(createModuleFunctionBridge(buildLightCleanupPipeline(), FAM));
    MPM.addPass(createModuleFunctionBridge(buildScalarCleanupPipeline(), FAM));
    MPM.addPass(DeadFunctionEliminationPass(FAM));
    //12. 在主要内联/循环优化完成后为合适递归函数插入 memoization，避免早期缓存 CFG
    //干扰结构变换。
    MPM.addPass(FunctionMemoizationPass(FAM));
    //13. 宽松 Global2Local 与 Mem2Reg 再标量化缓存/模块清理暴露的小全局，随后运行
    //标量固定点和 late 循环再发现。
    MPM.addPass(createModuleFunctionBridge(Global2LocalPass(
         true), FAM));
    //late Global2Local可能重新暴露小局部聚合；直接复用已有的
    //常量路径标量化，再交给Mem2Reg进入SSA。
    MPM.addPass(createModuleFunctionBridge(
        LocalAggregateSimplifyPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(PromotePass(), FAM));
    MPM.addPass(createModuleFunctionBridge(CFGSimplifyPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(SCCPPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(RangeAwareSimplifyPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(ConstraintEliminationPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(CFGSimplifyPass(), FAM));
    MPM.addPass(createFixedPointPass<Module, ModuleAnalysisPassManager>(
        createModuleFunctionBridge(buildScalarCleanupPipeline(), FAM), 2));
    MPM.addPass(createModuleFunctionBridge(
        buildLateLoopRediscoveryPipeline(), FAM));
	//13. 最后一轮地址规范化后，只提升由该阶段重新暴露的循环不变GEP；随后执行
	//store合流、load、DSE、死值和CFG清理并进入late控制流。
	MPM.addPass(createModuleFunctionBridge(GepCombinePass(), FAM));
	MPM.addPass(createModuleFunctionBridge(GepEvaluatePass(), FAM));
	MPM.addPass(createModuleFunctionBridge(
		LICMPass(LICMMode::AddressOnly), FAM));
	MPM.addPass(createModuleFunctionBridge(StoreMergePass(), FAM));
    MPM.addPass(createModuleFunctionBridge(LoadEliminationPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(DSEPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(DCEPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(CFGSimplifyPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(buildLatePipeline(), FAM));
    //14. 所有GEP/GVN重建结束后再运行RV64跨基址原子LSR，使新建的
    //non-inbounds指针递推不再经过会重建地址语义的中端变换。
    MPM.addPass(createModuleFunctionBridge(
        LoopStrengthReductionPass( true), FAM));
    MPM.addPass(createModuleFunctionBridge(
        createFixedPointPass<Function, FunctionAnalysisPassManager>(
            LoopLoadEliminationPass(), 4), FAM));
    //15. 模块收尾删除只写/无 use 全局与死函数，确保新增特化、缓存和内联残骸不输出。
    MPM.addPass(DeadGlobalCleanupPass());
    MPM.addPass(DeadFunctionEliminationPass(FAM));
    return MPM;
}
