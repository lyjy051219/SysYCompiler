#include "../../../include/riscv/Passes.hpp"

#include <algorithm>
#include <utility>
#include <vector>

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

bool hasSingleRegDef(const MachineInstr &instr) {
  return instr.defs.size() == 1 && instr.defs.front().kind == OperandKind::Reg;
}

bool isRegUse(const MachineInstr &instr, std::size_t index) {
  return index < instr.uses.size() &&
         instr.uses[index].kind == OperandKind::Reg;
}

bool isZeroReg(const MachineOperand &operand) {
  return operand.kind == OperandKind::Reg && operand.regValue.physical &&
         operand.regValue.phys == PhysReg::X0;
}

bool isSafeToRewrite(const MachineInstr &instr) {
  return !instr.sideEffects && !instr.terminator && instr.implicitDefs.empty() &&
         instr.implicitUses.empty() && hasSingleRegDef(instr);
}

MachineInstr makeCopyFrom(const MachineInstr &instr,
                          MachineOperand sourceOperand) {
  MachineInstr copy;
  copy.opcode = Opcode::COPY;
  copy.defs = instr.defs;
  copy.uses.push_back(std::move(sourceOperand));
  copy.comment = instr.comment;
  return copy;
}

MachineOperand zeroForDef(const MachineInstr &instr) {
  return MachineOperand::reg(
      MachineReg::preg(PhysReg::X0, instr.defs.front().regValue.rc));
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

bool foldAddImmediateZero(const MachineInstr &instr, MachineInstr &folded) {
  if (!isSafeToRewrite(instr) ||
      (instr.opcode != Opcode::ADDI && instr.opcode != Opcode::ADDIW) ||
      instr.uses.size() != 2 || !isRegUse(instr, 0) ||
      instr.uses[1].kind != OperandKind::Imm || instr.uses[1].immValue != 0) {
    return false;
  }
  folded = makeCopyFrom(instr, instr.uses[0]);
  return true;
}

bool foldBinaryZeroIdentity(const MachineInstr &instr, MachineInstr &folded) {
  if (!isSafeToRewrite(instr) || instr.uses.size() != 2 ||
      !isRegUse(instr, 0) || !isRegUse(instr, 1)) {
    return false;
  }

  if (instr.opcode == Opcode::ADD || instr.opcode == Opcode::ADDW ||
      instr.opcode == Opcode::OR || instr.opcode == Opcode::XOR) {
    if (isZeroReg(instr.uses[0])) {
      folded = makeCopyFrom(instr, instr.uses[1]);
      return true;
    }
    if (isZeroReg(instr.uses[1])) {
      folded = makeCopyFrom(instr, instr.uses[0]);
      return true;
    }
  }

  if ((instr.opcode == Opcode::SUB || instr.opcode == Opcode::SUBW ||
       instr.opcode == Opcode::SLLW || instr.opcode == Opcode::SRAW ||
       instr.opcode == Opcode::SRLW) &&
      isZeroReg(instr.uses[1])) {
    folded = makeCopyFrom(instr, instr.uses[0]);
    return true;
  }

  if (instr.opcode == Opcode::AND) {
    if (isZeroReg(instr.uses[0])) {
      folded = makeCopyFrom(instr, zeroForDef(instr));
      return true;
    }
    if (isZeroReg(instr.uses[1])) {
      folded = makeCopyFrom(instr, zeroForDef(instr));
      return true;
    }
  }

  if (instr.opcode == Opcode::MUL || instr.opcode == Opcode::MULW) {
    if (isZeroReg(instr.uses[0]) || isZeroReg(instr.uses[1])) {
      folded = makeCopyFrom(instr, zeroForDef(instr));
      return true;
    }
  }

  if ((instr.opcode == Opcode::SLLW || instr.opcode == Opcode::SRAW ||
       instr.opcode == Opcode::SRLW) &&
      isZeroReg(instr.uses[0])) {
    folded = makeCopyFrom(instr, zeroForDef(instr));
    return true;
  }

  return false;
}

bool foldSameOperand(const MachineInstr &instr, MachineInstr &folded) {
  if (!isSafeToRewrite(instr) || instr.uses.size() != 2 ||
      !isRegUse(instr, 0) || !isRegUse(instr, 1) ||
      !sameRegister(instr.uses[0].regValue, instr.uses[1].regValue)) {
    return false;
  }

  switch (instr.opcode) {
  case Opcode::AND:
  case Opcode::OR:
    folded = makeCopyFrom(instr, instr.uses[0]);
    return true;
  case Opcode::SUB:
  case Opcode::SUBW:
  case Opcode::XOR:
  case Opcode::SLT:
  case Opcode::SLTU:
    folded = makeCopyFrom(instr, zeroForDef(instr));
    return true;
  default:
    return false;
  }
}

bool foldZeroIdentity(const MachineInstr &instr, MachineInstr &folded) {
  return foldAddImmediateZero(instr, folded) ||
         foldBinaryZeroIdentity(instr, folded) ||
         foldSameOperand(instr, folded);
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
    std::vector<MachineInstr> rewritten;
    rewritten.reserve(block.instrs.size());

    for (const MachineInstr &instr : block.instrs) {
      if (isRemovablePostRAInstr(instr)) {
        changed = true;
        continue;
      }

      MachineInstr folded;
      if (foldZeroIdentity(instr, folded)) {
        changed = true;
        if (!isRemovablePostRAInstr(folded)) {
          rewritten.push_back(std::move(folded));
        }
        continue;
      }

      rewritten.push_back(instr);
    }

    block.instrs = std::move(rewritten);
  }
  return changed;
}

} // namespace riscv
