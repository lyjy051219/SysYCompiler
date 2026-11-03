// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "riscv/MIR.hpp"
#include "riscv/opt/MachineAnalysis.hpp"

namespace riscv {

// 可开关机器层优化的底层实现函数。
// 新增 optional pass 时优先把实现和声明放在 rv/opt 这一层。
bool runBranchCompareFolding(MachineFunction &mf);
bool runPreRAGlobalAddressReuse(MachineFunction &mf);
bool runPreRAAffineCombine(MachineFunction &mf);
bool runPreRAAddressCombine(MachineFunction &mf);
bool runPreRACopyPropagation(MachineFunction &mf,
                             MachineFunctionAnalysisManager &am);
bool runPreRALocalCSE(MachineFunction &mf);
bool runPreRAGuardedLoopPreheader(MachineFunction &mf,
                                   MachineFunctionAnalysisManager &am);
bool runPreRAMachineLICM(MachineFunction &mf,
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

} // namespace riscv
