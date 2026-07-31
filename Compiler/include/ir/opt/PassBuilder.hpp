#pragma once
#include "ir/opt/NewPassManager.hpp"

//OptimizationLevel：表示编译器中端优化流水线的等级
enum class OptimizationLevel{
    O0 = 0,
    O1 = 1,
    O2 = 2
};

//PassBuilder：用来组织中端PassPipeline
class PassBuilder{
public:
    //构造不持有额外状态的PassBuilder
    explicit PassBuilder();
    //注册函数级AnalysisPass
    void registerFunctionAnalyses(FunctionAnalysisPassManager &FAM);
    //构造中端PassPipeline
    ModulePassManager buildModulePipeline(OptimizationLevel Level, FunctionAnalysisPassManager &FAM);
private:
    //构造建立SSA、简化标量表达式和CFG的轻量清理流水线
    static FunctionPassManager buildLightCleanupPipeline();
    //构造执行值编号、Load消除和死存储清理的重型内存流水线
    static FunctionPassManager buildHeavyMemoryPipeline();
    //构造收敛标量表达式与控制流的清理流水线
    static FunctionPassManager buildScalarCleanupPipeline();
    //构造循环展开后恢复标量、内存和循环规范形态的清理流水线
    static FunctionPassManager buildPostUnrollCleanupPipeline();
    //构造循环规范化、结构变换和循环优化的主流水线
    static FunctionPassManager buildLoopPipeline();
    //构造循环主流水线结束后的地址、内存和CFG清理流水线
    static FunctionPassManager buildPostLoopCleanupPipeline();
    //构造晚期重新发现并消费循环优化机会的流水线
    static FunctionPassManager buildLateLoopRediscoveryPipeline();
    //构造晚期控制流化简与IfConversion流水线
    static FunctionPassManager buildLateControlFlowPipeline();
    //构造最终控制流、常量外提和函数出口规范化流水线
    static FunctionPassManager buildLatePipeline();
};
