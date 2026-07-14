#include "riscv/opt/MachineOptPasses.hpp"

#include <optional>
#include <string>
#include <vector>

namespace riscv {
namespace {

bool isConditionalBranch(Opcode OpcodeValue) {
  return OpcodeValue == Opcode::BEQZ || OpcodeValue == Opcode::BNEZ ||
         OpcodeValue == Opcode::BEQ || OpcodeValue == Opcode::BNE ||
         OpcodeValue == Opcode::BLT || OpcodeValue == Opcode::BGE;
}

std::optional<Opcode> invertedBranch(Opcode OpcodeValue) {
  switch (OpcodeValue) {
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
    return std::nullopt;
  }
}

std::optional<std::string> branchTarget(const MachineInstr &Branch) {
  for (auto It = Branch.uses.rbegin(); It != Branch.uses.rend(); ++It) {
    if (It->kind == OperandKind::Block)
      return It->blockLabel;
  }
  return std::nullopt;
}

bool endsWithJumpTo(const MachineBasicBlock &Block,
                    const std::string &Target) {
  if (Block.instrs.empty())
    return false;
  const MachineInstr &Terminator = Block.instrs.back();
  const auto TargetLabel = branchTarget(Terminator);
  return Terminator.opcode == Opcode::J && TargetLabel &&
         *TargetLabel == Target;
}

MachineInstr makeJump(const std::string &Target) {
  MachineInstr Jump;
  Jump.opcode = Opcode::J;
  Jump.uses.push_back(MachineOperand::block(Target));
  Jump.terminator = true;
  return Jump;
}

struct Rewrite {
  std::size_t bodyIndex = 0;
  MachineInstr continueBranch;
  std::string exitLabel;
};

} // namespace

bool runPostRALoopBackedgeBypass(MachineFunction &MF) {
  std::vector<Rewrite> Rewrites;
  for (std::size_t HeaderIndex = 0; HeaderIndex + 1 < MF.blocks.size();
       ++HeaderIndex) {
    const MachineBasicBlock &Header = MF.blocks[HeaderIndex];
    if (Header.instrs.size() != 1 ||
        !isConditionalBranch(Header.instrs.front().opcode)) {
      continue;
    }

    const MachineInstr &Condition = Header.instrs.front();
    const auto TrueTarget = branchTarget(Condition);
    if (!TrueTarget)
      continue;
    const std::string FalseTarget = MF.blocks[HeaderIndex + 1].label;

    std::size_t TrueIndex = MF.blocks.size();
    for (std::size_t I = 0; I < MF.blocks.size(); ++I) {
      if (MF.blocks[I].label == *TrueTarget) {
        TrueIndex = I;
        break;
      }
    }
    if (TrueIndex == MF.blocks.size())
      continue;

    const std::size_t FalseIndex = HeaderIndex + 1;
    const bool TrueIsBody =
        TrueIndex != HeaderIndex &&
        endsWithJumpTo(MF.blocks[TrueIndex], Header.label);
    const bool FalseIsBody =
        FalseIndex != HeaderIndex &&
        endsWithJumpTo(MF.blocks[FalseIndex], Header.label);
    if (TrueIsBody == FalseIsBody)
      continue;

    const std::size_t BodyIndex = TrueIsBody ? TrueIndex : FalseIndex;
    const std::string ExitLabel = TrueIsBody ? FalseTarget : *TrueTarget;
    const MachineBasicBlock &Body = MF.blocks[BodyIndex];
    if (Body.succs.size() != 1 || Body.succs.front() !=
                                      static_cast<int>(HeaderIndex)) {
      continue;
    }

    MachineInstr Continue = Condition;
    if (!TrueIsBody) {
      const auto Inverse = invertedBranch(Continue.opcode);
      if (!Inverse)
        continue;
      Continue.opcode = *Inverse;
    }
    bool ReplacedTarget = false;
    for (MachineOperand &Use : Continue.uses) {
      if (Use.kind == OperandKind::Block) {
        Use.blockLabel = Body.label;
        ReplacedTarget = true;
      }
    }
    if (!ReplacedTarget)
      continue;
    Rewrites.push_back(Rewrite{BodyIndex, std::move(Continue), ExitLabel});
  }

  for (Rewrite &Item : Rewrites) {
    MachineBasicBlock &Body = MF.blocks[Item.bodyIndex];
    Body.instrs.pop_back();
    Body.instrs.push_back(std::move(Item.continueBranch));
    Body.instrs.push_back(makeJump(Item.exitLabel));
  }
  return !Rewrites.empty();
}

} // namespace riscv
