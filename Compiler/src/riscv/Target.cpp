// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

// Target.cpp —— RISC-V 后端入口。
// 负责把中端 IR 一路走完整个后端流水线,最终发射成 RV64 汇编文本。
// 顺序为：代码生成准备、指令选择与 lowering、注册分析、运行代码生成流水线、汇编打印。
#include "riscv/Target.hpp"

#include "riscv/AsmPrinter.hpp"
#include "riscv/ISel.hpp"
#include "riscv/opt/CodeGenPrepare.hpp"
#include "riscv/opt/MachinePassBuilder.hpp"
#include "riscv/opt/MachinePassManager.hpp"

#include <ostream>

namespace riscv {

// 后端总入口:把一个中端 Module 编译为 RISC-V 汇编写入 out。
// options 携带优化级别和 pass 开关等后端选项。
void emitAssembly(Module &module, std::ostream &out,
                  const BackendOptions &options) {
  // 在指令选择前做目标相关的地址归纳准备，保留 IR 的 SSA 形式。
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
  MachineModulePassManager MPM =
      MPB.buildModulePipeline(options, MFAM);
  MPM.run(machine, MMAM);

  // 汇编打印
  AsmPrinter(out).emitModule(machine);
}

} // namespace riscv
