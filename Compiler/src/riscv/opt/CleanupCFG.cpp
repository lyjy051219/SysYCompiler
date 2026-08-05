#include "riscv/opt/MachineOptPasses.hpp"

#include "riscv/Passes.hpp"
#include "riscv/Target.hpp"

#include <algorithm>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace riscv {
// CleanupCFG：反复化简控制流图，包括删除不可达块、消除空块/跳板块、
// 合并线性(单前驱单后继)块、以及删除指向下一块的多余跳转。变换过程中会
// 同步维护 PHI 的前驱来源和分支目标,保持语义不变。位于后端优化流水线中,
// 依赖并会重建 CFG(rebuildCFG)。此为 RISC-V 与 ARM64 两后端相同的
// 公共实现,两后端各保留一份相同实现。

namespace {

// 前置声明:清理过程中相互调用的若干辅助函数。
void appendJump(MachineBasicBlock &block, const std::string &label);
void retargetBranchTargets(MachineFunction &mf, const std::string &oldLabel,
                           const std::string &newLabel);
void removePhiIncomingPreds(MachineFunction &mf,
                            const std::set<std::string> &removedLabels);
void retargetPhiIncomingPreds(MachineFunction &mf, const std::string &oldLabel,
                              const std::string &newLabel);
int findBlockIndex(const MachineFunction &mf, const std::string &label);

// 判断操作码是否为终结指令(与其他文件保持一致的分类)。
bool isTerminator(Opcode opcode) {
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

// 无条件跳转 J 与返回 RET 不会顺序落入下一块。
bool isNoFallthroughTerminator(Opcode opcode) {
  return opcode == Opcode::RET || opcode == Opcode::J;
}

// 指令是否为终结指令:显式 terminator 标记或操作码属于终结类。
bool isTerminatorInstr(const MachineInstr &instr) {
  return instr.terminator || isTerminator(instr.opcode);
}

// 块是否可顺序落入下一块:自块尾找到首条终结指令按其类型判定;无终结指令视为可落入。
bool blockCanFallThrough(const MachineBasicBlock &block) {
  for (auto it = block.instrs.rbegin(); it != block.instrs.rend(); ++it) {
    if (isTerminatorInstr(*it)) {
      return !isNoFallthroughTerminator(it->opcode);
    }
  }
  return true;
}

// 判断指令是否为跳转到指定标签的 J 指令。
bool isJumpTo(const MachineInstr &instr, const std::string &label) {
  return instr.opcode == Opcode::J && !instr.uses.empty() &&
         instr.uses.front().kind == OperandKind::Block &&
         instr.uses.front().blockLabel == label;
}

// 判断块的最后一条指令是否为跳转到指定标签。
bool blockEndsWithJumpTo(const MachineBasicBlock &block,
                         const std::string &label) {
  return !block.instrs.empty() && isJumpTo(block.instrs.back(), label);
}

// 比较两个机器寄存器是否完全相同(物理性、id、寄存器类、物理编号都相等)。
bool sameReg(const MachineReg &lhs, const MachineReg &rhs) {
  return lhs.physical == rhs.physical && lhs.id == rhs.id && lhs.rc == rhs.rc &&
         lhs.phys == rhs.phys;
}

// 比较两个操作数是否相等:先比种类,再按种类比较各自的值字段。
// 用于判断 PHI 不同来源的传入值是否一致(一致才可安全合并前驱)。
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

// 块中是否含有 PHI 指令。含 PHI 的块在合并/删除时需要特别处理前驱来源。
bool blockHasPhi(const MachineBasicBlock &block) {
  return std::any_of(block.instrs.begin(), block.instrs.end(),
                     [](const MachineInstr &instr) {
                       return instr.opcode == Opcode::PHI;
                     });
}

// 判断块是否为"空块或跳板块":不含 PHI,且指令全部是 NOP 或 J(仅做无条件转发)。
bool isEmptyOrTrampolineBlock(const MachineBasicBlock &block) {
  if (blockHasPhi(block)) {
    return false;
  }
  return std::all_of(block.instrs.begin(), block.instrs.end(),
                     [](const MachineInstr &instr) {
                       return instr.opcode == Opcode::NOP ||
                              instr.opcode == Opcode::J;
                     });
}

// 判断空块能否被删除:必须是空块或跳板块,且不含 NOP/J 以外的任何指令。
bool isRemovableEmptyBlock(const MachineBasicBlock &block) {
  if (!isEmptyOrTrampolineBlock(block)) {
    return false;
  }
  return std::none_of(block.instrs.begin(), block.instrs.end(),
                      [](const MachineInstr &instr) {
                        return instr.opcode != Opcode::NOP &&
                               instr.opcode != Opcode::J;
                      });
}

// 求空块的唯一转发目标标签。若块内有唯一 J 则取其目标;若没有 J 且可落入,
// 则取下一块。存在多个 J(目标不唯一)时返回空表示不可安全删除。
std::string emptyBlockTarget(const MachineFunction &mf,
                             std::size_t blockIndex) {
  const MachineBasicBlock &block = mf.blocks[blockIndex];
  std::string target;
  for (const MachineInstr &instr : block.instrs) {
    if (instr.opcode != Opcode::J) {
      continue;
    }
    if (!target.empty()) {
      return {};
    }
    if (instr.uses.empty() || instr.uses.front().kind != OperandKind::Block) {
      throw BackendError("malformed machine jump in CFG cleanup");
    }
    target = instr.uses.front().blockLabel;
  }
  if (!target.empty()) {
    return target;
  }
  if (blockCanFallThrough(block) && blockIndex + 1 < mf.blocks.size()) {
    return mf.blocks[blockIndex + 1].label;
  }
  return {};
}

// 收集指定块的前驱标签列表(去重、排除自身)。用于删除空块后把前驱接到目标块。
std::vector<std::string> predecessorLabels(const MachineFunction &mf,
                                           std::size_t blockIndex) {
  std::vector<std::string> labels;
  for (int pred : mf.blocks[blockIndex].preds) {
    if (pred < 0 || pred >= static_cast<int>(mf.blocks.size())) {
      throw BackendError("invalid CFG predecessor during cleanup");
    }
    if (pred == static_cast<int>(blockIndex)) {
      continue;
    }
    const std::string &label = mf.blocks[static_cast<std::size_t>(pred)].label;
    if (std::find(labels.begin(), labels.end(), label) == labels.end()) {
      labels.push_back(label);
    }
  }
  return labels;
}

// 判断 pred 是否通过顺序落入(而非显式跳转)到达 succ:两块相邻且 pred 可落入。
bool blockFallsThroughTo(const MachineFunction &mf, std::size_t predIndex,
                         std::size_t succIndex) {
  return predIndex + 1 == succIndex && predIndex < mf.blocks.size() &&
         blockCanFallThrough(mf.blocks[predIndex]);
}

// 从块列表中删除指定下标的块。
void eraseBlockAt(MachineFunction &mf, std::size_t blockIndex) {
  mf.blocks.erase(mf.blocks.begin() + static_cast<long>(blockIndex));
}

// 删除块中所有以指定标签为目标的终结指令(用于合并块时去掉指向被并块的跳转)。
void removeTerminatorsTargeting(MachineBasicBlock &block,
                                const std::string &label) {
  block.instrs.erase(
      std::remove_if(block.instrs.begin(), block.instrs.end(),
                     [&](const MachineInstr &instr) {
                       if (!isTerminatorInstr(instr)) {
                         return false;
                       }
                       return std::any_of(
                           instr.uses.begin(), instr.uses.end(),
                           [&](const MachineOperand &use) {
                             return use.kind == OperandKind::Block &&
                                    use.blockLabel == label;
                           });
                     }),
      block.instrs.end());
}

// 标签是否在列表中。
bool labelInList(const std::vector<std::string> &labels,
                 const std::string &label) {
  return std::find(labels.begin(), labels.end(), label) != labels.end();
}

// 求 PHI 中来自 oldLabel 的传入值。若 oldLabel 出现多次,要求这些传入值一致;
// 一致则通过 value 返回并返回 true,否则返回 false(不可安全改写)。
bool phiReplacementValue(const MachineInstr &phi, const std::string &oldLabel,
                         MachineOperand &value) {
  bool found = false;
  for (std::size_t i = 0; i < phi.uses.size(); i += 2) {
    if (i + 1 >= phi.uses.size()) {
      throw BackendError("malformed PHI incoming list");
    }
    const MachineOperand &pred = phi.uses[i + 1];
    if (pred.kind != OperandKind::Block || pred.blockLabel != oldLabel) {
      continue;
    }
    if (!found) {
      value = phi.uses[i];
      found = true;
      continue;
    }
    if (!sameOperand(value, phi.uses[i])) {
      return false;
    }
  }
  return found;
}

// 判断某条 PHI 能否在"把 oldLabel 前驱替换为 newLabels 一组前驱"后仍保持一致:
// oldLabel 的传入值需存在且唯一,且若某个新前驱已在 PHI 中出现,其传入值必须
// 与替换值相同,否则会产生同前驱不同值的冲突。
bool canRewritePhiIncomingPreds(const MachineInstr &phi,
                                const std::string &oldLabel,
                                const std::vector<std::string> &newLabels) {
  MachineOperand replacement;
  if (!phiReplacementValue(phi, oldLabel, replacement)) {
    return false;
  }
  for (std::size_t i = 0; i < phi.uses.size(); i += 2) {
    const MachineOperand &value = phi.uses[i];
    const MachineOperand &pred = phi.uses[i + 1];
    if (pred.kind != OperandKind::Block ||
        !labelInList(newLabels, pred.blockLabel)) {
      continue;
    }
    if (!sameOperand(value, replacement)) {
      return false;
    }
  }
  return true;
}

// 判断目标块中所有 PHI 是否都能在删除空块 oldLabel、改由 newLabels 前驱到达后
// 安全改写。任一 PHI 不可改写则整体放弃删除。
bool canRewritePhiTargetAfterEmptyBlockRemoval(
    const MachineBasicBlock &target, const std::string &oldLabel,
    const std::vector<std::string> &newLabels) {
  for (const MachineInstr &instr : target.instrs) {
    if (instr.opcode == Opcode::PHI &&
        !canRewritePhiIncomingPreds(instr, oldLabel, newLabels)) {
      return false;
    }
  }
  return true;
}

// 在已重写的 PHI 传入列表中查找是否已存在以指定标签为前驱的项(用于去重)。
bool rewrittenPhiHasPred(const std::vector<MachineOperand> &rewritten,
                         const std::string &label) {
  for (std::size_t i = 1; i < rewritten.size(); i += 2) {
    if (rewritten[i].kind == OperandKind::Block &&
        rewritten[i].blockLabel == label) {
      return true;
    }
  }
  return false;
}

// 在目标块中实际执行 PHI 前驱改写:删去来自 oldLabel 的项,对已由 newLabels
// 覆盖且值一致的重复前驱去重,最后为尚未出现的每个新前驱补上一项(值取替换值)。
void rewritePhiIncomingPredsInTarget(MachineBasicBlock &target,
                                     const std::string &oldLabel,
                                     const std::vector<std::string> &newLabels) {
  for (MachineInstr &instr : target.instrs) {
    if (instr.opcode != Opcode::PHI) {
      continue;
    }

    MachineOperand replacement;
    if (!phiReplacementValue(instr, oldLabel, replacement)) {
      throw BackendError("failed to rewrite PHI incoming predecessor");
    }

    std::vector<MachineOperand> rewritten;
    for (std::size_t i = 0; i < instr.uses.size(); i += 2) {
      if (i + 1 >= instr.uses.size()) {
        throw BackendError("malformed PHI incoming list");
      }
      const MachineOperand &value = instr.uses[i];
      const MachineOperand &pred = instr.uses[i + 1];
      if (pred.kind == OperandKind::Block && pred.blockLabel == oldLabel) {
        continue;
      }
      if (pred.kind == OperandKind::Block &&
          labelInList(newLabels, pred.blockLabel) &&
          sameOperand(value, replacement)) {
        if (rewrittenPhiHasPred(rewritten, pred.blockLabel)) {
          continue;
        }
      }
      rewritten.push_back(value);
      rewritten.push_back(pred);
    }

    for (const std::string &newLabel : newLabels) {
      if (!rewrittenPhiHasPred(rewritten, newLabel)) {
        rewritten.push_back(replacement);
        rewritten.push_back(MachineOperand::block(newLabel));
      }
    }
    instr.uses = std::move(rewritten);
  }
}

// 删除从入口不可达的块。
// 关键步骤:从入口沿后继做可达性标记;把不可达块的标签从各 PHI 前驱中移除;
// 再重建仅含入口与可达块的新列表。返回是否删除过块。
bool removeUnreachableBlocks(MachineFunction &mf) {
  if (mf.blocks.empty()) {
    return false;
  }

  std::vector<bool> reachable(mf.blocks.size(), false);
  std::vector<int> work{0};
  reachable[0] = true;
  while (!work.empty()) {
    const int block = work.back();
    work.pop_back();
    for (int succ : mf.blocks[static_cast<std::size_t>(block)].succs) {
      if (succ < 0 || succ >= static_cast<int>(mf.blocks.size())) {
        throw BackendError("invalid CFG successor during cleanup");
      }
      if (!reachable[static_cast<std::size_t>(succ)]) {
        reachable[static_cast<std::size_t>(succ)] = true;
        work.push_back(succ);
      }
    }
  }

  std::set<std::string> removedLabels;
  for (std::size_t i = 1; i < mf.blocks.size(); ++i) {
    if (!reachable[i]) {
      removedLabels.insert(mf.blocks[i].label);
    }
  }
  if (removedLabels.empty()) {
    return false;
  }

  removePhiIncomingPreds(mf, removedLabels);
  std::vector<MachineBasicBlock> kept;
  kept.reserve(mf.blocks.size() - removedLabels.size());
  for (std::size_t i = 0; i < mf.blocks.size(); ++i) {
    if (i == 0 || reachable[i]) {
      kept.push_back(std::move(mf.blocks[i]));
    }
  }
  mf.blocks = std::move(kept);
  return true;
}

// 消除一个可删除的空块/跳板块(每次调用至多处理一个,便于外层循环重建 CFG)。
// 关键步骤:求出唯一转发目标;若目标含 PHI 则先检查前驱改写是否安全;
// 把靠落入到达空块的前驱补上显式跳转;将所有指向空块的分支改指目标;
// 相应改写目标块的 PHI 前驱;最后删除空块。成功删除返回 true。
bool removeEmptyBlock(MachineFunction &mf) {
  for (std::size_t i = 1; i < mf.blocks.size(); ++i) {
    if (!isRemovableEmptyBlock(mf.blocks[i])) {
      continue;
    }

    const std::string oldLabel = mf.blocks[i].label;
    const std::string targetLabel = emptyBlockTarget(mf, i);
    if (targetLabel.empty() || targetLabel == oldLabel) {
      continue;
    }
    const int targetIndex = findBlockIndex(mf, targetLabel);
    if (targetIndex < 0) {
      throw BackendError("machine branch targets unknown block " + targetLabel);
    }

    const std::vector<std::string> predLabels = predecessorLabels(mf, i);
    const std::size_t targetBlockIndex = static_cast<std::size_t>(targetIndex);
    const bool targetHasPhi = blockHasPhi(mf.blocks[targetBlockIndex]);
    if (targetHasPhi &&
        !canRewritePhiTargetAfterEmptyBlockRemoval(
            mf.blocks[targetBlockIndex], oldLabel, predLabels)) {
      continue;
    }

    for (int pred : mf.blocks[i].preds) {
      if (pred < 0 || pred >= static_cast<int>(mf.blocks.size()) ||
          pred == static_cast<int>(i)) {
        continue;
      }
      if (blockFallsThroughTo(mf, static_cast<std::size_t>(pred), i)) {
        appendJump(mf.blocks[static_cast<std::size_t>(pred)], targetLabel);
      }
    }

    retargetBranchTargets(mf, oldLabel, targetLabel);
    if (targetHasPhi) {
      rewritePhiIncomingPredsInTarget(mf.blocks[targetBlockIndex], oldLabel,
                                      predLabels);
    } else if (predLabels.empty()) {
      removePhiIncomingPreds(mf, {oldLabel});
    }
    eraseBlockAt(mf, i);
    return true;
  }
  return false;
}

// 合并线性块:把只有单一后继的 pred 与只有单一前驱、不含 PHI 的后继 succ 合并。
// 关键步骤:确认 succ 是唯一后继且靠后于 pred(非入口、非回边到自身);去掉 pred
// 中指向 succ 的跳转;把 succ 指令搬入 pred;若 succ 原本靠落入到达下一块,则在
// pred 末尾补跳转以保持原落入目标;改写以 succ 为前驱的 PHI;删除 succ。
bool mergeLinearBlocks(MachineFunction &mf) {
  for (std::size_t predIndex = 0; predIndex < mf.blocks.size(); ++predIndex) {
    if (mf.blocks[predIndex].succs.size() != 1) {
      continue;
    }
    const int succ = mf.blocks[predIndex].succs.front();
    if (succ < 0 || succ >= static_cast<int>(mf.blocks.size()) ||
        succ == static_cast<int>(predIndex) || succ == 0) {
      continue;
    }
    const std::size_t succIndex = static_cast<std::size_t>(succ);
    if (succIndex <= predIndex) {
      continue;
    }
    if (mf.blocks[succIndex].preds.size() != 1 ||
        blockHasPhi(mf.blocks[succIndex])) {
      continue;
    }

    const std::string succLabel = mf.blocks[succIndex].label;
    const bool movedBlockFallsThrough = blockCanFallThrough(mf.blocks[succIndex]);
    const bool movedFallthroughChanges =
        movedBlockFallsThrough && succIndex + 1 < mf.blocks.size() &&
        succIndex != predIndex + 1;
    const std::string movedFallthroughLabel =
        movedFallthroughChanges ? mf.blocks[succIndex + 1].label : std::string();
    removeTerminatorsTargeting(mf.blocks[predIndex], succLabel);

    std::vector<MachineInstr> moved = std::move(mf.blocks[succIndex].instrs);
    mf.blocks[predIndex].instrs.insert(mf.blocks[predIndex].instrs.end(),
                                       std::make_move_iterator(moved.begin()),
                                       std::make_move_iterator(moved.end()));
    if (!movedFallthroughLabel.empty()) {
      appendJump(mf.blocks[predIndex], movedFallthroughLabel);
    }
    retargetPhiIncomingPreds(mf, succLabel, mf.blocks[predIndex].label);
    eraseBlockAt(mf, succIndex);
    return true;
  }
  return false;
}

// 删除指向紧邻下一块的多余无条件跳转:该跳转与顺序落入等价,可直接去掉。
bool removeRedundantNextJumps(MachineFunction &mf) {
  for (std::size_t i = 0; i + 1 < mf.blocks.size(); ++i) {
    MachineBasicBlock &block = mf.blocks[i];
    const MachineBasicBlock &next = mf.blocks[i + 1];
    if (!blockEndsWithJumpTo(block, next.label)) {
      continue;
    }
    block.instrs.pop_back();
    return true;
  }
  return false;
}

// 按标签线性查找块下标,未找到返回 -1。
int findBlockIndex(const MachineFunction &mf, const std::string &label) {
  for (std::size_t i = 0; i < mf.blocks.size(); ++i) {
    if (mf.blocks[i].label == label) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

// 把全函数中所有终结指令里以 oldLabel 为目标的块操作数改指 newLabel。
void retargetBranchTargets(MachineFunction &mf, const std::string &oldLabel,
                           const std::string &newLabel) {
  for (MachineBasicBlock &block : mf.blocks) {
    for (MachineInstr &instr : block.instrs) {
      if (!isTerminatorInstr(instr)) {
        continue;
      }
      for (MachineOperand &use : instr.uses) {
        if (use.kind == OperandKind::Block && use.blockLabel == oldLabel) {
          use.blockLabel = newLabel;
        }
      }
    }
  }
}

// 从所有 PHI 的传入列表中删除来自 removedLabels 中任一标签的项(值/前驱成对删除)。
void removePhiIncomingPreds(MachineFunction &mf,
                            const std::set<std::string> &removedLabels) {
  for (MachineBasicBlock &block : mf.blocks) {
    for (MachineInstr &instr : block.instrs) {
      if (instr.opcode != Opcode::PHI) {
        continue;
      }
      std::vector<MachineOperand> rewritten;
      for (std::size_t i = 0; i < instr.uses.size(); i += 2) {
        if (i + 1 >= instr.uses.size()) {
          throw BackendError("malformed PHI incoming list");
        }
        const MachineOperand &pred = instr.uses[i + 1];
        if (pred.kind == OperandKind::Block &&
            removedLabels.count(pred.blockLabel) != 0) {
          continue;
        }
        rewritten.push_back(instr.uses[i]);
        rewritten.push_back(pred);
      }
      instr.uses = std::move(rewritten);
    }
  }
}

// 在块末尾追加一条到 label 的无条件跳转 J。
void appendJump(MachineBasicBlock &block, const std::string &label) {
  MachineInstr jump;
  jump.opcode = Opcode::J;
  jump.uses.push_back(MachineOperand::block(label));
  jump.terminator = true;
  block.instrs.push_back(std::move(jump));
}

// 把所有 PHI 传入前驱中等于 oldLabel 的改为 newLabel(仅改前驱标签,不动传入值)。
void retargetPhiIncomingPreds(MachineFunction &mf, const std::string &oldLabel,
                              const std::string &newLabel) {
  for (MachineBasicBlock &block : mf.blocks) {
    for (MachineInstr &instr : block.instrs) {
      if (instr.opcode != Opcode::PHI) {
        continue;
      }
      for (std::size_t i = 1; i < instr.uses.size(); i += 2) {
        MachineOperand &pred = instr.uses[i];
        if (pred.kind == OperandKind::Block && pred.blockLabel == oldLabel) {
          pred.blockLabel = newLabel;
        }
      }
    }
  }
}

} // namespace

// CFG 清理的对外入口:反复应用四类化简(删不可达块、删空块、合并线性块、
// 删多余跳转),每次有改动就重建 CFG,直到不动点为止。返回是否发生过改动。
bool cleanupMachineCFG(MachineFunction &mf) {
  bool changed = false;
  bool localChanged = true;
  rebuildCFG(mf);

  while (localChanged) {
    localChanged = false;
    // 四类化简按优先级依次尝试,每轮只执行首个成功的一种,随后重建 CFG。
    if (removeUnreachableBlocks(mf)) {
      localChanged = true;
    } else if (removeEmptyBlock(mf)) {
      localChanged = true;
    } else if (mergeLinearBlocks(mf)) {
      localChanged = true;
    } else if (removeRedundantNextJumps(mf)) {
      localChanged = true;
    }

    if (localChanged) {
      changed = true;
      rebuildCFG(mf);
    }
  }
  return changed;
}

} // namespace riscv
