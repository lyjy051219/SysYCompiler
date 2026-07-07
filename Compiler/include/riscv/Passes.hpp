#pragma once

#include "MIR.hpp"

#include <vector>

namespace riscv {

class MachineFunctionPassManager {
public:
  using Pass = bool (*)(MachineFunction &);

  void addPass(Pass pass);
  bool run(MachineFunction &mf) const;

private:
  std::vector<Pass> passes_;
};

void rebuildCFG(MachineFunction &mf);
void cleanupMachineCFG(MachineFunction &mf);
void eliminatePhiNodes(MachineFunction &mf);
void lowerSelectPseudo(MachineFunction &mf);
void runPreRAOptimizations(MachineFunction &mf);
void runPostRAOptimizations(MachineFunction &mf);
bool runPostRAPeephole(MachineFunction &mf);

} // namespace riscv
