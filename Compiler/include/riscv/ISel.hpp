#pragma once

#include "MIR.hpp"
#include "../lib/CFG.hpp"

namespace riscv {

class IRLowering {
  Module &module_;

public:
  explicit IRLowering(Module &module);
  MachineModule lowerModule();

private:
  void lowerGlobals(MachineModule &mm);
  MachineFunction lowerFunction(Function &function);
};

} // namespace riscv
