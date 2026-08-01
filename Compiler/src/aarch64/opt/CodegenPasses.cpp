// CodegenPasses.cpp —— 代码生成各 pass 的 run 入口薄封装(AArch64 版)。
// 每个 pass 把实际变换委托给对应的 runXxx/eliminateXxx 等实现函数,
// 并返回 MachinePreservationStatus 告诉 pass 管理器哪些分析结果仍然有效:
// all() 表示未改动(全部保留),none() 表示改动较大(全部失效),
// 也可用 keep<Analysis>() 精确保留部分分析,避免不必要的重算。
// 与 rv 版共用同一套 pass 语义,额外多出四个 AArch64 专有 pass:
// MulAddFusion(乘加融合)、SelectToCsel(转 csel)、RedundantSxtwElim(消冗余符号扩展)、
// AddressIndexFold(索引寻址折叠)。
#include "aarch64/opt/CodegenPasses.hpp"

#include "aarch64/Frame.hpp"
#include "aarch64/Passes.hpp"
#include "aarch64/RegAlloc.hpp"
#include "aarch64/opt/MachineAnalysis.hpp"
#include "aarch64/opt/MachineOptPasses.hpp"

#include <cstdlib>
#include <iostream>

namespace aarch64 {

namespace {

// 一个常用的保留集合:CFG 结构未变时,保留 CFG、循环、块频率三项分析。
MachinePreservationStatus preserveBlockStructureAnalyses() {
  auto PA = MachinePreservationStatus::none();
  PA.keep<MachineCFGAnalysis>();
  PA.keep<MachineLoopAnalysis>();
  PA.keep<MachineBlockFrequencyAnalysis>();
  return PA;
}

} // namespace

// 重建 CFG:根据指令中的分支目标重新计算各块的前驱/后继关系。
MachinePreservationStatus
RebuildCFGPass::run(MachineFunction &mf, MachineFunctionAnalysisManager &am) {
  (void)am;
  rebuildCFG(mf);
  // 只改写了块的前驱后继，指令本身没有变化，统计分析仍然有效
  auto PA = MachinePreservationStatus::none();
  PA.keep<MachineInstrStatsAnalysis>();
  return PA;
}

// 清理 CFG:合并/删除空跳板块、冗余顺序跳转等。有改动则全部失效。
MachinePreservationStatus
CleanupCFGPass::run(MachineFunction &mf, MachineFunctionAnalysisManager &am) {
  (void)am;
  const bool changed = cleanupMachineCFG(mf);
  return changed ? MachinePreservationStatus::none()
                 : MachinePreservationStatus::all();
}

// 下降 SELECT 伪指令:branchlessOnly 为真时只做无分支整数形式,
// 否则把剩余 SELECT 展开成条件分支结构(会改动 CFG)。
MachinePreservationStatus
LowerSelectPass::run(MachineFunction &mf, MachineFunctionAnalysisManager &am) {
  (void)am;
  const bool changed = lowerSelectPseudo(mf, branchlessOnly);
  return changed ? MachinePreservationStatus::none()
                 : MachinePreservationStatus::all();
}

// 消除 PHI 节点:在前驱块末尾插入拷贝,退出 SSA 形式,为寄存器分配做准备。
MachinePreservationStatus
EliminatePhiPass::run(MachineFunction &mf, MachineFunctionAnalysisManager &am) {
  (void)am;
  eliminatePhiNodes(mf);
  return MachinePreservationStatus::none();
}

// 寄存器分配:把虚拟寄存器指派为物理寄存器,溢出时生成栈访问。改动很大,全部失效。
MachinePreservationStatus
RegisterAllocationPass::run(MachineFunction &mf,
                            MachineFunctionAnalysisManager &am) {
  allocateRegisters(mf, am);
  return MachinePreservationStatus::none();
}

// frame lowering:确定栈帧布局(各栈对象的偏移),并把 FrameIndex 操作数
// 展开为 sp 相对的具体偏移。之后 MIR 中不再有抽象栈下标。
MachinePreservationStatus
FrameLoweringPass::run(MachineFunction &mf,
                       MachineFunctionAnalysisManager &am) {
  (void)am;
  FrameLowering lowering;
  lowering.assignFrameLayout(mf);
  lowering.expandFrameIndices(mf);
  return MachinePreservationStatus::none();
}

// 分支-比较折叠:把独立的比较指令与其后的条件分支合并为 ARM64 的复合分支
// (cmp + b.cond)。仅在块结构不变的前提下改动块内指令。
MachinePreservationStatus
BranchCompareFoldingPass::run(MachineFunction &mf,
                              MachineFunctionAnalysisManager &am) {
  (void)am;
  const bool changed = runBranchCompareFolding(mf);
  return changed ? preserveBlockStructureAnalyses()
                 : MachinePreservationStatus::all();
}

// 分配前全局地址复用:合并对同一全局符号的重复取址(adrp+add),减少冗余地址计算。
MachinePreservationStatus
PreRAGlobalAddressReusePass::run(MachineFunction &mf,
                                 MachineFunctionAnalysisManager &am) {
  (void)am;
  const bool changed = runPreRAGlobalAddressReuse(mf);
  return changed ? preserveBlockStructureAnalyses()
                 : MachinePreservationStatus::all();
}

// 分配前地址合并:把基址加偏移的地址计算尽量并入访存指令的偏移域,减少 add。
MachinePreservationStatus
PreRAAddressCombinePass::run(MachineFunction &mf,
                             MachineFunctionAnalysisManager &am) {
  (void)am;
  const bool changed = runPreRAAddressCombine(mf);
  return changed ? preserveBlockStructureAnalyses()
                 : MachinePreservationStatus::all();
}

// 分配前拷贝传播:把对拷贝源的引用直接替换成原始寄存器,消除中间拷贝。
MachinePreservationStatus
PreRACopyPropagationPass::run(MachineFunction &mf,
                              MachineFunctionAnalysisManager &am) {
  (void)am;
  const bool changed = runPreRACopyPropagation(mf);
  return changed ? preserveBlockStructureAnalyses()
                 : MachinePreservationStatus::all();
}

// 乘加融合:把相邻的 mul 与 add/sub 合并为 madd/msub(32 位)或 smaddl/smsubl
// (32x32+64 位),减少一条指令。只改块内指令、不动 CFG,保留块结构相关分析。
MachinePreservationStatus
MulAddFusionPass::run(MachineFunction &mf,
                      MachineFunctionAnalysisManager &am) {
  (void)am;
  // 只改块内指令、不动 CFG，保留块结构相关分析。
  const bool changed = runMulAddFusion(mf);
  return changed ? preserveBlockStructureAnalyses()
                 : MachinePreservationStatus::all();
}

// SELECT 转 csel:把整型 SELECT 及其产生条件的比较合并成 ARM64 的 csel
// (无分支条件选择),避免展开成分支。只改块内指令、不动 CFG。
MachinePreservationStatus
SelectToCselPass::run(MachineFunction &mf,
                      MachineFunctionAnalysisManager &am) {
  (void)am;
  // 只改块内指令、不动 CFG。
  const bool changed = runSelectToCsel(mf);
  return changed ? preserveBlockStructureAnalyses()
                 : MachinePreservationStatus::all();
}

// 冗余符号扩展消除:乘加融合生成 smaddl/smull 后,其 w 视图乘数会自行符号扩展,
// 若该乘数原本来自一条 SXTW,则这条 SXTW 变得多余,予以删除。只改块内指令。
MachinePreservationStatus
RedundantSxtwElimPass::run(MachineFunction &mf,
                           MachineFunctionAnalysisManager &am) {
  (void)am;
  const bool changed = runRedundantSxtwElim(mf);
  return changed ? preserveBlockStructureAnalyses()
                 : MachinePreservationStatus::all();
}

// 索引寻址折叠:把 base + index<<scale 的地址计算折进访存指令的寄存器索引寻址
// [base, index, lsl/sxtw #scale],省去单独的移位与加法。只改块内指令。
MachinePreservationStatus
AddressIndexFoldPass::run(MachineFunction &mf,
                          MachineFunctionAnalysisManager &am) {
  (void)am;
  const bool changed = runAddressIndexFold(mf);
  return changed ? preserveBlockStructureAnalyses()
                 : MachinePreservationStatus::all();
}


// 分配前局部公共子表达式删除:在基本块内复用已算过的相同表达式结果。
MachinePreservationStatus
PreRALocalCSEPass::run(MachineFunction &mf,
                       MachineFunctionAnalysisManager &am) {
  (void)am;
  const bool changed = runPreRALocalCSE(mf);
  return changed ? preserveBlockStructureAnalyses()
                 : MachinePreservationStatus::all();
}

// 分配前带 guard 的循环预头块:为循环插入预头块(可能新增块),依赖分析结果 am。
MachinePreservationStatus PreRAGuardedLoopPreheaderPass::run(
    MachineFunction &mf, MachineFunctionAnalysisManager &am) {
  const bool changed = runPreRAGuardedLoopPreheader(mf, am);
  return changed ? MachinePreservationStatus::none()
                 : MachinePreservationStatus::all();
}

// 分配前机器级死代码删除:删除结果无人使用且无副作用的指令。
// 只删指令不改块结构,故有改动时仍保留 CFG/循环/块频率分析。
MachinePreservationStatus
PreRAMachineDCEPass::run(MachineFunction &mf,
                         MachineFunctionAnalysisManager &am) {
  const bool changed = runPreRAMachineDCE(mf, am);
  if (!changed) {
    return MachinePreservationStatus::all();
  }

  auto PA = MachinePreservationStatus::none();
  PA.keep<MachineCFGAnalysis>();
  PA.keep<MachineLoopAnalysis>();
  PA.keep<MachineBlockFrequencyAnalysis>();
  return PA;
}

// 分配后窥孔优化:在已确定物理寄存器的 MIR 上做局部指令替换/合并/删除。
MachinePreservationStatus
PostRAPeepholePass::run(MachineFunction &mf,
                        MachineFunctionAnalysisManager &am) {
  (void)am;
  const bool changed = runPostRAPeephole(mf);
  return changed ? MachinePreservationStatus::none()
                 : MachinePreservationStatus::all();
}

// 分配后循环回边旁路:优化回边跳转路径,减少循环边界处的跳转开销。
MachinePreservationStatus PostRALoopBackedgeBypassPass::run(
    MachineFunction &mf, MachineFunctionAnalysisManager &am) {
  (void)am;
  const bool changed = runPostRALoopBackedgeBypass(mf);
  return changed ? MachinePreservationStatus::none()
                 : MachinePreservationStatus::all();
}

// 分配后表调度(list scheduling):按块频率对单个基本块内指令重排以隐藏延迟,
// 不改动 CFG。重排只影响块内顺序,故 CFG/循环/频率/活跃性/指令统计均可保留。
MachinePreservationStatus
PostRAListSchedulePass::run(MachineFunction &mf,
                            MachineFunctionAnalysisManager &am) {
  const auto &frequencies = am.getResult<MachineBlockFrequencyAnalysis>(mf);
  const bool changed = runPostRAListSchedule(mf, frequencies);
  if (!changed) {
    return MachinePreservationStatus::all();
  }

  auto PA = MachinePreservationStatus::none();
  PA.keep<MachineCFGAnalysis>();
  PA.keep<MachineLoopAnalysis>();
  PA.keep<MachineBlockFrequencyAnalysis>();
  PA.keep<MachineLivenessAnalysis>();
  PA.keep<MachineInstrStatsAnalysis>();
  return PA;
}

// 块布局:依据循环信息和块频率重排基本块顺序,使热路径顺序落靠、减少跳转。
MachinePreservationStatus
BlockPlacementPass::run(MachineFunction &mf,
                        MachineFunctionAnalysisManager &am) {
  const auto &loops = am.getResult<MachineLoopAnalysis>(mf);
  const auto &frequencies = am.getResult<MachineBlockFrequencyAnalysis>(mf);
  const bool changed = runBlockPlacement(mf, loops, frequencies);
  return changed ? MachinePreservationStatus::none()
                 : MachinePreservationStatus::all();
}

// 机器校验 pass:只做完整性检查(有无非法虚寄存器),不修改 MIR,故保留全部分析。
MachinePreservationStatus
MachineVerifyPass::run(MachineFunction &mf,
                       MachineFunctionAnalysisManager &am) {
  (void)am;
  validateNoInvalidVRegs(mf);
  return MachinePreservationStatus::all();
}

// 机器转储 pass:把当前 MIR 打印到 stderr,并标注阶段 tag,只观察不修改。
MachinePreservationStatus
MachineDumpPass::run(MachineFunction &mf,
                     MachineFunctionAnalysisManager &am) {
  (void)am;
  std::cerr << "# MIR dump at " << tag << "\n";
  dumpMachineFunction(mf, std::cerr);
  return MachinePreservationStatus::all();
}

} // namespace aarch64
