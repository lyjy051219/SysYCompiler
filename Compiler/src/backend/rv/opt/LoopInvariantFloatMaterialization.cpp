#include "riscv/opt/MachineOptPasses.hpp"

#include "riscv/Registers.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
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

  bool operator<(const Location &other) const {
    return block < other.block ||
           (block == other.block && instr < other.instr);
  }
};

using Dominators = std::vector<std::set<int>>;
using Locations = std::map<uint32_t, std::vector<Location>>;
using Classes = std::map<uint32_t, RegClass>;

bool isVirtual(const MachineOperand &operand) {
  return operand.kind == OperandKind::Reg && !operand.regValue.physical;
}

bool isVirtualGpr(const MachineOperand &operand) {
  return isVirtual(operand) &&
         (operand.regValue.rc == RegClass::GPR32 ||
          operand.regValue.rc == RegClass::GPR64);
}

bool isVirtualFpr32(const MachineOperand &operand) {
  return isVirtual(operand) && operand.regValue.rc == RegClass::FPR32;
}

bool sameRegister(const MachineReg &lhs, const MachineReg &rhs) {
  return !lhs.physical && !rhs.physical && lhs.id == rhs.id &&
         lhs.rc == rhs.rc;
}

bool isPure(const MachineInstr &instr) {
  return !instr.sideEffects && !instr.terminator &&
         instr.implicitDefs.empty() && instr.implicitUses.empty();
}

void collectLocations(const MachineFunction &mf, Locations &defs,
                      Locations &uses, Classes &classes) {
  for (std::size_t block = 0; block < mf.blocks.size(); ++block) {
    for (std::size_t instr = 0; instr < mf.blocks[block].instrs.size();
         ++instr) {
      const Location location{block, instr};
      const MachineInstr &instruction = mf.blocks[block].instrs[instr];
      for (const MachineOperand &def : instruction.defs) {
        if (!isVirtual(def))
          continue;
        defs[def.regValue.id].push_back(location);
        classes.emplace(def.regValue.id, def.regValue.rc);
      }
      for (const MachineOperand &use : instruction.uses) {
        if (!isVirtual(use))
          continue;
        uses[use.regValue.id].push_back(location);
        classes.emplace(use.regValue.id, use.regValue.rc);
      }
    }
  }
}

Dominators computeDominators(const MachineCFGAnalysis::Result &cfg) {
  const std::size_t count = cfg.succs.size();
  Dominators dominators(count);
  if (count == 0)
    return dominators;

  std::vector<bool> reachable(count, false);
  std::vector<int> work{0};
  reachable[0] = true;
  while (!work.empty()) {
    const int block = work.back();
    work.pop_back();
    for (int successor : cfg.succs[static_cast<std::size_t>(block)]) {
      if (successor < 0 || successor >= static_cast<int>(count))
        return {};
      if (!reachable[static_cast<std::size_t>(successor)]) {
        reachable[static_cast<std::size_t>(successor)] = true;
        work.push_back(successor);
      }
    }
  }

  std::set<int> allReachable;
  for (std::size_t block = 0; block < count; ++block)
    if (reachable[block])
      allReachable.insert(static_cast<int>(block));
  dominators[0].insert(0);
  for (std::size_t block = 1; block < count; ++block)
    if (reachable[block])
      dominators[block] = allReachable;

  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t block = 1; block < count; ++block) {
      if (!reachable[block])
        continue;
      std::set<int> next;
      bool first = true;
      for (int predecessor : cfg.preds[block]) {
        if (predecessor < 0 || predecessor >= static_cast<int>(count) ||
            !reachable[static_cast<std::size_t>(predecessor)])
          continue;
        if (first) {
          next = dominators[static_cast<std::size_t>(predecessor)];
          first = false;
        } else {
          std::set<int> intersection;
          std::set_intersection(
              next.begin(), next.end(),
              dominators[static_cast<std::size_t>(predecessor)].begin(),
              dominators[static_cast<std::size_t>(predecessor)].end(),
              std::inserter(intersection, intersection.begin()));
          next = std::move(intersection);
        }
      }
      if (first)
        next.clear();
      next.insert(static_cast<int>(block));
      if (next != dominators[block]) {
        dominators[block] = std::move(next);
        changed = true;
      }
    }
  }
  return dominators;
}

bool dominates(const Dominators &dominators, int dominator, int block) {
  return block >= 0 && static_cast<std::size_t>(block) < dominators.size() &&
         dominators[static_cast<std::size_t>(block)].count(dominator) != 0;
}

std::set<int> naturalLoopMembers(int header, int latch,
                                 const MachineCFGAnalysis::Result &cfg,
                                 const Dominators &dominators) {
  std::set<int> members{header, latch};
  std::vector<int> work;
  if (header != latch)
    work.push_back(latch);
  while (!work.empty()) {
    const int block = work.back();
    work.pop_back();
    for (int predecessor : cfg.preds[static_cast<std::size_t>(block)]) {
      if (predecessor == header || !dominates(dominators, header, predecessor))
        continue;
      if (members.insert(predecessor).second)
        work.push_back(predecessor);
    }
  }
  return members;
}

bool hasCall(const MachineFunction &mf, const std::set<int> &members) {
  for (int member : members)
    for (const MachineInstr &instr :
         mf.blocks[static_cast<std::size_t>(member)].instrs)
      if (instr.opcode == Opcode::CALL)
        return true;
  return false;
}

struct Candidate {
  MachineReg source;
  std::vector<Location> locations;
  std::vector<uint32_t> destinations;
};

bool beforeTerminator(const MachineFunction &mf, const Location &location) {
  const auto &instructions = mf.blocks[location.block].instrs;
  if (location.instr >= instructions.size())
    return false;
  if (instructions[location.instr].terminator)
    return false;
  for (std::size_t index = 0; index <= location.instr; ++index) {
    if (index != location.instr && instructions[index].terminator)
      return false;
  }
  return true;
}

bool sourceDominatesPreheader(const MachineFunction &mf,
                              const MachineOperand &source, int preheader,
                              const std::set<int> &members,
                              const Locations &defs,
                              const Dominators &dominators) {
  if (!isVirtualGpr(source))
    return false;
  const auto found = defs.find(source.regValue.id);
  if (found == defs.end() || found->second.size() != 1)
    return false;
  const Location definition = found->second.front();
  if (members.count(static_cast<int>(definition.block)) != 0)
    return false;
  if (!beforeTerminator(mf, definition))
    return false;
  return dominates(dominators, static_cast<int>(definition.block), preheader);
}

bool isCandidateFMV(const MachineInstr &instr, MachineReg &destination,
                    MachineReg &source) {
  if (!isPure(instr) || instr.opcode != Opcode::FMV_W_X ||
      instr.defs.size() != 1 || instr.uses.size() != 1 ||
      !isVirtualFpr32(instr.defs.front()) ||
      !isVirtualGpr(instr.uses.front()))
    return false;
  destination = instr.defs.front().regValue;
  source = instr.uses.front().regValue;
  return true;
}

bool allUsesInLoop(uint32_t destination, const Location &definition,
                   const Locations &uses, const std::set<int> &members,
                   const Dominators &dominators) {
  const auto found = uses.find(destination);
  if (found == uses.end() || found->second.empty())
    return false;
  return std::all_of(found->second.begin(), found->second.end(),
                     [&](const Location &location) {
                       if (members.count(static_cast<int>(location.block)) == 0)
                         return false;
                       if (location.block == definition.block)
                         return location.instr > definition.instr;
                       return dominates(dominators,
                                        static_cast<int>(definition.block),
                                        static_cast<int>(location.block));
                     });
}

std::size_t peakFprPressure(const MachineFunction &mf,
                            const std::set<int> &members,
                            const MachineLivenessAnalysis::Result &liveness,
                            const Classes &classes) {
  std::size_t peak = 0;
  for (int member : members) {
    if (member < 0 || static_cast<std::size_t>(member) >= liveness.liveOut.size())
      continue;
    MachineLivenessAnalysis::RegSet live =
        liveness.liveOut[static_cast<std::size_t>(member)];
    auto update = [&] {
      const std::size_t pressure = static_cast<std::size_t>(std::count_if(
          live.begin(), live.end(), [&](uint32_t id) {
            const auto found = classes.find(id);
            return found != classes.end() && found->second == RegClass::FPR32;
          }));
      peak = std::max(peak, pressure);
    };
    update();
    const auto &instructions = mf.blocks[static_cast<std::size_t>(member)].instrs;
    for (auto it = instructions.rbegin(); it != instructions.rend(); ++it) {
      for (const MachineOperand &def : it->defs)
        if (isVirtual(def))
          live.erase(def.regValue.id);
      for (const MachineOperand &use : it->uses)
        if (isVirtual(use))
          live.insert(use.regValue.id);
      update();
    }
  }
  return peak;
}

bool hoistLoop(MachineFunction &mf, int header, int preheader,
               const std::set<int> &members,
               const MachineCFGAnalysis::Result &cfg,
               const Dominators &dominators,
               const MachineLivenessAnalysis::Result &liveness,
               const Locations &defs, const Locations &uses,
               const Classes &classes) {
  if (hasCall(mf, members))
    return false;

  std::map<std::pair<uint32_t, RegClass>, Candidate> grouped;
  for (int member : members) {
    const auto &instructions = mf.blocks[static_cast<std::size_t>(member)].instrs;
    for (std::size_t index = 0; index < instructions.size(); ++index) {
      MachineReg destination;
      MachineReg source;
      if (!isCandidateFMV(instructions[index], destination, source))
        continue;
      const Location definition{static_cast<std::size_t>(member), index};
      const auto destinationDefs = defs.find(destination.id);
      const bool exactDef =
          destinationDefs != defs.end() &&
          destinationDefs->second.size() == 1 &&
          destinationDefs->second.front() == definition;
      const bool validUses =
          allUsesInLoop(destination.id, definition, uses, members, dominators);
      const bool validSource = sourceDominatesPreheader(
          mf, MachineOperand::reg(source), preheader, members, defs, dominators);
      if (!exactDef || !validUses || !validSource)
        continue;
      Candidate &candidate = grouped[{source.id, source.rc}];
      candidate.source = source;
      candidate.locations.push_back(Location{static_cast<std::size_t>(member), index});
      candidate.destinations.push_back(destination.id);
    }
  }
  for (auto it = grouped.begin(); it != grouped.end();) {
    if (it->second.locations.size() < 2)
      it = grouped.erase(it);
    else
      ++it;
  }
  if (grouped.empty())
    return false;

  constexpr std::size_t kFprReserve = 2;
  constexpr std::size_t kHeadroom = 2;
  const std::size_t colors = allocatableRegs(RegClass::FPR32).size();
  const std::size_t peak = peakFprPressure(mf, members, liveness, classes);
  const std::size_t budget = colors > peak + kFprReserve + kHeadroom
                                 ? colors - peak - kFprReserve - kHeadroom
                                 : 0;
  if (budget == 0)
    return false;

  std::vector<Candidate *> selected;
  for (auto &[source, candidate] : grouped) {
    (void)source;
    selected.push_back(&candidate);
  }
  std::stable_sort(selected.begin(), selected.end(), [](const Candidate *lhs,
                                                        const Candidate *rhs) {
    if (lhs->locations.size() != rhs->locations.size())
      return lhs->locations.size() > rhs->locations.size();
    if (lhs->source.id != rhs->source.id)
      return lhs->source.id < rhs->source.id;
    return lhs->source.rc < rhs->source.rc;
  });
  constexpr std::size_t kMaxHoistsPerLoop = 8;
  const std::size_t limit = std::min(budget, kMaxHoistsPerLoop);
  if (selected.size() > limit)
    return false;

  std::map<uint32_t, MachineReg> replacements;
  std::set<Location> erased;
  std::vector<MachineInstr> hoisted;
  for (const Candidate *candidate : selected) {
    MachineReg cached = mf.createVReg(RegClass::FPR32);
    MachineInstr materialization;
    materialization.opcode = Opcode::FMV_W_X;
    materialization.defs.push_back(MachineOperand::reg(cached));
    materialization.uses.push_back(MachineOperand::reg(candidate->source));
    hoisted.push_back(std::move(materialization));
    for (uint32_t destination : candidate->destinations)
      replacements[destination] = cached;
    erased.insert(candidate->locations.begin(), candidate->locations.end());
  }

  for (MachineBasicBlock &block : mf.blocks)
    for (MachineInstr &instr : block.instrs)
      for (MachineOperand &use : instr.uses) {
        if (!isVirtual(use))
          continue;
        const auto replacement = replacements.find(use.regValue.id);
        if (replacement != replacements.end())
          use.regValue = replacement->second;
      }

  for (std::size_t block = 0; block < mf.blocks.size(); ++block) {
    auto &instructions = mf.blocks[block].instrs;
    std::vector<MachineInstr> rewritten;
    rewritten.reserve(instructions.size());
    for (std::size_t index = 0; index < instructions.size(); ++index) {
      if (erased.count(Location{block, index}) != 0)
        continue;
      rewritten.push_back(std::move(instructions[index]));
    }
    instructions = std::move(rewritten);
  }

  MachineBasicBlock &preheaderBlock =
      mf.blocks[static_cast<std::size_t>(preheader)];
  auto insertion = std::find_if(
      preheaderBlock.instrs.begin(), preheaderBlock.instrs.end(),
      [](const MachineInstr &instr) { return instr.terminator; });
  preheaderBlock.instrs.insert(insertion,
                               std::make_move_iterator(hoisted.begin()),
                               std::make_move_iterator(hoisted.end()));
  (void)header;
  (void)cfg;
  return true;
}

} // namespace

bool runPreRALoopInvariantFloatMaterialization(
    MachineFunction &mf, MachineFunctionAnalysisManager &am) {
  if (mf.blocks.empty() ||
      std::getenv("BACKEND_DISABLE_FLOAT_CONSTANT_HOIST") != nullptr)
    return false;
  constexpr std::size_t kInstructionBudget = 4096;
  std::size_t instructionCount = 0;
  for (const MachineBasicBlock &block : mf.blocks)
    instructionCount += block.instrs.size();
  if (instructionCount > kInstructionBudget)
    return false;

  const auto &cfg = am.getResult<MachineCFGAnalysis>(mf);
  const auto &loops = am.getResult<MachineLoopAnalysis>(mf);
  const Dominators dominators = computeDominators(cfg);
  if (dominators.size() != mf.blocks.size())
    return false;

  std::map<int, std::vector<int>> latchesByHeader;
  for (const auto &[latch, header] : loops.backedges)
    latchesByHeader[header].push_back(latch);

  bool changed = false;
  for (const auto &[header, latches] : latchesByHeader) {
    if (latches.size() != 1 || header < 0 ||
        header >= static_cast<int>(mf.blocks.size()))
      continue;
    const int latch = latches.front();
    const std::set<int> members =
        naturalLoopMembers(header, latch, cfg, dominators);
    int preheader = -1;
    for (int predecessor : cfg.preds[static_cast<std::size_t>(header)]) {
      if (members.count(predecessor) != 0)
        continue;
      if (preheader != -1) {
        preheader = -1;
        break;
      }
      preheader = predecessor;
    }
    if (preheader < 0 ||
        cfg.succs[static_cast<std::size_t>(preheader)].size() != 1 ||
        cfg.succs[static_cast<std::size_t>(preheader)].front() != header)
      continue;
    MachineFunctionAnalysisManager localAM;
    localAM.registerPass([] { return MachineCFGAnalysis(); });
    localAM.registerPass([] { return MachineLivenessAnalysis(); });
    const auto &liveness = localAM.getResult<MachineLivenessAnalysis>(mf);
    Locations defs;
    Locations uses;
    Classes classes;
    collectLocations(mf, defs, uses, classes);
    const bool loopChanged = hoistLoop(
        mf, header, preheader, members, cfg, dominators, liveness, defs, uses,
        classes);
    changed |= loopChanged;
  }
  return changed;
}

} // namespace riscv
