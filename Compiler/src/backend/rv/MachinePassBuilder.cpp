#include "../../../include/riscv/MachinePassBuilder.hpp"

#include "../../../include/riscv/CodegenPasses.hpp"
#include "../../../include/riscv/MachineAnalysis.hpp"

#include <cstdlib>

namespace riscv {
namespace {

// 环境变量开关：BACKEND_VERIFY_MIR 打开后在关键阶段之间插入校验
bool verifyEnabled() { return std::getenv("BACKEND_VERIFY_MIR") != nullptr; }

// 环境变量开关：BACKEND_DUMP_MIR 打开后在关键阶段之间打印 MIR
bool dumpEnabled() { return std::getenv("BACKEND_DUMP_MIR") != nullptr; }

// 按开关在流水线中插入调试用的校验和打印
void addDebugPasses(MachineFunctionPassManager &MPM, const char *tag) {
  if (verifyEnabled()) {
    MPM.addPass(MachineVerifyPass());
  }
  if (dumpEnabled()) {
    MPM.addPass(MachineDumpPass(tag));
  }
}

} // namespace

void MachinePassBuilder::registerFunctionAnalyses(
    MachineFunctionAnalysisManager &MFAM) {
  // 新增函数级分析在这里注册
  MFAM.registerPass([] { return MachineInstrStatsAnalysis(); });
  MFAM.registerPass([] { return MachineCFGAnalysis(); });
}

void MachinePassBuilder::registerModuleAnalyses(
    MachineModuleAnalysisManager &MMAM) {
  // 暂无模块级分析，保留注册入口
  (void)MMAM;
}

MachineFunctionPassManager
MachinePassBuilder::buildPreRAOptPipeline(CodegenOptLevel Level) {
  MachineFunctionPassManager MPM;
  if (Level == CodegenOptLevel::O0) {
    return MPM;
  }
  // 分配前优化挂在这里，此时仍是带虚拟寄存器的 MIR
  // 例如拷贝合并，机器层公共子表达式删除，块布局调整
  return MPM;
}

MachineFunctionPassManager
MachinePassBuilder::buildPostRAOptPipeline(CodegenOptLevel Level) {
  MachineFunctionPassManager MPM;
  if (Level == CodegenOptLevel::O0) {
    return MPM;
  }
  // 分配后优化挂在这里，此时寄存器和栈帧都已确定
  MPM.addPass(PostRAPeepholePass());
  return MPM;
}

MachineFunctionPassManager
MachinePassBuilder::buildCodegenPipeline(CodegenOptLevel Level) {
  MachineFunctionPassManager MPM;

  // 指令选择之后先整理控制流
  MPM.addPass(RebuildCFGPass());
  MPM.addPass(CleanupCFGPass());
  addDebugPasses(MPM, "after isel");

  // 伪指令下降：SELECT 展开成分支，PHI 改写成前驱末尾拷贝
  MPM.addPass(LowerSelectPass());
  MPM.addPass(EliminatePhiPass());
  addDebugPasses(MPM, "after pseudo lowering");

  // 分配前优化
  MPM.addPass(buildPreRAOptPipeline(Level));

  // 寄存器分配和栈帧下降
  MPM.addPass(RegisterAllocationPass());
  MPM.addPass(FrameLoweringPass());
  addDebugPasses(MPM, "after regalloc and frame");

  // 分配后优化
  MPM.addPass(buildPostRAOptPipeline(Level));
  addDebugPasses(MPM, "final");

  return MPM;
}

MachineModulePassManager
MachinePassBuilder::buildModulePipeline(CodegenOptLevel Level,
                                        MachineFunctionAnalysisManager &MFAM) {
  MachineModulePassManager MPM;
  MPM.addPass(createMachineModuleFunctionBridge(buildCodegenPipeline(Level),
                                                MFAM));
  return MPM;
}

} // namespace riscv
