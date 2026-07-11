#include "../../../include/riscv/Target.hpp"

#include "../../../include/riscv/AsmPrinter.hpp"
#include "../../../include/riscv/ISel.hpp"
#include "../../../include/riscv/MachinePassBuilder.hpp"
#include "../../../include/riscv/MachinePassManager.hpp"

#include <ostream>

namespace riscv {

void emitAssembly(Module &module, std::ostream &out,
                  const BackendOptions &options) {
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
  MachineModulePassManager MPM =
      MPB.buildModulePipeline(options, MFAM);
  MPM.run(machine, MMAM);

  // 汇编打印
  AsmPrinter(out).emitModule(machine);
}

} // namespace riscv
