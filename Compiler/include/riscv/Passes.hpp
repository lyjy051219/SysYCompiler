#pragma once

#include "MIR.hpp"

namespace riscv {

// 机器层各阶段的底层实现函数
// 旧的函数指针式 MachineFunctionPassManager 已废弃，
// 统一改用 MachinePassManager.hpp 的基础设施，
// 这些函数由 CodegenPasses.hpp 中对应的 pass 包装后进入流水线

void rebuildCFG(MachineFunction &mf);
void cleanupMachineCFG(MachineFunction &mf);
void eliminatePhiNodes(MachineFunction &mf);
void lowerSelectPseudo(MachineFunction &mf);
bool runPostRAPeephole(MachineFunction &mf);

} // namespace riscv
