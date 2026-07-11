#include "riscv/opt/CodegenPasses.hpp"

#include "riscv/Frame.hpp"
#include "riscv/Passes.hpp"
#include "riscv/RegAlloc.hpp"
#include "riscv/opt/MachineAnalysis.hpp"
#include "riscv/opt/MachineOptPasses.hpp"

#include <cstdlib>
#include <iostream>

namespace riscv {

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
  return changed ? MachinePreservationStatus::none()
                 : MachinePreservationStatus::all();
}

MachinePreservationStatus
PreRAGlobalAddressReusePass::run(MachineFunction &mf,
                                 MachineFunctionAnalysisManager &am) {
  (void)am;
  const bool changed = runPreRAGlobalAddressReuse(mf);
  return changed ? MachinePreservationStatus::none()
                 : MachinePreservationStatus::all();
}

MachinePreservationStatus
PreRACopyPropagationPass::run(MachineFunction &mf,
                              MachineFunctionAnalysisManager &am) {
  (void)am;
  const bool changed = runPreRACopyPropagation(mf);
  return changed ? MachinePreservationStatus::none()
                 : MachinePreservationStatus::all();
}

MachinePreservationStatus
PreRALocalCSEPass::run(MachineFunction &mf,
                       MachineFunctionAnalysisManager &am) {
  (void)am;
  const bool changed = runPreRALocalCSE(mf);
  return changed ? MachinePreservationStatus::none()
                 : MachinePreservationStatus::all();
}

MachinePreservationStatus
PostRAPeepholePass::run(MachineFunction &mf,
                        MachineFunctionAnalysisManager &am) {
  (void)am;
  const bool changed = runPostRAPeephole(mf);
  return changed ? MachinePreservationStatus::none()
                 : MachinePreservationStatus::all();
}

MachinePreservationStatus
BlockPlacementPass::run(MachineFunction &mf,
                         MachineFunctionAnalysisManager &am) {
  (void)am;
  const bool changed = runBlockPlacement(mf);
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
