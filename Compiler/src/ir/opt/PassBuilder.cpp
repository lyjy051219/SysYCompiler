#include "ir/opt/PassBuilder.hpp"
#include "ir/opt/Mem2Reg.hpp"
#include "ir/opt/DAE.hpp"
#include "ir/opt/DeadFunctionElimination.hpp"
#include "ir/opt/DCE.hpp"
#include "ir/opt/DSE.hpp"
#include "ir/opt/StoreMerge.hpp"
#include "ir/opt/LoadElimination.hpp"
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
#include "ir/opt/LoopInterchange.hpp"
#include "ir/opt/LoopDistribution.hpp"
#include "ir/opt/LoopFusion.hpp"
#include "ir/opt/UnrollAndJam.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/analysis/LoopDependence.hpp"
#include "ir/analysis/LoopAccessAnalysis.hpp"
PassBuilder::PassBuilder() = default;
//注册函数级AnalysisPass
void PassBuilder::registerFunctionAnalyses(FunctionAnalysisPassManager &FAM){
    //通过定义表逐项实例化并注册全部函数分析，registerPass负责去重和跨IR属性登记
    #define FUNCTION_ANALYSIS(NAME,CREATE_PASS)\
        FAM.registerPass([&] {return CREATE_PASS;});
    #include "./FunctionAnalysisPass.def"
    #undef FUNCTION_ANALYSIS
}
//构造建立SSA、简化标量表达式和CFG的轻量清理流水线
FunctionPassManager PassBuilder::buildLightCleanupPipeline() {
    //1. 先简化 CFG 并把可提升局部内存转成 SSA，再由 SCCP、InstSimplify 和
    //Reassociate 收敛常量与标量表达式。
    FunctionPassManager FPM;
    FPM.addPass(CFGSimplifyPass());
    FPM.addPass(PromotePass());
    FPM.addPass(SCCPPass());
    FPM.addPass(InstSimplifyPass());
    FPM.addPass(ReassociatePass());
    //2. 建立规范循环形态后消费范围/路径约束，再合并和拆分 GEP，形成稳定地址表达式。
    FPM.addPass(LoopSimplifyPass());
    FPM.addPass(RangeAwareSimplifyPass());
    FPM.addPass(LoopSimplifyPass());
    FPM.addPass(ConstraintEliminationPass());
    FPM.addPass(GepCombinePass());
    FPM.addPass(GepEvaluatePass());
    //3. 删除前序阶段暴露的死值，并以 CFGSimplify 清掉常量边与不可达块。
    FPM.addPass(DCEPass());
    FPM.addPass(CFGSimplifyPass());
    return FPM;
}
//构造执行值编号、Load消除和死存储清理的重型内存流水线
FunctionPassManager PassBuilder::buildHeavyMemoryPipeline() {
    //1. 该链包含会重建大范围值流/内存状态的优化，由模块 pipeline 顺序执行一次；
    //先拆关键边为跨块值合流提供插入位置，再用 LocalGVN 清除块内重复表达式。
    FunctionPassManager FPM;
    FPM.addPass(BreakCriticalEdgesPass());
    FPM.addPass(LocalGVNPass());
    //2. 全局 GVN/PRE 合并支配冗余并补齐部分冗余路径。
    FPM.addPass(GVNPREPass());
    //3. 值编号暴露的常量由 SCCP 消费，随后立即清除死分支。
    FPM.addPass(SCCPPass());
    FPM.addPass(CFGSimplifyPass());
    //4. 先把各控制流路径末尾的store合流成统一内存定义，再转发冗余load并删除
    //因此变得不可观察或被覆盖的store。
    FPM.addPass(StoreMergePass());
    FPM.addPass(LoadEliminationPass());
    FPM.addPass(DSEPass());
    //5. ADCE 沿数据/控制依赖清理整个死图，最后再次规范 CFG。
    FPM.addPass(ADCEPass());
    FPM.addPass(CFGSimplifyPass());
    return FPM;
}
//构造收敛标量表达式与控制流的清理流水线
FunctionPassManager PassBuilder::buildScalarCleanupPipeline() {
    //1. 局部化简与保持地址算术的重关联先规范表达式树。
    FunctionPassManager FPM;
    FPM.addPass(InstSimplifyPass());
    FPM.addPass(ReassociatePass( true));
    //2. LocalGVN 后接关闭 PRE 的全局值编号，只合并已有冗余，控制清理成本与代码增长。
    FPM.addPass(LocalGVNPass());
    FPM.addPass(GVNPREPass( false));
    //3. 删除旧表达式树和由替换产生的死 CFG。
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
//构造循环规范化、结构变换和循环优化的主流水线
FunctionPassManager PassBuilder::buildLoopPipeline() {
    //1. 拆关键边并把循环规范到 LoopSimplify+LCSSA，作为全部结构变换的共同前置条件。
    FunctionPassManager FPM;
    FPM.addPass(BreakCriticalEdgesPass());
    FPM.addPass(createFixedPointPass<Function, FunctionAnalysisPassManager>(
        LoopSimplifyPass(), 8));
    FPM.addPass(LCSSAPass());
    //2. 先规范归纳变量，再依次尝试 interchange、distribution、再次 interchange、
    //fusion 与有界 unroll-and-jam；前一步改变嵌套形态后，后一步重新发现机会。
    FPM.addPass(IndVarSimplifyPass());
    FPM.addPass(LoopInterchangePass());
    FPM.addPass(LoopDistributionPass());
    FPM.addPass(LoopInterchangePass());
    FPM.addPass(LoopFusionPass());
    FPM.addPass(createFixedPointPass<Function, FunctionAnalysisPassManager>(
        UnrollAndJamPass(), 6));
    //3. Rotate 改变 header/latch 后立刻清 CFG、重建 LoopSimplify/LCSSA/IV，并运行 LICM。
    FPM.addPass(LoopRotatePass());
    FPM.addPass(CFGSimplifyPass());
    FPM.addPass(createFixedPointPass<Function, FunctionAnalysisPassManager>(
        LoopSimplifyPass(), 8));
    FPM.addPass(LCSSAPass());
    FPM.addPass(IndVarSimplifyPass());
    FPM.addPass(LoopExitValueSimplifyPass());
    FPM.addPass(LICMPass());
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
    //6. 最后展开剩余适合循环，并对展开结果运行最多两轮专用清理，末尾关闭 CFG/死值。
    FPM.addPass(LoopUnrollPass());
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
    //2. 常量外提后统一函数出口，连续两次 PHI/CFG 清理消化新汇合结构。
    LateFPM.addPass(ConstantHoistPass());
    LateFPM.addPass(UnifyFuncExitsPass());
    LateFPM.addPass(LatePhiCleanupPass());
    LateFPM.addPass(CFGSimplifyPass());
    LateFPM.addPass(LatePhiCleanupPass());
    return LateFPM;
}

ModulePassManager PassBuilder::buildModulePipeline(OptimizationLevel Level, FunctionAnalysisPassManager &FAM) {
    //1. 模块 pipeline 把任意模块 IR 变化回调到 FAM.clear，防止跨函数摘要陈旧；
    //O0 保持空 pipeline，当前 O1/O2 共用下方完整顺序。
    ModulePassManager MPM;
    MPM.setIRMutationCallback([&FAM] { FAM.clear(); });
    if (Level == OptimizationLevel::O0) {
        return MPM;
    }
    //2. 启动阶段先静态化 main 入口全局拷贝，再用两轮轻量函数清理建立 SSA 基线。
    MPM.addPass(GlobalInitializerFoldPass());
    MPM.addPass(createFixedPointPass<Module, ModuleAnalysisPassManager>(
        createModuleFunctionBridge(buildLightCleanupPipeline(), FAM), 2));
    //3. 大栈对象全局化后再次折叠其初始化，并删除只写全局与无用索引写。
    MPM.addPass(Local2GlobalPass());
    MPM.addPass(GlobalInitializerFoldPass());
    MPM.addPass(StoreOnlyGlobalEliminationPass(FAM));
    //4. 先消除尾递归并做低成本 GVN，再积极内联；随后删除死函数并以标量固定点
    //消费内联暴露的表达式图。
    MPM.addPass(createModuleFunctionBridge(TailRecursionEliminationPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(LocalGVNPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(
        GVNPREPass( false), FAM));
    MPM.addPass(createModuleFunctionBridge(FuncInlinePass( true), FAM));
    MPM.addPass(DeadFunctionEliminationPass(FAM));
    MPM.addPass(createFixedPointPass<Module, ModuleAnalysisPassManager>(
        createModuleFunctionBridge(buildScalarCleanupPipeline(), FAM), 2));
    //5. 保守 Global2Local 把小全局放回 main 栈，紧接 CFG/Mem2Reg 让标量进入 SSA。
    MPM.addPass(createModuleFunctionBridge(Global2LocalPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(CFGSimplifyPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(PromotePass(), FAM));
    MPM.addPass(createModuleFunctionBridge(CFGSimplifyPass(), FAM));
    //6. DAE 做参数特化/消除，再以轻量、聚合标量化和标量固定点清理专用函数体。
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
    //7. 第二轮 DAE 捕获首次清理暴露的跨调用机会，之后再次删除死函数和只写全局。
    MPM.addPass(DAEPass());
    MPM.addPass(createModuleFunctionBridge(buildScalarCleanupPipeline(), FAM));
    MPM.addPass(DeadFunctionEliminationPass(FAM));
    MPM.addPass(StoreOnlyGlobalEliminationPass(FAM));
    //8. 在重型内存阶段前先做一次 if-conversion，再依次运行 GVN/PRE、load、DSE、ADCE。
    MPM.addPass(createModuleFunctionBridge(IfConversionPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(buildHeavyMemoryPipeline(), FAM));
    //9. 循环主 pipeline 以模块级三轮固定点运行，随后做一次专用 post-loop 闭合。
    MPM.addPass(createFixedPointPass<Module, ModuleAnalysisPassManager>(
        createModuleFunctionBridge(buildLoopPipeline(), FAM), 3));
    MPM.addPass(createFixedPointPass<Module, ModuleAnalysisPassManager>(
        createModuleFunctionBridge(buildPostLoopCleanupPipeline(), FAM), 1));
    //10. 循环变换改变调用参数与常量后再运行DAE/标量清理；随后用低增长模型
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
    //11. 在主要内联/循环优化完成后为合适递归函数插入 memoization，避免早期缓存 CFG
    //干扰结构变换。
    MPM.addPass(FunctionMemoizationPass(FAM));
    //12. 宽松 Global2Local 与 Mem2Reg 再标量化缓存/模块清理暴露的小全局，随后运行
    //标量固定点和 late 循环再发现。
    MPM.addPass(createModuleFunctionBridge(Global2LocalPass(
         true), FAM));
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
    //13. 最后一轮地址、store合流、load、DSE、死值和CFG清理后进入late控制流。
    MPM.addPass(createModuleFunctionBridge(GepCombinePass(), FAM));
    MPM.addPass(createModuleFunctionBridge(GepEvaluatePass(), FAM));
    MPM.addPass(createModuleFunctionBridge(StoreMergePass(), FAM));
    MPM.addPass(createModuleFunctionBridge(LoadEliminationPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(DSEPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(DCEPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(CFGSimplifyPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(buildLatePipeline(), FAM));
    //14. 模块收尾删除只写/无 use 全局与死函数，确保新增特化、缓存和内联残骸不输出。
    MPM.addPass(DeadGlobalCleanupPass());
    MPM.addPass(DeadFunctionEliminationPass(FAM));
    return MPM;
}
