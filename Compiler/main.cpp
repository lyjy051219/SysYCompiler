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

// 保存语法分析生成的 AST 根节点和后续构建的模块级 IR。
std::unique_ptr<CompUnit> ASTRoot = nullptr;
std::unique_ptr<Module> ModuleIR = std::make_unique<Module>();
extern FILE *yyin;

static void dumpRuntimeDeclarationsForExternalLink() {
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
    std::cout << "declare void @llvm.memcpy.p0.p0.i32(ptr, ptr, i32, i1)\n\n";
    std::cout << "declare void @llvm.memmove.p0.p0.i32(ptr, ptr, i32, i1)\n\n";
    std::cout << "declare void @llvm.memset.p0.i32(ptr, i8, i32, i1)\n\n";
}

int main(int argc, const char *argv[]) {
    (void)argc;

    // 读取源文件并通过词法、语法分析构建 AST。
    std::string outputPath = argv[3];
    yyin = fopen(argv[4],"r");
    yy::parser parser;
    parser.parse();
    fclose(yyin);

    // 如需检查语法树，可在此启用 ASTPrinter。
    // ASTPrinter astPrinter;
    // astPrinter.visit(*ASTRoot);

    // 遍历 AST 并生成模块级 IR。
    IRGenerator irGenerator;
    irGenerator.visit(*ASTRoot);

    // 注册模块级与函数级分析，并构建默认的 O1 优化管道。
    ModuleAnalysisPassManager MAM;
    FunctionAnalysisPassManager FAM;
    PassBuilder PB;
    PB.registerModuleAnalyses(MAM);
    PB.registerFunctionAnalyses(FAM);
    OptimizationLevel OptLevel = OptimizationLevel::O1;
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
#else
    freopen(outputPath.c_str(), "w", stdout);
    // LLVM IR 模式先输出外部运行时声明，再输出模块正文。
    dumpRuntimeDeclarationsForExternalLink();
    ModuleIR->dumpIR();
    fclose(stdout);
#endif
    return 0;
}
