#include "./include/yacc/Bison.hpp"
#include "./include/lib/IRGenerator.hpp"
#include "./include/lib/IRTarget.hpp"
#include "./include/ir/opt/PassBuilder.hpp"
#include "./include/Common.hpp"
#include "./include/riscv/Target.hpp"
#include "./include/aarch64/Target.hpp"
#include <iostream>
#include <string>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

std::unique_ptr<CompUnit> ASTRoot = nullptr;
std::unique_ptr<Module> ModuleIR = std::make_unique<Module>();
extern FILE *yyin;

// 目标架构选择：
// - 本地 CMake 构建仍以编译期宏为准（RISC-V / AArch64 各自只编一个后端）；
// - 评测平台直接 clang++ 编译（不定义 ENABLE_*_BACKEND 宏），默认后端由
//   Common.hpp 的 DEFAULT_BACKEND_ARCH_RISCV 决定（当前为 RISC-V 赛道），
//   可用环境变量 BACKEND_ARCH=riscv/aarch64 覆盖以便对照。
static TargetArch defaultTargetArch() {
#if defined(ENABLE_AARCH64_BACKEND) && !defined(ENABLE_RISCV_BACKEND)
  return TargetArch::AArch64;
#elif defined(ENABLE_RISCV_BACKEND) && !defined(ENABLE_AARCH64_BACKEND)
  return TargetArch::RISCV64;
#else
  const char *arch = std::getenv("BACKEND_ARCH");
  if (arch && std::strcmp(arch, "riscv") == 0) {
    return TargetArch::RISCV64;
  }
  if (arch && std::strcmp(arch, "aarch64") == 0) {
    return TargetArch::AArch64;
  }
#ifdef DEFAULT_BACKEND_ARCH_RISCV
  return TargetArch::RISCV64;
#else
  return TargetArch::AArch64;
#endif
#endif
}

// 汇编发射：与 defaultTargetArch 保持同一套选择逻辑。
static void emitAssemblyImpl(std::ostream &out) {
#if defined(ENABLE_RISCV_BACKEND) && !defined(ENABLE_AARCH64_BACKEND)
  riscv::emitAssembly(*ModuleIR, out);
#elif defined(ENABLE_AARCH64_BACKEND) && !defined(ENABLE_RISCV_BACKEND)
  aarch64::emitAssembly(*ModuleIR, out);
#else
  const char *arch = std::getenv("BACKEND_ARCH");
  if (arch && std::strcmp(arch, "riscv") == 0) {
    riscv::emitAssembly(*ModuleIR, out);
    return;
  }
  if (arch && std::strcmp(arch, "aarch64") == 0) {
    aarch64::emitAssembly(*ModuleIR, out);
    return;
  }
#ifdef DEFAULT_BACKEND_ARCH_RISCV
  riscv::emitAssembly(*ModuleIR, out);
#else
  aarch64::emitAssembly(*ModuleIR, out);
#endif
#endif
}

int main(int argc, const char *argv[]) {
    /*
     * 功能测试：
     *   compiler testcase.sysy -S -o testcase.s
     * 性能测试：
     *   compiler testcase.sysy -S -o testcase.s -O1
     * 评测平台实际调用（两种顺序都兼容）：
     *   compiler -S -o testcase.s testcase.sy [-O1]
     */

    // 在解析和生成 IR 前确定目标，使指针、索引及聚合类型布局全程一致。
    TargetInfo::get().setArch(defaultTargetArch());

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

    try {
        std::ofstream asmOut(outputPath);
        if (!asmOut.is_open()) {
            std::cerr << "Error: Cannot open output file " << outputPath << std::endl;
            return 1;
        }
        emitAssemblyImpl(asmOut);
    } catch (const riscv::BackendError &error) {
        std::cerr << "RISC-V backend error: " << error.message() << std::endl;
        return 1;
    } catch (const aarch64::BackendError &error) {
        std::cerr << "ARM64 backend error: " << error.message() << std::endl;
        return 1;
    }
    return 0;
}
