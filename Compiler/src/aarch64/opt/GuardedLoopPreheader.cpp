#include "aarch64/opt/MachineOptPasses.hpp"

#include "aarch64/Passes.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace aarch64 {
namespace {

constexpr unsigned kMinGuardedCost = 8;

struct Candidate {
  std::size_t preheader = 0;
  std::size_t header = 0;
  unsigned cost = 0;
  std::vector<bool> guardSlice;
  MachineInstr guard;
};

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

std::optional<Opcode> invertedBranch(Opcode opcode) {
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
    return std::nullopt;
  }
}

bool isVirtualRegister(const MachineOperand &operand) {
  return operand.kind == OperandKind::Reg && !operand.regValue.physical;
}

bool hasSingleBlockTarget(const MachineInstr &instr, std::string &target) {
  target.clear();
  for (const MachineOperand &use : instr.uses) {
    if (use.kind != OperandKind::Block) {
      continue;
    }
    if (!target.empty()) {
      return false;
    }
    target = use.blockLabel;
  }
  return !target.empty();
}

bool replaceSingleBlockTarget(MachineInstr &instr, const std::string &target) {
  MachineOperand *blockUse = nullptr;
  for (MachineOperand &use : instr.uses) {
    if (use.kind != OperandKind::Block) {
      continue;
    }
    if (blockUse != nullptr) {
      return false;
    }
    blockUse = &use;
  }
  if (blockUse == nullptr) {
    return false;
  }
  blockUse->blockLabel = target;
  return true;
}

bool wellFormedConditionalBranch(const MachineInstr &instr) {
  if (!isConditionalBranch(instr.opcode) || !instr.terminator ||
      instr.sideEffects || !instr.defs.empty() || !instr.implicitDefs.empty() ||
      !instr.implicitUses.empty()) {
    return false;
  }

  const std::size_t expectedRegisters =
      instr.opcode == Opcode::BEQZ || instr.opcode == Opcode::BNEZ ? 1 : 2;
  std::size_t registers = 0;
  std::size_t blocks = 0;
  for (const MachineOperand &use : instr.uses) {
    if (use.kind == OperandKind::Reg) {
      ++registers;
    } else if (use.kind == OperandKind::Block) {
      ++blocks;
    } else {
      return false;
    }
  }
  return registers == expectedRegisters && blocks == 1;
}

bool wellFormedJump(const MachineInstr &instr, std::string &target) {
  return instr.opcode == Opcode::J && instr.terminator && !instr.sideEffects &&
         instr.defs.empty() && instr.implicitDefs.empty() &&
         instr.implicitUses.empty() && instr.uses.size() == 1 &&
         hasSingleBlockTarget(instr, target);
}

bool isSpeculatablePreheaderInstruction(const MachineInstr &instr) {
  if (instr.sideEffects || instr.terminator || instr.defs.empty() ||
      !instr.implicitDefs.empty() || !instr.implicitUses.empty()) {
    return false;
  }
  for (const MachineOperand &def : instr.defs) {
    if (!isVirtualRegister(def)) {
      return false;
    }
  }
  for (const MachineOperand &use : instr.uses) {
    if (use.kind != OperandKind::Reg && use.kind != OperandKind::Imm) {
      return false;
    }
  }

  switch (instr.opcode) {
  case Opcode::COPY:
  case Opcode::F_COPY:
  case Opcode::FMV_W_X:
  case Opcode::FMV_X_W:
  case Opcode::LI:
  case Opcode::ADD:
  case Opcode::ADDW:
  case Opcode::ADDI:
  case Opcode::ADDIW:
  case Opcode::SUB:
  case Opcode::SUBW:
  case Opcode::MUL:
  case Opcode::MULW:
  case Opcode::AND:
  case Opcode::OR:
  case Opcode::XOR:
  case Opcode::ANDI:
  case Opcode::ORI:
  case Opcode::XORI:
  case Opcode::SLL:
  case Opcode::SLLW:
  case Opcode::SRAW:
  case Opcode::SRLW:
  case Opcode::SLLI:
  case Opcode::SLLIW:
  case Opcode::SRAI:
  case Opcode::SRAIW:
  case Opcode::SRLIW:
  case Opcode::SLT:
  case Opcode::SLTU:
  case Opcode::SEQZ:
  case Opcode::SNEZ:
  case Opcode::FEQ_S:
  case Opcode::FLT_S:
  case Opcode::FLE_S:
  case Opcode::FADD_S:
  case Opcode::FSUB_S:
  case Opcode::FMUL_S:
  case Opcode::FDIV_S:
  case Opcode::FCVT_W_S:
  case Opcode::FCVT_S_W:
    return true;
  default:
    return false;
  }
}

unsigned instructionCost(const MachineInstr &instr) {
  switch (instr.opcode) {
  case Opcode::FDIV_S:
    return 16;
  case Opcode::MUL:
  case Opcode::MULW:
  case Opcode::FMUL_S:
    return 3;
  case Opcode::LI:
    return 2;
  default:
    return 1;
  }
}

std::map<std::string, std::size_t> blockIndices(const MachineFunction &mf) {
  std::map<std::string, std::size_t> result;
  for (std::size_t index = 0; index < mf.blocks.size(); ++index) {
    result.emplace(mf.blocks[index].label, index);
  }
  return result;
}

std::map<uint32_t, std::size_t>
virtualDefinitionCounts(const MachineFunction &mf) {
  std::map<uint32_t, std::size_t> result;
  for (const MachineBasicBlock &block : mf.blocks) {
    for (const MachineInstr &instr : block.instrs) {
      for (const MachineOperand &def : instr.defs) {
        if (isVirtualRegister(def)) {
          ++result[def.regValue.id];
        }
      }
    }
  }
  return result;
}

std::vector<bool> retainedDependencySlice(
    const MachineBasicBlock &preheader, std::size_t computeEnd,
    const MachineInstr &condition,
    const std::map<uint32_t, std::size_t> &definitionCounts) {
  std::set<uint32_t> needed;
  for (const MachineOperand &use : condition.uses) {
    if (isVirtualRegister(use)) {
      needed.insert(use.regValue.id);
    }
  }

  std::vector<bool> slice(computeEnd, false);
  // Phi elimination can leave non-SSA copies and use-before-def order in P.
  // Retain every multi-defined seed, then close over all P-local definitions
  // of retained uses without relying on instruction order.
  for (std::size_t index = 0; index < computeEnd; ++index) {
    const MachineInstr &instr = preheader.instrs[index];
    const bool hasNonUniqueDefinition = std::any_of(
        instr.defs.begin(), instr.defs.end(), [&](const MachineOperand &def) {
          if (!isVirtualRegister(def)) {
            return false;
          }
          auto count = definitionCounts.find(def.regValue.id);
          return count == definitionCounts.end() || count->second != 1;
        });
    slice[index] = hasNonUniqueDefinition;
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t index = 0; index < computeEnd; ++index) {
      if (!slice[index]) {
        continue;
      }
      for (const MachineOperand &use : preheader.instrs[index].uses) {
        if (isVirtualRegister(use) && needed.insert(use.regValue.id).second) {
          changed = true;
        }
      }
    }

    for (std::size_t index = 0; index < computeEnd; ++index) {
      if (slice[index]) {
        continue;
      }
      const MachineInstr &instr = preheader.instrs[index];
      const bool definesNeeded = std::any_of(
          instr.defs.begin(), instr.defs.end(), [&](const MachineOperand &def) {
            return isVirtualRegister(def) && needed.count(def.regValue.id) != 0;
          });
      if (definesNeeded) {
        slice[index] = true;
        changed = true;
      }
    }
  }
  return slice;
}

bool partitionCanReorder(const MachineBasicBlock &preheader,
                         const std::vector<bool> &guardSlice) {
  std::map<uint32_t, std::size_t> lastGuardDefinition;
  for (std::size_t index = 0; index < guardSlice.size(); ++index) {
    if (!guardSlice[index]) {
      continue;
    }
    for (const MachineOperand &def : preheader.instrs[index].defs) {
      if (isVirtualRegister(def)) {
        lastGuardDefinition[def.regValue.id] = index;
      }
    }
  }

  for (std::size_t index = 0; index < guardSlice.size(); ++index) {
    if (guardSlice[index]) {
      continue;
    }
    for (const MachineOperand &use : preheader.instrs[index].uses) {
      if (!isVirtualRegister(use)) {
        continue;
      }
      auto definition = lastGuardDefinition.find(use.regValue.id);
      if (definition != lastGuardDefinition.end() &&
          definition->second > index) {
        return false;
      }
    }
  }
  return true;
}

std::set<int> naturalLoopMembers(int header, int latch,
                                 const MachineCFGAnalysis::Result &cfg) {
  std::set<int> members{header, latch};
  std::vector<int> work;
  if (latch != header) {
    work.push_back(latch);
  }
  while (!work.empty()) {
    const int block = work.back();
    work.pop_back();
    for (int pred : cfg.preds[static_cast<std::size_t>(block)]) {
      if (pred != header && members.insert(pred).second) {
        work.push_back(pred);
      }
    }
  }
  return members;
}

bool blockEndsWithJumpTo(const MachineBasicBlock &block,
                         const std::string &target) {
  if (block.instrs.empty()) {
    return false;
  }
  std::string actual;
  if (!wellFormedJump(block.instrs.back(), actual) || actual != target) {
    return false;
  }
  return std::none_of(
      block.instrs.begin(), block.instrs.end() - 1,
      [](const MachineInstr &instr) { return instr.terminator; });
}

bool exitHasPhi(const MachineBasicBlock &block) {
  return std::any_of(
      block.instrs.begin(), block.instrs.end(),
      [](const MachineInstr &instr) { return instr.opcode == Opcode::PHI; });
}

std::optional<Candidate>
buildCandidate(MachineFunction &mf, std::size_t headerIndex, int latchIndex,
               const MachineCFGAnalysis::Result &cfg,
               const MachineLivenessAnalysis::Result &liveness,
               const MachineLoopAnalysis::Result &loops,
               const std::map<std::string, std::size_t> &indices,
               const std::map<uint32_t, std::size_t> &definitionCounts) {
  const std::set<int> members =
      naturalLoopMembers(static_cast<int>(headerIndex), latchIndex, cfg);
  if (members.size() != 2 || members.count(latchIndex) == 0) {
    return std::nullopt;
  }

  const MachineBasicBlock &header = mf.blocks[headerIndex];
  if (header.instrs.size() != 2 ||
      !wellFormedConditionalBranch(header.instrs[0])) {
    return std::nullopt;
  }

  std::string conditionalLabel;
  std::string jumpLabel;
  if (!hasSingleBlockTarget(header.instrs[0], conditionalLabel) ||
      !wellFormedJump(header.instrs[1], jumpLabel)) {
    return std::nullopt;
  }
  auto conditionalFound = indices.find(conditionalLabel);
  auto jumpFound = indices.find(jumpLabel);
  if (conditionalFound == indices.end() || jumpFound == indices.end()) {
    return std::nullopt;
  }
  const std::size_t conditionalTarget = conditionalFound->second;
  const std::size_t jumpTarget = jumpFound->second;

  bool conditionalEnters = false;
  std::size_t bodyIndex = 0;
  std::size_t exitIndex = 0;
  if (conditionalTarget == static_cast<std::size_t>(latchIndex) &&
      members.count(static_cast<int>(jumpTarget)) == 0) {
    conditionalEnters = true;
    bodyIndex = conditionalTarget;
    exitIndex = jumpTarget;
  } else if (jumpTarget == static_cast<std::size_t>(latchIndex) &&
             members.count(static_cast<int>(conditionalTarget)) == 0) {
    bodyIndex = jumpTarget;
    exitIndex = conditionalTarget;
  } else {
    return std::nullopt;
  }

  if (cfg.succs[bodyIndex].size() != 1 ||
      cfg.succs[bodyIndex].front() != static_cast<int>(headerIndex) ||
      !blockEndsWithJumpTo(mf.blocks[bodyIndex], header.label)) {
    return std::nullopt;
  }

  std::vector<int> outsidePreds;
  for (int pred : cfg.preds[headerIndex]) {
    if (members.count(pred) == 0) {
      outsidePreds.push_back(pred);
    }
  }
  if (outsidePreds.size() != 1 || outsidePreds.front() < 0) {
    return std::nullopt;
  }
  const std::size_t preheaderIndex =
      static_cast<std::size_t>(outsidePreds.front());
  if (preheaderIndex + 1 != headerIndex ||
      preheaderIndex >= loops.depth.size() ||
      headerIndex >= loops.depth.size() || loops.depth[preheaderIndex] <= 0 ||
      loops.depth[headerIndex] <= loops.depth[preheaderIndex] ||
      cfg.succs[preheaderIndex].size() != 1 ||
      cfg.succs[preheaderIndex].front() != static_cast<int>(headerIndex) ||
      exitIndex >= mf.blocks.size() || exitHasPhi(mf.blocks[exitIndex])) {
    return std::nullopt;
  }

  const MachineBasicBlock &preheader = mf.blocks[preheaderIndex];
  std::size_t computeEnd = preheader.instrs.size();
  if (computeEnd != 0 && preheader.instrs.back().opcode == Opcode::J) {
    std::string target;
    if (!wellFormedJump(preheader.instrs.back(), target) ||
        target != header.label) {
      return std::nullopt;
    }
    --computeEnd;
  }
  if (computeEnd == 0) {
    return std::nullopt;
  }

  for (std::size_t index = 0; index < computeEnd; ++index) {
    if (!isSpeculatablePreheaderInstruction(preheader.instrs[index])) {
      return std::nullopt;
    }
  }

  const std::vector<bool> guardSlice = retainedDependencySlice(
      preheader, computeEnd, header.instrs[0], definitionCounts);
  if (!partitionCanReorder(preheader, guardSlice)) {
    return std::nullopt;
  }

  std::set<uint32_t> computeDefs;
  unsigned cost = 0;
  for (std::size_t index = 0; index < computeEnd; ++index) {
    if (guardSlice[index]) {
      continue;
    }
    const MachineInstr &instr = preheader.instrs[index];
    cost += instructionCost(instr);
    for (const MachineOperand &def : instr.defs) {
      computeDefs.insert(def.regValue.id);
    }
  }
  if (cost < kMinGuardedCost) {
    return std::nullopt;
  }

  for (uint32_t def : computeDefs) {
    auto count = definitionCounts.find(def);
    if (count == definitionCounts.end() || count->second != 1 ||
        liveness.liveIn[exitIndex].count(def) != 0) {
      return std::nullopt;
    }
  }
  for (const MachineOperand &use : header.instrs[0].uses) {
    if (isVirtualRegister(use) && computeDefs.count(use.regValue.id) != 0) {
      return std::nullopt;
    }
  }

  for (std::size_t blockIndex = 0; blockIndex < mf.blocks.size();
       ++blockIndex) {
    if (blockIndex == preheaderIndex ||
        members.count(static_cast<int>(blockIndex)) != 0) {
      continue;
    }
    for (const MachineInstr &instr : mf.blocks[blockIndex].instrs) {
      for (const MachineOperand &use : instr.uses) {
        if (isVirtualRegister(use) && computeDefs.count(use.regValue.id) != 0) {
          return std::nullopt;
        }
      }
    }
  }

  MachineInstr guard = header.instrs[0];
  if (conditionalEnters) {
    auto inverse = invertedBranch(guard.opcode);
    if (!inverse) {
      return std::nullopt;
    }
    guard.opcode = *inverse;
  }
  if (!replaceSingleBlockTarget(guard, mf.blocks[exitIndex].label)) {
    return std::nullopt;
  }
  guard.comment = "skip zero-trip loop preheader";

  Candidate candidate;
  candidate.preheader = preheaderIndex;
  candidate.header = headerIndex;
  candidate.cost = cost;
  candidate.guardSlice = guardSlice;
  candidate.guard = std::move(guard);
  return candidate;
}

std::string uniqueComputeLabel(const MachineFunction &mf,
                               const std::string &preheaderLabel) {
  const std::string base = preheaderLabel + ".guarded_compute";
  std::string candidate = base;
  unsigned suffix = 0;
  auto used = [&](const std::string &label) {
    return std::any_of(
        mf.blocks.begin(), mf.blocks.end(),
        [&](const MachineBasicBlock &block) { return block.label == label; });
  };
  while (used(candidate)) {
    candidate = base + "." + std::to_string(++suffix);
  }
  return candidate;
}

void applyCandidate(MachineFunction &mf, Candidate candidate) {
  MachineBasicBlock &preheader = mf.blocks[candidate.preheader];
  MachineBasicBlock compute;
  compute.label = uniqueComputeLabel(mf, preheader.label);
  compute.irBlock = preheader.irBlock;
  std::vector<MachineInstr> original = std::move(preheader.instrs);
  preheader.instrs.clear();
  for (std::size_t index = 0; index < candidate.guardSlice.size(); ++index) {
    if (candidate.guardSlice[index]) {
      preheader.instrs.push_back(std::move(original[index]));
    } else {
      compute.instrs.push_back(std::move(original[index]));
    }
  }
  preheader.instrs.push_back(std::move(candidate.guard));
  using BlockDifference = std::vector<MachineBasicBlock>::difference_type;
  mf.blocks.insert(mf.blocks.begin() +
                       static_cast<BlockDifference>(candidate.header),
                   std::move(compute));
  rebuildCFG(mf);
}

} // namespace

bool runPreRAGuardedLoopPreheader(MachineFunction &mf,
                                  MachineFunctionAnalysisManager &am) {
  if (mf.blocks.size() < 5) {
    return false;
  }

  const auto &cfg = am.getResult<MachineCFGAnalysis>(mf);
  const auto &liveness = am.getResult<MachineLivenessAnalysis>(mf);
  const auto &loops = am.getResult<MachineLoopAnalysis>(mf);
  const auto indices = blockIndices(mf);
  const auto definitionCounts = virtualDefinitionCounts(mf);

  std::optional<Candidate> best;
  for (std::size_t header = 0; header < mf.blocks.size(); ++header) {
    std::vector<int> latches;
    for (const auto &[latch, target] : loops.backedges) {
      if (target == static_cast<int>(header)) {
        latches.push_back(latch);
      }
    }
    if (latches.size() != 1) {
      continue;
    }

    auto candidate = buildCandidate(mf, header, latches.front(), cfg, liveness,
                                    loops, indices, definitionCounts);
    if (!candidate) {
      continue;
    }
    if (!best || candidate->cost > best->cost) {
      best = std::move(candidate);
    }
  }

  if (!best) {
    return false;
  }
  // CFG mutation invalidates the cached analyses used above. Transform only
  // the highest-cost candidate per invocation and let the caller rerun with
  // fresh analyses if it wants a fixed point.
  applyCandidate(mf, std::move(*best));
  return true;
}

} // namespace aarch64
