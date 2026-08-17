#include "riscv/VectorLegalize.hpp"

#include "riscv/Target.hpp"

namespace riscv {

void legalizeAndSelectGenericVectors(MachineModule &module) {
  validateGenericVectorMIR(module);
  if (containsGenericVectorMIR(module)) {
    throw BackendError(
        "generic vector MIR reached the target boundary; configure the final "
        "SIMD legalizer, register file and instruction mapping before "
        "emitting assembly");
  }
}

} // namespace riscv
