#pragma once

#include "MIR.hpp"

namespace riscv {

void rebuildCFG(MachineFunction &mf);
void cleanupMachineCFG(MachineFunction &mf);
void eliminatePhiNodes(MachineFunction &mf);
void lowerSelectPseudo(MachineFunction &mf);
void runPreRAOptimizations(MachineFunction &mf);
void runPostRAOptimizations(MachineFunction &mf);

} // namespace riscv
