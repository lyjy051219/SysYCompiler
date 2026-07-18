#include "riscv/opt/MachineOptPasses.hpp"

#include "riscv/Registers.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace riscv {
namespace {

constexpr std::size_t kMaxHoistedImmediatesPerLoop = 10;
constexpr const char *kHoistedComment = "hoisted loop-invariant immediate";

bool isVirtualRegister(const MachineOperand &operand) {
  return operand.kind == OperandKind::Reg && !operand.regValue.physical;
}

bool fitsSigned12(int64_t value) { return value >= -2048 && value <= 2047; }

std::set<int> naturalLoopMembers(int header, int latch,
                                 const MachineCFGAnalysis::Result &cfg) {
  std::set<int> members{header, latch};
  std::vector<int> work;
  if (header != latch)
    work.push_back(latch);
  while (!work.empty()) {
    const int block = work.back();
    work.pop_back();
    for (int predecessor : cfg.preds[static_cast<std::size_t>(block)]) {
      if (predecessor != header && members.insert(predecessor).second)
        work.push_back(predecessor);
    }
  }
  return members;
}

using DominatorSets = std::vector<std::set<int>>;

DominatorSets computeDominators(const MachineCFGAnalysis::Result &cfg) {
  const std::size_t blockCount = cfg.preds.size();
  DominatorSets dominators(blockCount);
  if (blockCount == 0)
    return dominators;

  std::vector<bool> reachable(blockCount, false);
  std::vector<int> work{0};
  reachable[0] = true;
  while (!work.empty()) {
    const int block = work.back();
    work.pop_back();
    for (int successor : cfg.succs[static_cast<std::size_t>(block)]) {
      if (successor >= 0 && successor < static_cast<int>(blockCount) &&
          !reachable[static_cast<std::size_t>(successor)]) {
        reachable[static_cast<std::size_t>(successor)] = true;
        work.push_back(successor);
      }
    }
  }

  std::set<int> allReachable;
  for (std::size_t block = 0; block < blockCount; ++block)
    if (reachable[block])
      allReachable.insert(static_cast<int>(block));
  dominators[0].insert(0);
  for (std::size_t block = 1; block < blockCount; ++block)
    if (reachable[block])
      dominators[block] = allReachable;

  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t block = 1; block < blockCount; ++block) {
      if (!reachable[block])
        continue;
      std::set<int> next;
      bool first = true;
      for (int predecessor : cfg.preds[block]) {
        if (predecessor < 0 || predecessor >= static_cast<int>(blockCount) ||
            !reachable[static_cast<std::size_t>(predecessor)]) {
          continue;
        }
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
      next.insert(static_cast<int>(block));
      if (next != dominators[block]) {
        dominators[block] = std::move(next);
        changed = true;
      }
    }
  }
  return dominators;
}

std::map<uint32_t, std::size_t>
definitionCounts(const MachineFunction &function) {
  std::map<uint32_t, std::size_t> counts;
  for (const MachineBasicBlock &block : function.blocks)
    for (const MachineInstr &instruction : block.instrs)
      for (const MachineOperand &definition : instruction.defs)
        if (isVirtualRegister(definition))
          ++counts[definition.regValue.id];
  return counts;
}

std::map<uint32_t, std::size_t> useCounts(const MachineFunction &function) {
  std::map<uint32_t, std::size_t> counts;
  for (const MachineBasicBlock &block : function.blocks)
    for (const MachineInstr &instruction : block.instrs)
      for (const MachineOperand &use : instruction.uses)
        if (isVirtualRegister(use))
          ++counts[use.regValue.id];
  return counts;
}

std::map<uint32_t, std::set<int>>
useBlocks(const MachineFunction &function) {
  std::map<uint32_t, std::set<int>> result;
  for (std::size_t block = 0; block < function.blocks.size(); ++block)
    for (const MachineInstr &instruction : function.blocks[block].instrs)
      for (const MachineOperand &use : instruction.uses)
        if (isVirtualRegister(use))
          result[use.regValue.id].insert(static_cast<int>(block));
  return result;
}

std::map<uint32_t, RegClass>
virtualRegisterClasses(const MachineFunction &function) {
  std::map<uint32_t, RegClass> result;
  for (const MachineBasicBlock &block : function.blocks) {
    for (const MachineInstr &instruction : block.instrs) {
      for (const MachineOperand &definition : instruction.defs)
        if (isVirtualRegister(definition))
          result.emplace(definition.regValue.id, definition.regValue.rc);
      for (const MachineOperand &use : instruction.uses)
        if (isVirtualRegister(use))
          result.emplace(use.regValue.id, use.regValue.rc);
    }
  }
  return result;
}

bool containsCall(const MachineFunction &function,
                  const std::set<int> &members) {
  for (int member : members) {
    if (member < 0 || member >= static_cast<int>(function.blocks.size()))
      return true;
    for (const MachineInstr &instruction :
         function.blocks[static_cast<std::size_t>(member)].instrs) {
      if (instruction.opcode == Opcode::CALL)
        return true;
    }
  }
  return false;
}

struct Candidate {
  int64_t immediate = 0;
  RegClass regClass = RegClass::GPR32;
  unsigned materializationCost = 0;
  std::vector<uint32_t> definitions;
  MachineInstr exemplar;
  bool initialized = false;
  bool requiredForHeaderCompare = false;
};

using CandidateKey = std::tuple<int64_t, RegClass>;

struct HeaderImmediateCompare {
  int64_t immediate = 0;
  RegClass regClass = RegClass::GPR32;
  Opcode directOpcode = Opcode::BEQ;
};

bool sameRegister(const MachineReg &left, const MachineReg &right) {
  if (left.physical != right.physical)
    return false;
  return left.physical ? left.phys == right.phys : left.id == right.id;
}

MachineInstr makeImmediateMaterialization(const MachineReg &placeholder,
                                          int64_t immediate) {
  MachineInstr instruction;
  instruction.opcode = Opcode::LI;
  instruction.defs.push_back(MachineOperand::reg(placeholder));
  instruction.uses.push_back(MachineOperand::imm(immediate));
  return instruction;
}

std::optional<HeaderImmediateCompare> findHeaderImmediateCompare(
    const MachineFunction &function, int header,
    const std::set<int> &members, const MachineCFGAnalysis::Result &cfg,
    const std::map<uint32_t, std::size_t> &defs,
    const std::map<uint32_t, std::size_t> &uses) {
  if (header < 0 || header >= static_cast<int>(function.blocks.size()) ||
      static_cast<std::size_t>(header) >= cfg.succs.size() ||
      std::getenv("BACKEND_DISABLE_SMALL_LI_HOIST") != nullptr) {
    return std::nullopt;
  }

  // Restrict this to an exiting natural-loop header.  Reducing that header to
  // one register branch also lets the post-RA backedge-bypass pass reuse the
  // condition directly at every latch.
  const auto &successors = cfg.succs[static_cast<std::size_t>(header)];
  if (successors.size() != 2 ||
      std::count_if(successors.begin(), successors.end(), [&](int successor) {
        return members.count(successor) != 0;
      }) != 1) {
    return std::nullopt;
  }

  const MachineBasicBlock &block =
      function.blocks[static_cast<std::size_t>(header)];
  if (block.instrs.size() != 2)
    return std::nullopt;
  const MachineInstr &xori = block.instrs[0];
  const MachineInstr &branch = block.instrs[1];
  if (xori.opcode != Opcode::XORI || xori.sideEffects || xori.terminator ||
      !xori.implicitDefs.empty() || !xori.implicitUses.empty() ||
      xori.defs.size() != 1 || xori.uses.size() != 2 ||
      !isVirtualRegister(xori.defs.front()) ||
      !isVirtualRegister(xori.uses[0]) ||
      xori.uses[1].kind != OperandKind::Imm ||
      !fitsSigned12(xori.uses[1].immValue) ||
      (xori.uses[0].regValue.rc != RegClass::GPR32 &&
       xori.uses[0].regValue.rc != RegClass::GPR64) ||
      (branch.opcode != Opcode::BEQZ && branch.opcode != Opcode::BNEZ) ||
      !branch.terminator || branch.sideEffects || !branch.defs.empty() ||
      !branch.implicitDefs.empty() || !branch.implicitUses.empty() ||
      branch.uses.size() != 2 || branch.uses[0].kind != OperandKind::Reg ||
      branch.uses[1].kind != OperandKind::Block ||
      !sameRegister(branch.uses[0].regValue, xori.defs.front().regValue)) {
    return std::nullopt;
  }

  const uint32_t temporary = xori.defs.front().regValue.id;
  auto defCount = defs.find(temporary);
  auto useCount = uses.find(temporary);
  if (defCount == defs.end() || defCount->second != 1 ||
      useCount == uses.end() || useCount->second != 1) {
    return std::nullopt;
  }

  HeaderImmediateCompare result;
  result.immediate = xori.uses[1].immValue;
  result.regClass = xori.uses[0].regValue.rc;
  result.directOpcode =
      branch.opcode == Opcode::BEQZ ? Opcode::BEQ : Opcode::BNE;
  return result;
}

bool isLoopInvariantImmediateDefinition(
    const MachineInstr &instruction,
    const std::map<uint32_t, std::size_t> &defs,
    const std::map<uint32_t, std::size_t> &uses,
    uint32_t &definition, int64_t &immediate, RegClass &regClass) {
  if (instruction.opcode != Opcode::LI || instruction.sideEffects ||
      instruction.terminator || !instruction.implicitDefs.empty() ||
      !instruction.implicitUses.empty() || instruction.defs.size() != 1 ||
      instruction.uses.size() != 1 ||
      !isVirtualRegister(instruction.defs.front()) ||
      instruction.uses.front().kind != OperandKind::Imm ||
      instruction.comment == kHoistedComment) {
    return false;
  }
  definition = instruction.defs.front().regValue.id;
  regClass = instruction.defs.front().regValue.rc;
  immediate = instruction.uses.front().immValue;
  auto defCount = defs.find(definition);
  auto useCount = uses.find(definition);
  const bool smallHoistDisabled =
      fitsSigned12(immediate) &&
      std::getenv("BACKEND_DISABLE_SMALL_LI_HOIST") != nullptr;
  return (regClass == RegClass::GPR32 || regClass == RegClass::GPR64) &&
         !smallHoistDisabled && defCount != defs.end() &&
         defCount->second == 1 && useCount != uses.end() &&
         useCount->second != 0;
}

bool hoistForLoop(MachineFunction &function, int header,
                  const std::set<int> &members, const std::set<int> &latches,
                  int preheader, const MachineCFGAnalysis::Result &cfg,
                  const DominatorSets &dominators,
                  const MachineLoopAnalysis::Result &loops,
                  const MachineLivenessAnalysis::Result &liveness,
                  const std::map<uint32_t, RegClass> &classes,
                  const std::map<uint32_t, std::size_t> &defs,
                  const std::map<uint32_t, std::size_t> &uses,
                  const std::map<uint32_t, std::set<int>> &useBlockMap) {
  if (header < 0 || preheader < 0 ||
      header >= static_cast<int>(function.blocks.size()) ||
      preheader >= static_cast<int>(function.blocks.size()) ||
      static_cast<std::size_t>(header) >= loops.depth.size()) {
    return false;
  }
  const int loopDepth = loops.depth[static_cast<std::size_t>(header)];
  if (loopDepth <= 0 || containsCall(function, members))
    return false;

  std::map<CandidateKey, Candidate> grouped;
  const std::optional<HeaderImmediateCompare> headerCompare =
      findHeaderImmediateCompare(function, header, members, cfg, defs, uses);
  if (headerCompare) {
    Candidate &candidate =
        grouped[{headerCompare->immediate, headerCompare->regClass}];
    candidate.immediate = headerCompare->immediate;
    candidate.regClass = headerCompare->regClass;
    candidate.materializationCost = 1;
    candidate.exemplar = makeImmediateMaterialization(
        function.blocks[static_cast<std::size_t>(header)]
            .instrs.front()
            .defs.front()
            .regValue,
        headerCompare->immediate);
    candidate.initialized = true;
    candidate.requiredForHeaderCompare = true;
  }
  for (int member : members) {
    if (member < 0 || member >= static_cast<int>(function.blocks.size()) ||
        static_cast<std::size_t>(member) >= loops.depth.size() ||
        loops.depth[static_cast<std::size_t>(member)] != loopDepth) {
      continue;
    }
    for (const MachineInstr &instruction :
         function.blocks[static_cast<std::size_t>(member)].instrs) {
      uint32_t definition = 0;
      int64_t immediate = 0;
      RegClass regClass = RegClass::GPR32;
      if (!isLoopInvariantImmediateDefinition(instruction, defs, uses,
                                              definition, immediate,
                                              regClass)) {
        continue;
      }
      const bool dominatesEveryLatch =
          std::all_of(latches.begin(), latches.end(), [&](int latch) {
            return latch >= 0 &&
                   static_cast<std::size_t>(latch) < dominators.size() &&
                   dominators[static_cast<std::size_t>(latch)].count(member) !=
                       0;
          });
      auto definitionUses = useBlockMap.find(definition);
      const bool allUsesInLoop =
          definitionUses != useBlockMap.end() &&
          std::all_of(definitionUses->second.begin(),
                      definitionUses->second.end(),
                      [&](int useBlock) {
                        return members.count(useBlock) != 0;
                      });
      if (!dominatesEveryLatch || !allUsesInLoop)
        continue;
      Candidate &candidate = grouped[{immediate, regClass}];
      if (!candidate.initialized) {
        candidate.immediate = immediate;
        candidate.regClass = regClass;
        candidate.materializationCost = fitsSigned12(immediate) ? 1 : 2;
        candidate.exemplar = instruction;
        candidate.initialized = true;
      }
      candidate.definitions.push_back(definition);
    }
  }
  if (grouped.empty())
    return false;

  auto isGPR = [&](uint32_t id) {
    auto regClass = classes.find(id);
    return regClass != classes.end() && regClass->second != RegClass::FPR32;
  };
  std::size_t maxGPRLive = 0;
  for (int member : members) {
    if (member < 0 ||
        static_cast<std::size_t>(member) >= liveness.liveOut.size()) {
      continue;
    }
    MachineLivenessAnalysis::RegSet live =
        liveness.liveOut[static_cast<std::size_t>(member)];
    auto updatePeak = [&] {
      const std::size_t pressure =
          static_cast<std::size_t>(std::count_if(live.begin(), live.end(), isGPR));
      maxGPRLive = std::max(maxGPRLive, pressure);
    };
    updatePeak();
    const auto &instructions =
        function.blocks[static_cast<std::size_t>(member)].instrs;
    for (auto instruction = instructions.rbegin();
         instruction != instructions.rend(); ++instruction) {
      for (const MachineOperand &definition : instruction->defs)
        if (isVirtualRegister(definition))
          live.erase(definition.regValue.id);
      for (const MachineOperand &use : instruction->uses)
        if (isVirtualRegister(use))
          live.insert(use.regValue.id);
      updatePeak();
    }
  }
  constexpr std::size_t kTemporaryReserve = 6;
  // Block live-in alone misses short-lived address and arithmetic temporaries.
  // Keep additional color headroom after accounting for the measured peak so
  // that one cached immediate cannot push a whole hot loop into callee-saved
  // registers or spills.
  constexpr std::size_t kMinimumPressureHeadroom = 8;
  const std::size_t colors = allocatableRegs(RegClass::GPR64).size();
  const std::size_t pressureBudget =
      colors > maxGPRLive + kTemporaryReserve
          ? colors - maxGPRLive - kTemporaryReserve
          : 0;
  if (pressureBudget < kMinimumPressureHeadroom)
    return false;

  std::vector<Candidate> candidates;
  for (auto &[key, candidate] : grouped) {
    (void)key;
    candidates.push_back(std::move(candidate));
  }
  std::stable_sort(candidates.begin(), candidates.end(),
                   [](const Candidate &left, const Candidate &right) {
                     if (left.requiredForHeaderCompare !=
                         right.requiredForHeaderCompare) {
                       return left.requiredForHeaderCompare;
                     }
                     const std::size_t leftBenefit =
                         left.materializationCost * left.definitions.size();
                     const std::size_t rightBenefit =
                         right.materializationCost * right.definitions.size();
                     if (leftBenefit != rightBenefit)
                       return leftBenefit > rightBenefit;
                     if (left.immediate != right.immediate)
                       return left.immediate < right.immediate;
                     return left.regClass < right.regClass;
                   });
  const std::size_t limit = std::min(kMaxHoistedImmediatesPerLoop,
                                     pressureBudget);
  if (candidates.size() > limit)
    candidates.resize(limit);

  if (std::getenv("BACKEND_LI_HOIST_STATS") != nullptr) {
    std::cerr << "li-hoist " << function.name << " header=" << header
              << " depth=" << loopDepth << " max-live=" << maxGPRLive
              << " budget=" << pressureBudget
              << " selected=" << candidates.size() << '\n';
  }

  std::map<uint32_t, MachineReg> replacements;
  std::map<CandidateKey, MachineReg> cachedByKey;
  std::vector<MachineInstr> hoisted;
  for (Candidate &candidate : candidates) {
    MachineReg cached = function.createVReg(candidate.regClass);
    cachedByKey.emplace(
        CandidateKey{candidate.immediate, candidate.regClass}, cached);
    MachineInstr materialization = candidate.exemplar;
    materialization.defs.front() = MachineOperand::reg(cached);
    materialization.comment = kHoistedComment;
    hoisted.push_back(std::move(materialization));
    for (uint32_t oldDefinition : candidate.definitions)
      replacements.emplace(oldDefinition, cached);
  }

  for (MachineBasicBlock &block : function.blocks) {
    for (MachineInstr &instruction : block.instrs) {
      for (MachineOperand &use : instruction.uses) {
        if (!isVirtualRegister(use))
          continue;
        auto replacement = replacements.find(use.regValue.id);
        if (replacement != replacements.end())
          use.regValue = replacement->second;
      }
    }
  }

  for (MachineBasicBlock &block : function.blocks) {
    block.instrs.erase(
        std::remove_if(block.instrs.begin(), block.instrs.end(),
                       [&](const MachineInstr &instruction) {
                         return instruction.opcode == Opcode::LI &&
                                instruction.defs.size() == 1 &&
                                isVirtualRegister(instruction.defs.front()) &&
                                replacements.count(
                                    instruction.defs.front().regValue.id) != 0;
                       }),
        block.instrs.end());
  }

  if (headerCompare) {
    auto cached = cachedByKey.find(
        CandidateKey{headerCompare->immediate, headerCompare->regClass});
    if (cached == cachedByKey.end())
      return false;
    MachineBasicBlock &headerBlock =
        function.blocks[static_cast<std::size_t>(header)];
    // Re-read the source after ordinary LI replacement so a value shared by
    // both optimizations is reflected in the direct comparison too.
    const MachineInstr &xori = headerBlock.instrs[0];
    const MachineInstr &zeroBranch = headerBlock.instrs[1];
    MachineInstr directBranch = zeroBranch;
    directBranch.opcode = headerCompare->directOpcode;
    directBranch.uses = {xori.uses[0], MachineOperand::reg(cached->second),
                         zeroBranch.uses[1]};
    headerBlock.instrs.erase(headerBlock.instrs.begin());
    headerBlock.instrs.front() = std::move(directBranch);
  }

  MachineBasicBlock &insertionBlock =
      function.blocks[static_cast<std::size_t>(preheader)];
  auto insertion = std::find_if(
      insertionBlock.instrs.begin(), insertionBlock.instrs.end(),
      [](const MachineInstr &instruction) { return instruction.terminator; });
  insertionBlock.instrs.insert(insertion,
                               std::make_move_iterator(hoisted.begin()),
                               std::make_move_iterator(hoisted.end()));
  return true;
}

} // namespace

bool runPreRALoopInvariantImmediateHoist(
    MachineFunction &function, MachineFunctionAnalysisManager &am) {
  if (function.blocks.empty())
    return false;
  const auto &cfg = am.getResult<MachineCFGAnalysis>(function);
  const auto &loops = am.getResult<MachineLoopAnalysis>(function);
  if (loops.backedges.empty())
    return false;

  const DominatorSets dominators = computeDominators(cfg);
  std::map<int, std::set<int>> membersByHeader;
  std::map<int, std::set<int>> latchesByHeader;
  for (const auto &[latch, header] : loops.backedges) {
    if (header < 0 || latch < 0 ||
        header >= static_cast<int>(function.blocks.size()) ||
        latch >= static_cast<int>(function.blocks.size())) {
      continue;
    }
    std::set<int> members = naturalLoopMembers(header, latch, cfg);
    for (auto member = members.begin(); member != members.end();) {
      if (*member < 0 || static_cast<std::size_t>(*member) >= dominators.size() ||
          dominators[static_cast<std::size_t>(*member)].count(header) == 0) {
        member = members.erase(member);
      } else {
        ++member;
      }
    }
    membersByHeader[header].insert(members.begin(), members.end());
    latchesByHeader[header].insert(latch);
  }

  std::vector<int> headers;
  for (const auto &[header, members] : membersByHeader) {
    (void)members;
    headers.push_back(header);
  }
  std::stable_sort(headers.begin(), headers.end(), [&](int left, int right) {
    const int leftDepth = loops.depth[static_cast<std::size_t>(left)];
    const int rightDepth = loops.depth[static_cast<std::size_t>(right)];
    return leftDepth != rightDepth ? leftDepth < rightDepth : left < right;
  });

  const auto defs = definitionCounts(function);
  const auto uses = useCounts(function);
  const auto useBlockMap = useBlocks(function);
  bool changed = false;
  for (int header : headers) {
    const std::set<int> &members = membersByHeader.at(header);
    std::vector<int> outsidePredecessors;
    for (int predecessor : cfg.preds[static_cast<std::size_t>(header)]) {
      if (members.count(predecessor) == 0)
        outsidePredecessors.push_back(predecessor);
    }
    if (outsidePredecessors.size() != 1)
      continue;
    const int preheader = outsidePredecessors.front();
    if (preheader < 0 ||
        static_cast<std::size_t>(preheader) >= cfg.succs.size() ||
        cfg.succs[static_cast<std::size_t>(preheader)].size() != 1 ||
        cfg.succs[static_cast<std::size_t>(preheader)].front() != header) {
      continue;
    }
    // Earlier outer-loop hoists create new values whose live ranges can cross
    // a nested loop.  Recompute liveness and the register-class map before
    // every profitability decision so the next loop sees that added pressure.
    MachineFunctionAnalysisManager localAM;
    localAM.registerPass([] { return MachineCFGAnalysis(); });
    localAM.registerPass([] { return MachineLivenessAnalysis(); });
    const auto &currentLiveness =
        localAM.getResult<MachineLivenessAnalysis>(function);
    const auto currentClasses = virtualRegisterClasses(function);
    changed |= hoistForLoop(
        function, header, members, latchesByHeader.at(header), preheader,
        cfg, dominators, loops, currentLiveness, currentClasses, defs, uses,
        useBlockMap);
  }
  return changed;
}

} // namespace riscv
