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
#include <unordered_set>

/*注意该项目只能用clang-18编译为可执行文件
  clang++ --std=c++17 -O2 -lm
  只能用c++17标准
*/

//全局变量，用来作为AST根节点
std::unique_ptr<CompUnit> ASTRoot = nullptr;
std::unique_ptr<Module> ModuleIR = std::make_unique<Module>();
#if 0
const std::string RUNTIME_PATH = std::string(RUNTIME_DIR) + "/runtime.ll";
#endif
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
}

int main(int argc, const char *argv[]) {
    (void)argc;
    //./Compiler -S -o program.s/.ll文件 program.sy

    //1.文件读取->词法分析->语法分析
    std::string outputPath = argv[3];
    yyin = fopen(argv[4],"r");
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
    //4.运行时库注入到.ll最前面（lli 测试需要）
    // llc + qemu 测试链路会在链接阶段使用 runtime/sylib.c，暂时不要把 runtime.ll 拼进 IR。
#if 0
    std::ifstream runtimeFile(RUNTIME_PATH);
    if (!runtimeFile.is_open()) {
        std::cerr << "Error: Cannot open runtime.ll at " << RUNTIME_PATH << std::endl;
    } else {
        std::cout << runtimeFile.rdbuf();
        std::cout << "\n";
        runtimeFile.close();
    }
#endif


    //5.输出生成的IR（追加在运行时库之后）
    dumpRuntimeDeclarationsForExternalLink();
    ModuleIR->dumpIR();
    fclose(stdout);
#endif
    return 0;
}

//https://understanding-llvm-transformation-passes.readthedocs.io/en/latest/LLVM-transformation-passes/21-sparse-conditional-constant-propagation.html
//https://llvm-study-notes.readthedocs.io/en/latest/misc/index.html
//https://enna1.github.io/blog/
//pass相关汇总
