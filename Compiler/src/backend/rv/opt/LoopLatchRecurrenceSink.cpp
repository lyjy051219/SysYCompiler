#include "riscv/opt/MachineOptPasses.hpp"

#include "riscv/Target.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace riscv {
namespace {

struct InstrLocation {
  std::size_t block = 0;
  std::size_t instr = 0;

  bool operator==(const InstrLocation &other) const {
    return block == other.block && instr == other.instr;
  }

  bool operator<(const InstrLocation &other) const {
    return block < other.block || (block == other.block && instr < other.instr);
  }
};

using LocationMap = std::map<uint32_t, std::vector<InstrLocation>>;
using DominatorSets = std::vector<std::set<int>>;

bool sameRegister(const MachineReg &lhs, const MachineReg &rhs) {
  return lhs.physical == rhs.physical && lhs.id == rhs.id && lhs.rc == rhs.rc &&
         lhs.phys == rhs.phys;
}

bool isVirtualRegister(const MachineOperand &operand) {
  return operand.kind == OperandKind::Reg && !operand.regValue.physical;
}

void collectDefUseLocations(const MachineFunction &mf, LocationMap &defs,
                            LocationMap &uses) {
  for (std::size_t blockIndex = 0; blockIndex < mf.blocks.size();
       ++blockIndex) {
    const MachineBasicBlock &block = mf.blocks[blockIndex];
    for (std::size_t instrIndex = 0; instrIndex < block.instrs.size();
         ++instrIndex) {
      const InstrLocation location{blockIndex, instrIndex};
      const MachineInstr &instr = block.instrs[instrIndex];
      for (const MachineOperand &def : instr.defs) {
        if (isVirtualRegister(def)) {
          defs[def.regValue.id].push_back(location);
        }
      }
      for (const MachineOperand &use : instr.uses) {
        if (isVirtualRegister(use)) {
          uses[use.regValue.id].push_back(location);
        }
      }
    }
  }
}

DominatorSets computeDominators(const MachineCFGAnalysis::Result &cfg) {
  const std::size_t blockCount = cfg.succs.size();
  DominatorSets dominators(blockCount);
  if (blockCount == 0) {
    return dominators;
  }

  std::vector<bool> reachable(blockCount, false);
  std::vector<int> work{0};
  reachable[0] = true;
  while (!work.empty()) {
    const int block = work.back();
    work.pop_back();
    for (int succ : cfg.succs[static_cast<std::size_t>(block)]) {
      if (succ < 0 || succ >= static_cast<int>(blockCount)) {
        throw BackendError("invalid CFG successor during recurrence sinking");
      }
      if (!reachable[static_cast<std::size_t>(succ)]) {
        reachable[static_cast<std::size_t>(succ)] = true;
        work.push_back(succ);
      }
    }
  }

  std::set<int> allReachable;
  for (std::size_t block = 0; block < blockCount; ++block) {
    if (reachable[block]) {
      allReachable.insert(static_cast<int>(block));
    }
  }
  dominators[0].insert(0);
  for (std::size_t block = 1; block < blockCount; ++block) {
    if (reachable[block]) {
      dominators[block] = allReachable;
    }
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t block = 1; block < blockCount; ++block) {
      if (!reachable[block]) {
        continue;
      }
      std::set<int> next;
      bool first = true;
      for (int pred : cfg.preds[block]) {
        if (pred < 0 || pred >= static_cast<int>(blockCount)) {
          throw BackendError(
              "invalid CFG predecessor during recurrence sinking");
        }
        if (!reachable[static_cast<std::size_t>(pred)]) {
          continue;
        }
        if (first) {
          next = dominators[static_cast<std::size_t>(pred)];
          first = false;
          continue;
        }
        std::set<int> intersection;
        std::set_intersection(
            next.begin(), next.end(),
            dominators[static_cast<std::size_t>(pred)].begin(),
            dominators[static_cast<std::size_t>(pred)].end(),
            std::inserter(intersection, intersection.begin()));
        next = std::move(intersection);
      }
      if (first) {
        next.clear();
      }
      next.insert(static_cast<int>(block));
      if (next != dominators[block]) {
        dominators[block] = std::move(next);
        changed = true;
      }
    }
  }
  return dominators;
}

bool dominates(const DominatorSets &dominators, std::size_t dominator,
               std::size_t block) {
  return block < dominators.size() &&
         dominators[block].count(static_cast<int>(dominator)) != 0;
}

std::set<int> naturalLoopMembers(int header, int latch,
                                 const MachineCFGAnalysis::Result &cfg,
                                 const DominatorSets &dominators) {
  std::set<int> members{header, latch};
  std::vector<int> work;
  if (latch != header) {
    work.push_back(latch);
  }
  while (!work.empty()) {
    const int block = work.back();
    work.pop_back();
    for (int pred : cfg.preds[static_cast<std::size_t>(block)]) {
      if (pred < 0 || pred >= static_cast<int>(cfg.preds.size())) {
        throw BackendError(
            "invalid natural-loop predecessor during recurrence sinking");
      }
      if (pred == header ||
          !dominates(dominators, static_cast<std::size_t>(header),
                     static_cast<std::size_t>(pred))) {
        continue;
      }
      if (members.insert(pred).second) {
        work.push_back(pred);
      }
    }
  }
  return members;
}

bool isPhiCopyCandidate(const MachineInstr &copy) {
  return copy.opcode == Opcode::COPY && copy.phiCopy &&
         !copy.copyCoalescingBarrier && !copy.sideEffects && !copy.terminator &&
         copy.implicitDefs.empty() && copy.implicitUses.empty() &&
         copy.defs.size() == 1 && copy.uses.size() == 1 &&
         isVirtualRegister(copy.defs.front()) &&
         isVirtualRegister(copy.uses.front()) &&
         copy.defs.front().regValue.rc == copy.uses.front().regValue.rc &&
         !sameRegister(copy.defs.front().regValue, copy.uses.front().regValue);
}

bool isSelfAddImmediate(const MachineInstr &producer,
                        const MachineReg &destination,
                        const MachineReg &source) {
  if ((producer.opcode != Opcode::ADDI && producer.opcode != Opcode::ADDIW) ||
      producer.sideEffects || producer.terminator ||
      !producer.implicitDefs.empty() || !producer.implicitUses.empty() ||
      producer.defs.size() != 1 || producer.uses.size() != 2 ||
      !isVirtualRegister(producer.defs.front()) ||
      !isVirtualRegister(producer.uses.front()) ||
      producer.uses[1].kind != OperandKind::Imm) {
    return false;
  }
  return sameRegister(producer.defs.front().regValue, source) &&
         sameRegister(producer.uses.front().regValue, destination) &&
         source.rc == destination.rc;
}

bool hasOnlyCandidateDefinitionInLoop(const MachineReg &destination,
                                      const InstrLocation &copyLocation,
                                      const std::set<int> &members,
                                      const LocationMap &defs) {
  const auto found = defs.find(destination.id);
  if (found == defs.end()) {
    return false;
  }
  bool hasOutsideDefinition = false;
  for (const InstrLocation &definition : found->second) {
    if (members.count(static_cast<int>(definition.block)) == 0) {
      hasOutsideDefinition = true;
      continue;
    }
    if (!(definition == copyLocation)) {
      return false;
    }
  }
  return hasOutsideDefinition;
}

struct Rewrite {
  InstrLocation producer;
  InstrLocation copy;
  MachineInstr replacement;
};

void applyRewrites(MachineFunction &mf, std::vector<Rewrite> rewrites) {
  std::set<InstrLocation> erasedProducers;
  std::map<InstrLocation, MachineInstr> replacements;
  for (Rewrite &rewrite : rewrites) {
    erasedProducers.insert(rewrite.producer);
    replacements.emplace(rewrite.copy, std::move(rewrite.replacement));
  }

  for (std::size_t blockIndex = 0; blockIndex < mf.blocks.size();
       ++blockIndex) {
    MachineBasicBlock &block = mf.blocks[blockIndex];
    std::vector<MachineInstr> rewritten;
    rewritten.reserve(block.instrs.size());
    for (std::size_t instrIndex = 0; instrIndex < block.instrs.size();
         ++instrIndex) {
      const InstrLocation location{blockIndex, instrIndex};
      auto replacement = replacements.find(location);
      if (replacement != replacements.end()) {
        rewritten.push_back(std::move(replacement->second));
        continue;
      }
      if (erasedProducers.count(location) != 0) {
        continue;
      }
      rewritten.push_back(std::move(block.instrs[instrIndex]));
    }
    block.instrs = std::move(rewritten);
  }
}

} // namespace

bool runPreRALoopLatchRecurrenceSink(MachineFunction &mf,
                                     MachineFunctionAnalysisManager &am) {
  if (mf.blocks.empty()) {
    return false;
  }

  const auto &cfg = am.getResult<MachineCFGAnalysis>(mf);
  const auto &loops = am.getResult<MachineLoopAnalysis>(mf);
  const DominatorSets dominators = computeDominators(cfg);

  LocationMap defs;
  LocationMap uses;
  collectDefUseLocations(mf, defs, uses);

  std::map<int, std::vector<int>> latchesByHeader;
  for (const auto &[latch, header] : loops.backedges) {
    latchesByHeader[header].push_back(latch);
  }

  std::vector<Rewrite> rewrites;
  for (const auto &[header, latches] : latchesByHeader) {
    // Multiple latches require reasoning about several incoming recurrence
    // definitions at once.  Keep the first version deliberately single-latch.
    if (latches.size() != 1) {
      continue;
    }
    const int latch = latches.front();
    if (header < 0 || latch < 0 ||
        header >= static_cast<int>(mf.blocks.size()) ||
        latch >= static_cast<int>(mf.blocks.size()) ||
        cfg.succs[static_cast<std::size_t>(latch)].size() != 1 ||
        cfg.succs[static_cast<std::size_t>(latch)].front() != header) {
      continue;
    }

    const std::set<int> members =
        naturalLoopMembers(header, latch, cfg, dominators);
    const MachineBasicBlock &latchBlock =
        mf.blocks[static_cast<std::size_t>(latch)];
    for (std::size_t copyIndex = 0; copyIndex < latchBlock.instrs.size();
         ++copyIndex) {
      const MachineInstr &copy = latchBlock.instrs[copyIndex];
      if (!isPhiCopyCandidate(copy)) {
        continue;
      }

      const MachineReg destination = copy.defs.front().regValue;
      const MachineReg source = copy.uses.front().regValue;
      const InstrLocation copyLocation{static_cast<std::size_t>(latch),
                                       copyIndex};
      const auto sourceDefs = defs.find(source.id);
      const auto sourceUses = uses.find(source.id);
      if (sourceDefs == defs.end() || sourceDefs->second.size() != 1 ||
          sourceUses == uses.end() || sourceUses->second.size() != 1 ||
          !(sourceUses->second.front() == copyLocation)) {
        continue;
      }

      const InstrLocation producerLocation = sourceDefs->second.front();
      if (producerLocation.block >= mf.blocks.size() ||
          producerLocation.instr >=
              mf.blocks[producerLocation.block].instrs.size() ||
          members.count(static_cast<int>(producerLocation.block)) == 0 ||
          !dominates(dominators, producerLocation.block,
                     static_cast<std::size_t>(latch)) ||
          (producerLocation.block == copyLocation.block &&
           producerLocation.instr >= copyLocation.instr)) {
        continue;
      }

      const MachineInstr &producer =
          mf.blocks[producerLocation.block].instrs[producerLocation.instr];
      if (!isSelfAddImmediate(producer, destination, source) ||
          !hasOnlyCandidateDefinitionInLoop(destination, copyLocation, members,
                                            defs)) {
        continue;
      }

      MachineInstr replacement = producer;
      replacement.defs.front() = MachineOperand::reg(destination);
      replacement.phiCopy = false;
      replacement.copyCoalescingBarrier = false;
      rewrites.push_back(
          Rewrite{producerLocation, copyLocation, std::move(replacement)});
    }
  }

  if (rewrites.empty()) {
    return false;
  }
  applyRewrites(mf, std::move(rewrites));
  return true;
}

} // namespace riscv
