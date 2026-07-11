#pragma once

#include "MIR.hpp"
#include "MachinePassManager.hpp"

namespace riscv {

struct RegAllocStats {
  uint64_t rounds = 0;
  uint64_t coalescedMoves = 0;
  uint64_t spilledVRegs = 0;
  uint64_t spillLoads = 0;
  uint64_t spillStores = 0;
  uint64_t rematerializations = 0;
  uint64_t spillSlots = 0;
};

void allocateRegisters(MachineFunction &mf);
void allocateRegisters(MachineFunction &mf, MachineFunctionAnalysisManager &am);
void allocateRegisters(MachineFunction &mf, RegAllocStats *stats);

} // namespace riscv
