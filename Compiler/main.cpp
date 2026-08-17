#include "./include/yacc/Bison.hpp"
#include "./include/lib/IRGenerator.hpp"
#include "./include/lib/Target.hpp"
#include "./include/ir/opt/PassBuilder.hpp"
#include "./include/Common.hpp"
#include "./include/riscv/Target.hpp"
#include <iostream>
#include <string>
#include <memory>
#include <cstdio>
#include <fstream>

std::unique_ptr<CompUnit> ASTRoot = nullptr;
std::unique_ptr<Module> ModuleIR = std::make_unique<Module>();
extern FILE *yyin;

int main(int argc, const char *argv[]) {
    /*
     * 功能测试：
     *   compiler testcase.sysy -S -o testcase.s
     * 性能测试：
     *   compiler testcase.sysy -S -o testcase.s -O1
     * 评测平台实际调用（两种顺序都兼容）：
     *   compiler -S -o testcase.s testcase.sy [-O1]
     */

    // 解析命令行：支持 compiler <input> -S -o <out> 与
    // compiler -S -o <out> <input> [-O1] 两种形态；-S/-o/-O1 位置无关。
    std::string inputPath;
    std::string outputPath;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i] ? argv[i] : "";
        if (arg == "-S") {
            continue;
        }
        if (arg == "-o" && i + 1 < argc) {
            outputPath = argv[++i];
            continue;
        }
        if (arg == "-O1" || arg == "-O0" || arg == "-O2") {
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            continue; // 未知选项忽略
        }
        if (!arg.empty()) {
            inputPath = arg; // 最后一个非选项参数视为输入文件
        }
    }
    if (inputPath.empty() || outputPath.empty()) {
        std::cerr << "usage: compiler <input.sy> -S -o <output.s> [-O1]" << std::endl;
        return 2;
    }

    //默认中端优化等级为O1
    OptimizationLevel OptLevel = OptimizationLevel::O1;

    // 固定宽度自动向量化的唯一开关：
    // Disabled：关闭；SIMD64：VF2；SIMD128：VF4；
    // SIMD64And128：优先 VF4，并在合法时回退 VF2。
    TargetInfo::get().setFixedSIMDMode(FixedSIMDMode::Disabled);

    //读取源文件->词法分析->语法分析->生成AST
    yyin = fopen(inputPath.c_str(), "r");
    if (!yyin) {
        std::cerr << "Error: cannot open input file " << inputPath << std::endl;
        return 1;
    }
    yy::parser parser;
    parser.parse();
    fclose(yyin);

    //IR生成
    IRGenerator irGenerator;
    irGenerator.visit(*ASTRoot);

    //注册AnalysisPass
    ModuleAnalysisPassManager MAM;
    FunctionAnalysisPassManager FAM;
    PassBuilder PB;
    PB.registerFunctionAnalyses(FAM);
    //构建优化流水线并运行
    ModulePassManager MPM = PB.buildModulePipeline(OptLevel, FAM);
    MPM.run(*ModuleIR, MAM);
//======临时检查
    std::string llvmOutputPath = outputPath.substr(0, outputPath.find_last_of('.')) + ".ll";
    std::ofstream llvmOut(llvmOutputPath);
    std::streambuf *oldBuffer = std::cout.rdbuf(llvmOut.rdbuf());
    ModuleIR->dumpIR();
    std::cout.rdbuf(oldBuffer);
//======临时检查
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
    return 0;

}
