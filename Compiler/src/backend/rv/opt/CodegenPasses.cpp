#include "riscv/opt/CodegenPasses.hpp"

#include "riscv/Frame.hpp"
#include "riscv/Passes.hpp"
#include "riscv/RegAlloc.hpp"
#include "riscv/opt/MachineAnalysis.hpp"
#include "riscv/opt/MachineOptPasses.hpp"

#include <cstdlib>
#include <iostream>

namespace riscv {

namespace {

MachinePreservationStatus preserveBlockStructureAnalyses() {
  auto PA = MachinePreservationStatus::none();
  PA.keep<MachineCFGAnalysis>();
  PA.keep<MachineLoopAnalysis>();
  PA.keep<MachineBlockFrequencyAnalysis>();
  return PA;
}

} // namespace

MachinePreservationStatus
RebuildCFGPass::run(MachineFunction &mf, MachineFunctionAnalysisManager &am) {
  (void)am;
  rebuildCFG(mf);
  // 只改写了块的前驱后继，指令本身没有变化，统计分析仍然有效
  auto PA = MachinePreservationStatus::none();
  PA.keep<MachineInstrStatsAnalysis>();
  return PA;
}

MachinePreservationStatus
CleanupCFGPass::run(MachineFunction &mf, MachineFunctionAnalysisManager &am) {
  (void)am;
  const bool changed = cleanupMachineCFG(mf);
  return changed ? MachinePreservationStatus::none()
                 : MachinePreservationStatus::all();
}

MachinePreservationStatus
LowerSelectPass::run(MachineFunction &mf, MachineFunctionAnalysisManager &am) {
  (void)am;
  const bool changed = lowerSelectPseudo(mf, branchlessOnly);
  return changed ? MachinePreservationStatus::none()
                 : MachinePreservationStatus::all();
}

MachinePreservationStatus
EliminatePhiPass::run(MachineFunction &mf, MachineFunctionAnalysisManager &am) {
  (void)am;
  eliminatePhiNodes(mf);
  return MachinePreservationStatus::none();
}

MachinePreservationStatus
RegisterAllocationPass::run(MachineFunction &mf,
                            MachineFunctionAnalysisManager &am) {
  allocateRegisters(mf, am);
  return MachinePreservationStatus::none();
}

MachinePreservationStatus
FrameLoweringPass::run(MachineFunction &mf,
                       MachineFunctionAnalysisManager &am) {
  (void)am;
  FrameLowering lowering;
  lowering.assignFrameLayout(mf);
  lowering.expandFrameIndices(mf);
  return MachinePreservationStatus::none();
}

MachinePreservationStatus
BranchCompareFoldingPass::run(MachineFunction &mf,
                              MachineFunctionAnalysisManager &am) {
  (void)am;
  const bool changed = runBranchCompareFolding(mf);
  return changed ? preserveBlockStructureAnalyses()
                 : MachinePreservationStatus::all();
}

MachinePreservationStatus
PreRAGlobalAddressReusePass::run(MachineFunction &mf,
                                 MachineFunctionAnalysisManager &am) {
  (void)am;
  const bool changed = runPreRAGlobalAddressReuse(mf);
  return changed ? preserveBlockStructureAnalyses()
                 : MachinePreservationStatus::all();
}

MachinePreservationStatus
PreRALoopInvariantImmediateHoistPass::run(
    MachineFunction &mf, MachineFunctionAnalysisManager &am) {
  const bool changed = runPreRALoopInvariantImmediateHoist(mf, am);
  return changed ? preserveBlockStructureAnalyses()
                 : MachinePreservationStatus::all();
}

MachinePreservationStatus
PreRAAddressCombinePass::run(MachineFunction &mf,
                             MachineFunctionAnalysisManager &am) {
  (void)am;
  const bool changed = runPreRAAddressCombine(mf);
  return changed ? preserveBlockStructureAnalyses()
                 : MachinePreservationStatus::all();
}

MachinePreservationStatus
PreRACopyPropagationPass::run(MachineFunction &mf,
                              MachineFunctionAnalysisManager &am) {
  (void)am;
  const bool changed = runPreRACopyPropagation(mf);
  return changed ? preserveBlockStructureAnalyses()
                 : MachinePreservationStatus::all();
}

MachinePreservationStatus
PreRALocalCSEPass::run(MachineFunction &mf,
                       MachineFunctionAnalysisManager &am) {
  (void)am;
  const bool changed = runPreRALocalCSE(mf);
  return changed ? preserveBlockStructureAnalyses()
                 : MachinePreservationStatus::all();
}

MachinePreservationStatus PreRAGuardedLoopPreheaderPass::run(
    MachineFunction &mf, MachineFunctionAnalysisManager &am) {
  const bool changed = runPreRAGuardedLoopPreheader(mf, am);
  return changed ? MachinePreservationStatus::none()
                 : MachinePreservationStatus::all();
}

MachinePreservationStatus PreRALoopLatchRecurrenceSinkPass::run(
    MachineFunction &mf, MachineFunctionAnalysisManager &am) {
  const bool changed = runPreRALoopLatchRecurrenceSink(mf, am);
  return changed ? preserveBlockStructureAnalyses()
                 : MachinePreservationStatus::all();
}

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

MachinePreservationStatus
PostRAPeepholePass::run(MachineFunction &mf,
                        MachineFunctionAnalysisManager &am) {
  (void)am;
  const bool changed = runPostRAPeephole(mf);
  return changed ? MachinePreservationStatus::none()
                 : MachinePreservationStatus::all();
}

MachinePreservationStatus PostRALoopBackedgeBypassPass::run(
    MachineFunction &mf, MachineFunctionAnalysisManager &am) {
  (void)am;
  const bool changed = runPostRALoopBackedgeBypass(mf);
  return changed ? MachinePreservationStatus::none()
                 : MachinePreservationStatus::all();
}

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

MachinePreservationStatus
BlockPlacementPass::run(MachineFunction &mf,
                        MachineFunctionAnalysisManager &am) {
  const auto &loops = am.getResult<MachineLoopAnalysis>(mf);
  const auto &frequencies = am.getResult<MachineBlockFrequencyAnalysis>(mf);
  const bool changed = runBlockPlacement(mf, loops, frequencies);
  return changed ? MachinePreservationStatus::none()
                 : MachinePreservationStatus::all();
}

MachinePreservationStatus
MachineVerifyPass::run(MachineFunction &mf,
                       MachineFunctionAnalysisManager &am) {
  (void)am;
  validateNoInvalidVRegs(mf);
  return MachinePreservationStatus::all();
}

MachinePreservationStatus
MachineDumpPass::run(MachineFunction &mf,
                     MachineFunctionAnalysisManager &am) {
  (void)am;
  std::cerr << "# MIR dump at " << tag << "\n";
  dumpMachineFunction(mf, std::cerr);
  return MachinePreservationStatus::all();
}

} // namespace riscv
