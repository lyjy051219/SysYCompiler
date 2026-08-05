#pragma once

#include "aarch64/MIR.hpp"
#include "aarch64/opt/MachineAnalysis.hpp"

namespace aarch64 {

// 可开关机器层优化的底层实现函数。
// 新增 optional pass 时优先把实现和声明放在 rv/opt 这一层。
bool runBranchCompareFolding(MachineFunction &mf);
bool runPreRAGlobalAddressReuse(MachineFunction &mf);
bool runPreRAAffineCombine(MachineFunction &mf);
bool runMulAddFusion(MachineFunction &mf);
bool runSelectToCsel(MachineFunction &mf);
bool runRedundantSxtwElim(MachineFunction &mf);
bool runAddressIndexFold(MachineFunction &mf);
bool runPreRAAddressCombine(MachineFunction &mf);
bool runPreRACopyPropagation(MachineFunction &mf,
                             MachineFunctionAnalysisManager &am);
bool runPreRALocalCSE(MachineFunction &mf);
bool runPreRAGuardedLoopPreheader(MachineFunction &mf,
                                  MachineFunctionAnalysisManager &am);
bool runPreRAMachineDCE(MachineFunction &mf,
                        MachineFunctionAnalysisManager &am);
bool cleanupMachineCFG(MachineFunction &mf);
bool runPostRAPeephole(MachineFunction &mf);
bool runPostRALoopBackedgeBypass(MachineFunction &mf);
bool runPostRAListSchedule(
    MachineFunction &mf, const MachineBlockFrequencyAnalysis::Result &frequencies);
bool runBlockPlacement(
    MachineFunction &mf, const MachineLoopAnalysis::Result &loops,
    const MachineBlockFrequencyAnalysis::Result &frequencies);

} // namespace aarch64
