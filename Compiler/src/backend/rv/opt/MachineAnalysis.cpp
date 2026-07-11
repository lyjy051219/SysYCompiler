#include "../../../include/riscv/MachineAnalysis.hpp"

#include "../../../include/riscv/Target.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace riscv {
namespace {

// 与 Passes.cpp 中保持一致的终结指令判断
bool isTerminatorOpcode(Opcode opcode) {
  switch (opcode) {
  case Opcode::RET:
  case Opcode::BEQZ:
  case Opcode::BNEZ:
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

} // namespace riscv
