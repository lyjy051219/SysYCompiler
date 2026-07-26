#pragma once
#include "ir/opt/NewPassManager.hpp"

//PassBuilder

enum class OptimizationLevel{
    O0 = 0,
    O1 = 1,
    O2 = 2
};


class PassBuilder{
    public:
    explicit PassBuilder();
    //AnalysisPass注册相关函数
    void registerFunctionAnalyses(FunctionAnalysisPassManager &FAM);
    //Pipeline Building
    ModulePassManager buildModulePipeline(OptimizationLevel Level, FunctionAnalysisPassManager &FAM);

    private:
    static FunctionPassManager buildLightCleanupPipeline();
    static FunctionPassManager buildHeavyMemoryPipeline();
    static FunctionPassManager buildScalarCleanupPipeline();
    static FunctionPassManager buildEarlyPeelingPipeline();
    static FunctionPassManager buildPostUnrollCleanupPipeline();
    static FunctionPassManager buildLoopPipeline();
    static FunctionPassManager buildPostLoopCleanupPipeline();
    static FunctionPassManager buildLateLoopRediscoveryPipeline();
    static FunctionPassManager buildLateControlFlowPipeline();
    static FunctionPassManager buildLatePipeline();
};
