#include "./include/yacc/Bison.hpp"
#include "./include/lib/ASTPrinter.hpp"
#include "./include/lib/IRGenerator.hpp"
#include "./include/ir/opt/PassBuilder.hpp"
#include <iostream>
#include <string>
#include <memory>
#include <cstdio>
#include <fstream>
#include <unordered_set>

/*注意该项目只能用clang-18编译为可执行文件
  clang++ --std=c++17 -O2 -lm
  只能用c++17标准
*/

//全局变量，用来作为AST根节点
std::unique_ptr<CompUnit> ASTRoot = nullptr;
std::unique_ptr<Module> ModuleIR = std::make_unique<Module>();
const std::string RUNTIME_PATH = std::string(RUNTIME_DIR) + "/runtime.ll";
extern FILE *yyin;
int main(int argc, const char *argv[]) {
    (void)argc;
    //./Compiler -S -o program.s/.ll文件 program.sy

    //1.文件读取->词法分析->语法分析
    std::string outputPath = argv[3];
    yyin = fopen(argv[4],"r");
    //重定向：用于IR生成调试
    freopen(outputPath.c_str(), "w", stdout);
    yy::parser parser;
    // parser.set_debug_level(1);  //觉得语法分析有错误的时候再开这个
    parser.parse();
    fclose(yyin);

    //AST调试部分
    //ASTPrinter astPrinter;
    //astPrinter.visit(*ASTRoot);

    //2.IR生成
    IRGenerator irGenerator;
    irGenerator.visit(*ASTRoot);

    //3.中端优化：

    /*注意：
    添加新AnalysisPass就将其加入到相应的def文件里面
    添加新TransformPass就将其加入到buildModulePipeline函数里面*/

    //注册AnalysisPassManager
    ModuleAnalysisPassManager MAM;
    FunctionAnalysisPassManager FAM;
    //创建PassBuilder
    PassBuilder PB;
    //注册所有分析Pass
    PB.registerModuleAnalyses(MAM);
    PB.registerFunctionAnalyses(FAM);
    //构建优化管道
//目前先默认为O1  如果要改优化等级就在这里硬编码
    OptimizationLevel OptLevel = OptimizationLevel::O1;
    ModulePassManager MPM = PB.buildModulePipeline(OptLevel, FAM);
    //运行优化--传入ModuleIR和ModuleAnalysisPassManager
    MPM.run(*ModuleIR, MAM);

/*
    //备选方案：运行时库声明
    //使用qemu模拟RISC-V环境下进行测试的时候用这个
    // 1. 打印 Target Triple
    std::cout << "target datalayout = \"e-m:e-p:64:64-i64:64-i128:128-n64-S128\"\n";
    std::cout << "target triple = \"riscv64-linux-gnu\"\n";
    std::cout << "\n";

    // 2. 运行时库函数声明

    std::cout << "declare i32 @getint()\n";
    std::cout << "declare i32 @getch()\n";
    std::cout << "declare float @getfloat()\n";
    std::cout << "declare i32 @getarray(i32*)\n";
    std::cout << "declare i32 @getfarray(float*)\n";
    std::cout << "declare void @putint(i32)\n";
    std::cout << "declare void @putch(i32)\n";
    std::cout << "declare void @putfloat(float)\n";
    std::cout << "declare void @putarray(i32, i32*)\n";
    std::cout << "declare void @putfarray(i32, float*)\n";
    std::cout << "declare void @putf(i8*, ...)\n";
    std::cout << "declare void @_sysy_starttime(i32)\n";
    std::cout << "declare void @_sysy_stoptime(i32)\n";
    std::cout << "declare void @llvm.memset.p0.i32(i8*, i8, i32, i1)\n";
    std::cout << "\n";
*/

    //4.运行时库注入到.ll最前面（lli 测试需要）
    std::ifstream runtimeFile(RUNTIME_PATH);
    if (!runtimeFile.is_open()) {
        std::cerr << "Error: Cannot open runtime.ll at " << RUNTIME_PATH << std::endl;
    } else {
        std::cout << runtimeFile.rdbuf();
        std::cout << "\n";
        runtimeFile.close();
    }


    //5.输出生成的IR（追加在运行时库之后）
    ModuleIR->dumpIR();
    fclose(stdout);
    return 0;
}

//https://understanding-llvm-transformation-passes.readthedocs.io/en/latest/LLVM-transformation-passes/21-sparse-conditional-constant-propagation.html
//https://llvm-study-notes.readthedocs.io/en/latest/misc/index.html
//https://enna1.github.io/blog/
//pass相关汇总
