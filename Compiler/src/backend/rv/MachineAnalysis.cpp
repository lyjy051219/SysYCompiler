#include "../../../include/riscv/MachineAnalysis.hpp"

#include "../../../include/riscv/Target.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>

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

} // namespace

MachinePassID MachineInstrStatsAnalysis::Key;
MachinePassID MachineCFGAnalysis::Key;

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
        auto &succList = result.succs[i];
        if (std::find(succList.begin(), succList.end(), to) == succList.end()) {
          succList.push_back(to);
        }
        auto &predList = result.preds[to];
        if (std::find(predList.begin(), predList.end(), from) ==
            predList.end()) {
          predList.push_back(from);
        }
      }
    }
  }
  return result;
}

} // namespace riscv
