#include "aarch64/opt/MachinePassBuilder.hpp"

#include "aarch64/opt/CodegenPasses.hpp"
#include "aarch64/opt/MachineAnalysis.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace aarch64 {
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

MachineFunctionPassManager buildPreRALinearPipeline(
    const BackendOptions &Options) {
  MachineFunctionPassManager MPM;
  addOptionalPass(MPM, Options, "pre-ra-copy-prop",
                  PreRACopyPropagationPass());
  addOptionalPass(MPM, Options, "pre-ra-affine-combine",
                  PreRAAffineCombinePass());
  addOptionalPass(MPM, Options, "pre-ra-address-combine",
                  PreRAAddressCombinePass());
  addOptionalPass(MPM, Options, "pre-ra-local-cse", PreRALocalCSEPass());
  addOptionalPass(MPM, Options, "pre-ra-machine-dce",
                  PreRAMachineDCEPass());
  return MPM;
}

MachineFunctionPassManager buildPreRAFixedPointCorePipeline(
    const BackendOptions &Options) {
  MachineFunctionPassManager MPM;
  addOptionalPass(MPM, Options, "pre-ra-copy-prop",
                  PreRACopyPropagationPass());
  addOptionalPass(MPM, Options, "pre-ra-machine-dce",
                  PreRAMachineDCEPass());
  return MPM;
}

} // namespace

void MachinePassBuilder::registerFunctionAnalyses(
    MachineFunctionAnalysisManager &MFAM) {
  // 新增函数级分析在这里注册
  MFAM.registerPass([] { return MachineInstrStatsAnalysis(); });
  MFAM.registerPass([] { return MachineCFGAnalysis(); });
  MFAM.registerPass([] { return MachineLivenessAnalysis(); });
  MFAM.registerPass([] { return MachineLoopAnalysis(); });
  MFAM.registerPass([] { return MachineBlockFrequencyAnalysis(); });
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
  // 例如拷贝合并，机器层公共子表达式删除，块布局调整。
  addOptionalPass(MPM, Options, "branch-compare-folding",
                  BranchCompareFoldingPass());
  addOptionalPass(MPM, Options, "pre-ra-global-address-reuse",
                  PreRAGlobalAddressReusePass());

  // copy-prop / local CSE / DCE 里，只有 copy-prop 和 DCE 需要反复喂回；
  // local CSE 只跑一轮，减少重复 local CSE 带来的活跃区间膨胀。
  if (passDisabled(Options, "pre-ra-fixed-point")) {
    MachineFunctionPassManager linear = buildPreRALinearPipeline(Options);
    if (!linear.empty()) {
      MPM.addPass(std::move(linear));
    }
  } else {
    MachineFunctionPassManager prefix;
    addOptionalPass(prefix, Options, "pre-ra-copy-prop",
                    PreRACopyPropagationPass());
    addOptionalPass(prefix, Options, "pre-ra-affine-combine",
                    PreRAAffineCombinePass());
    addOptionalPass(prefix, Options, "pre-ra-address-combine",
                    PreRAAddressCombinePass());
    addOptionalPass(prefix, Options, "pre-ra-local-cse", PreRALocalCSEPass());
    if (!prefix.empty()) {
      MPM.addPass(std::move(prefix));
    }

    MachineFunctionPassManager core = buildPreRAFixedPointCorePipeline(Options);
    if (!core.empty()) {
      MPM.addPass(createMachineFixedPointPass<MachineFunction>(
          std::move(core), 3));
    }
  }
  addOptionalPass(MPM, Options, "pre-ra-guarded-loop-preheader",
                  PreRAGuardedLoopPreheaderPass());
  // 乘加融合放在地址/仿射归一化之后，此时 mul 与相邻 add 已稳定。
  addOptionalPass(MPM, Options, "mul-add-fusion", MulAddFusionPass());
  // 乘加融合生成 smaddl 后，其乘数若来自 SXTW 可消除冗余符号扩展。
  addOptionalPass(MPM, Options, "redundant-sxtw-elim", RedundantSxtwElimPass());
  // 注：address-index-fold（索引寻址折叠）默认关闭。其收益依赖「地址是否可
  // 共享/外提」的判断，而复用只有寄存器分配后才可见；分配前逐次折叠会破坏
  // 循环不变地址的共享，在 qemu 下反而变慢（crypto 家族 -24 分）。保留实现，
  // 待改造为分配后 pass 再启用。可用 BACKEND_DISABLE_PASSES 反向验证。
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
  // 寄存器合并可能把分配前保留的 phi 跳板变成空块；分配后重新建图，
  // 再用独立开关清理这些只剩跳转的块和冗余顺序跳转。
  MPM.addPass(RebuildCFGPass());
  addOptionalPass(MPM, Options, "post-ra-cleanup-cfg", CleanupCFGPass());
  addOptionalPass(MPM, Options, "block-placement", BlockPlacementPass());
  MPM.addPass(RebuildCFGPass());
  addOptionalPass(MPM, Options, "post-ra-loop-backedge-bypass",
                  PostRALoopBackedgeBypassPass());
  MPM.addPass(RebuildCFGPass());
  addOptionalPass(MPM, Options, "post-ra-cleanup-cfg", CleanupCFGPass());
  // 最后做块内 list scheduling，不再改变 CFG，只重排单个基本块内部顺序。
  addOptionalPass(MPM, Options, "post-ra-list-schedule",
                  PostRAListSchedulePass());
  return MPM;
}

MachineFunctionPassManager
MachinePassBuilder::buildCodegenPipeline(const BackendOptions &Options) {
  MachineFunctionPassManager MPM;

  // 指令选择之后先整理控制流
  MPM.addPass(RebuildCFGPass());
  addOptionalPass(MPM, Options, "cleanup-cfg", CleanupCFGPass());
  addDebugPasses(MPM, "after isel");

  // 伪指令下降：优先把整型 SELECT 转成 ARM64 的 csel（折进产生条件的比较），
  // 再对剩余的走无分支掩码 / 分支展开。select-to-csel 可单独关闭以便 A/B。
  addOptionalPass(MPM, Options, "select-to-csel", SelectToCselPass());
  addOptionalPass(MPM, Options, "branchless-select",
                  LowerSelectPass(/*branchlessOnly=*/true));
  MPM.addPass(LowerSelectPass(/*branchlessOnly=*/false));
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

} // namespace aarch64
