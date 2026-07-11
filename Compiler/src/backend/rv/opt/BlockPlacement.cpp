#include "riscv/opt/MachineOptPasses.hpp"

#include "riscv/Passes.hpp"

#include <string>

namespace riscv {
namespace {

bool isConditionalBranch(Opcode opcode) {
  switch (opcode) {
  case Opcode::BEQZ:
  case Opcode::BNEZ:
  case Opcode::BEQ:
  case Opcode::BNE:
  case Opcode::BLT:
  case Opcode::BGE:
    return true;
  default:
    return false;
  }
}

std::string branchTarget(const MachineInstr &instr) {
  for (const MachineOperand &use : instr.uses) {
    if (use.kind == OperandKind::Block) {
      return use.blockLabel;
    }
  }
  return {};
}

void replaceBranchTarget(MachineInstr &instr, const std::string &target) {
  for (MachineOperand &use : instr.uses) {
    if (use.kind == OperandKind::Block) {
      use.blockLabel = target;
      return;
    }
  }
}

Opcode invertedBranch(Opcode opcode) {
  switch (opcode) {
  case Opcode::BEQZ:
    return Opcode::BNEZ;
  case Opcode::BNEZ:
    return Opcode::BEQZ;
  case Opcode::BEQ:
    return Opcode::BNE;
  case Opcode::BNE:
    return Opcode::BEQ;
  case Opcode::BLT:
    return Opcode::BGE;
  case Opcode::BGE:
    return Opcode::BLT;
  default:
    return opcode;
  }
}

bool simplifyAdjacentConditionalJump(MachineFunction &mf) {
  for (std::size_t index = 0; index + 1 < mf.blocks.size(); ++index) {
    MachineBasicBlock &block = mf.blocks[index];
    const MachineBasicBlock &next = mf.blocks[index + 1];
    if (block.instrs.size() < 2) {
      continue;
    }

    MachineInstr &conditional = block.instrs[block.instrs.size() - 2];
    MachineInstr &jump = block.instrs.back();
    if (!isConditionalBranch(conditional.opcode) ||
        jump.opcode != Opcode::J) {
      continue;
    }
    const std::string conditionalTarget = branchTarget(conditional);
    const std::string jumpTarget = branchTarget(jump);
    if (conditionalTarget.empty() || jumpTarget.empty()) {
      continue;
    }

    if (jumpTarget == next.label) {
      // 条件不满足时本来就落到下一块，删除显式 J。
      block.instrs.pop_back();
      return true;
    }

    if (conditionalTarget == next.label) {
      // 条件满足时本来就落到下一块，反转条件让另一目标成为显式分支。
      conditional.opcode = invertedBranch(conditional.opcode);
      replaceBranchTarget(conditional, jumpTarget);
      block.instrs.pop_back();
      return true;
    }
  }
  return false;
}

} // namespace

bool runBlockPlacement(MachineFunction &mf) {
  bool changed = false;
  while (simplifyAdjacentConditionalJump(mf)) {
    changed = true;
    rebuildCFG(mf);
  }
  return changed;
}

} // namespace riscv
