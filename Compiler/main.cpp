#include "./include/yacc/Bison.hpp"
#include "./include/lib/ASTPrinter.hpp"
#include "./include/lib/IRGenerator.hpp"
#include "./include/ir/opt/PassBuilder.hpp"
#ifdef ENABLE_RISCV_BACKEND
#include "./include/riscv/Target.hpp"
#endif
#include <iostream>
#include <string>
#include <memory>
#include <cstdio>
#include <fstream>

std::unique_ptr<CompUnit> ASTRoot = nullptr;
std::unique_ptr<Module> ModuleIR = std::make_unique<Module>();
extern FILE *yyin;

int main(int argc, const char *argv[]) {
    (void)argc;
    /*
     * 功能测试：
     *   compiler testcase.sysy -S -o testcase.s
     * 性能测试：
     *   compiler testcase.sysy -S -o testcase.s -O1
     */

    // 参数位置固定，可以直接读取输入、输出文件路径。
    std::string inputPath = argv[1];
    std::string outputPath = argv[4];

    //默认中端优化等级为O1
    OptimizationLevel OptLevel = OptimizationLevel::O1;

    //读取源文件->词法分析->语法分析->生成AST
    yyin = fopen(inputPath.c_str(), "r");
    yy::parser parser;
    parser.parse();
    fclose(yyin);

    //调试AST时使用
    //ASTPrinter astPrinter;
    //astPrinter.visit(*ASTRoot);

    //IR生成
    IRGenerator irGenerator;
    irGenerator.visit(*ASTRoot);

    //注册AnalysisPass
    ModuleAnalysisPassManager MAM;
    FunctionAnalysisPassManager FAM;
    PassBuilder PB;
    PB.registerModuleAnalyses(MAM);
    PB.registerFunctionAnalyses(FAM);
    //构建优化流水线并运行
    ModulePassManager MPM = PB.buildModulePipeline(OptLevel, FAM);
    MPM.run(*ModuleIR, MAM);

#ifdef ENABLE_RISCV_BACKEND
    try {
        std::ofstream asmOut(outputPath);
        if (!asmOut.is_open()) {
            std::cerr << "Error: Cannot open output file " << outputPath << std::endl;
            return 1;
        }
        riscv::emitAssembly(*ModuleIR, asmOut);
    } catch (const riscv::BackendError &error) {
        std::cerr << "RISC-V backend error: " << error.message() << std::endl;
        return 1;
    }
#elif defined(ENABLE_AARCH64_BACKEND)
    try {
        std::ofstream asmOut(outputPath);
        if (!asmOut.is_open()) {
            std::cerr << "Error: Cannot open output file " << outputPath << std::endl;
            return 1;
        }
        aarch64::emitAssembly(*ModuleIR, asmOut);
    } catch (const aarch64::BackendError &error) {
        std::cerr << "ARM64 backend error: " << error.message() << std::endl;
        return 1;
    }
#else
    freopen(outputPath.c_str(), "w", stdout);
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
    std::cout << "declare void @_sysy_stoptime(i32)\n\n";
    std::cout
        << "declare void @llvm.memcpy.p0.p0.i32"
        << "(ptr, ptr, i32, i1)\n\n";
    std::cout
        << "declare void @llvm.memmove.p0.p0.i32"
        << "(ptr, ptr, i32, i1)\n\n";
    std::cout
        << "declare void @llvm.memset.p0.i32"
        << "(ptr, i8, i32, i1)\n\n";
    ModuleIR->dumpIR();
    fclose(stdout);
#endif
    return 0;
}