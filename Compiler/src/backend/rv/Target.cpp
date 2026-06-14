#include "../../../include/riscv/Target.hpp"

#include "../../../include/riscv/AsmPrinter.hpp"
#include "../../../include/riscv/Frame.hpp"
#include "../../../include/riscv/ISel.hpp"
#include "../../../include/riscv/Passes.hpp"
#include "../../../include/riscv/RegAlloc.hpp"

#include <ostream>

namespace riscv {

void emitAssembly(Module &module, std::ostream &out,
                  const BackendOptions &options) {
  (void)options;
  IRLowering lowering(module);
  MachineModule machine = lowering.lowerModule();
  for (MachineFunction &function : machine.functions) {
    rebuildCFG(function);
    cleanupMachineCFG(function);
    lowerSelectPseudo(function);
    eliminatePhiNodes(function);
    runPreRAOptimizations(function);
    allocateRegisters(function);
    FrameLowering frameLowering;
    frameLowering.assignFrameLayout(function);
    frameLowering.expandFrameIndices(function);
    runPostRAOptimizations(function);
  }
  AsmPrinter(out).emitModule(machine);
}

} // namespace riscv
