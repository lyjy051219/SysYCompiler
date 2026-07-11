#pragma once

#include "riscv/Target.hpp"
#include "riscv/opt/MachinePassManager.hpp"

// 后端 MachinePassBuilder
// 仿照中端 PassBuilder 的职责划分：集中注册分析 pass，按优化等级组装流水线。
// 新增分析在 registerFunctionAnalyses 或 registerModuleAnalyses 里注册，
// 新增变换 pass 在 buildPreRAOptPipeline 或 buildPostRAOptPipeline 里挂接。

namespace riscv {

class MachinePassBuilder {
public:
  MachinePassBuilder() = default;

  // 注册所有函数级分析
  void registerFunctionAnalyses(MachineFunctionAnalysisManager &MFAM);
  // 注册所有模块级分析，当前为空位
  void registerModuleAnalyses(MachineModuleAnalysisManager &MMAM);

  // 寄存器分配前的优化流水线，此时 MIR 仍是虚拟寄存器形式
  MachineFunctionPassManager
  buildPreRAOptPipeline(const BackendOptions &Options);
  // 寄存器分配和栈帧展开之后的优化流水线
  MachineFunctionPassManager
  buildPostRAOptPipeline(const BackendOptions &Options);
  // 完整的函数级代码生成流水线：控制流整理，伪指令下降，
  // 分配前优化，寄存器分配，栈帧下降，分配后优化
  MachineFunctionPassManager buildCodegenPipeline(const BackendOptions &Options);
  // 模块级流水线，内部通过桥接把函数级流水线套到每个函数上
  MachineModulePassManager
  buildModulePipeline(const BackendOptions &Options,
                      MachineFunctionAnalysisManager &MFAM);
};

} // namespace riscv
