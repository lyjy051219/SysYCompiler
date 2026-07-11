#pragma once

#include "riscv/MIR.hpp"

namespace riscv {

// 可开关机器层优化的底层实现函数。
// 新增 optional pass 时优先把实现和声明放在 rv/opt 这一层。
bool runBranchCompareFolding(MachineFunction &mf);
bool runPreRAGlobalAddressReuse(MachineFunction &mf);
bool runPreRACopyPropagation(MachineFunction &mf);
bool runPreRALocalCSE(MachineFunction &mf);
bool cleanupMachineCFG(MachineFunction &mf);
bool runPostRAPeephole(MachineFunction &mf);
bool runBlockPlacement(MachineFunction &mf);

} // namespace riscv
