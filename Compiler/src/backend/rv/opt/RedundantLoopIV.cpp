#include "riscv/opt/MachineOptPasses.hpp"

#include "riscv/Target.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace riscv {
namespace {

struct Location {
  std::size_t block = 0;
  std::size_t instr = 0;

  bool operator==(const Location &other) const {
    return block == other.block && instr == other.instr;
  }
};

using Dominators = std::vector<std::set<int>>;
using Locations = std::map<uint32_t, std::vector<Location>>;

bool sameRegister(const MachineReg &lhs, const MachineReg &rhs) {
  return !lhs.physical && !rhs.physical && lhs.id == rhs.id &&
         lhs.rc == rhs.rc;
}

bool isVirtualGpr32(const MachineOperand &operand) {
  return operand.kind == OperandKind::Reg && !operand.regValue.physical &&
         operand.regValue.rc == RegClass::GPR32;
}

bool isX0(const MachineOperand &operand) {
  return operand.kind == OperandKind::Reg && operand.regValue.physical &&
         operand.regValue.phys == PhysReg::X0;
}

bool isGpr32OrX0(const MachineOperand &operand) {
  return isVirtualGpr32(operand) || isX0(operand);
}

bool isPure(const MachineInstr &instr) {
  return !instr.sideEffects && !instr.terminator &&
         instr.implicitDefs.empty() && instr.implicitUses.empty();
}

void collectLocations(const MachineFunction &mf, Locations &defs,
                      Locations &uses) {
  for (std::size_t block = 0; block < mf.blocks.size(); ++block) {
    for (std::size_t instr = 0; instr < mf.blocks[block].instrs.size();
         ++instr) {
      const Location location{block, instr};
      const MachineInstr &instruction = mf.blocks[block].instrs[instr];
      for (const MachineOperand &def : instruction.defs) {
        if (isVirtualGpr32(def)) {
          defs[def.regValue.id].push_back(location);
        }
      }
      for (const MachineOperand &use : instruction.uses) {
        if (isVirtualGpr32(use)) {
          uses[use.regValue.id].push_back(location);
        }
      }
    }
  }
}

Dominators computeDominators(const MachineCFGAnalysis::Result &cfg) {
  const std::size_t count = cfg.succs.size();
  Dominators result(count);
  if (count == 0) {
    return result;
  }

  std::vector<bool> reachable(count, false);
  std::vector<int> work{0};
  reachable[0] = true;
  while (!work.empty()) {
    const int block = work.back();
    work.pop_back();
    for (int successor : cfg.succs[static_cast<std::size_t>(block)]) {
      if (successor < 0 || successor >= static_cast<int>(count)) {
        return {};
      }
      if (!reachable[static_cast<std::size_t>(successor)]) {
        reachable[static_cast<std::size_t>(successor)] = true;
        work.push_back(successor);
      }
    }
  }

  std::set<int> allReachable;
  for (std::size_t block = 0; block < count; ++block) {
    if (reachable[block]) {
      allReachable.insert(static_cast<int>(block));
    }
  }
  result[0].insert(0);
  for (std::size_t block = 1; block < count; ++block) {
    if (reachable[block]) {
      result[block] = allReachable;
    }
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t block = 1; block < count; ++block) {
      if (!reachable[block]) {
        continue;
      }
      std::set<int> next;
      bool first = true;
      for (int predecessor : cfg.preds[block]) {
        if (predecessor < 0 || predecessor >= static_cast<int>(count)) {
          return {};
        }
        if (!reachable[static_cast<std::size_t>(predecessor)]) {
          continue;
        }
        if (first) {
          next = result[static_cast<std::size_t>(predecessor)];
          first = false;
        } else {
          std::set<int> intersection;
          std::set_intersection(
              next.begin(), next.end(),
              result[static_cast<std::size_t>(predecessor)].begin(),
              result[static_cast<std::size_t>(predecessor)].end(),
              std::inserter(intersection, intersection.begin()));
          next = std::move(intersection);
        }
      }
      if (first) {
        next.clear();
      }
      next.insert(static_cast<int>(block));
      if (next != result[block]) {
        result[block] = std::move(next);
        changed = true;
      }
    }
  }
  return result;
}

bool dominates(const Dominators &dominators, std::size_t dominator,
               std::size_t block) {
  return block < dominators.size() &&
         dominators[block].count(static_cast<int>(dominator)) != 0;
}

std::set<int> naturalLoopMembers(int header, int latch,
                                 const MachineCFGAnalysis::Result &cfg,
                                 const Dominators &dominators) {
  std::set<int> members{header, latch};
  std::vector<int> work;
  if (header != latch) {
    work.push_back(latch);
  }
  while (!work.empty()) {
    const int block = work.back();
    work.pop_back();
    if (block < 0 || block >= static_cast<int>(cfg.preds.size())) {
      return {};
    }
    for (int predecessor : cfg.preds[static_cast<std::size_t>(block)]) {
      if (predecessor < 0 ||
          predecessor >= static_cast<int>(cfg.preds.size())) {
        return {};
      }
      if (predecessor == header ||
          !dominates(dominators, static_cast<std::size_t>(header),
                     static_cast<std::size_t>(predecessor))) {
        continue;
      }
      if (members.insert(predecessor).second) {
        work.push_back(predecessor);
      }
    }
  }
  return members;
}

bool hasOneVirtualDef(const MachineInstr &instr, MachineReg reg) {
  return instr.defs.size() == 1 && isVirtualGpr32(instr.defs.front()) &&
         sameRegister(instr.defs.front().regValue, reg);
}

bool pureAdd(const MachineInstr &instr, MachineReg destination,
             MachineReg source, int64_t immediate) {
  return isPure(instr) && instr.opcode == Opcode::ADDIW &&
         hasOneVirtualDef(instr, destination) && instr.uses.size() == 2 &&
         isVirtualGpr32(instr.uses[0]) &&
         sameRegister(instr.uses[0].regValue, source) &&
         instr.uses[1].kind == OperandKind::Imm &&
         instr.uses[1].immValue == immediate;
}

bool purePhiCopy(const MachineInstr &instr, MachineReg destination,
                 MachineReg source) {
  return isPure(instr) && instr.opcode == Opcode::COPY && instr.phiCopy &&
         !instr.copyCoalescingBarrier && hasOneVirtualDef(instr, destination) &&
         instr.uses.size() == 1 && isVirtualGpr32(instr.uses.front()) &&
         sameRegister(instr.uses.front().regValue, source);
}

bool resolvePreheaderConstant(const MachineFunction &mf, int preheader,
                              MachineReg reg, const Locations &defs,
                              std::set<uint32_t> &visiting, int64_t &value) {
  if (reg.physical) {
    if (reg.phys == PhysReg::X0) {
      value = 0;
      return true;
    }
    return false;
  }
  if (reg.rc != RegClass::GPR32 || !visiting.insert(reg.id).second) {
    return false;
  }

  const auto found = defs.find(reg.id);
  if (found == defs.end()) {
    visiting.erase(reg.id);
    return false;
  }
  const Location *definition = nullptr;
  for (const Location &location : found->second) {
    if (location.block != static_cast<std::size_t>(preheader)) {
      continue;
    }
    if (definition != nullptr) {
      visiting.erase(reg.id);
      return false;
    }
    definition = &location;
  }
  if (definition == nullptr || definition->instr >=
                                  mf.blocks[static_cast<std::size_t>(preheader)]
                                      .instrs.size()) {
    visiting.erase(reg.id);
    return false;
  }

  const MachineInstr &instruction =
      mf.blocks[static_cast<std::size_t>(preheader)]
          .instrs[definition->instr];
  bool resolved = false;
  if (isPure(instruction) && instruction.opcode == Opcode::LI &&
      hasOneVirtualDef(instruction, reg) && instruction.uses.size() == 1 &&
      instruction.uses.front().kind == OperandKind::Imm) {
    value = instruction.uses.front().immValue;
    resolved = value >= std::numeric_limits<int32_t>::min() &&
               value <= std::numeric_limits<int32_t>::max();
  } else if (isPure(instruction) && instruction.opcode == Opcode::COPY &&
             hasOneVirtualDef(instruction, reg) && instruction.uses.size() == 1 &&
             (isX0(instruction.uses.front()) ||
              isVirtualGpr32(instruction.uses.front()))) {
    if (isX0(instruction.uses.front())) {
      value = 0;
      resolved = true;
    } else {
      resolved = resolvePreheaderConstant(
          mf, preheader, instruction.uses.front().regValue, defs, visiting,
          value);
    }
  }
  visiting.erase(reg.id);
  return resolved;
}

bool resolvePreheaderConstant(const MachineFunction &mf, int preheader,
                              const MachineOperand &operand,
                              const Locations &defs, int64_t &value) {
  if (operand.kind != OperandKind::Reg) {
    return false;
  }
  std::set<uint32_t> visiting;
  return resolvePreheaderConstant(mf, preheader, operand.regValue, defs,
                                  visiting, value);
}

bool resolvePreheaderConstant(const MachineFunction &mf, int preheader,
                              MachineReg reg, const Locations &defs,
                              int64_t &value) {
  std::set<uint32_t> visiting;
  return resolvePreheaderConstant(mf, preheader, reg, defs, visiting, value);
}

bool hasOnlyPreheaderConstantDefs(
    const MachineFunction &mf, int preheader, const MachineOperand &operand,
    const Locations &defs, std::set<uint32_t> &visiting) {
  if (operand.kind != OperandKind::Reg) {
    return false;
  }
  if (operand.regValue.physical) {
    return operand.regValue.phys == PhysReg::X0;
  }
  const MachineReg reg = operand.regValue;
  if (reg.rc != RegClass::GPR32 || !visiting.insert(reg.id).second) {
    return false;
  }
  const auto found = defs.find(reg.id);
  if (found == defs.end() || found->second.size() != 1 ||
      found->second.front().block != static_cast<std::size_t>(preheader)) {
    visiting.erase(reg.id);
    return false;
  }
  const Location location = found->second.front();
  const MachineInstr &instruction =
      mf.blocks[static_cast<std::size_t>(preheader)].instrs[location.instr];
  bool valid = isPure(instruction) &&
               ((instruction.opcode == Opcode::LI && instruction.uses.size() == 1 &&
                 instruction.uses.front().kind == OperandKind::Imm) ||
                (instruction.opcode == Opcode::COPY && instruction.uses.size() == 1 &&
                 hasOnlyPreheaderConstantDefs(mf, preheader,
                                              instruction.uses.front(), defs,
                                              visiting)));
  visiting.erase(reg.id);
  return valid;
}

bool hasOnlyUse(const Locations &uses, MachineReg reg,
                const std::vector<Location> &expected) {
  const auto found = uses.find(reg.id);
  if (found == uses.end() || found->second.size() != expected.size()) {
    return false;
  }
  for (const Location &location : expected) {
    if (std::find(found->second.begin(), found->second.end(), location) ==
        found->second.end()) {
      return false;
    }
  }
  return true;
}

bool hasOnlyDefinitions(const Locations &defs, MachineReg reg,
                        const std::vector<Location> &expected) {
  const auto found = defs.find(reg.id);
  if (found == defs.end() || found->second.size() != expected.size()) {
    return false;
  }
  for (const Location &location : expected) {
    if (std::find(found->second.begin(), found->second.end(), location) ==
        found->second.end()) {
      return false;
    }
  }
  return true;
}

bool uniqueDefinitionInBlock(const Locations &defs, MachineReg reg,
                             int block, Location &location) {
  const auto found = defs.find(reg.id);
  if (found == defs.end()) {
    return false;
  }
  bool present = false;
  for (const Location &candidate : found->second) {
    if (candidate.block != static_cast<std::size_t>(block)) {
      continue;
    }
    if (present) {
      return false;
    }
    location = candidate;
    present = true;
  }
  return present;
}

bool hasSingleExitingBlock(const std::set<int> &members,
                           const MachineCFGAnalysis::Result &cfg,
                           int expected) {
  int exiting = 0;
  for (int member : members) {
    if (member < 0 || member >= static_cast<int>(cfg.succs.size())) {
      return false;
    }
    bool leaves = false;
    for (int successor : cfg.succs[static_cast<std::size_t>(member)]) {
      if (!members.count(successor)) {
        leaves = true;
      }
    }
    if (leaves) {
      ++exiting;
      if (member != expected) {
        return false;
      }
    }
  }
  return exiting == 1;
}

struct Candidate {
  int header = -1;
  int latch = -1;
  int preheader = -1;
  int condition = -1;
  int exit = -1;
  std::size_t counterAdd = 0;
  std::size_t counterCopy = 0;
  std::size_t compare = 0;
  std::size_t branch = 0;
  MachineReg counter;
  MachineReg counterNext;
  MachineReg index;
  int64_t indexStep = 0;
  int64_t threshold = 0;
};

bool findCandidate(const MachineFunction &mf,
                   const MachineCFGAnalysis::Result &cfg,
                   const Dominators &dominators, const Locations &defs,
                   const Locations &uses, int header, int latch,
                   Candidate &candidate) {
  if (header < 0 || latch < 0 || header >= static_cast<int>(mf.blocks.size()) ||
      latch >= static_cast<int>(mf.blocks.size()) ||
      cfg.succs[static_cast<std::size_t>(latch)].size() != 1 ||
      cfg.succs[static_cast<std::size_t>(latch)].front() != header) {
    return false;
  }
  const std::set<int> members =
      naturalLoopMembers(header, latch, cfg, dominators);
  if (members.empty()) {
    return false;
  }

  int preheader = -1;
  for (int predecessor : cfg.preds[static_cast<std::size_t>(header)]) {
    if (members.count(predecessor) != 0) {
      continue;
    }
    if (preheader != -1) {
      return false;
    }
    preheader = predecessor;
  }
  if (preheader < 0 ||
      cfg.succs[static_cast<std::size_t>(preheader)].size() != 1 ||
      cfg.succs[static_cast<std::size_t>(preheader)].front() != header) {
    return false;
  }

  const MachineBasicBlock &latchBlock =
      mf.blocks[static_cast<std::size_t>(latch)];
  if (cfg.preds[static_cast<std::size_t>(latch)].size() != 1) {
    return false;
  }

  std::vector<std::size_t> copyIndices;
  for (std::size_t index = 0; index < latchBlock.instrs.size(); ++index) {
    const MachineInstr &instruction = latchBlock.instrs[index];
    if (instruction.opcode != Opcode::COPY || !instruction.phiCopy ||
        instruction.copyCoalescingBarrier || !isPure(instruction) ||
        instruction.defs.size() != 1 || instruction.uses.size() != 1 ||
        !isVirtualGpr32(instruction.defs.front()) ||
        !isVirtualGpr32(instruction.uses.front())) {
      continue;
    }
    copyIndices.push_back(index);
  }

  for (std::size_t copyIndex : copyIndices) {
    const MachineInstr &copy = latchBlock.instrs[copyIndex];
    const MachineReg counter = copy.defs.front().regValue;
    const MachineReg counterNext = copy.uses.front().regValue;

    std::vector<std::size_t> addIndices;
    int condition = -1;
    for (int member : members) {
      const MachineBasicBlock &block =
          mf.blocks[static_cast<std::size_t>(member)];
      for (std::size_t instructionIndex = 0;
           instructionIndex < block.instrs.size(); ++instructionIndex) {
        if (pureAdd(block.instrs[instructionIndex], counterNext, counter, 1)) {
          if (member == latch) {
            addIndices.clear();
            condition = -1;
            break;
          }
          addIndices.push_back(instructionIndex);
          condition = member;
        }
      }
      if (condition == -1 && !addIndices.empty()) {
        return false;
      }
    }
    if (addIndices.size() != 1 || condition < 0 || condition == latch) {
      continue;
    }
    const std::size_t addIndex = addIndices.front();
    const MachineBasicBlock &conditionBlock =
        mf.blocks[static_cast<std::size_t>(condition)];

    std::vector<std::pair<std::size_t, std::size_t>> branches;
    for (std::size_t instructionIndex = 0;
         instructionIndex < conditionBlock.instrs.size(); ++instructionIndex) {
      const MachineInstr &instruction = conditionBlock.instrs[instructionIndex];
      if (instruction.opcode == Opcode::BLT && instruction.terminator &&
          !instruction.sideEffects && instruction.implicitDefs.empty() &&
          instruction.implicitUses.empty() &&
          instruction.uses.size() == 3 &&
          isVirtualGpr32(instruction.uses[0]) &&
          sameRegister(instruction.uses[0].regValue, counterNext) &&
          instruction.uses[2].kind == OperandKind::Block &&
          instruction.uses[2].blockLabel == latchBlock.label) {
        branches.emplace_back(instructionIndex, instructionIndex);
      }
      if (instruction.opcode == Opcode::BNEZ && instruction.terminator &&
          !instruction.sideEffects && instruction.implicitDefs.empty() &&
          instruction.implicitUses.empty() &&
          instruction.uses.size() == 2 && isVirtualGpr32(instruction.uses[0]) &&
          instruction.uses[1].kind == OperandKind::Block &&
          instruction.uses[1].blockLabel == latchBlock.label &&
          instructionIndex > 0) {
        const MachineInstr &compare =
            conditionBlock.instrs[instructionIndex - 1];
        if (isPure(compare) && compare.opcode == Opcode::SLT &&
            compare.defs.size() == 1 && compare.uses.size() == 2 &&
            isVirtualGpr32(compare.defs.front()) &&
            isVirtualGpr32(compare.uses[0]) &&
            sameRegister(compare.uses[0].regValue, counterNext) &&
            isGpr32OrX0(compare.uses[1]) &&
            sameRegister(compare.defs.front().regValue,
                         instruction.uses[0].regValue)) {
          branches.emplace_back(instructionIndex - 1, instructionIndex);
        }
      }
    }
    if (branches.size() != 1 || addIndex >= branches.front().first) {
      continue;
    }
    const std::size_t compareIndex = branches.front().first;
    const std::size_t branchIndex = branches.front().second;
    const MachineInstr &compare = conditionBlock.instrs[compareIndex];
    const MachineInstr &branch = conditionBlock.instrs[branchIndex];
    int exit = -1;
    if (branchIndex + 1 == conditionBlock.instrs.size()) {
      const int fallthrough = condition + 1;
      if (fallthrough < 0 || fallthrough >= static_cast<int>(mf.blocks.size()) ||
          std::find(cfg.succs[static_cast<std::size_t>(condition)].begin(),
                    cfg.succs[static_cast<std::size_t>(condition)].end(),
                    fallthrough) ==
              cfg.succs[static_cast<std::size_t>(condition)].end()) {
        continue;
      }
      exit = fallthrough;
    } else if (branchIndex + 2 == conditionBlock.instrs.size() &&
               conditionBlock.instrs[branchIndex + 1].opcode == Opcode::J &&
               conditionBlock.instrs[branchIndex + 1].terminator &&
               !conditionBlock.instrs[branchIndex + 1].sideEffects &&
               conditionBlock.instrs[branchIndex + 1].implicitDefs.empty() &&
               conditionBlock.instrs[branchIndex + 1].implicitUses.empty() &&
               conditionBlock.instrs[branchIndex + 1].uses.size() == 1 &&
               conditionBlock.instrs[branchIndex + 1].uses.front().kind ==
                   OperandKind::Block) {
      const std::string &exitLabel =
          conditionBlock.instrs[branchIndex + 1].uses.front().blockLabel;
      for (std::size_t blockIndex = 0; blockIndex < mf.blocks.size();
           ++blockIndex) {
        if (mf.blocks[blockIndex].label == exitLabel) {
          exit = static_cast<int>(blockIndex);
          break;
        }
      }
    } else {
      continue;
    }
    if (exit < 0 || members.count(exit) != 0 ||
        cfg.preds[static_cast<std::size_t>(exit)].size() != 1 ||
        cfg.preds[static_cast<std::size_t>(exit)].front() != condition ||
        cfg.succs[static_cast<std::size_t>(condition)].size() != 2 ||
        std::find(cfg.succs[static_cast<std::size_t>(condition)].begin(),
                  cfg.succs[static_cast<std::size_t>(condition)].end(), latch) ==
            cfg.succs[static_cast<std::size_t>(condition)].end() ||
        std::find(cfg.succs[static_cast<std::size_t>(condition)].begin(),
                  cfg.succs[static_cast<std::size_t>(condition)].end(), exit) ==
            cfg.succs[static_cast<std::size_t>(condition)].end() ||
        !hasSingleExitingBlock(members, cfg, condition)) {
      continue;
    }

    std::vector<std::size_t> indexAddIndices;
    MachineReg index{};
    int64_t indexStep = 0;
    for (std::size_t instructionIndex = 0;
         instructionIndex < latchBlock.instrs.size(); ++instructionIndex) {
      const MachineInstr &instruction = latchBlock.instrs[instructionIndex];
      if (!isPure(instruction) || instruction.opcode != Opcode::ADDIW ||
          instruction.defs.size() != 1 || instruction.uses.size() != 2 ||
          !isVirtualGpr32(instruction.defs.front()) ||
          !isVirtualGpr32(instruction.uses[0]) ||
          instruction.uses[1].kind != OperandKind::Imm ||
          !sameRegister(instruction.defs.front().regValue,
                        instruction.uses[0].regValue) ||
          instruction.uses[1].immValue <= 1 ||
          instruction.uses[1].immValue > 2047) {
        continue;
      }
      indexAddIndices.push_back(instructionIndex);
      index = instruction.defs.front().regValue;
      indexStep = instruction.uses[1].immValue;
    }
    if (indexAddIndices.size() != 1) {
      continue;
    }
    const Location counterAddLocation{static_cast<std::size_t>(condition),
                                      addIndex};
    const Location counterCopyLocation{static_cast<std::size_t>(latch),
                                       copyIndex};
    const Location indexAddLocation{static_cast<std::size_t>(latch),
                                    indexAddIndices.front()};

    Location counterInitLocation;
    Location indexInitLocation;
    if (!uniqueDefinitionInBlock(defs, counter, preheader,
                                 counterInitLocation) ||
        !uniqueDefinitionInBlock(defs, index, preheader, indexInitLocation) ||
        !hasOnlyDefinitions(defs, counter,
                            {counterInitLocation, counterCopyLocation}) ||
        !hasOnlyDefinitions(defs, counterNext, {counterAddLocation}) ||
        !hasOnlyDefinitions(defs, index,
                            {indexInitLocation, indexAddLocation}) ||
        !hasOnlyUse(uses, counter, {counterAddLocation}) ||
        !hasOnlyUse(uses, counterNext, {counterCopyLocation,
                                        Location{static_cast<std::size_t>(condition),
                                                 compareIndex}})) {
      continue;
    }

    const auto indexUses = uses.find(index.id);
    if (indexUses == uses.end()) {
      continue;
    }
    bool validIndexUses = true;
    for (const Location &location : indexUses->second) {
      if (members.count(static_cast<int>(location.block)) != 0) {
        if (!(location == indexAddLocation)) {
          validIndexUses = false;
          break;
        }
      } else if (!dominates(dominators, static_cast<std::size_t>(exit),
                            location.block)) {
        validIndexUses = false;
        break;
      }
    }
    if (!validIndexUses) {
      continue;
    }

    int64_t counterStart = 0;
    int64_t counterBound = 0;
    int64_t indexStart = 0;
    std::set<uint32_t> boundVisiting;
    if (!resolvePreheaderConstant(
            mf, preheader, counter, defs, counterStart) ||
        !hasOnlyPreheaderConstantDefs(mf, preheader, compare.uses[1], defs,
                                      boundVisiting) ||
        !resolvePreheaderConstant(mf, preheader, compare.uses[1], defs,
                                  counterBound) ||
        !resolvePreheaderConstant(mf, preheader, index, defs, indexStart) ||
        counterBound <= counterStart) {
      continue;
    }
    const int64_t delta = counterBound - counterStart;
    if (delta <= 0 || delta > std::numeric_limits<int32_t>::max()) {
      continue;
    }
    const int64_t steps = delta - 1;
    if (steps > (std::numeric_limits<int64_t>::max() -
                 (indexStart >= 0 ? indexStart : 0)) /
                    indexStep) {
      continue;
    }
    const int64_t threshold = indexStart + steps * indexStep;
    if (threshold < std::numeric_limits<int32_t>::min() ||
        threshold > std::numeric_limits<int32_t>::max()) {
      continue;
    }

    candidate = Candidate{header, latch, preheader, condition, exit, addIndex,
                          copyIndex, compareIndex, branchIndex, counter,
                          counterNext, index,
                          indexStep, threshold};
    return true;
  }
  return false;
}

void applyCandidate(MachineFunction &mf, const Candidate &candidate) {
  MachineBasicBlock &preheader =
      mf.blocks[static_cast<std::size_t>(candidate.preheader)];
  MachineReg bound = mf.createVReg(RegClass::GPR32);
  MachineInstr materialization;
  materialization.opcode = Opcode::LI;
  materialization.defs.push_back(MachineOperand::reg(bound));
  materialization.uses.push_back(MachineOperand::imm(candidate.threshold));
  std::size_t insertAt = preheader.instrs.size();
  for (std::size_t index = 0; index < preheader.instrs.size(); ++index) {
    const MachineInstr &instruction = preheader.instrs[index];
    if (instruction.terminator || instruction.opcode == Opcode::J ||
        instruction.opcode == Opcode::BEQZ || instruction.opcode == Opcode::BNEZ ||
        instruction.opcode == Opcode::BEQ || instruction.opcode == Opcode::BNE ||
        instruction.opcode == Opcode::BLT || instruction.opcode == Opcode::BGE ||
        instruction.opcode == Opcode::RET) {
      insertAt = index;
      break;
    }
  }
  preheader.instrs.insert(preheader.instrs.begin() +
                              static_cast<std::ptrdiff_t>(insertAt),
                          std::move(materialization));

  MachineBasicBlock &condition =
      mf.blocks[static_cast<std::size_t>(candidate.condition)];
  MachineInstr &compare = condition.instrs[candidate.compare];
  compare.uses[0] = MachineOperand::reg(candidate.index);
  compare.uses[1] = MachineOperand::reg(bound);

  MachineBasicBlock &latch = mf.blocks[static_cast<std::size_t>(candidate.latch)];
  if (candidate.counterCopy < latch.instrs.size()) {
    latch.instrs.erase(latch.instrs.begin() +
                       static_cast<std::ptrdiff_t>(candidate.counterCopy));
  }
  MachineBasicBlock &conditionAfter =
      mf.blocks[static_cast<std::size_t>(candidate.condition)];
  if (candidate.counterAdd < conditionAfter.instrs.size()) {
    conditionAfter.instrs.erase(conditionAfter.instrs.begin() +
                                static_cast<std::ptrdiff_t>(candidate.counterAdd));
  }
}

} // namespace

bool runPreRARedundantLoopIV(MachineFunction &mf,
                             MachineFunctionAnalysisManager &am) {
  if (std::getenv("BACKEND_DISABLE_REDUNDANT_LOOP_IV") != nullptr ||
      mf.blocks.empty()) {
    return false;
  }
  constexpr std::size_t kInstructionBudget = 4096;
  std::size_t instructionCount = 0;
  for (const MachineBasicBlock &block : mf.blocks)
    instructionCount += block.instrs.size();
  if (instructionCount > kInstructionBudget)
    return false;

  const auto &cfg = am.getResult<MachineCFGAnalysis>(mf);
  const auto &loops = am.getResult<MachineLoopAnalysis>(mf);
  const Dominators dominators = computeDominators(cfg);
  if (dominators.size() != mf.blocks.size()) {
    return false;
  }
  Locations defs;
  Locations uses;
  collectLocations(mf, defs, uses);

  std::map<int, std::vector<int>> latchesByHeader;
  for (const auto &[latch, header] : loops.backedges) {
    latchesByHeader[header].push_back(latch);
  }
  for (const auto &[header, latches] : latchesByHeader) {
    if (latches.size() != 1) {
      continue;
    }
    Candidate candidate;
    if (!findCandidate(mf, cfg, dominators, defs, uses, header,
                       latches.front(), candidate)) {
      continue;
    }
    applyCandidate(mf, candidate);
    return true;
  }
  return false;
}

} // namespace riscv
