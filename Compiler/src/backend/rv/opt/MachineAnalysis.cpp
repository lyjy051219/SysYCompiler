#include "riscv/opt/MachineAnalysis.hpp"

#include "riscv/Target.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace riscv {
namespace {

// 与 Passes.cpp 中保持一致的终结指令判断
bool isTerminatorOpcode(Opcode opcode) {
  switch (opcode) {
  case Opcode::RET:
  case Opcode::BEQZ:
  case Opcode::BNEZ:
  case Opcode::BEQ:
  case Opcode::BNE:
  case Opcode::BLT:
  case Opcode::BGE:
  case Opcode::J:
    return true;
  default:
    return false;
  }
}

bool isNoFallthroughOpcode(Opcode opcode) {
  return opcode == Opcode::RET || opcode == Opcode::J;
}

bool blockCanFallThrough(const MachineBasicBlock &block) {
  for (auto it = block.instrs.rbegin(); it != block.instrs.rend(); ++it) {
    if (it->terminator || isTerminatorOpcode(it->opcode)) {
      return !isNoFallthroughOpcode(it->opcode);
    }
  }
  return true;
}

void addEdge(MachineCFGAnalysis::Result &result, int from, int to) {
  auto &succList = result.succs[static_cast<std::size_t>(from)];
  if (std::find(succList.begin(), succList.end(), to) == succList.end()) {
    succList.push_back(to);
  }
  auto &predList = result.preds[static_cast<std::size_t>(to)];
  if (std::find(predList.begin(), predList.end(), from) == predList.end()) {
    predList.push_back(from);
  }
}

bool isVirtualReg(const MachineOperand &operand) {
  return operand.kind == OperandKind::Reg && !operand.regValue.physical;
}

std::vector<uint32_t> virtualUses(const MachineInstr &instr) {
  std::vector<uint32_t> result;
  for (const MachineOperand &operand : instr.uses) {
    if (isVirtualReg(operand)) {
      result.push_back(operand.regValue.id);
    }
  }
  return result;
}

std::vector<uint32_t> virtualDefs(const MachineInstr &instr) {
  std::vector<uint32_t> result;
  for (const MachineOperand &operand : instr.defs) {
    if (isVirtualReg(operand)) {
      result.push_back(operand.regValue.id);
    }
  }
  return result;
}

} // namespace

MachinePassID MachineInstrStatsAnalysis::Key;
MachinePassID MachineCFGAnalysis::Key;
MachinePassID MachineLivenessAnalysis::Key;
MachinePassID MachineLoopAnalysis::Key;
MachinePassID MachineBlockFrequencyAnalysis::Key;

MachineInstrStatsAnalysis::Result
MachineInstrStatsAnalysis::run(MachineFunction &mf,
                               MachineFunctionAnalysisManager &am) const {
  (void)am;
  Result result;
  result.blockCount = mf.blocks.size();
  for (const MachineBasicBlock &block : mf.blocks) {
    result.instrCount += block.instrs.size();
    for (const MachineInstr &instr : block.instrs) {
      switch (instr.opcode) {
      case Opcode::COPY:
      case Opcode::F_COPY:
      case Opcode::PARALLEL_COPY:
        ++result.copyCount;
        break;
      case Opcode::CALL:
        ++result.callCount;
        break;
      case Opcode::PHI:
        ++result.phiCount;
        break;
      default:
        break;
      }
    }
  }
  return result;
}

MachineCFGAnalysis::Result
MachineCFGAnalysis::run(MachineFunction &mf,
                        MachineFunctionAnalysisManager &am) const {
  (void)am;
  Result result;
  const std::size_t blockCount = mf.blocks.size();
  result.preds.resize(blockCount);
  result.succs.resize(blockCount);

  // 标签到块下标的映射，避免逐块线性查找
  std::unordered_map<std::string, int> labelToIndex;
  for (std::size_t i = 0; i < blockCount; ++i) {
    labelToIndex[mf.blocks[i].label] = static_cast<int>(i);
  }

  for (std::size_t i = 0; i < blockCount; ++i) {
    for (const MachineInstr &instr : mf.blocks[i].instrs) {
      if (!instr.terminator && !isTerminatorOpcode(instr.opcode)) {
        continue;
      }
      for (const MachineOperand &use : instr.uses) {
        if (use.kind != OperandKind::Block) {
          continue;
        }
        auto it = labelToIndex.find(use.blockLabel);
        if (it == labelToIndex.end()) {
          throw BackendError("machine branch targets unknown block " +
                             use.blockLabel);
        }
        const int from = static_cast<int>(i);
        const int to = it->second;
        addEdge(result, from, to);
      }
    }
    if (blockCanFallThrough(mf.blocks[i]) && i + 1 < blockCount) {
      addEdge(result, static_cast<int>(i), static_cast<int>(i + 1));
    }
  }
  return result;
}

MachineLivenessAnalysis::Result
MachineLivenessAnalysis::run(MachineFunction &mf,
                             MachineFunctionAnalysisManager &am) const {
  Result live;
  const auto &cfg = am.getResult<MachineCFGAnalysis>(mf);
  const std::size_t blockCount = mf.blocks.size();
  live.uses.resize(blockCount);
  live.defs.resize(blockCount);
  live.liveIn.resize(blockCount);
  live.liveOut.resize(blockCount);

  for (std::size_t blockIndex = 0; blockIndex < blockCount; ++blockIndex) {
    for (const MachineInstr &instr : mf.blocks[blockIndex].instrs) {
      for (uint32_t use : virtualUses(instr)) {
        if (live.defs[blockIndex].count(use) == 0) {
          live.uses[blockIndex].insert(use);
        }
      }
      for (uint32_t def : virtualDefs(instr)) {
        live.defs[blockIndex].insert(def);
      }
    }
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t reverse = 0; reverse < blockCount; ++reverse) {
      const std::size_t blockIndex = blockCount - reverse - 1;
      RegSet newOut;
      for (int succ : cfg.succs[blockIndex]) {
        if (succ < 0 || succ >= static_cast<int>(blockCount)) {
          throw BackendError("invalid CFG successor during liveness analysis");
        }
        const RegSet &succIn = live.liveIn[static_cast<std::size_t>(succ)];
        newOut.insert(succIn.begin(), succIn.end());
      }

      RegSet newIn = live.uses[blockIndex];
      for (uint32_t value : newOut) {
        if (live.defs[blockIndex].count(value) == 0) {
          newIn.insert(value);
        }
      }
      if (newIn != live.liveIn[blockIndex] ||
          newOut != live.liveOut[blockIndex]) {
        live.liveIn[blockIndex] = std::move(newIn);
        live.liveOut[blockIndex] = std::move(newOut);
        changed = true;
      }
    }
  }
  return live;
}

MachineLoopAnalysis::Result
MachineLoopAnalysis::run(MachineFunction &mf,
                         MachineFunctionAnalysisManager &am) const {
  Result result;
  const auto &cfg = am.getResult<MachineCFGAnalysis>(mf);
  const std::size_t count = mf.blocks.size();
  result.depth.assign(count, 0);
  if (count == 0) {
    return result;
  }

  std::vector<bool> reachable(count, false);
  std::vector<int> reachWork{0};
  reachable[0] = true;
  while (!reachWork.empty()) {
    const int block = reachWork.back();
    reachWork.pop_back();
    for (int succ : cfg.succs[static_cast<std::size_t>(block)]) {
      if (succ < 0 || succ >= static_cast<int>(count)) {
        throw BackendError("invalid CFG successor during loop reachability");
      }
      if (!reachable[static_cast<std::size_t>(succ)]) {
        reachable[static_cast<std::size_t>(succ)] = true;
        reachWork.push_back(succ);
      }
    }
  }

  std::vector<std::set<int>> dominators(count);
  std::set<int> all;
  for (std::size_t i = 0; i < count; ++i) {
    if (reachable[i]) {
      all.insert(static_cast<int>(i));
    }
  }
  dominators[0].insert(0);
  for (std::size_t i = 1; i < count; ++i) {
    if (reachable[i]) {
      dominators[i] = all;
    }
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t i = 1; i < count; ++i) {
      if (!reachable[i]) {
        continue;
      }
      std::set<int> next;
      bool firstPred = true;
      for (int pred : cfg.preds[i]) {
        if (pred < 0 || pred >= static_cast<int>(count)) {
          throw BackendError("invalid CFG predecessor during loop analysis");
        }
        if (!reachable[static_cast<std::size_t>(pred)]) {
          continue;
        }
        if (firstPred) {
          next = dominators[static_cast<std::size_t>(pred)];
          firstPred = false;
        } else {
          std::set<int> intersection;
          std::set_intersection(
              next.begin(), next.end(),
              dominators[static_cast<std::size_t>(pred)].begin(),
              dominators[static_cast<std::size_t>(pred)].end(),
              std::inserter(intersection, intersection.begin()));
          next = std::move(intersection);
        }
      }
      if (firstPred) {
        next.clear();
      }
      next.insert(static_cast<int>(i));
      if (next != dominators[i]) {
        dominators[i] = std::move(next);
        changed = true;
      }
    }
  }

  // 同一 header 的多个 latch 属于同一个自然循环。先保留所有回边明细，
  // 再按 header 合并成员，避免把 latch 数误当成嵌套深度。
  std::map<int, std::set<int>> latchesByHeader;
  for (std::size_t from = 0; from < count; ++from) {
    if (!reachable[from]) {
      continue;
    }
    for (int succ : cfg.succs[from]) {
      if (succ < 0 || succ >= static_cast<int>(count)) {
        throw BackendError("invalid CFG successor during loop analysis");
      }
      if (dominators[from].count(succ) == 0) {
        continue;
      }

      result.backedges.emplace(static_cast<int>(from), succ);
      latchesByHeader[succ].insert(static_cast<int>(from));
    }
  }

  for (const auto &[header, latches] : latchesByHeader) {
    std::set<int> members{header};
    std::vector<int> work;
    for (int latch : latches) {
      members.insert(latch);
      // 自环的自然循环只有 header；绝不能从 header 反向走到 preheader。
      if (latch != header) {
        work.push_back(latch);
      }
    }
    while (!work.empty()) {
      const int block = work.back();
      work.pop_back();
      for (int pred : cfg.preds[static_cast<std::size_t>(block)]) {
        if (pred < 0 || pred >= static_cast<int>(count)) {
          throw BackendError("invalid natural-loop predecessor");
        }
        if (pred != header && reachable[static_cast<std::size_t>(pred)] &&
            dominators[static_cast<std::size_t>(pred)].count(header) != 0 &&
            members.insert(pred).second) {
          work.push_back(pred);
        }
      }
    }
    for (int block : members) {
      ++result.depth[static_cast<std::size_t>(block)];
    }
  }
  return result;
}

namespace {

uint64_t saturatingAdd(uint64_t lhs, uint64_t rhs) {
  const uint64_t limit = std::numeric_limits<uint64_t>::max();
  return rhs > limit - lhs ? limit : lhs + rhs;
}

uint64_t saturatingAddChecked(uint64_t lhs, uint64_t rhs, bool &saturated) {
  const uint64_t limit = std::numeric_limits<uint64_t>::max();
  if (rhs > limit - lhs) {
    saturated = true;
    return limit;
  }
  return lhs + rhs;
}

uint64_t scaledProduct(uint64_t value, uint32_t probability) {
  constexpr uint64_t scale = MachineBlockFrequencyAnalysis::ProbabilityScale;
  const uint64_t whole = value / scale;
  const uint64_t remainder = value % scale;
  const uint64_t limit = std::numeric_limits<uint64_t>::max();
  if (whole != 0 && probability > limit / whole) {
    return limit;
  }
  return saturatingAdd(whole * probability, remainder * probability / scale);
}

uint32_t edgePreference(const MachineLoopAnalysis::Result &loops, int from,
                        int to) {
  if (loops.backedges.count({from, to}) != 0) {
    return 8;
  }
  const int fromDepth = loops.depth[static_cast<std::size_t>(from)];
  const int toDepth = loops.depth[static_cast<std::size_t>(to)];
  if (toDepth > fromDepth) {
    return 4;
  }
  if (toDepth < fromDepth) {
    return 1;
  }
  return 2;
}

bool hasClosedCyclicSCC(const std::vector<std::vector<int>> &successors) {
  const std::size_t count = successors.size();
  if (count == 0) {
    return false;
  }
  std::vector<bool> reachable(count, false);
  std::vector<int> reachWork{0};
  reachable[0] = true;
  while (!reachWork.empty()) {
    const int node = reachWork.back();
    reachWork.pop_back();
    for (int succ : successors[static_cast<std::size_t>(node)]) {
      if (succ < 0 || succ >= static_cast<int>(count)) {
        throw BackendError("invalid CFG successor during SCC reachability");
      }
      if (!reachable[static_cast<std::size_t>(succ)]) {
        reachable[static_cast<std::size_t>(succ)] = true;
        reachWork.push_back(succ);
      }
    }
  }
  std::vector<int> index(count, -1);
  std::vector<int> lowlink(count, -1);
  std::vector<int> stack;
  std::vector<bool> onStack(count, false);
  std::vector<int> componentMark(count, 0);
  int componentEpoch = 0;
  int nextIndex = 0;
  bool found = false;

  std::function<void(int)> visit = [&](int node) {
    const std::size_t nodeIndex = static_cast<std::size_t>(node);
    index[nodeIndex] = lowlink[nodeIndex] = nextIndex++;
    stack.push_back(node);
    onStack[nodeIndex] = true;
    for (int succ : successors[nodeIndex]) {
      if (succ < 0 || succ >= static_cast<int>(count)) {
        throw BackendError("invalid CFG successor during SCC analysis");
      }
      const std::size_t succIndex = static_cast<std::size_t>(succ);
      if (!reachable[succIndex]) {
        continue;
      }
      if (index[succIndex] < 0) {
        visit(succ);
        lowlink[nodeIndex] = std::min(lowlink[nodeIndex], lowlink[succIndex]);
      } else if (onStack[succIndex]) {
        lowlink[nodeIndex] = std::min(lowlink[nodeIndex], index[succIndex]);
      }
    }
    if (lowlink[nodeIndex] != index[nodeIndex]) {
      return;
    }

    std::vector<int> component;
    while (true) {
      const int member = stack.back();
      stack.pop_back();
      onStack[static_cast<std::size_t>(member)] = false;
      component.push_back(member);
      if (member == node) {
        break;
      }
    }
    ++componentEpoch;
    for (int member : component) {
      componentMark[static_cast<std::size_t>(member)] = componentEpoch;
    }
    bool cyclic = component.size() > 1;
    bool closed = true;
    for (int member : component) {
      for (int succ : successors[static_cast<std::size_t>(member)]) {
        if (succ == member) {
          cyclic = true;
        }
        if (componentMark[static_cast<std::size_t>(succ)] != componentEpoch) {
          closed = false;
        }
      }
    }
    found = found || (cyclic && closed);
  };

  for (std::size_t node = 0; node < count; ++node) {
    if (reachable[node] && index[node] < 0) {
      visit(static_cast<int>(node));
    }
  }
  return found;
}

} // namespace

MachineBlockFrequencyAnalysis::Result
MachineBlockFrequencyAnalysis::run(MachineFunction &mf,
                                   MachineFunctionAnalysisManager &am) const {
  Result result;
  const auto &cfg = am.getResult<MachineCFGAnalysis>(mf);
  const auto &loops = am.getResult<MachineLoopAnalysis>(mf);
  const std::size_t count = mf.blocks.size();
  result.blockFrequency.assign(count, 0);
  result.successors = cfg.succs;
  result.edgeProbability.resize(count);
  result.edgeWeight.resize(count);
  if (count == 0) {
    result.converged = true;
    result.reliable = true;
    return result;
  }
  result.reliable = cfg.preds[0].empty() && !hasClosedCyclicSCC(cfg.succs);

  for (std::size_t from = 0; from < count; ++from) {
    const auto &succs = cfg.succs[from];
    auto &probabilities = result.edgeProbability[from];
    probabilities.assign(succs.size(), 0);
    if (succs.empty()) {
      continue;
    }
    if (succs.size() == 1) {
      probabilities[0] = ProbabilityScale;
      continue;
    }

    std::vector<uint32_t> preferences;
    preferences.reserve(succs.size());
    uint32_t totalPreference = 0;
    for (int succ : succs) {
      if (succ < 0 || succ >= static_cast<int>(count)) {
        throw BackendError("invalid CFG successor during frequency analysis");
      }
      const uint32_t preference =
          edgePreference(loops, static_cast<int>(from), succ);
      preferences.push_back(preference);
      totalPreference += preference;
    }
    uint32_t assigned = 0;
    for (std::size_t i = 0; i < succs.size(); ++i) {
      probabilities[i] = ProbabilityScale * preferences[i] / totalPreference;
      assigned += probabilities[i];
    }
    // 整除余数按原 successor 顺序发放；该顺序由原块下标确定。
    for (std::size_t i = 0; assigned < ProbabilityScale; ++assigned) {
      ++probabilities[i % probabilities.size()];
    }
  }

  // entry 的基准频率为 1（用 1024 表示）。这是最多 96 轮、确定性的
  // expected-visit 有界估计。converged 只记录是否达到精确定点；普通
  // 可退出循环即使尚未精确稳定，估计仍可用于布局。
  std::vector<uint64_t> frequencies(count, 0);
  frequencies[0] = ProbabilityScale;
  bool propagationSaturated = false;
  for (int iteration = 0; iteration < 96; ++iteration) {
    std::vector<uint64_t> next(count, 0);
    next[0] = ProbabilityScale;
    for (std::size_t from = 0; from < count; ++from) {
      for (std::size_t edge = 0; edge < cfg.succs[from].size(); ++edge) {
        const int to = cfg.succs[from][edge];
        if (to == 0) {
          continue;
        }
        const uint64_t contribution = scaledProduct(
            frequencies[from], result.edgeProbability[from][edge]);
        next[static_cast<std::size_t>(to)] =
            saturatingAddChecked(next[static_cast<std::size_t>(to)],
                                 contribution, propagationSaturated);
      }
    }
    if (next == frequencies) {
      frequencies = std::move(next);
      result.converged = true;
      break;
    }
    frequencies = std::move(next);
  }
  result.blockFrequency = std::move(frequencies);
  if (propagationSaturated) {
    result.reliable = false;
  }

  for (std::size_t from = 0; from < count; ++from) {
    auto &weights = result.edgeWeight[from];
    weights.reserve(cfg.succs[from].size());
    for (uint32_t probability : result.edgeProbability[from]) {
      weights.push_back(
          scaledProduct(result.blockFrequency[from], probability));
    }
  }
  return result;
}

} // namespace riscv
