#pragma once
#include "ir/opt/NewPassManager.hpp"

enum class OptimizationLevel{
    O0 = 0,
    O1 = 1,
    O2 = 2
};

// PassBuilder 只负责声明顺序与固定点边界，不执行优化本身。流程：
// 1. 注册函数分析；2. 用小型函数 pipeline 组成轻量清理、重型内存、循环及 late
// 阶段；3. 通过 ModuleFunctionBridge 把函数阶段接入模块 pipeline；4. 只在需要
// 收敛新机会的阶段外包有限固定点；5. 模块级变换通过 mutation callback 清理 FAM。
class PassBuilder{
    public:
    explicit PassBuilder();

    void registerFunctionAnalyses(FunctionAnalysisPassManager &FAM);

    ModulePassManager buildModulePipeline(OptimizationLevel Level, FunctionAnalysisPassManager &FAM);

    private:
    static FunctionPassManager buildLightCleanupPipeline();
    static FunctionPassManager buildHeavyMemoryPipeline();
    static FunctionPassManager buildScalarCleanupPipeline();
    static FunctionPassManager buildPostUnrollCleanupPipeline();
    static FunctionPassManager buildLoopPipeline();
    static FunctionPassManager buildPostLoopCleanupPipeline();
    static FunctionPassManager buildLateLoopRediscoveryPipeline();
    static FunctionPassManager buildLateControlFlowPipeline();
    static FunctionPassManager buildLatePipeline();
};
