#include "aarch64/opt/MachineOptPasses.hpp"

#include <algorithm>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace aarch64 {
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
  std::optional<MachineInstr> replayLoad;
  MachineInstr continueBranch;
  std::string exitLabel;
};

bool sameRegister(const MachineReg &Left, const MachineReg &Right) {
  return Left.physical == Right.physical && Left.id == Right.id &&
         Left.rc == Right.rc && Left.phys == Right.phys;
}

bool isPhysicalGPR(const MachineOperand &Operand) {
  if (Operand.kind != OperandKind::Reg || !Operand.regValue.physical)
    return false;
  return Operand.regValue.rc == RegClass::GPR32 ||
         Operand.regValue.rc == RegClass::GPR64;
}

bool isReplayableIntegerLoad(const MachineInstr &Load) {
  if (Load.opcode != Opcode::LBU && Load.opcode != Opcode::LW &&
      Load.opcode != Opcode::LD)
    return false;
  if (Load.sideEffects || Load.terminator || Load.defs.size() != 1 ||
      Load.uses.size() != 2 || !Load.implicitDefs.empty() ||
      !Load.implicitUses.empty() || !isPhysicalGPR(Load.defs.front()) ||
      !isPhysicalGPR(Load.uses.front()) ||
      Load.uses[1].kind != OperandKind::Imm)
    return false;
  return Load.defs.front().regValue.phys != PhysReg::X0;
}

bool zeroBranchTestsDefinition(const MachineInstr &Branch,
                               const MachineInstr &Load) {
  if (Branch.opcode != Opcode::BEQZ && Branch.opcode != Opcode::BNEZ)
    return false;
  if (!Branch.terminator || Branch.sideEffects || !Branch.defs.empty() ||
      Branch.uses.size() != 2 || !Branch.implicitDefs.empty() ||
      !Branch.implicitUses.empty() || !isPhysicalGPR(Branch.uses.front()) ||
      Branch.uses[1].kind != OperandKind::Block)
    return false;
  return sameRegister(Branch.uses.front().regValue,
                      Load.defs.front().regValue);
}

using DominatorSets = std::vector<std::set<int>>;

DominatorSets computeDominators(const MachineFunction &MF) {
  const std::size_t Count = MF.blocks.size();
  DominatorSets Dominators(Count);
  if (Count == 0)
    return Dominators;

  std::vector<bool> Reachable(Count, false);
  std::vector<int> Work{0};
  Reachable[0] = true;
  while (!Work.empty()) {
    const int Block = Work.back();
    Work.pop_back();
    for (int Successor : MF.blocks[static_cast<std::size_t>(Block)].succs) {
      if (Successor >= 0 && Successor < static_cast<int>(Count) &&
          !Reachable[static_cast<std::size_t>(Successor)]) {
        Reachable[static_cast<std::size_t>(Successor)] = true;
        Work.push_back(Successor);
      }
    }
  }

  std::set<int> AllReachable;
  for (std::size_t Block = 0; Block < Count; ++Block)
    if (Reachable[Block])
      AllReachable.insert(static_cast<int>(Block));
  Dominators[0].insert(0);
  for (std::size_t Block = 1; Block < Count; ++Block)
    if (Reachable[Block])
      Dominators[Block] = AllReachable;

  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (std::size_t Block = 1; Block < Count; ++Block) {
      if (!Reachable[Block])
        continue;
      std::set<int> Next;
      bool First = true;
      for (int Predecessor : MF.blocks[Block].preds) {
        if (Predecessor < 0 || Predecessor >= static_cast<int>(Count) ||
            !Reachable[static_cast<std::size_t>(Predecessor)])
          continue;
        if (First) {
          Next = Dominators[static_cast<std::size_t>(Predecessor)];
          First = false;
        } else {
          std::set<int> Intersection;
          std::set_intersection(
              Next.begin(), Next.end(),
              Dominators[static_cast<std::size_t>(Predecessor)].begin(),
              Dominators[static_cast<std::size_t>(Predecessor)].end(),
              std::inserter(Intersection, Intersection.begin()));
          Next = std::move(Intersection);
        }
      }
      Next.insert(static_cast<int>(Block));
      if (Next != Dominators[Block]) {
        Dominators[Block] = std::move(Next);
        Changed = true;
      }
    }
  }
  return Dominators;
}

std::set<int> naturalLoopMembers(const MachineFunction &MF, int Header,
                                 int Latch) {
  std::set<int> Members{Header, Latch};
  std::vector<int> Work;
  if (Header != Latch)
    Work.push_back(Latch);
  while (!Work.empty()) {
    const int Block = Work.back();
    Work.pop_back();
    for (int Predecessor :
         MF.blocks[static_cast<std::size_t>(Block)].preds) {
      if (Predecessor != Header && Members.insert(Predecessor).second)
        Work.push_back(Predecessor);
    }
  }
  return Members;
}

} // namespace

bool runPostRALoopBackedgeBypass(MachineFunction &MF) {
  std::vector<Rewrite> Rewrites;
  const DominatorSets Dominators = computeDominators(MF);
  for (std::size_t HeaderIndex = 0; HeaderIndex + 1 < MF.blocks.size();
       ++HeaderIndex) {
    const MachineBasicBlock &Header = MF.blocks[HeaderIndex];
    const MachineInstr *ReplayLoad = nullptr;
    const MachineInstr *Condition = nullptr;
    if (Header.instrs.size() == 1 &&
        isConditionalBranch(Header.instrs.front().opcode)) {
      Condition = &Header.instrs.front();
    } else if (Header.instrs.size() == 2 &&
               isReplayableIntegerLoad(Header.instrs.front()) &&
               zeroBranchTestsDefinition(Header.instrs.back(),
                                         Header.instrs.front())) {
      ReplayLoad = &Header.instrs.front();
      Condition = &Header.instrs.back();
    } else {
      continue;
    }

    const auto TrueTarget = branchTarget(*Condition);
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
    for (std::size_t LatchIndex = 0; LatchIndex < MF.blocks.size();
         ++LatchIndex) {
      if (LatchIndex == HeaderIndex ||
          !endsWithJumpTo(MF.blocks[LatchIndex], Header.label) ||
          MF.blocks[LatchIndex].succs.size() != 1 ||
          MF.blocks[LatchIndex].succs.front() !=
              static_cast<int>(HeaderIndex) ||
          LatchIndex >= Dominators.size() ||
          Dominators[LatchIndex].count(static_cast<int>(HeaderIndex)) == 0) {
        continue;
      }

      const std::set<int> Members = naturalLoopMembers(
          MF, static_cast<int>(HeaderIndex), static_cast<int>(LatchIndex));
      const bool TrueIsBody = Members.count(static_cast<int>(TrueIndex)) != 0;
      const bool FalseIsBody =
          Members.count(static_cast<int>(FalseIndex)) != 0;
      if (TrueIsBody == FalseIsBody)
        continue;

      const std::size_t BodyIndex = TrueIsBody ? TrueIndex : FalseIndex;
      const std::string ExitLabel = TrueIsBody ? FalseTarget : *TrueTarget;
      MachineInstr Continue = *Condition;
      if (!TrueIsBody) {
        const auto Inverse = invertedBranch(Continue.opcode);
        if (!Inverse)
          continue;
        Continue.opcode = *Inverse;
      }
      bool ReplacedTarget = false;
      for (MachineOperand &Use : Continue.uses) {
        if (Use.kind == OperandKind::Block) {
          Use.blockLabel = MF.blocks[BodyIndex].label;
          ReplacedTarget = true;
        }
      }
      if (!ReplacedTarget)
        continue;
      Rewrites.push_back(Rewrite{
          LatchIndex,
          ReplayLoad ? std::optional<MachineInstr>(*ReplayLoad) : std::nullopt,
          std::move(Continue), ExitLabel});
    }
  }

  for (Rewrite &Item : Rewrites) {
    MachineBasicBlock &Body = MF.blocks[Item.bodyIndex];
    Body.instrs.pop_back();
    if (Item.replayLoad)
      Body.instrs.push_back(std::move(*Item.replayLoad));
    Body.instrs.push_back(std::move(Item.continueBranch));
    Body.instrs.push_back(makeJump(Item.exitLabel));
  }
  return !Rewrites.empty();
}

} // namespace aarch64
