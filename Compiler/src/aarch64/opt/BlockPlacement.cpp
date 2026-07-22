#include "aarch64/opt/MachineOptPasses.hpp"

#include "aarch64/Passes.hpp"
#include "aarch64/Target.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace aarch64 {
namespace {

enum class ControlKind { Return, Unconditional, Conditional };

struct SavedControl {
  ControlKind kind = ControlKind::Return;
  std::size_t originalIndex = 0;
  MachineInstr terminator;
  std::string trueTarget;
  std::string falseTarget;
};

struct WeightedEdge {
  int from = -1;
  int to = -1;
  uint64_t weight = 0;
};

struct LayoutCost {
  uint64_t dynamicJumpExecutions = 0;
  uint64_t staticJumpCount = 0;
  uint64_t brokenFallthroughWeight = 0;
  uint64_t blockDisplacement = 0;

  bool operator<(const LayoutCost &other) const {
    if (dynamicJumpExecutions != other.dynamicJumpExecutions) {
      return dynamicJumpExecutions < other.dynamicJumpExecutions;
    }
    if (staticJumpCount != other.staticJumpCount) {
      return staticJumpCount < other.staticJumpCount;
    }
    if (brokenFallthroughWeight != other.brokenFallthroughWeight) {
      return brokenFallthroughWeight < other.brokenFallthroughWeight;
    }
    return blockDisplacement < other.blockDisplacement;
  }
};

using LabelEdge = std::pair<std::string, std::string>;
using LabelEdgeSet = std::set<LabelEdge>;

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

bool isKnownTerminator(Opcode opcode) {
  return opcode == Opcode::RET || opcode == Opcode::J ||
         isConditionalBranch(opcode);
}

bool invertedBranch(Opcode opcode, Opcode &inverted) {
  switch (opcode) {
  case Opcode::BEQZ:
    inverted = Opcode::BNEZ;
    return true;
  case Opcode::BNEZ:
    inverted = Opcode::BEQZ;
    return true;
  case Opcode::BEQ:
    inverted = Opcode::BNE;
    return true;
  case Opcode::BNE:
    inverted = Opcode::BEQ;
    return true;
  case Opcode::BLT:
    inverted = Opcode::BGE;
    return true;
  case Opcode::BGE:
    inverted = Opcode::BLT;
    return true;
  default:
    return false;
  }
}

bool verificationEnabled() {
#ifndef NDEBUG
  return true;
#else
  return std::getenv("BACKEND_VERIFY_MIR") != nullptr;
#endif
}

bool statisticsEnabled() {
  return std::getenv("BACKEND_BLOCK_PLACEMENT_STATS") != nullptr;
}

bool sameReg(const MachineReg &lhs, const MachineReg &rhs) {
  return lhs.physical == rhs.physical && lhs.id == rhs.id && lhs.rc == rhs.rc &&
         lhs.phys == rhs.phys;
}

bool sameOperand(const MachineOperand &lhs, const MachineOperand &rhs) {
  if (lhs.kind != rhs.kind) {
    return false;
  }
  switch (lhs.kind) {
  case OperandKind::Reg:
    return sameReg(lhs.regValue, rhs.regValue);
  case OperandKind::Imm:
    return lhs.immValue == rhs.immValue;
  case OperandKind::FrameIndex:
    return lhs.frameIndexValue == rhs.frameIndexValue;
  case OperandKind::Global:
    return lhs.symbol == rhs.symbol;
  case OperandKind::Block:
    return lhs.blockLabel == rhs.blockLabel;
  case OperandKind::ValueRef:
    return lhs.valueRef == rhs.valueRef;
  }
  return false;
}

bool sameOperandList(const std::vector<MachineOperand> &lhs,
                     const std::vector<MachineOperand> &rhs,
                     bool ignoreBlockTargets) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    if (ignoreBlockTargets && lhs[i].kind == OperandKind::Block &&
        rhs[i].kind == OperandKind::Block) {
      continue;
    }
    if (!sameOperand(lhs[i], rhs[i])) {
      return false;
    }
  }
  return true;
}

bool sameInstruction(const MachineInstr &lhs, const MachineInstr &rhs,
                     bool ignoreOpcodeAndBlockTargets = false) {
  return (ignoreOpcodeAndBlockTargets || lhs.opcode == rhs.opcode) &&
         sameOperandList(lhs.defs, rhs.defs, false) &&
         sameOperandList(lhs.uses, rhs.uses, ignoreOpcodeAndBlockTargets) &&
         lhs.implicitDefs == rhs.implicitDefs &&
         lhs.implicitUses == rhs.implicitUses &&
         lhs.sideEffects == rhs.sideEffects &&
         lhs.terminator == rhs.terminator && lhs.comment == rhs.comment;
}

bool sameProgram(const std::vector<MachineBasicBlock> &lhs,
                 const std::vector<MachineBasicBlock> &rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t block = 0; block < lhs.size(); ++block) {
    if (lhs[block].label != rhs[block].label ||
        lhs[block].instrs.size() != rhs[block].instrs.size()) {
      return false;
    }
    for (std::size_t instr = 0; instr < lhs[block].instrs.size(); ++instr) {
      if (!sameInstruction(lhs[block].instrs[instr],
                           rhs[block].instrs[instr])) {
        return false;
      }
    }
  }
  return true;
}

uint64_t saturatingAdd(uint64_t lhs, uint64_t rhs) {
  const uint64_t limit = std::numeric_limits<uint64_t>::max();
  return rhs > limit - lhs ? limit : lhs + rhs;
}

std::string branchTarget(const MachineInstr &instr) {
  std::string target;
  for (const MachineOperand &use : instr.uses) {
    if (use.kind != OperandKind::Block) {
      continue;
    }
    if (!target.empty()) {
      throw BackendError("machine branch has multiple block targets");
    }
    target = use.blockLabel;
  }
  if (target.empty()) {
    throw BackendError("machine branch has no block target");
  }
  return target;
}

uint64_t edgeWeightForTarget(
    const MachineBlockFrequencyAnalysis::Result &frequencies, std::size_t from,
    std::size_t to) {
  if (from >= frequencies.successors.size() ||
      from >= frequencies.edgeWeight.size()) {
    throw BackendError("stale machine frequency edge lookup");
  }
  const auto &successors = frequencies.successors[from];
  const auto &weights = frequencies.edgeWeight[from];
  if (successors.size() != weights.size()) {
    throw BackendError("malformed machine frequency edge table");
  }
  for (std::size_t index = 0; index < successors.size(); ++index) {
    if (successors[index] == static_cast<int>(to)) {
      return weights[index];
    }
  }
  throw BackendError("missing machine frequency edge");
}

void setBranchTarget(MachineInstr &instr, const std::string &target) {
  bool replaced = false;
  for (MachineOperand &use : instr.uses) {
    if (use.kind != OperandKind::Block) {
      continue;
    }
    if (replaced) {
      throw BackendError("machine branch has multiple block targets");
    }
    use.blockLabel = target;
    replaced = true;
  }
  if (!replaced) {
    throw BackendError("machine branch has no block target");
  }
}

void appendJump(MachineBasicBlock &block, const std::string &label) {
  MachineInstr jump;
  jump.opcode = Opcode::J;
  jump.uses.push_back(MachineOperand::block(label));
  jump.terminator = true;
  block.instrs.push_back(std::move(jump));
}

bool saveControlFlow(const MachineFunction &mf,
                     std::map<std::string, SavedControl> &controls,
                     LabelEdgeSet &edges) {
  std::set<std::string> labels;
  for (const MachineBasicBlock &block : mf.blocks) {
    if (block.label.empty() || !labels.insert(block.label).second) {
      throw BackendError("duplicate or empty machine block label");
    }
  }

  auto validateTarget = [&](const std::string &target) {
    if (labels.count(target) == 0) {
      throw BackendError("machine branch targets unknown block " + target);
    }
  };

  for (std::size_t index = 0; index < mf.blocks.size(); ++index) {
    const MachineBasicBlock &block = mf.blocks[index];
    std::vector<const MachineInstr *> terminators;
    bool sawTerminator = false;
    for (const MachineInstr &instr : block.instrs) {
      const bool terminator =
          instr.terminator || isKnownTerminator(instr.opcode);
      if (!terminator) {
        if (sawTerminator) {
          return false;
        }
        continue;
      }
      sawTerminator = true;
      if (!isKnownTerminator(instr.opcode)) {
        // 未知条件码或未知 terminator 没有可靠的反转/重建规则。
        return false;
      }
      terminators.push_back(&instr);
    }

    SavedControl saved;
    saved.originalIndex = index;
    if (terminators.empty()) {
      if (index + 1 >= mf.blocks.size()) {
        // MIR 没有表达“无 successor 但也不 RET”的终结形式。
        return false;
      }
      saved.kind = ControlKind::Unconditional;
      saved.trueTarget = mf.blocks[index + 1].label;
    } else if (terminators.size() == 1) {
      const MachineInstr &term = *terminators.front();
      if (term.opcode == Opcode::RET) {
        for (const MachineOperand &use : term.uses) {
          if (use.kind == OperandKind::Block) {
            throw BackendError("RET must not have a machine successor");
          }
        }
        saved.kind = ControlKind::Return;
        saved.terminator = term;
      } else if (term.opcode == Opcode::J) {
        saved.kind = ControlKind::Unconditional;
        saved.trueTarget = branchTarget(term);
      } else if (isConditionalBranch(term.opcode)) {
        if (index + 1 >= mf.blocks.size()) {
          return false;
        }
        Opcode ignored = Opcode::NOP;
        if (!invertedBranch(term.opcode, ignored)) {
          return false;
        }
        saved.kind = ControlKind::Conditional;
        saved.terminator = term;
        saved.trueTarget = branchTarget(term);
        // 关键：在移动前固化旧布局产生的 false/fallthrough successor。
        saved.falseTarget = mf.blocks[index + 1].label;
      } else {
        return false;
      }
    } else if (terminators.size() == 2 &&
               isConditionalBranch(terminators[0]->opcode) &&
               terminators[1]->opcode == Opcode::J) {
      Opcode ignored = Opcode::NOP;
      if (!invertedBranch(terminators[0]->opcode, ignored)) {
        return false;
      }
      saved.kind = ControlKind::Conditional;
      saved.terminator = *terminators[0];
      saved.trueTarget = branchTarget(*terminators[0]);
      saved.falseTarget = branchTarget(*terminators[1]);
    } else {
      return false;
    }

    if (saved.kind != ControlKind::Return) {
      validateTarget(saved.trueTarget);
      edges.emplace(block.label, saved.trueTarget);
    }
    if (saved.kind == ControlKind::Conditional) {
      validateTarget(saved.falseTarget);
      edges.emplace(block.label, saved.falseTarget);
    }
    controls.emplace(block.label, std::move(saved));
  }
  return true;
}

std::vector<int> buildPettisHansenOrder(
    std::size_t blockCount,
    const MachineLoopAnalysis::Result &loops,
    const MachineBlockFrequencyAnalysis::Result &frequencies) {
  if (frequencies.blockFrequency.size() != blockCount ||
      frequencies.successors.size() != blockCount ||
      frequencies.edgeWeight.size() != blockCount) {
    throw BackendError("stale block frequency analysis");
  }

  std::vector<WeightedEdge> edges;
  for (std::size_t from = 0; from < blockCount; ++from) {
    if (frequencies.successors[from].size() !=
        frequencies.edgeWeight[from].size()) {
      throw BackendError("malformed block frequency edge table");
    }
    for (std::size_t edge = 0; edge < frequencies.successors[from].size();
         ++edge) {
      const int to = frequencies.successors[from][edge];
      if (to < 0 || to >= static_cast<int>(blockCount)) {
        throw BackendError("invalid weighted CFG edge");
      }
      if (loops.backedges.count({static_cast<int>(from), to}) != 0) {
        continue;
      }
      edges.push_back(
          {static_cast<int>(from), to, frequencies.edgeWeight[from][edge]});
    }
  }
  std::sort(edges.begin(), edges.end(),
            [](const WeightedEdge &lhs, const WeightedEdge &rhs) {
              if (lhs.weight != rhs.weight) {
                return lhs.weight > rhs.weight;
              }
              if (lhs.from != rhs.from) {
                return lhs.from < rhs.from;
              }
              return lhs.to < rhs.to;
            });

  std::vector<std::vector<int>> chains(blockCount);
  std::vector<int> owner(blockCount);
  std::vector<bool> alive(blockCount, true);
  for (std::size_t i = 0; i < blockCount; ++i) {
    chains[i].push_back(static_cast<int>(i));
    owner[i] = static_cast<int>(i);
  }

  for (const WeightedEdge &edge : edges) {
    const int fromChain = owner[static_cast<std::size_t>(edge.from)];
    const int toChain = owner[static_cast<std::size_t>(edge.to)];
    if (fromChain == toChain ||
        chains[static_cast<std::size_t>(fromChain)].back() != edge.from ||
        chains[static_cast<std::size_t>(toChain)].front() != edge.to) {
      continue;
    }
    // 入口 chain 不能被接到其他 chain 后面，否则入口块不再是第一个。
    if (toChain == owner[0]) {
      continue;
    }
    auto &destination = chains[static_cast<std::size_t>(fromChain)];
    auto &source = chains[static_cast<std::size_t>(toChain)];
    destination.insert(destination.end(), source.begin(), source.end());
    for (int block : source) {
      owner[static_cast<std::size_t>(block)] = fromChain;
    }
    alive[static_cast<std::size_t>(toChain)] = false;
  }

  std::vector<int> order;
  order.reserve(blockCount);
  std::vector<bool> placed(blockCount, false);
  auto appendChain = [&](int chain) {
    for (int block : chains[static_cast<std::size_t>(chain)]) {
      if (placed[static_cast<std::size_t>(block)]) {
        throw BackendError("machine block placed more than once");
      }
      placed[static_cast<std::size_t>(block)] = true;
      order.push_back(block);
    }
    alive[static_cast<std::size_t>(chain)] = false;
  };

  appendChain(owner[0]);
  while (order.size() < blockCount) {
    int best = -1;
    uint64_t bestConnection = 0;
    uint64_t bestHeat = 0;
    int bestOriginal = std::numeric_limits<int>::max();
    for (std::size_t chain = 0; chain < blockCount; ++chain) {
      if (!alive[chain]) {
        continue;
      }
      uint64_t connection = 0;
      uint64_t heat = 0;
      int original = std::numeric_limits<int>::max();
      for (int block : chains[chain]) {
        heat = saturatingAdd(
            heat, frequencies.blockFrequency[static_cast<std::size_t>(block)]);
        original = std::min(original, block);
      }
      for (const WeightedEdge &edge : edges) {
        const bool fromHere = owner[static_cast<std::size_t>(edge.from)] ==
                              static_cast<int>(chain);
        const bool toHere =
            owner[static_cast<std::size_t>(edge.to)] == static_cast<int>(chain);
        if ((fromHere && placed[static_cast<std::size_t>(edge.to)]) ||
            (toHere && placed[static_cast<std::size_t>(edge.from)])) {
          connection = saturatingAdd(connection, edge.weight);
        }
      }
      if (best < 0 || connection > bestConnection ||
          (connection == bestConnection && heat > bestHeat) ||
          (connection == bestConnection && heat == bestHeat &&
           original < bestOriginal)) {
        best = static_cast<int>(chain);
        bestConnection = connection;
        bestHeat = heat;
        bestOriginal = original;
      }
    }
    if (best < 0) {
      throw BackendError("failed to place every machine block");
    }
    appendChain(best);
  }
  if (order.empty() || order.front() != 0) {
    throw BackendError("machine block placement moved the entry block");
  }
  return order;
}

uint64_t
weightedFallthrough(const std::vector<int> &order,
                    const MachineBlockFrequencyAnalysis::Result &frequencies);

uint64_t
totalEdgeWeight(const MachineBlockFrequencyAnalysis::Result &frequencies);

LayoutCost computeLayoutCost(
    const MachineFunction &originalMf, const std::vector<int> &order,
    const std::map<std::string, SavedControl> &controls,
    const MachineBlockFrequencyAnalysis::Result &frequencies,
    bool useWeightedOrientation) {
  if (originalMf.blocks.size() != order.size()) {
    throw BackendError("stale machine block placement order");
  }

  LayoutCost cost;
  const uint64_t totalWeight = totalEdgeWeight(frequencies);
  uint64_t weighted = weightedFallthrough(order, frequencies);
  if (weighted > totalWeight) {
    weighted = totalWeight;
  }
  cost.brokenFallthroughWeight = totalWeight - weighted;

  for (std::size_t newPosition = 0; newPosition < order.size(); ++newPosition) {
    const int oldIndex = order[newPosition];
    if (oldIndex < 0 ||
        oldIndex >= static_cast<int>(originalMf.blocks.size())) {
      throw BackendError("invalid machine block placement order");
    }
    const MachineBasicBlock &block =
        originalMf.blocks[static_cast<std::size_t>(oldIndex)];
    const auto savedIt = controls.find(block.label);
    if (savedIt == controls.end()) {
      throw BackendError("lost machine block control-flow record");
    }
    const SavedControl &saved = savedIt->second;
    const std::string next =
        newPosition + 1 < order.size()
            ? originalMf.blocks[static_cast<std::size_t>(order[newPosition + 1])]
                  .label
            : std::string();
    cost.blockDisplacement = saturatingAdd(
        cost.blockDisplacement,
        static_cast<uint64_t>(std::llabs(static_cast<long long>(newPosition) -
                                         static_cast<long long>(saved.originalIndex))));

    switch (saved.kind) {
    case ControlKind::Return:
      break;
    case ControlKind::Unconditional:
      if (saved.trueTarget != next) {
        cost.dynamicJumpExecutions = saturatingAdd(
            cost.dynamicJumpExecutions,
            frequencies.blockFrequency[static_cast<std::size_t>(saved.originalIndex)]);
        ++cost.staticJumpCount;
      }
      break;
    case ControlKind::Conditional: {
      if (saved.trueTarget == next || saved.falseTarget == next) {
        break;
      }
      ++cost.staticJumpCount;
      const std::size_t from = saved.originalIndex;
      const std::size_t trueTarget =
          controls.at(saved.trueTarget).originalIndex;
      const std::size_t falseTarget =
          controls.at(saved.falseTarget).originalIndex;
      const uint64_t trueWeight =
          edgeWeightForTarget(frequencies, from, trueTarget);
      const uint64_t falseWeight =
          edgeWeightForTarget(frequencies, from, falseTarget);
      if (useWeightedOrientation) {
        cost.dynamicJumpExecutions = saturatingAdd(
            cost.dynamicJumpExecutions, std::min(trueWeight, falseWeight));
      } else {
        cost.dynamicJumpExecutions = saturatingAdd(
            cost.dynamicJumpExecutions, falseWeight);
      }
      break;
    }
    }
  }

  return cost;
}

void considerBackedgeFallthroughOrders(
    const MachineFunction &mf, const std::vector<int> &seed,
    const LayoutCost &seedCost, const MachineLoopAnalysis::Result &loops,
    const MachineBlockFrequencyAnalysis::Result &frequencies,
    const std::map<std::string, SavedControl> &controls,
    bool useWeightedOrientation, std::vector<int> &bestOrder,
    LayoutCost &bestCost) {
  if (!useWeightedOrientation ||
      std::getenv("BACKEND_DISABLE_BACKEDGE_FALLTHROUGH_PLACEMENT") !=
          nullptr) {
    return;
  }

  for (const auto &[latch, header] : loops.backedges) {
    if (latch < 0 || header <= 0 || latch == header ||
        latch >= static_cast<int>(mf.blocks.size()) ||
        header >= static_cast<int>(mf.blocks.size())) {
      continue;
    }

    const std::string &latchLabel =
        mf.blocks[static_cast<std::size_t>(latch)].label;
    const std::string &headerLabel =
        mf.blocks[static_cast<std::size_t>(header)].label;
    const auto saved = controls.find(latchLabel);
    if (saved == controls.end() ||
        saved->second.kind != ControlKind::Unconditional ||
        saved->second.trueTarget != headerLabel) {
      continue;
    }

    std::vector<int> trial = seed;
    const auto latchPosition = std::find(trial.begin(), trial.end(), latch);
    if (latchPosition == trial.end()) {
      throw BackendError("backedge latch missing from placement order");
    }
    trial.erase(latchPosition);
    const auto headerPosition = std::find(trial.begin(), trial.end(), header);
    if (headerPosition == trial.end()) {
      throw BackendError("backedge header missing from placement order");
    }
    trial.insert(headerPosition, latch);

    const LayoutCost trialCost = computeLayoutCost(
        mf, trial, controls, frequencies, useWeightedOrientation);
    // A loop rotation changes the branch direction and code adjacency.  Only
    // accept it for the primary objective: replacing a more frequently
    // executed explicit jump with a cheaper one.  Static-size or displacement
    // tie breakers alone are not sufficient evidence for rotating a loop.
    if (trialCost.dynamicJumpExecutions >=
        seedCost.dynamicJumpExecutions) {
      continue;
    }
    if (trialCost < bestCost) {
      bestOrder = std::move(trial);
      bestCost = trialCost;
    }
  }
}

uint64_t
weightedFallthrough(const std::vector<int> &order,
                    const MachineBlockFrequencyAnalysis::Result &frequencies) {
  std::vector<int> position(order.size(), -1);
  for (std::size_t i = 0; i < order.size(); ++i) {
    position[static_cast<std::size_t>(order[i])] = static_cast<int>(i);
  }
  uint64_t weight = 0;
  for (std::size_t from = 0; from < frequencies.successors.size(); ++from) {
    for (std::size_t edge = 0; edge < frequencies.successors[from].size();
         ++edge) {
      const int to = frequencies.successors[from][edge];
      if (position[from] + 1 == position[static_cast<std::size_t>(to)]) {
        weight = saturatingAdd(weight, frequencies.edgeWeight[from][edge]);
      }
    }
  }
  return weight;
}

uint64_t
totalEdgeWeight(const MachineBlockFrequencyAnalysis::Result &frequencies) {
  uint64_t result = 0;
  for (const auto &blockEdges : frequencies.edgeWeight) {
    for (uint64_t weight : blockEdges) {
      result = saturatingAdd(result, weight);
    }
  }
  return result;
}

void eraseTerminators(MachineBasicBlock &block) {
  block.instrs.erase(std::remove_if(block.instrs.begin(), block.instrs.end(),
                                    [](const MachineInstr &instr) {
                                      return instr.terminator ||
                                             isKnownTerminator(instr.opcode);
                                    }),
                     block.instrs.end());
}

void rebuildTerminators(
    MachineFunction &mf, const std::map<std::string, SavedControl> &controls,
    const MachineBlockFrequencyAnalysis::Result &frequencies,
    bool useWeightedOrientation) {
  for (std::size_t index = 0; index < mf.blocks.size(); ++index) {
    MachineBasicBlock &block = mf.blocks[index];
    const auto savedIt = controls.find(block.label);
    if (savedIt == controls.end()) {
      throw BackendError("lost machine block control-flow record");
    }
    const SavedControl &saved = savedIt->second;
    const std::string next = index + 1 < mf.blocks.size()
                                 ? mf.blocks[index + 1].label
                                 : std::string();
    eraseTerminators(block);

    if (saved.kind == ControlKind::Return) {
      block.instrs.push_back(saved.terminator);
      continue;
    }
    if (saved.kind == ControlKind::Unconditional) {
      if (saved.trueTarget != next) {
        appendJump(block, saved.trueTarget);
      }
      continue;
    }

    MachineInstr conditional = saved.terminator;
    if (saved.falseTarget == next) {
      setBranchTarget(conditional, saved.trueTarget);
      block.instrs.push_back(std::move(conditional));
      continue;
    }
    if (saved.trueTarget == next) {
      Opcode inverted = Opcode::NOP;
      if (!invertedBranch(conditional.opcode, inverted)) {
        throw BackendError("cannot invert unknown machine condition opcode");
      }
      conditional.opcode = inverted;
      setBranchTarget(conditional, saved.falseTarget);
      block.instrs.push_back(std::move(conditional));
      continue;
    }

    if (useWeightedOrientation) {
      const std::size_t from = saved.originalIndex;
      const std::size_t trueTarget = controls.at(saved.trueTarget).originalIndex;
      const std::size_t falseTarget =
          controls.at(saved.falseTarget).originalIndex;
      const uint64_t trueWeight =
          edgeWeightForTarget(frequencies, from, trueTarget);
      const uint64_t falseWeight =
          edgeWeightForTarget(frequencies, from, falseTarget);
      if (falseWeight > trueWeight) {
        Opcode inverted = Opcode::NOP;
        if (!invertedBranch(conditional.opcode, inverted)) {
          throw BackendError("cannot invert unknown machine condition opcode");
        }
        conditional.opcode = inverted;
        setBranchTarget(conditional, saved.falseTarget);
        block.instrs.push_back(std::move(conditional));
        appendJump(block, saved.trueTarget);
        continue;
      }
    }

    setBranchTarget(conditional, saved.trueTarget);
    block.instrs.push_back(std::move(conditional));
    appendJump(block, saved.falseTarget);
  }
}

LabelEdgeSet cfgLabelEdges(const MachineFunction &mf) {
  LabelEdgeSet result;
  for (const MachineBasicBlock &block : mf.blocks) {
    for (int succ : block.succs) {
      if (succ < 0 || succ >= static_cast<int>(mf.blocks.size())) {
        throw BackendError("invalid CFG successor after block placement");
      }
      result.emplace(block.label,
                     mf.blocks[static_cast<std::size_t>(succ)].label);
    }
  }
  return result;
}

void verifyPlacedFunction(const MachineFunction &mf,
                          const std::map<std::string, SavedControl> &controls,
                          const LabelEdgeSet &oldEdges) {
  if (mf.blocks.empty()) {
    return;
  }
  std::set<std::string> labels;
  for (const MachineBasicBlock &block : mf.blocks) {
    if (!labels.insert(block.label).second) {
      throw BackendError("machine block duplicated by block placement");
    }
    const auto saved = controls.find(block.label);
    if (saved == controls.end()) {
      throw BackendError("machine block added by block placement");
    }
    if (saved->second.kind == ControlKind::Return && !block.succs.empty()) {
      throw BackendError("RET acquired a successor after block placement");
    }
  }
  if (labels.size() != controls.size()) {
    throw BackendError("machine block lost by block placement");
  }
  if (cfgLabelEdges(mf) != oldEdges) {
    throw BackendError("logical CFG edges changed during block placement");
  }

  std::map<std::string, SavedControl> rebuiltControls;
  LabelEdgeSet rebuiltEdges;
  if (!saveControlFlow(mf, rebuiltControls, rebuiltEdges)) {
    throw BackendError("cannot recover control flow after block placement");
  }
  for (const auto &[label, oldControl] : controls) {
    const auto rebuilt = rebuiltControls.find(label);
    if (rebuilt == rebuiltControls.end() ||
        rebuilt->second.kind != oldControl.kind) {
      throw BackendError("terminator kind changed during block placement");
    }
    const SavedControl &newControl = rebuilt->second;
    if (oldControl.kind == ControlKind::Return) {
      if (!sameInstruction(oldControl.terminator, newControl.terminator)) {
        throw BackendError("RET operands changed during block placement");
      }
      continue;
    }
    if (oldControl.kind == ControlKind::Unconditional) {
      if (oldControl.trueTarget != newControl.trueTarget) {
        throw BackendError(
            "unconditional target changed during block placement");
      }
      continue;
    }

    if (!sameInstruction(oldControl.terminator, newControl.terminator, true)) {
      throw BackendError("conditional operands changed during block placement");
    }
    const bool sameDirection =
        oldControl.terminator.opcode == newControl.terminator.opcode &&
        oldControl.trueTarget == newControl.trueTarget &&
        oldControl.falseTarget == newControl.falseTarget;
    Opcode inverse = Opcode::NOP;
    const bool reversedDirection =
        invertedBranch(oldControl.terminator.opcode, inverse) &&
        inverse == newControl.terminator.opcode &&
        oldControl.trueTarget == newControl.falseTarget &&
        oldControl.falseTarget == newControl.trueTarget;
    if (!sameDirection && !reversedDirection) {
      throw BackendError(
          "conditional true/false semantics changed during block placement");
    }
  }
}

} // namespace

bool runBlockPlacement(
    MachineFunction &mf, const MachineLoopAnalysis::Result &loops,
    const MachineBlockFrequencyAnalysis::Result &frequencies) {
  if (mf.blocks.empty()) {
    return false;
  }
  if (loops.depth.size() != mf.blocks.size()) {
    throw BackendError("stale machine loop analysis");
  }
  std::map<std::string, SavedControl> controls;
  LabelEdgeSet oldEdges;
  if (!saveControlFlow(mf, controls, oldEdges)) {
    return false;
  }

  const std::string entryLabel = mf.blocks.front().label;
  std::vector<int> originalOrder(mf.blocks.size());
  for (std::size_t index = 0; index < originalOrder.size(); ++index) {
    originalOrder[index] = static_cast<int>(index);
  }
  std::vector<int> candidateOrder = originalOrder;
  const bool useWeightedOrientation = frequencies.reliable;
  if (useWeightedOrientation) {
    candidateOrder = buildPettisHansenOrder(mf.blocks.size(), loops, frequencies);
  }

  const LayoutCost originalCost =
      computeLayoutCost(mf, originalOrder, controls, frequencies,
                        useWeightedOrientation);
  LayoutCost candidateCost = useWeightedOrientation
                                 ? computeLayoutCost(mf, candidateOrder,
                                                     controls, frequencies,
                                                     useWeightedOrientation)
                                 : originalCost;
  if (useWeightedOrientation) {
    const std::vector<int> pettisHansenOrder = candidateOrder;
    const LayoutCost pettisHansenCost = candidateCost;
    considerBackedgeFallthroughOrders(
        mf, originalOrder, originalCost, loops, frequencies, controls,
        useWeightedOrientation, candidateOrder, candidateCost);
    considerBackedgeFallthroughOrders(
        mf, pettisHansenOrder, pettisHansenCost, loops, frequencies, controls,
        useWeightedOrientation, candidateOrder, candidateCost);
  }

  const std::vector<int> *selectedOrder = &originalOrder;
  const LayoutCost *selectedCost = &originalCost;
  if (useWeightedOrientation && candidateCost < originalCost) {
    selectedOrder = &candidateOrder;
    selectedCost = &candidateCost;
  }

  if (statisticsEnabled()) {
    std::cerr << "# block-placement-stats blocks=" << mf.blocks.size()
              << " reliable=" << (frequencies.reliable ? 1 : 0)
              << " converged=" << (frequencies.converged ? 1 : 0)
              << " edge-weight=" << totalEdgeWeight(frequencies)
              << " weighted-fallthrough-before="
              << weightedFallthrough(originalOrder, frequencies)
              << " weighted-fallthrough-candidate="
              << weightedFallthrough(candidateOrder, frequencies)
              << " weighted-fallthrough-after="
              << weightedFallthrough(*selectedOrder, frequencies)
              << " dynamic-j-before=" << originalCost.dynamicJumpExecutions
              << " dynamic-j-candidate="
              << candidateCost.dynamicJumpExecutions
              << " dynamic-j-after=" << selectedCost->dynamicJumpExecutions
              << " static-j-before=" << originalCost.staticJumpCount
              << " static-j-candidate=" << candidateCost.staticJumpCount
              << " static-j-after=" << selectedCost->staticJumpCount
              << " broken-fallthrough-before="
              << originalCost.brokenFallthroughWeight
              << " broken-fallthrough-candidate="
              << candidateCost.brokenFallthroughWeight
              << " broken-fallthrough-after="
              << selectedCost->brokenFallthroughWeight
              << '\n';
  }

  MachineFunction candidate;
  candidate.blocks = mf.blocks;
  std::vector<MachineBasicBlock> placed;
  placed.reserve(candidate.blocks.size());
  std::vector<bool> moved(candidate.blocks.size(), false);
  for (int oldIndex : *selectedOrder) {
    if (oldIndex < 0 || oldIndex >= static_cast<int>(candidate.blocks.size()) ||
        moved[static_cast<std::size_t>(oldIndex)]) {
      throw BackendError("invalid machine block placement order");
    }
    moved[static_cast<std::size_t>(oldIndex)] = true;
    placed.push_back(
        std::move(candidate.blocks[static_cast<std::size_t>(oldIndex)]));
  }
  candidate.blocks = std::move(placed);

  // 从这里起不得再使用旧 block index、旧块引用或分析中的下标缓存。
  if (candidate.blocks.size() != controls.size() ||
      candidate.blocks.front().label != entryLabel) {
    throw BackendError("invalid result from machine block placement");
  }
  rebuildTerminators(candidate, controls, frequencies, useWeightedOrientation);
  rebuildCFG(candidate);
  if (verificationEnabled()) {
    verifyPlacedFunction(candidate, controls, oldEdges);
  }
  if (sameProgram(mf.blocks, candidate.blocks)) {
    return false;
  }
  mf.blocks = std::move(candidate.blocks);
  return true;
}

} // namespace aarch64
