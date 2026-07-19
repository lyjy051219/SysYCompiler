#include "aarch64/Target.hpp"

#include "aarch64/AsmPrinter.hpp"
#include "aarch64/ISel.hpp"
#include "aarch64/opt/MachinePassBuilder.hpp"
#include "aarch64/opt/MachinePassManager.hpp"

#include <ostream>

namespace aarch64 {

void emitAssembly(Module &module, std::ostream &out,
                  const BackendOptions &options) {
  // ARM64 后端首版以正确性为先：走 O0 流水线并禁用全部可选优化 pass，
  // 只保留必经的代码生成阶段（控制流整理、SELECT/PHI 下降、寄存器分配、
  // 栈帧下降）。地址归纳准备等 RISC-V 调优 pass 暂不启用，后续再逐步开启。
  BackendOptions codegenOptions = options;
  codegenOptions.optLevel = CodegenOptLevel::O0;
  codegenOptions.disabledMachinePasses.emplace_back("all");

  // 指令选择：中端 IR 下降为带虚拟寄存器的 MIR
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
