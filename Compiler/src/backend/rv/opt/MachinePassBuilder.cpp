#include "../../../include/riscv/MachinePassBuilder.hpp"

#include "../../../include/riscv/CodegenPasses.hpp"
#include "../../../include/riscv/MachineAnalysis.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace riscv {
namespace {

// 环境变量开关：BACKEND_VERIFY_MIR 打开后在关键阶段之间插入校验
bool verifyEnabled() { return std::getenv("BACKEND_VERIFY_MIR") != nullptr; }

// 环境变量开关：BACKEND_DUMP_MIR 打开后在关键阶段之间打印 MIR
bool dumpEnabled() { return std::getenv("BACKEND_DUMP_MIR") != nullptr; }

std::string normalizedPassName(std::string name) {
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char ch) {
                   if (ch == '_') {
                     return '-';
                   }
                   return static_cast<char>(std::tolower(ch));
                 });
  return name;
}

std::vector<std::string> parseDisabledPassEnv() {
  const char *raw = std::getenv("BACKEND_DISABLE_PASSES");
  if (raw == nullptr) {
    return {};
  }

  std::string text(raw);
  for (char &ch : text) {
    if (ch == ',' || ch == ';') {
      ch = ' ';
    }
  }

  std::vector<std::string> names;
  std::istringstream input(text);
  std::string name;
  while (input >> name) {
    names.push_back(normalizedPassName(std::move(name)));
  }
  return names;
}

bool nameListDisables(const std::vector<std::string> &names,
                      const char *passName) {
  const std::string normalized = normalizedPassName(passName);
  return std::any_of(names.begin(), names.end(), [&](const std::string &name) {
    const std::string normalizedName = normalizedPassName(name);
    return normalizedName == "all" || normalizedName == normalized;
  });
}

bool passDisabled(const BackendOptions &options, const char *passName) {
  if (nameListDisables(options.disabledMachinePasses, passName)) {
    return true;
  }
  static const std::vector<std::string> envDisabled = parseDisabledPassEnv();
  return nameListDisables(envDisabled, passName);
}

template <typename PassT>
void addOptionalPass(MachineFunctionPassManager &MPM,
                     const BackendOptions &options, const char *passName,
                     PassT &&pass) {
  if (passDisabled(options, passName)) {
    return;
  }
  MPM.addPass(std::forward<PassT>(pass));
}

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
  MFAM.registerPass([] { return MachineLivenessAnalysis(); });
}

void MachinePassBuilder::registerModuleAnalyses(
    MachineModuleAnalysisManager &MMAM) {
  // 暂无模块级分析，保留注册入口
  (void)MMAM;
}

MachineFunctionPassManager
MachinePassBuilder::buildPreRAOptPipeline(const BackendOptions &Options) {
  MachineFunctionPassManager MPM;
  if (Options.optLevel == CodegenOptLevel::O0) {
    return MPM;
  }
  // 分配前优化挂在这里，此时仍是带虚拟寄存器的 MIR
  // 例如拷贝合并，机器层公共子表达式删除，块布局调整
  return MPM;
}

MachineFunctionPassManager
MachinePassBuilder::buildPostRAOptPipeline(const BackendOptions &Options) {
  MachineFunctionPassManager MPM;
  if (Options.optLevel == CodegenOptLevel::O0) {
    return MPM;
  }
  // 分配后优化挂在这里，此时寄存器和栈帧都已确定
  addOptionalPass(MPM, Options, "post-ra-peephole", PostRAPeepholePass());
  return MPM;
}

MachineFunctionPassManager
MachinePassBuilder::buildCodegenPipeline(const BackendOptions &Options) {
  MachineFunctionPassManager MPM;

  // 指令选择之后先整理控制流
  MPM.addPass(RebuildCFGPass());
  addOptionalPass(MPM, Options, "cleanup-cfg", CleanupCFGPass());
  addDebugPasses(MPM, "after isel");

  // 伪指令下降：SELECT 展开成分支，PHI 改写成前驱末尾拷贝
  MPM.addPass(LowerSelectPass());
  MPM.addPass(EliminatePhiPass());
  addOptionalPass(MPM, Options, "cleanup-cfg", CleanupCFGPass());
  addDebugPasses(MPM, "after pseudo lowering");

  // 分配前优化
  MPM.addPass(buildPreRAOptPipeline(Options));

  // 寄存器分配和栈帧下降
  MPM.addPass(RegisterAllocationPass());
  MPM.addPass(FrameLoweringPass());
  addDebugPasses(MPM, "after regalloc and frame");

  // 分配后优化
  MPM.addPass(buildPostRAOptPipeline(Options));
  addDebugPasses(MPM, "final");

  return MPM;
}

MachineModulePassManager
MachinePassBuilder::buildModulePipeline(const BackendOptions &Options,
                                        MachineFunctionAnalysisManager &MFAM) {
  MachineModulePassManager MPM;
  MPM.addPass(createMachineModuleFunctionBridge(buildCodegenPipeline(Options),
                                                MFAM));
  return MPM;
}

} // namespace riscv
