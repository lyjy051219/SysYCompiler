#include "./include/yacc/Bison.hpp"
#include "./include/lib/IRGenerator.hpp"
#include "./include/lib/Target.hpp"
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
    (void)argc;
    /*
     * 功能测试：
     *   compiler testcase.sysy -S -o testcase.s
     * 性能测试：
     *   compiler testcase.sysy -S -o testcase.s -O1
     */

    // 在解析和生成 IR 前确定目标，使指针、索引及聚合类型布局全程一致。
    TargetInfo::get().setArch(defaultTargetArch());

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
