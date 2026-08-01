// Target.cpp —— AArch64(ARM64) 后端入口。
// 负责把中端 IR 一路走完整个后端流水线,最终发射成 ARM64 汇编文本。
// 顺序为：代码生成准备、指令选择与 lowering、注册分析、运行代码生成流水线、汇编打印。
// 机器层各 pass 与 rv 后端语义完全一致,可直接复用;真正做 ISA 翻译的只有 AsmPrinter。
#include "aarch64/Target.hpp"

#include "aarch64/AsmPrinter.hpp"
#include "aarch64/ISel.hpp"
#include "aarch64/opt/CodeGenPrepare.hpp"
#include "aarch64/opt/MachinePassBuilder.hpp"
#include "aarch64/opt/MachinePassManager.hpp"

#include <ostream>

namespace aarch64 {

// 后端总入口:把一个中端 Module 编译为 ARM64 汇编写入 out。
// options 携带优化级别和 pass 开关等后端选项。
void emitAssembly(Module &module, std::ostream &out,
                  const BackendOptions &options) {
  // 启用与 rv 后端一致的优化流水线来提升性能。
  // 机器层 pass 语义与 rv 相同，只有 AsmPrinter 做指令翻译，可直接复用。
  // 需要排查时可用环境变量 BACKEND_DISABLE_PASSES 单独关闭某个 pass。
  BackendOptions codegenOptions = options;

  // 指令选择前先做地址计算准备，保留 IR 的 SSA 形式。
  runCodeGenPrepare(module);

  // 指令选择：中端 IR lower 为带虚拟寄存器的 MIR
  IRLowering lowering(module);
  MachineModule machine = lowering.lowerModule();

  // 建立分析管理器并注册所有后端分析
  MachineModuleAnalysisManager MMAM;
  MachineFunctionAnalysisManager MFAM;
  MachinePassBuilder MPB;
  MPB.registerModuleAnalyses(MMAM);
  MPB.registerFunctionAnalyses(MFAM);

  // 构建并运行代码生成流水线
  MachineModulePassManager MPM = MPB.buildModulePipeline(codegenOptions, MFAM);
  MPM.run(machine, MMAM);

  // 汇编打印
  AsmPrinter(out).emitModule(machine);
}

} // namespace aarch64
