#pragma once

#include "MIR.hpp"
#include "Target.hpp"
#include "../lib/CFG.hpp"

namespace aarch64 {

class IRLowering {
  Module &module_;
  BackendOptions options_;

public:
  IRLowering(Module &module, const BackendOptions &options);
  MachineModule lowerModule();

private:
  void lowerGlobals(MachineModule &mm);
  MachineFunction lowerFunction(Function &function);
};

} // namespace aarch64
