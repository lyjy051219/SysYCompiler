#include "riscv/opt/MachineOptPasses.hpp"

#include "riscv/Registers.hpp"
#include "riscv/Target.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <type_traits>
#include <utility>
#include <vector>

namespace riscv {
namespace {

struct RegKey {
  bool physical = false;
  uint32_t id = 0;
  PhysReg phys = PhysReg::NoReg;
};

bool operator<(const RegKey &lhs, const RegKey &rhs) {
  if (lhs.physical != rhs.physical) {
    return lhs.physical < rhs.physical;
  }
  if (lhs.physical) {
    return static_cast<uint16_t>(lhs.phys) < static_cast<uint16_t>(rhs.phys);
  }
  return lhs.id < rhs.id;
}

bool operator==(const RegKey &lhs, const RegKey &rhs) {
  return lhs.physical == rhs.physical && lhs.id == rhs.id &&
         lhs.phys == rhs.phys;
}

RegKey makeRegKey(const MachineReg &reg) {
  if (reg.physical) {
    return RegKey{true, 0, reg.phys};
  }
  return RegKey{false, reg.id, PhysReg::NoReg};
}

RegKey makeRegKey(PhysReg reg) { return RegKey{true, 0, reg}; }

bool isZeroRegister(const RegKey &reg) {
  return reg.physical && reg.phys == PhysReg::X0;
}

bool isLoadOpcode(Opcode opcode) {
  return opcode == Opcode::LBU || opcode == Opcode::LW ||
         opcode == Opcode::LD || opcode == Opcode::FLW;
}

bool isStoreOpcode(Opcode opcode) {
  return opcode == Opcode::SB || opcode == Opcode::SW ||
         opcode == Opcode::SD || opcode == Opcode::FSW;
}

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

bool isBarrierInstruction(const MachineInstr &instr) {
  if (instr.terminator || isTerminatorOpcode(instr.opcode)) {
    return true;
  }
  if (instr.opcode == Opcode::CALL || instr.opcode == Opcode::MEMCPY_INLINE ||
      instr.opcode == Opcode::MEMSET_INLINE) {
    return true;
  }
  return instr.sideEffects && !isStoreOpcode(instr.opcode);
}

bool isMemoryOrderInstruction(const MachineInstr &instr) {
  return isLoadOpcode(instr.opcode) || isStoreOpcode(instr.opcode) ||
         instr.opcode == Opcode::CALL || instr.opcode == Opcode::MEMCPY_INLINE ||
         instr.opcode == Opcode::MEMSET_INLINE ||
         (instr.sideEffects && !isStoreOpcode(instr.opcode));
}

uint32_t opcodeLatency(Opcode opcode) {
  switch (opcode) {
  case Opcode::NOP:
    return 0;
  case Opcode::COPY:
  case Opcode::F_COPY:
  case Opcode::FMV_W_X:
  case Opcode::FMV_X_W:
  case Opcode::FMV_X_D:
  case Opcode::LI:
  case Opcode::LA:
  case Opcode::ADD:
  case Opcode::ADDW:
  case Opcode::ADDI:
  case Opcode::ADDIW:
  case Opcode::SUB:
  case Opcode::SUBW:
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
  case Opcode::BEQZ:
  case Opcode::BNEZ:
  case Opcode::BEQ:
  case Opcode::BNE:
  case Opcode::BLT:
  case Opcode::BGE:
    return 1;
  case Opcode::MUL:
  case Opcode::MULW:
    return 3;
  case Opcode::FADD_S:
  case Opcode::FSUB_S:
  case Opcode::FMUL_S:
    return 3;
  case Opcode::DIV:
  case Opcode::DIVW:
  case Opcode::DIVUW:
  case Opcode::REM:
  case Opcode::REMW:
  case Opcode::REMUW:
  case Opcode::FDIV_S:
    return 12;
  case Opcode::FCVT_W_S:
  case Opcode::FCVT_S_W:
  case Opcode::FCVT_D_S:
  case Opcode::FEQ_S:
  case Opcode::FLT_S:
  case Opcode::FLE_S:
    return 3;
  case Opcode::LBU:
  case Opcode::LW:
  case Opcode::LD:
  case Opcode::FLW:
    return 3;
  case Opcode::SB:
  case Opcode::SW:
  case Opcode::SD:
  case Opcode::FSW:
    return 1;
  case Opcode::CALL:
    return 20;
  case Opcode::MEMCPY_INLINE:
    return 8;
  case Opcode::MEMSET_INLINE:
    return 8;
  case Opcode::RET:
  case Opcode::J:
    return 1;
  case Opcode::PHI:
  case Opcode::SELECT:
  case Opcode::PARALLEL_COPY:
    return 1;
  case Opcode::GEP:
    return 1;
  }
  return 1;
}

template <typename ContainerT>
std::vector<RegKey> collectRegKeys(const ContainerT &operands) {
  std::vector<RegKey> result;
  result.reserve(operands.size());
  for (const auto &operand : operands) {
    if constexpr (std::is_same_v<std::decay_t<decltype(operand)>, MachineOperand>) {
      if (operand.kind != OperandKind::Reg || !operand.regValue.isValid()) {
        continue;
      }
      RegKey key = makeRegKey(operand.regValue);
      if (!isZeroRegister(key)) {
        result.push_back(key);
      }
    } else {
      if (!isValidPhysReg(operand) || operand == PhysReg::X0) {
        continue;
      }
      result.push_back(makeRegKey(operand));
    }
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

struct InstructionInfo {
  uint32_t latency = 0;
  bool memoryOp = false;
  bool barrier = false;
  std::vector<RegKey> uses;
  std::vector<RegKey> defs;
};

std::vector<InstructionInfo>
buildInstructionInfo(const std::vector<MachineInstr> &instrs) {
  std::vector<InstructionInfo> info;
  info.reserve(instrs.size());
  for (const MachineInstr &instr : instrs) {
    InstructionInfo item;
    item.latency = opcodeLatency(instr.opcode);
    item.barrier = isBarrierInstruction(instr);
    item.memoryOp = isMemoryOrderInstruction(instr);
    item.uses = collectRegKeys(instr.uses);
    item.defs = collectRegKeys(instr.defs);
    const auto implicitUses = collectRegKeys(instr.implicitUses);
    const auto implicitDefs = collectRegKeys(instr.implicitDefs);
    item.uses.insert(item.uses.end(), implicitUses.begin(), implicitUses.end());
    item.defs.insert(item.defs.end(), implicitDefs.begin(), implicitDefs.end());
    std::sort(item.uses.begin(), item.uses.end());
    item.uses.erase(std::unique(item.uses.begin(), item.uses.end()),
                    item.uses.end());
    std::sort(item.defs.begin(), item.defs.end());
    item.defs.erase(std::unique(item.defs.begin(), item.defs.end()),
                    item.defs.end());
    info.push_back(std::move(item));
  }
  return info;
}

struct ScheduleState {
  std::map<RegKey, uint64_t> regReady;
  uint64_t memoryReady = 0;
  uint64_t cycle = 0;
  uint64_t finish = 0;
};

uint64_t readyTimeForReg(const ScheduleState &state, const RegKey &reg) {
  auto it = state.regReady.find(reg);
  if (it == state.regReady.end()) {
    return 0;
  }
  return it->second;
}

void setRegReady(ScheduleState &state, const RegKey &reg, uint64_t readyTime) {
  if (!isZeroRegister(reg)) {
    state.regReady[reg] = readyTime;
  }
}

void clobberCallerSavedRegs(ScheduleState &state, uint64_t readyTime) {
  for (PhysReg reg : callerSavedRegs(RegClass::GPR64)) {
    setRegReady(state, makeRegKey(reg), readyTime);
  }
  for (PhysReg reg : callerSavedRegs(RegClass::FPR32)) {
    setRegReady(state, makeRegKey(reg), readyTime);
  }
}

uint64_t issueInstruction(const MachineInstr &instr, const InstructionInfo &info,
                          ScheduleState &state) {
  uint64_t issue = state.cycle;
  for (const RegKey &use : info.uses) {
    issue = std::max(issue, readyTimeForReg(state, use));
  }
  if (info.memoryOp) {
    issue = std::max(issue, state.memoryReady);
  }

  const uint64_t completion = issue + info.latency;
  state.cycle = issue + 1;
  state.finish = std::max(state.finish, completion);

  for (const RegKey &def : info.defs) {
    setRegReady(state, def, completion);
  }
  if (info.memoryOp) {
    state.memoryReady = completion;
  }
  if (instr.opcode == Opcode::CALL) {
    clobberCallerSavedRegs(state, completion);
  }
  return completion;
}

struct SegmentNode {
  size_t originalIndex = 0;
  uint64_t releaseTime = 0;
  uint32_t latency = 0;
  int indegree = 0;
  std::vector<size_t> completionSuccs;
  std::vector<size_t> orderSuccs;
  std::vector<RegKey> uses;
  std::vector<RegKey> defs;
  bool memoryOp = false;
};

void addCompletionEdge(std::vector<SegmentNode> &nodes, size_t from, size_t to) {
  auto &succs = nodes[from].completionSuccs;
  if (std::find(succs.begin(), succs.end(), to) == succs.end()) {
    succs.push_back(to);
    ++nodes[to].indegree;
  }
}

void addOrderEdge(std::vector<SegmentNode> &nodes, size_t from, size_t to) {
  auto &succs = nodes[from].orderSuccs;
  if (std::find(succs.begin(), succs.end(), to) == succs.end()) {
    succs.push_back(to);
    ++nodes[to].indegree;
  }
}

bool moreImportantNode(size_t lhs, size_t rhs,
                       const std::vector<SegmentNode> &nodes,
                       const std::vector<uint64_t> &priorities) {
  if (priorities[lhs] != priorities[rhs]) {
    return priorities[lhs] > priorities[rhs];
  }
  return nodes[lhs].originalIndex < nodes[rhs].originalIndex;
}

uint64_t maxPrioritySuccessor(const std::vector<SegmentNode> &nodes,
                             const std::vector<uint64_t> &priorities,
                             size_t index) {
  uint64_t best = 0;
  for (size_t succ : nodes[index].completionSuccs) {
    best = std::max(best, priorities[succ]);
  }
  for (size_t succ : nodes[index].orderSuccs) {
    best = std::max(best, priorities[succ]);
  }
  return nodes[index].latency + best;
}

std::vector<size_t>
scheduleSegment(const std::vector<MachineInstr> &instrs,
                const std::vector<InstructionInfo> &infos,
                const std::vector<size_t> &segmentIndices,
                ScheduleState &state) {
  std::vector<size_t> empty;
  if (segmentIndices.empty()) {
    return empty;
  }

  std::vector<SegmentNode> nodes(segmentIndices.size());
  std::map<RegKey, size_t> lastDef;
  std::map<RegKey, std::vector<size_t>> openUses;
  size_t lastMemoryNode = std::numeric_limits<size_t>::max();

  for (size_t localIndex = 0; localIndex < segmentIndices.size(); ++localIndex) {
    const size_t originalIndex = segmentIndices[localIndex];
    const InstructionInfo &info = infos[originalIndex];
    SegmentNode &node = nodes[localIndex];
    node.originalIndex = originalIndex;
    node.releaseTime = 0;
    node.latency = info.latency;
    node.uses = info.uses;
    node.defs = info.defs;
    node.memoryOp = info.memoryOp;

    if (node.memoryOp) {
      if (lastMemoryNode != std::numeric_limits<size_t>::max()) {
        addOrderEdge(nodes, lastMemoryNode, localIndex);
      } else {
        node.releaseTime = std::max(node.releaseTime, state.cycle);
      }
      lastMemoryNode = localIndex;
    }

    for (const RegKey &use : node.uses) {
      auto found = lastDef.find(use);
      if (found != lastDef.end()) {
        addCompletionEdge(nodes, found->second, localIndex);
      } else {
        node.releaseTime =
            std::max(node.releaseTime, readyTimeForReg(state, use));
      }
    }

    for (const RegKey &def : node.defs) {
      auto defFound = lastDef.find(def);
      if (defFound != lastDef.end()) {
        addOrderEdge(nodes, defFound->second, localIndex);
      }
      auto useFound = openUses.find(def);
      if (useFound != openUses.end()) {
        for (size_t useIndex : useFound->second) {
          addOrderEdge(nodes, useIndex, localIndex);
        }
        useFound->second.clear();
      }
    }

    for (const RegKey &def : node.defs) {
      lastDef[def] = localIndex;
    }
    for (const RegKey &use : node.uses) {
      openUses[use].push_back(localIndex);
    }
  }

  std::vector<uint64_t> priorities(nodes.size(), 0);
  for (size_t reverse = 0; reverse < nodes.size(); ++reverse) {
    const size_t index = nodes.size() - reverse - 1;
    priorities[index] = maxPrioritySuccessor(nodes, priorities, index);
  }

  std::vector<bool> scheduled(nodes.size(), false);
  std::vector<size_t> order;
  order.reserve(nodes.size());
  size_t scheduledCount = 0;
  uint64_t issueCycle = state.cycle;

  while (scheduledCount < nodes.size()) {
    size_t chosen = std::numeric_limits<size_t>::max();
    uint64_t nextRelease = std::numeric_limits<uint64_t>::max();

    for (size_t index = 0; index < nodes.size(); ++index) {
      if (scheduled[index] || nodes[index].indegree != 0) {
        continue;
      }
      if (nodes[index].releaseTime <= issueCycle) {
        if (chosen == std::numeric_limits<size_t>::max() ||
            moreImportantNode(index, chosen, nodes, priorities)) {
          chosen = index;
        }
      } else {
        nextRelease = std::min(nextRelease, nodes[index].releaseTime);
      }
    }

    if (chosen == std::numeric_limits<size_t>::max()) {
      if (nextRelease == std::numeric_limits<uint64_t>::max()) {
        throw BackendError("post-RA list scheduler encountered a cycle");
      }
      issueCycle = nextRelease;
      state.cycle = issueCycle;
      continue;
    }

    state.cycle = issueCycle;
    const uint64_t completion =
        issueInstruction(instrs[nodes[chosen].originalIndex],
                         infos[nodes[chosen].originalIndex], state);
    issueCycle = state.cycle;
    scheduled[chosen] = true;
    ++scheduledCount;
    order.push_back(nodes[chosen].originalIndex);

    for (size_t succ : nodes[chosen].completionSuccs) {
      nodes[succ].indegree--;
      nodes[succ].releaseTime = std::max(nodes[succ].releaseTime, completion);
    }
    for (size_t succ : nodes[chosen].orderSuccs) {
      nodes[succ].indegree--;
      nodes[succ].releaseTime = std::max(nodes[succ].releaseTime, state.cycle);
    }
  }

  return order;
}

uint64_t simulateOrder(const std::vector<MachineInstr> &instrs,
                       const std::vector<InstructionInfo> &infos,
                       const std::vector<size_t> &order) {
  ScheduleState state;
  for (size_t index : order) {
    issueInstruction(instrs[index], infos[index], state);
  }
  return state.finish;
}

std::vector<size_t> identityOrder(const std::vector<MachineInstr> &instrs) {
  std::vector<size_t> order(instrs.size());
  for (size_t i = 0; i < instrs.size(); ++i) {
    order[i] = i;
  }
  return order;
}

bool sameOrder(const std::vector<size_t> &lhs, const std::vector<size_t> &rhs) {
  return lhs == rhs;
}

std::vector<size_t> buildCandidateOrder(const std::vector<MachineInstr> &instrs,
                                        const std::vector<InstructionInfo> &infos,
                                        ScheduleState &state) {
  std::vector<size_t> order;
  order.reserve(instrs.size());
  std::vector<size_t> pendingSegment;
  bool afterTerminator = false;

  for (size_t index = 0; index < instrs.size(); ++index) {
    if (afterTerminator) {
      order.push_back(index);
      issueInstruction(instrs[index], infos[index], state);
      continue;
    }

    const InstructionInfo &info = infos[index];
    if (info.barrier) {
      if (!pendingSegment.empty()) {
        std::vector<size_t> segmentOrder =
            scheduleSegment(instrs, infos, pendingSegment, state);
        order.insert(order.end(), segmentOrder.begin(), segmentOrder.end());
        pendingSegment.clear();
      }
      order.push_back(index);
      issueInstruction(instrs[index], infos[index], state);
      if (isTerminatorOpcode(instrs[index].opcode) || instrs[index].terminator) {
        afterTerminator = true;
      }
      continue;
    }

    pendingSegment.push_back(index);
  }

  if (!pendingSegment.empty()) {
    std::vector<size_t> segmentOrder =
        scheduleSegment(instrs, infos, pendingSegment, state);
    order.insert(order.end(), segmentOrder.begin(), segmentOrder.end());
  }

  return order;
}

bool blockHasUsefulSchedulingOpportunity(
    const std::vector<InstructionInfo> &infos) {
  std::vector<size_t> segment;
  for (size_t index = 0; index < infos.size(); ++index) {
    if (infos[index].barrier) {
      if (segment.size() >= 2) {
        for (size_t segmentIndex : segment) {
          if (infos[segmentIndex].memoryOp || infos[segmentIndex].latency > 1) {
            return true;
          }
        }
      }
      segment.clear();
      continue;
    }
    segment.push_back(index);
  }

  if (segment.size() < 2) {
    return false;
  }
  for (size_t segmentIndex : segment) {
    if (infos[segmentIndex].memoryOp || infos[segmentIndex].latency > 1) {
      return true;
    }
  }
  return false;
}

} // namespace

bool runPostRAListSchedule(MachineFunction &mf,
                           const MachineBlockFrequencyAnalysis::Result &frequencies) {
  if (frequencies.blockFrequency.size() != mf.blocks.size()) {
    throw BackendError("post-RA list scheduler received mismatched frequency data");
  }

  bool changed = false;
  for (size_t blockIndex = 0; blockIndex < mf.blocks.size(); ++blockIndex) {
    if (frequencies.blockFrequency[blockIndex] == 0) {
      continue;
    }

    MachineBasicBlock &block = mf.blocks[blockIndex];
    if (block.instrs.size() < 2) {
      continue;
    }

    const std::vector<InstructionInfo> infos = buildInstructionInfo(block.instrs);
    if (!blockHasUsefulSchedulingOpportunity(infos)) {
      continue;
    }

    const std::vector<size_t> originalOrder = identityOrder(block.instrs);
    const uint64_t originalFinish = simulateOrder(block.instrs, infos, originalOrder);

    ScheduleState candidateState;
    const std::vector<size_t> candidateOrder =
        buildCandidateOrder(block.instrs, infos, candidateState);
    if (sameOrder(candidateOrder, originalOrder)) {
      continue;
    }

    const uint64_t candidateFinish = candidateState.finish;
    if (candidateFinish >= originalFinish) {
      continue;
    }

    std::vector<MachineInstr> reordered;
    reordered.reserve(block.instrs.size());
    for (size_t index : candidateOrder) {
      reordered.push_back(block.instrs[index]);
    }
    block.instrs = std::move(reordered);
    changed = true;
  }

  return changed;
}

} // namespace riscv
