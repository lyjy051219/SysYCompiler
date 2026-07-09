#include "../../../include/riscv/CodegenPasses.hpp"

#include "../../../include/riscv/Frame.hpp"
#include "../../../include/riscv/MachineAnalysis.hpp"
#include "../../../include/riscv/Passes.hpp"
#include "../../../include/riscv/RegAlloc.hpp"

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
  cleanupMachineCFG(mf);
  // 当前实现是空操作，等实际做清理时改为按需失效
  return MachinePreservationStatus::all();
}

MachinePreservationStatus
LowerSelectPass::run(MachineFunction &mf, MachineFunctionAnalysisManager &am) {
  (void)am;
  lowerSelectPseudo(mf);
  return MachinePreservationStatus::none();
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
  (void)am;
  allocateRegisters(mf);
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
PostRAPeepholePass::run(MachineFunction &mf,
                        MachineFunctionAnalysisManager &am) {
  (void)am;
  const bool changed = runPostRAPeephole(mf);
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
