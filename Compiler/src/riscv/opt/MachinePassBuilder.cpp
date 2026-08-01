// MachinePassBuilder.cpp —— 后端代码生成流水线的装配中心。
// 负责注册各类机器分析,并按固定顺序把代码生成与优化 pass 串成流水线:
// 指令选择后整理 CFG，再依次执行伪指令 lowering、分配前优化、寄存器分配与frame lowering、分配后优化。
// 每个可选 pass 都能被关闭:通过 BackendOptions.disabledMachinePasses 或环境变量
// BACKEND_DISABLE_PASSES(逗号/分号/空格分隔的 pass 名,或 all 关闭全部可选 pass)。
// 另有 BACKEND_VERIFY_MIR / BACKEND_DUMP_MIR 两个调试开关,在阶段之间插入校验/打印。
#include "riscv/opt/MachinePassBuilder.hpp"

#include "riscv/opt/CodegenPasses.hpp"
#include "riscv/opt/MachineAnalysis.hpp"

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

// 归一化 pass 名:下划线转连字符并全部小写,使开关匹配对大小写和分隔符不敏感。
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

// 解析 BACKEND_DISABLE_PASSES 环境变量:把逗号/分号统一成空格再按空白切分,
// 返回归一化后的待关闭 pass 名列表;未设置时返回空列表。
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

// 判断名字列表是否要求关闭某 pass:列表含 "all" 或含该 pass 的归一化名则命中。
bool nameListDisables(const std::vector<std::string> &names,
                      const char *passName) {
  const std::string normalized = normalizedPassName(passName);
  return std::any_of(names.begin(), names.end(), [&](const std::string &name) {
    const std::string normalizedName = normalizedPassName(name);
    return normalizedName == "all" || normalizedName == normalized;
  });
}

// 综合判断某 pass 是否被关闭:先看 options 里的显式关闭列表,
// 再看环境变量列表(仅解析一次并缓存于 static)。二者任一命中即关闭。
bool passDisabled(const BackendOptions &options, const char *passName) {
  if (nameListDisables(options.disabledMachinePasses, passName)) {
    return true;
  }
  static const std::vector<std::string> envDisabled = parseDisabledPassEnv();
  return nameListDisables(envDisabled, passName);
}

// 按开关有条件地把一个 pass 加入流水线:若该 pass 被关闭则跳过,否则追加。
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

// 分配前线性流水线(当固定点迭代被关闭时使用):
// 拷贝传播、仿射合并、地址合并、局部 CSE、机器 DCE，各跑一遍不迭代。
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

// 分配前固定点迭代的核心体:只放需要反复喂回的 拷贝传播 + 机器 DCE,
// 这两者互相触发新的优化机会,故放进固定点循环反复运行至收敛。
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

// 注册全部函数级分析,供各 pass 通过 am.getResult<...>() 按需获取。
void MachinePassBuilder::registerFunctionAnalyses(
    MachineFunctionAnalysisManager &MFAM) {
  // 新增函数级分析在这里注册
  MFAM.registerPass([] { return MachineInstrStatsAnalysis(); });
  MFAM.registerPass([] { return MachineCFGAnalysis(); });
  MFAM.registerPass([] { return MachineLivenessAnalysis(); });
  MFAM.registerPass([] { return MachineLoopAnalysis(); });
  MFAM.registerPass([] { return MachineBlockFrequencyAnalysis(); });
}

// 注册模块级分析(当前无,保留入口以便将来扩展)。
void MachinePassBuilder::registerModuleAnalyses(
    MachineModuleAnalysisManager &MMAM) {
  // 暂无模块级分析，保留注册入口
  (void)MMAM;
}

// 装配分配前优化流水线。O0 直接返回空流水线不做优化。
// 顺序：分支比较折叠、全局地址复用、一段前缀优化加固定点核心（或线性回退）、带 guard 的循环预头块。
// pre-ra-fixed-point 被关闭时改走线性版本。
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
  // 带 guard 的循环预头默认关闭（BACKEND_ENABLE_GUARDED_LOOP_PREHEADER 打开）。
  if (std::getenv("BACKEND_ENABLE_GUARDED_LOOP_PREHEADER") != nullptr) {
    addOptionalPass(MPM, Options, "pre-ra-guarded-loop-preheader",
                    PreRAGuardedLoopPreheaderPass());
  }
  return MPM;
}

// 装配分配后优化流水线。O0 直接返回空。此时寄存器和栈帧都已固定。
// 顺序：窥孔、重建 CFG、清理 CFG、块布局、重建 CFG、回边旁路、重建 CFG、清理 CFG、块内表调度。
// 多次穿插重建 CFG 是因为
// 前面的清理/布局/旁路会改动块的连接关系,需重新建图供后续 pass 使用。
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
  // 循环回边旁路默认关闭（BACKEND_ENABLE_LOOP_BACKEDGE_BYPASS 打开）。
  if (std::getenv("BACKEND_ENABLE_LOOP_BACKEDGE_BYPASS") != nullptr) {
    addOptionalPass(MPM, Options, "post-ra-loop-backedge-bypass",
                    PostRALoopBackedgeBypassPass());
  }
  MPM.addPass(RebuildCFGPass());
  addOptionalPass(MPM, Options, "post-ra-cleanup-cfg", CleanupCFGPass());
  // 最后做块内 list scheduling，不再改变 CFG，只重排单个基本块内部顺序。
  addOptionalPass(MPM, Options, "post-ra-list-schedule",
                  PostRAListSchedulePass());
  return MPM;
}

// 装配完整的函数级代码生成流水线,串起从指令选择之后到最终 MIR 的全部阶段。
// 各阶段之间按开关插入调试用的校验/打印(addDebugPasses)。
MachineFunctionPassManager
MachinePassBuilder::buildCodegenPipeline(const BackendOptions &Options) {
  MachineFunctionPassManager MPM;

  // 指令选择之后先整理控制流
  MPM.addPass(RebuildCFGPass());
  addOptionalPass(MPM, Options, "cleanup-cfg", CleanupCFGPass());
  addDebugPasses(MPM, "after isel");

  // 伪指令 lowering：先尝试保守的无分支整数 SELECT，再把剩余 SELECT 展开成
  // 分支；branchless-select 可单独关闭以便 A/B。
  addOptionalPass(MPM, Options, "branchless-select",
                  LowerSelectPass(/*branchlessOnly=*/true));
  MPM.addPass(LowerSelectPass(/*branchlessOnly=*/false));
  MPM.addPass(EliminatePhiPass());
  addOptionalPass(MPM, Options, "cleanup-cfg", CleanupCFGPass());
  addDebugPasses(MPM, "after pseudo lowering");

  // 分配前优化
  MPM.addPass(buildPreRAOptPipeline(Options));

  // 寄存器分配和frame lowering
  MPM.addPass(RegisterAllocationPass());
  MPM.addPass(FrameLoweringPass());
  addDebugPasses(MPM, "after regalloc and frame");

  // 分配后优化
  MPM.addPass(buildPostRAOptPipeline(Options));
  addDebugPasses(MPM, "final");

  return MPM;
}

// 装配模块级流水线:通过函数桥接器把上面的函数级代码生成流水线套用到模块内每个函数,
// MFAM 提供函数级分析。这是 Target.cpp 里最终运行的顶层流水线。
MachineModulePassManager
MachinePassBuilder::buildModulePipeline(const BackendOptions &Options,
                                        MachineFunctionAnalysisManager &MFAM) {
  MachineModulePassManager MPM;
  MPM.addPass(createMachineModuleFunctionBridge(buildCodegenPipeline(Options),
                                                MFAM));
  return MPM;
}

} // namespace riscv
