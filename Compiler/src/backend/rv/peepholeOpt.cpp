#include "../../../include/riscv/Passes.hpp"

#include <algorithm>

namespace riscv {
namespace {

bool sameRegister(const MachineReg &lhs, const MachineReg &rhs) {
  return lhs.physical == rhs.physical && lhs.id == rhs.id &&
         lhs.rc == rhs.rc && lhs.phys == rhs.phys;
}

bool hasSingleRegDefUse(const MachineInstr &instr) {
  return instr.defs.size() == 1 && instr.uses.size() == 1 &&
         instr.defs.front().kind == OperandKind::Reg &&
         instr.uses.front().kind == OperandKind::Reg;
}

bool isRedundantCopy(const MachineInstr &instr) {
  if (instr.sideEffects || instr.terminator) {
    return false;
  }
  if (instr.opcode != Opcode::COPY && instr.opcode != Opcode::F_COPY &&
      instr.opcode != Opcode::PARALLEL_COPY) {
    return false;
  }
  return hasSingleRegDefUse(instr) &&
         sameRegister(instr.defs.front().regValue, instr.uses.front().regValue);
}

bool isRedundantAddZero(const MachineInstr &instr) {
  if (instr.sideEffects || instr.terminator || instr.opcode != Opcode::ADDI) {
    return false;
  }
  if (instr.defs.size() != 1 || instr.uses.size() != 2 ||
      instr.defs.front().kind != OperandKind::Reg ||
      instr.uses.front().kind != OperandKind::Reg ||
      instr.uses[1].kind != OperandKind::Imm || instr.uses[1].immValue != 0) {
    return false;
  }
  return sameRegister(instr.defs.front().regValue, instr.uses.front().regValue);
}

bool isRemovablePostRAInstr(const MachineInstr &instr) {
  if (!instr.sideEffects && !instr.terminator && instr.opcode == Opcode::NOP) {
    return true;
  }
  return isRedundantCopy(instr) || isRedundantAddZero(instr);
}

} // namespace

bool runPostRAPeephole(MachineFunction &mf) {
  bool changed = false;
  for (MachineBasicBlock &block : mf.blocks) {
    const auto oldSize = block.instrs.size();
    block.instrs.erase(std::remove_if(block.instrs.begin(), block.instrs.end(),
                                      isRemovablePostRAInstr),
                       block.instrs.end());
    changed = changed || block.instrs.size() != oldSize;
  }
  return changed;
}

} // namespace riscv
