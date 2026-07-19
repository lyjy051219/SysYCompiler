#include "aarch64/opt/MachineOptPasses.hpp"

#include <algorithm>
#include <set>
#include <vector>

namespace aarch64 {
namespace {

using LiveSet = MachineLivenessAnalysis::RegSet;

bool isVirtualRegOperand(const MachineOperand &operand) {
  return operand.kind == OperandKind::Reg && !operand.regValue.physical;
}

bool hasUnsupportedOperandKind(const MachineInstr &instr) {
  for (const MachineOperand &def : instr.defs) {
    if (def.kind != OperandKind::Reg) {
      return true;
    }
  }
  for (const MachineOperand &use : instr.uses) {
    if (use.kind == OperandKind::Block || use.kind == OperandKind::ValueRef) {
      return true;
    }
  }
  return false;
}

bool shouldAlwaysKeep(const MachineInstr &instr) {
  return instr.opcode == Opcode::CALL || instr.opcode == Opcode::PHI ||
         instr.opcode == Opcode::PARALLEL_COPY || instr.sideEffects ||
         instr.terminator || !instr.implicitDefs.empty() ||
         !instr.implicitUses.empty() || hasUnsupportedOperandKind(instr);
}

bool isRemovablePureInstr(const MachineInstr &instr, const LiveSet &live) {
  if (shouldAlwaysKeep(instr)) {
    return false;
  }

  for (const MachineOperand &def : instr.defs) {
    if (!isVirtualRegOperand(def)) {
      return false;
    }
    if (live.count(def.regValue.id) != 0) {
      return false;
    }
  }
  return true;
}

void updateLiveForKeptInstr(const MachineInstr &instr, LiveSet &live) {
  for (const MachineOperand &def : instr.defs) {
    if (isVirtualRegOperand(def)) {
      live.erase(def.regValue.id);
    }
  }
  for (const MachineOperand &use : instr.uses) {
    if (isVirtualRegOperand(use)) {
      live.insert(use.regValue.id);
    }
  }
}

} // namespace

bool runPreRAMachineDCE(MachineFunction &mf, MachineFunctionAnalysisManager &am) {
  const auto &live = am.getResult<MachineLivenessAnalysis>(mf);
  bool changed = false;

  for (std::size_t blockIndex = 0; blockIndex < mf.blocks.size();
       ++blockIndex) {
    LiveSet currentLive = live.liveOut[blockIndex];
    MachineBasicBlock &block = mf.blocks[blockIndex];
    std::vector<MachineInstr> rewritten;
    rewritten.reserve(block.instrs.size());

    for (auto it = block.instrs.rbegin(); it != block.instrs.rend(); ++it) {
      if (isRemovablePureInstr(*it, currentLive)) {
        changed = true;
        continue;
      }

      updateLiveForKeptInstr(*it, currentLive);
      rewritten.push_back(std::move(*it));
    }

    std::reverse(rewritten.begin(), rewritten.end());
    block.instrs = std::move(rewritten);
  }

  return changed;
}

} // namespace aarch64
