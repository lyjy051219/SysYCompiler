#include "ir/opt/PassBuilder.hpp"
#include "ir/opt/Mem2Reg.hpp"
#include "ir/opt/DAE.hpp"
#include "ir/opt/DeadFunctionElimination.hpp"
#include "ir/opt/DCE.hpp"
#include "ir/opt/DSE.hpp"
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
#include "ir/opt/GepOpt.hpp"
#include "ir/opt/StoreOnlyGlobalElimination.hpp"
#include "ir/opt/ConstantHoist.hpp"
#include "ir/opt/LateDeadGlobalCleanup.hpp"
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

// Pipeline 阶段构建器 (Phase Builders)
// 1. 轻量级清理 Pipeline
FunctionPassManager PassBuilder::buildLightCleanupPipeline() {
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
FunctionPassManager PassBuilder::buildHeavyMemoryPipeline() {
    FunctionPassManager FPM;
    FPM.addPass(BreakCriticalEdgesPass());

    // Always run the linear local value-numbering tier.  Full GVN/PRE keeps
    // its compile-time budget and may reject very large functions, whereas
    // LocalGVN still removes same-block GEP/arithmetic/cast redundancy.
    FPM.addPass(LocalGVNPass());

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

// A bounded scalar closure used after transforms that expose a large new
// expression graph (inlining, aggregate splitting, unrolling and relaxed
// Global2Local).  The order is intentional: local canonicalization gives the
// latency-aware reassociation pass a stable tree, global value numbering merges
// the resulting expressions, and DCE/CFG removes both old trees and newly
// constant edges.
FunctionPassManager PassBuilder::buildScalarCleanupPipeline() {
    FunctionPassManager FPM;
    FPM.addPass(InstSimplifyPass());
    FPM.addPass(ReassociatePass(/*PreserveAddressArithmetic=*/true));
    FPM.addPass(LocalGVNPass());
    FPM.addPass(GVNPREPass(/*EnablePRE=*/false));
    FPM.addPass(DCEPass());
    FPM.addPass(CFGSimplifyPass());
    return FPM;
}

// 2.5 展开后深度清理 Pipeline
// LoopUnroll 会复制控制流、扩大基本块并暴露退化 PHI / 常量分支 / 死副本。
// 这里使用偏局部、确定性的清理组合，收敛展开带来的直接噪声，再把循环形态
// 修回 LoopSimplify + LCSSA，并立即让 LICM 消费最后一轮展开暴露的不变式。
FunctionPassManager PassBuilder::buildPostUnrollCleanupPipeline() {
    FunctionPassManager FPM;
    FPM.addPass(CFGSimplifyPass());
    FPM.addPass(InstSimplifyPass());
    FPM.addPass(ReassociatePass(/*PreserveAddressArithmetic=*/true));
    FPM.addPass(SCCPPass());
    FPM.addPass(CFGSimplifyPass());
    // Full/partial unrolling frequently turns dynamic aggregate indices into
    // a small constant set.  Split/fold those accesses immediately and
    // promote the newly exposed scalar allocas before value numbering.
    FPM.addPass(LocalAggregateSimplifyPass());
    FPM.addPass(PromotePass());
    FPM.addPass(InstSimplifyPass());


    FPM.addPass(RangeAwareSimplifyPass());
    FPM.addPass(ConstraintEliminationPass());
    FPM.addPass(CFGSimplifyPass());
    FPM.addPass(InstSimplifyPass());
    FPM.addPass(ReassociatePass(/*PreserveAddressArithmetic=*/true));
    FPM.addPass(LocalGVNPass());
    FPM.addPass(GVNPREPass(/*EnablePRE=*/false));
    // Close the scalar graph immediately after unrolling so cloned dead
    // expressions and newly constant branches do not survive into the
    // memory-address cleanup below.  Reassociation keeps high-latency lane
    // dependencies near the root instead of extending the serial chain.
    FPM.addPass(DCEPass());
    FPM.addPass(CFGSimplifyPass());
    FPM.addPass(GepCombinePass());
    FPM.addPass(GepEvaluatePass());
    FPM.addPass(LoadEliminationPass());
    FPM.addPass(DSEPass());
    FPM.addPass(DCEPass());
    FPM.addPass(LatePhiCleanupPass());
    FPM.addPass(InstSimplifyPass());
    FPM.addPass(DCEPass());
    FPM.addPass(CFGSimplifyPass());
    FPM.addPass(createFixedPointPass<Function, FunctionAnalysisPassManager>(
        LoopSimplifyPass(), 4));
    FPM.addPass(LCSSAPass());
    FPM.addPass(IndVarSimplifyPass());
    FPM.addPass(LoopDeletionPass());
    // Do not rely on the next outer fixed-point iteration: the final allowed
    // unroll round must also get a chance to hoist newly exposed invariants.
    FPM.addPass(LICMPass());
    return FPM;
}

// 3. 循环优化 Pipeline
// 针对循环结构的深度优化，限制迭代次数，防止编译挂起
FunctionPassManager PassBuilder::buildLoopPipeline() {
    FunctionPassManager FPM;
    FPM.addPass(BreakCriticalEdgesPass());
    FPM.addPass(createFixedPointPass<Function, FunctionAnalysisPassManager>(
        LoopSimplifyPass(), 8));
    FPM.addPass(LCSSAPass());

    // 在旋转和强度削弱改变仿射坐标前完成循环调度。分布先隔离局部性需求
    // 不同且依赖允许拆分的语句，随后由交换、融合和展开合并继续优化。
    FPM.addPass(IndVarSimplifyPass());
    FPM.addPass(LoopInterchangePass());
    // 交换可能把不同局部性需求的语句暴露在同一内层循环中；先按依赖
    // 证明做循环分布，后续交换才能分别为每个分区选择合适的轴顺序。
    FPM.addPass(LoopDistributionPass());
    FPM.addPass(LoopInterchangePass());
    FPM.addPass(LoopFusionPass());
    // 余数前缀剥离会主动失效循环分析；在普通展开前用同一个 UAJ 实例
    // 重建分析并收敛，避免下一层候选先被普通展开改变结构。
    FPM.addPass(createFixedPointPass<Function, FunctionAnalysisPassManager>(
        UnrollAndJamPass(), 6));

    // 先旋转/规范化，再重新建立 LoopSimplify + LCSSA，让 Unroll 看到更稳定的
    // trip-count 和 latch/header 形态。
    FPM.addPass(LoopRotatePass());
    FPM.addPass(CFGSimplifyPass());
    FPM.addPass(createFixedPointPass<Function, FunctionAnalysisPassManager>(
        LoopSimplifyPass(), 8));
    FPM.addPass(LCSSAPass());
    FPM.addPass(IndVarSimplifyPass());
    FPM.addPass(LICMPass());         // 循环不变量外提
    // LICM makes full-unswitch conditions available at the preheader.
    // Unswitch changes both CFG and LCSSA exit structure, so immediately
    // remove specialized dead edges and restore the canonical loop form
    // before idiom recognition, LSR and unrolling consume it.
    FPM.addPass(LoopUnswitchPass());
    FPM.addPass(CFGSimplifyPass());
    FPM.addPass(createFixedPointPass<Function, FunctionAnalysisPassManager>(
        LoopSimplifyPass(), 4));
    FPM.addPass(LCSSAPass());
    // Convert the fully-proven constant-trip contiguous copy/fill subset to
    // memory intrinsics before unrolling duplicates the scalar operations.
    FPM.addPass(LoopIdiomRecognizePass());
    FPM.addPass(InstSimplifyPass());
    FPM.addPass(ReassociatePass(/*PreserveAddressArithmetic=*/true));
    FPM.addPass(LocalGVNPass());
    FPM.addPass(GVNPREPass(/*EnablePRE=*/false));
    FPM.addPass(ADCEPass());
    // Preserve affine GEP chains until LSR has converted them to pointer
    // recurrences.  CFG simplification at this exact point rewrites the chain
    // just before LSR and makes memory-heavy stencil loops recompute
    // multidimensional addresses.  CFG cleanup already runs in the
    // post-unroll and post-loop pipelines after the recurrence is captured.
    FPM.addPass(DCEPass());
    FPM.addPass(LoopDeletionPass()); // 消除死循环或无用循环
    FPM.addPass(LoopStrengthReductionPass()); // GEP 地址归纳变量强度削弱
    // One invocation considers loops from its entry snapshot, subject to a
    // function-wide transform budget.  Each CFG mutation rebuilds DT/LI/SCEV;
    // an original header is not partially unrolled again in a later fixed-
    // point round, while newly-created residual loops remain eligible.
    FPM.addPass(LoopUnrollPass());
    FPM.addPass(createFixedPointPass<Function, FunctionAnalysisPassManager>(
        buildPostUnrollCleanupPipeline(), 2));
    FPM.addPass(CFGSimplifyPass());
    FPM.addPass(DCEPass());
    return FPM;
}

// 3.1 Loop 后收敛 Pipeline
// LoopRotate/LICM/LoopDeletion 会暴露新的局部代数机会和 LCSSA/exit PHI。
// 这里刻意不用完整 RangeAware/Constraint/GVN/内存清理，避免 late CFG 上的
// 路径范围误判；只做控制流中性、可局部证明正确的收敛。
FunctionPassManager PassBuilder::buildPostLoopCleanupPipeline() {
    FunctionPassManager FPM;
    FPM.addPass(CFGSimplifyPass());
    FPM.addPass(InstSimplifyPass());
    FPM.addPass(GepCombinePass());
    FPM.addPass(GepEvaluatePass());
    FPM.addPass(LocalGVNPass());
    FPM.addPass(DCEPass());
    FPM.addPass(LateStrengthReducePass());
    FPM.addPass(InstSimplifyPass());
    FPM.addPass(GepCombinePass());
    FPM.addPass(GepEvaluatePass());
    FPM.addPass(LoadEliminationPass());
    FPM.addPass(DSEPass());
    FPM.addPass(CFGSimplifyPass());
    FPM.addPass(DCEPass());
    FPM.addPass(LatePhiCleanupPass());
    FPM.addPass(CFGSimplifyPass());
    return FPM;
}

// Relaxed Global2Local intentionally runs late because it can expose scalar
// state that earlier range reasoning treated as memory.  Rediscover only the
// analyses and transforms that can profit from the new PHIs; do not unroll a
// second time or perform another large CFG clone at this stage.
FunctionPassManager PassBuilder::buildLateLoopRediscoveryPipeline() {
    FunctionPassManager FPM;
    FPM.addPass(BreakCriticalEdgesPass());
    FPM.addPass(createFixedPointPass<Function, FunctionAnalysisPassManager>(
        LoopSimplifyPass(), 4));
    FPM.addPass(LCSSAPass());
    FPM.addPass(createFixedPointPass<Function, FunctionAnalysisPassManager>(
        IndVarSimplifyPass(), 2));
    FPM.addPass(LICMPass());
    FPM.addPass(LoopIdiomRecognizePass());
    FPM.addPass(InstSimplifyPass());
    FPM.addPass(ReassociatePass(/*PreserveAddressArithmetic=*/true));
    FPM.addPass(LocalGVNPass());
    FPM.addPass(GVNPREPass(/*EnablePRE=*/false));
    FPM.addPass(ADCEPass());
    FPM.addPass(DCEPass());
    FPM.addPass(CFGSimplifyPass());
    FPM.addPass(LoopDeletionPass());
    FPM.addPass(LoopStrengthReductionPass());
    FPM.addPass(LoadEliminationPass());
    FPM.addPass(DSEPass());
    FPM.addPass(DCEPass());
    FPM.addPass(CFGSimplifyPass());
    return FPM;
}

// 4. 晚期清理与后端准备 Pipeline
// 4.1 晚期控制流收敛 Pipeline
// LoopRotate/LICM/IfConversion 之后经常暴露新的常量分支、空块和可合并块。
// 这段只做控制流/指令局部收敛，避免把 ConstantHoist/UnifyFuncExits 放进不动点。
FunctionPassManager PassBuilder::buildLateControlFlowPipeline() {
    FunctionPassManager FPM;
    FPM.addPass(CFGSimplifyPass());
    FPM.addPass(InstSimplifyPass());
    FPM.addPass(CFGSimplifyPass());
    FPM.addPass(IfConversionPass());
    FPM.addPass(InstSimplifyPass());
    FPM.addPass(CFGSimplifyPass());
    FPM.addPass(DCEPass());
    FPM.addPass(CFGSimplifyPass());
    return FPM;
}

// 将 IR 规范化为后端代码生成器(CodeGen)最喜欢的形态
FunctionPassManager PassBuilder::buildLatePipeline() {
    FunctionPassManager LateFPM;
    LateFPM.addPass(createFixedPointPass<Function, FunctionAnalysisPassManager>(
        buildLateControlFlowPipeline(), 3));
    LateFPM.addPass(DCEPass());
    // 大整数常量提升（放在 InstSimplify 和 DCE 之后，免得 add 0,c 被折掉）
    LateFPM.addPass(ConstantHoistPass());
    //LateFPM.addPass(CFGSimplifyPass());
    //LateFPM.addPass(BreakCriticalEdgesPass());
    LateFPM.addPass(UnifyFuncExitsPass());
    LateFPM.addPass(LatePhiCleanupPass());
    LateFPM.addPass(CFGSimplifyPass());
    LateFPM.addPass(LatePhiCleanupPass());
    return LateFPM;
}


// PassBuilder 核心实现
void PassBuilder::registerFunctionAnalyses(FunctionAnalysisPassManager &FAM){
    #define FUNCTION_ANALYSIS(NAME,CREATE_PASS)\
        FAM.registerPass([&] {return CREATE_PASS;});
    #include "./FunctionAnalysisPass.def"
    #undef FUNCTION_ANALYSIS
}

ModulePassManager PassBuilder::buildModulePipeline(OptimizationLevel Level, FunctionAnalysisPassManager &FAM) {
    ModulePassManager MPM;
    MPM.setIRMutationCallback([&FAM] { FAM.clear(); });

    // O0 直接返回，不进行任何优化
    if (Level == OptimizationLevel::O0) {
        return MPM;
    }

    // A full-object memcpy in main's side-effect-free entry prefix is
    // equivalent to the destination's static initializer.  Fold it before
    // function passes obscure the direct global/source relationship.
    MPM.addPass(GlobalInitializerFoldPass());

    // Phase 1: Early Simplification (早期清理)
    // 目标：清理前端生成的冗余 Alloca、控制流，为 Inline 做准备
    // 限制迭代次数为 2 次，避免浪费时间
    MPM.addPass(createFixedPointPass<Module, ModuleAnalysisPassManager>(
        createModuleFunctionBridge(buildLightCleanupPipeline(), FAM), 2));

    // 全局化大数组
    MPM.addPass(Local2GlobalPass());
    // Local2Global can expose large entry allocas as direct global objects;
    // retry the initializer fold once that representation is available.
    MPM.addPass(GlobalInitializerFoldPass());

    // Propagate genuinely read-only scalar global initializers before loop
    // analysis.  This is a module-wide write/escape proof, so loop bounds can
    // become constants without relying on a global's name or source spelling.
    // The later invocation remains useful after inlining exposes more dead
    // stores and globals.
    MPM.addPass(StoreOnlyGlobalEliminationPass(FAM));

    // Phase 2: Interprocedural Optimization (IPO，过程间优化)
    MPM.addPass(createModuleFunctionBridge(TailRecursionEliminationPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(LocalGVNPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(
        GVNPREPass(/*EnablePRE=*/false), FAM));
    // 所有循环剥离统一交给主循环流水线中的通用成本模型，
    // 不再为某一种嵌套循环形态安排专用的内联前执行阶段。
    MPM.addPass(createModuleFunctionBridge(FuncInlinePass(/*enableRecursiveInline=*/true), FAM));
    // 内联后最容易产生不再被 main 可达调用图引用的 helper，先删掉以减少后续函数 pass 工作量。
    MPM.addPass(DeadFunctionEliminationPass(FAM));
    MPM.addPass(createFixedPointPass<Module, ModuleAnalysisPassManager>(
        createModuleFunctionBridge(buildScalarCleanupPipeline(), FAM), 2));
    // 早期仍使用保守模式，避免把循环携带的全局状态提前暴露给
    // Range/Constraint 链；晚期会在这些分析全部结束后做放宽版局部化。
    MPM.addPass(createModuleFunctionBridge(Global2LocalPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(CFGSimplifyPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(PromotePass(), FAM));
    MPM.addPass(createModuleFunctionBridge(CFGSimplifyPass(), FAM));

    // 死参数消除 (Dead Argument Elimination)
    MPM.addPass(DAEPass());

    // 内联后会暴露出大量新的常数折叠、死代码和 Alloca 机会，再做一次轻量清理
    MPM.addPass(createFixedPointPass<Module, ModuleAnalysisPassManager>(
        createModuleFunctionBridge(buildLightCleanupPipeline(), FAM), 2));
    MPM.addPass(createModuleFunctionBridge(LocalAggregateSimplifyPass(), FAM));
    // LocalAggregateSimplify may split an aggregate into scalar allocas.  Make
    // those values SSA immediately so the heavy memory and loop pipelines see
    // PHIs/constants instead of waiting until the late backend-prep phase.
    MPM.addPass(createModuleFunctionBridge(PromotePass(), FAM));
    MPM.addPass(createModuleFunctionBridge(SCCPPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(InstSimplifyPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(DCEPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(CFGSimplifyPass(), FAM));
    MPM.addPass(createFixedPointPass<Module, ModuleAnalysisPassManager>(
        createModuleFunctionBridge(buildScalarCleanupPipeline(), FAM), 2));

    // 标量规范化会把原先不同写法的实参合并为同一 SSA 值；再次运行
    // DAE 捕获新暴露的局部参数等价类，并立即清理特化副本。
    MPM.addPass(DAEPass());
    MPM.addPass(createModuleFunctionBridge(buildScalarCleanupPipeline(), FAM));

    // 清理阶段可能删除最后一个无副作用 call，再跑一次模块级 DFE 收掉新暴露的死函数。
    MPM.addPass(DeadFunctionEliminationPass(FAM));

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

    // Phase 4.5: Post-loop cleanup
    MPM.addPass(createFixedPointPass<Module, ModuleAnalysisPassManager>(
        createModuleFunctionBridge(buildPostLoopCleanupPipeline(), FAM), 1));

    // 循环展开、LICM 与 post-loop GVN 可能复制或规范化调用点。最后一轮
    // DAE 放在记忆化之前，避免把记忆化生成的缓存 CFG 和逐参数键逻辑克隆。
    MPM.addPass(DAEPass());
    MPM.addPass(createModuleFunctionBridge(buildScalarCleanupPipeline(), FAM));

    // 循环 DCE/CFG 清理后做最终函数图收敛，避免 late/backend 继续处理死函数体。
    MPM.addPass(DeadFunctionEliminationPass(FAM));

    // 在主体标量与循环优化完成后，为严格纯净的重叠递归插入有界精确键缓存。
    MPM.addPass(FunctionMemoizationPass(FAM));

    // Phase 5: Late CFG / Backend Prep (晚期清理与后端准备)
    // 范围和循环变换已经结束，此时可安全放宽标量 Global2Local：
    // 只要全局标量不逃逸、仅由 main 直接 load/store，就允许跨块和
    // 循环使用；随后立即 Mem2Reg，使循环状态成为 PHI。
    MPM.addPass(createModuleFunctionBridge(Global2LocalPass(
        /*RelaxScalarUses=*/true), FAM));
    MPM.addPass(createModuleFunctionBridge(PromotePass(), FAM));
    MPM.addPass(createModuleFunctionBridge(CFGSimplifyPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(SCCPPass(), FAM));
    MPM.addPass(createFixedPointPass<Module, ModuleAnalysisPassManager>(
        createModuleFunctionBridge(buildScalarCleanupPipeline(), FAM), 2));
    MPM.addPass(createModuleFunctionBridge(
        buildLateLoopRediscoveryPipeline(), FAM));
    MPM.addPass(createModuleFunctionBridge(GepCombinePass(), FAM));
    MPM.addPass(createModuleFunctionBridge(GepEvaluatePass(), FAM));
    MPM.addPass(createModuleFunctionBridge(LoadEliminationPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(DSEPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(DCEPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(CFGSimplifyPass(), FAM));
    MPM.addPass(createModuleFunctionBridge(buildLatePipeline(), FAM));
    MPM.addPass(LateDeadGlobalCleanupPass());
    // Late DCE 仍可能删掉最后一个纯调用，最终再收一次，保证输出 IR 不残留死函数体。
    MPM.addPass(DeadFunctionEliminationPass(FAM));

    return MPM;
}
