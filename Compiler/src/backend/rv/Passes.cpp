#include "../../../include/riscv/Passes.hpp"

#include "../../../include/lib/CFG.hpp"
#include "../../../include/riscv/Target.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace riscv {
namespace {

void appendJump(MachineBasicBlock &block, const std::string &label);
void retargetBranchTargets(MachineFunction &mf, const std::string &oldLabel,
                           const std::string &newLabel);
void removePhiIncomingPreds(MachineFunction &mf,
                            const std::set<std::string> &removedLabels);
void retargetPhiIncomingPreds(MachineFunction &mf, const std::string &oldLabel,
                              const std::string &newLabel);
int findBlockIndex(const MachineFunction &mf, const std::string &label);

bool isTerminator(Opcode opcode) {
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

bool isNoFallthroughTerminator(Opcode opcode) {
  return opcode == Opcode::RET || opcode == Opcode::J;
}

bool isTerminatorInstr(const MachineInstr &instr) {
  return instr.terminator || isTerminator(instr.opcode);
}

bool blockCanFallThrough(const MachineBasicBlock &block) {
  for (auto it = block.instrs.rbegin(); it != block.instrs.rend(); ++it) {
    if (isTerminatorInstr(*it)) {
      return !isNoFallthroughTerminator(it->opcode);
    }
  }
  return true;
}

bool isJumpTo(const MachineInstr &instr, const std::string &label) {
  return instr.opcode == Opcode::J && !instr.uses.empty() &&
         instr.uses.front().kind == OperandKind::Block &&
         instr.uses.front().blockLabel == label;
}

bool blockEndsWithJumpTo(const MachineBasicBlock &block,
                         const std::string &label) {
  return !block.instrs.empty() && isJumpTo(block.instrs.back(), label);
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

bool blockHasPhi(const MachineBasicBlock &block) {
  return std::any_of(block.instrs.begin(), block.instrs.end(),
                     [](const MachineInstr &instr) {
                       return instr.opcode == Opcode::PHI;
                     });
}

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

bool blockFallsThroughTo(const MachineFunction &mf, std::size_t predIndex,
                         std::size_t succIndex) {
  return predIndex + 1 == succIndex && predIndex < mf.blocks.size() &&
         blockCanFallThrough(mf.blocks[predIndex]);
}

void eraseBlockAt(MachineFunction &mf, std::size_t blockIndex) {
  mf.blocks.erase(mf.blocks.begin() + static_cast<long>(blockIndex));
}

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

bool labelInList(const std::vector<std::string> &labels,
                 const std::string &label) {
  return std::find(labels.begin(), labels.end(), label) != labels.end();
}

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

bool isFloatType(const std::shared_ptr<IRType> &type) {
  return type && type->getEnum() == IR_FLOAT;
}

bool isPointerType(const std::shared_ptr<IRType> &type) {
  return type && type->getEnum() == IR_POINTER;
}

RegClass regClassForValue(Value *value) {
  if (!value || !value->getIRType()) {
    throw BackendError("missing value type while running machine pass");
  }
  switch (value->getIRType()->getEnum()) {
  case IR_FLOAT:
    return RegClass::FPR32;
  case IR_POINTER:
    return RegClass::GPR64;
  case IR_INT:
    return RegClass::GPR32;
  default:
    throw BackendError("unsupported value type while running machine pass");
  }
}

int findBlockIndex(const MachineFunction &mf, const std::string &label) {
  for (std::size_t i = 0; i < mf.blocks.size(); ++i) {
    if (mf.blocks[i].label == label) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void addCFGEdge(MachineFunction &mf, int from, const std::string &toLabel) {
  const int to = findBlockIndex(mf, toLabel);
  if (to < 0) {
    throw BackendError("machine branch targets unknown block " + toLabel);
  }
  auto &succs = mf.blocks[from].succs;
  if (std::find(succs.begin(), succs.end(), to) == succs.end()) {
    succs.push_back(to);
  }
  auto &preds = mf.blocks[to].preds;
  if (std::find(preds.begin(), preds.end(), from) == preds.end()) {
    preds.push_back(from);
  }
}

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

void appendJump(MachineBasicBlock &block, const std::string &label) {
  MachineInstr jump;
  jump.opcode = Opcode::J;
  jump.uses.push_back(MachineOperand::block(label));
  jump.terminator = true;
  block.instrs.push_back(std::move(jump));
}

std::size_t firstTerminatorIndex(const MachineBasicBlock &block) {
  for (std::size_t i = 0; i < block.instrs.size(); ++i) {
    if (block.instrs[i].terminator || isTerminator(block.instrs[i].opcode)) {
      return i;
    }
  }
  return block.instrs.size();
}

void insertBeforeTerminators(MachineBasicBlock &block,
                             std::vector<MachineInstr> copies) {
  const std::size_t pos = firstTerminatorIndex(block);
  block.instrs.insert(block.instrs.begin() + static_cast<long>(pos),
                      copies.begin(), copies.end());
}

MachineReg materializeConstant(MachineFunction &mf,
                               std::vector<MachineInstr> &instrs,
                               Value *value) {
  int64_t intValue = 0;
  bool hasInt = false;
  if (auto *constant = dynamic_cast<ConstInt *>(value)) {
    intValue = constant->getVal();
    hasInt = true;
  } else if (auto *constant = dynamic_cast<ConstBool *>(value)) {
    intValue = constant->getVal() ? 1 : 0;
    hasInt = true;
  } else if (dynamic_cast<ConstPtr *>(value)) {
    intValue = 0;
    hasInt = true;
  } else if (dynamic_cast<UndefValue *>(value) && value->getIRType() &&
             (value->getIRType()->getEnum() == IR_INT ||
              value->getIRType()->getEnum() == IR_POINTER)) {
    intValue = 0;
    hasInt = true;
  }

  if (hasInt) {
    if (intValue == 0) {
      return MachineReg::preg(PhysReg::X0, regClassForValue(value));
    }
    MachineReg reg = mf.createVReg(regClassForValue(value));
    MachineInstr li;
    li.opcode = Opcode::LI;
    li.defs.push_back(MachineOperand::reg(reg));
    li.uses.push_back(MachineOperand::imm(intValue));
    instrs.push_back(std::move(li));
    return reg;
  }

  if (auto *constant = dynamic_cast<ConstFloat *>(value)) {
    uint32_t bits = 0;
    float raw = constant->getVal();
    std::memcpy(&bits, &raw, sizeof(bits));

    MachineReg bitsReg = mf.createVReg(RegClass::GPR64);
    MachineInstr li;
    li.opcode = Opcode::LI;
    li.defs.push_back(MachineOperand::reg(bitsReg));
    li.uses.push_back(MachineOperand::imm(static_cast<int64_t>(bits)));
    instrs.push_back(std::move(li));

    MachineReg floatReg = mf.createVReg(RegClass::FPR32);
    MachineInstr move;
    move.opcode = Opcode::FMV_W_X;
    move.defs.push_back(MachineOperand::reg(floatReg));
    move.uses.push_back(MachineOperand::reg(bitsReg));
    instrs.push_back(std::move(move));
    return floatReg;
  }

  if (dynamic_cast<UndefValue *>(value) && isFloatType(value->getIRType())) {
    MachineReg bitsReg = mf.createVReg(RegClass::GPR64);
    MachineInstr li;
    li.opcode = Opcode::LI;
    li.defs.push_back(MachineOperand::reg(bitsReg));
    li.uses.push_back(MachineOperand::imm(0));
    instrs.push_back(std::move(li));

    MachineReg floatReg = mf.createVReg(RegClass::FPR32);
    MachineInstr move;
    move.opcode = Opcode::FMV_W_X;
    move.defs.push_back(MachineOperand::reg(floatReg));
    move.uses.push_back(MachineOperand::reg(bitsReg));
    instrs.push_back(std::move(move));
    return floatReg;
  }

  std::string detail = value ? value->getIdent() : "<null>";
  if (detail.empty()) {
    detail = "<unnamed>";
  }
  if (value && value->getIRType()) {
    detail += " type=" + std::to_string(value->getIRType()->getEnum());
  }
  if (auto *instruction = dynamic_cast<Instruction *>(value)) {
    detail += " inst=" + std::to_string(static_cast<int>(instruction->instType));
  }
  throw BackendError("unsupported PHI constant incoming value " + detail);
}

MachineReg materializePointerValue(MachineFunction &mf,
                                   std::vector<MachineInstr> &instrs,
                                   Value *value) {
  auto stackObject = mf.valueStackObjects.find(value);
  if (stackObject != mf.valueStackObjects.end()) {
    MachineReg reg = mf.createVReg(RegClass::GPR64);
    MachineInstr addr;
    addr.opcode = Opcode::ADDI;
    addr.defs.push_back(MachineOperand::reg(reg));
    addr.uses.push_back(MachineOperand::frameIndex(stackObject->second));
    addr.uses.push_back(MachineOperand::imm(0));
    instrs.push_back(std::move(addr));
    return reg;
  }

  if (auto *global = dynamic_cast<Variable *>(value); global &&
                                                     global->isGlobal()) {
    MachineReg reg = mf.createVReg(RegClass::GPR64);
    MachineInstr addr;
    addr.opcode = Opcode::LA;
    addr.defs.push_back(MachineOperand::reg(reg));
    addr.uses.push_back(MachineOperand::global(global->getIdent()));
    instrs.push_back(std::move(addr));
    return reg;
  }

  if (dynamic_cast<ConstPtr *>(value)) {
    return materializeConstant(mf, instrs, value);
  }

  return materializeConstant(mf, instrs, value);
}

MachineReg valueRegInBlock(MachineFunction &mf, MachineBasicBlock &block,
                           std::vector<MachineInstr> &instrs, Value *value) {
  (void)block;
  auto mapped = mf.valueRegs.find(value);
  if (mapped != mf.valueRegs.end()) {
    return mapped->second;
  }
  if (value && value->getIRType() && value->getIRType()->getEnum() == IR_POINTER) {
    return materializePointerValue(mf, instrs, value);
  }
  return materializeConstant(mf, instrs, value);
}

MachineInstr makeParallelCopy(MachineReg dst, MachineReg src) {
  MachineInstr copy;
  copy.opcode = Opcode::PARALLEL_COPY;
  copy.defs.push_back(MachineOperand::reg(dst));
  copy.uses.push_back(MachineOperand::reg(src));
  return copy;
}

MachineInstr makeCopy(MachineReg dst, MachineReg src) {
  MachineInstr copy;
  copy.opcode = dst.rc == RegClass::FPR32 ? Opcode::F_COPY : Opcode::COPY;
  copy.defs.push_back(MachineOperand::reg(dst));
  copy.uses.push_back(MachineOperand::reg(src));
  return copy;
}

void replaceBranchTarget(MachineBasicBlock &block, const std::string &oldLabel,
                         const std::string &newLabel) {
  bool replaced = false;
  for (MachineInstr &instr : block.instrs) {
    if (!isTerminator(instr.opcode)) {
      continue;
    }
    for (MachineOperand &use : instr.uses) {
      if (use.kind == OperandKind::Block && use.blockLabel == oldLabel) {
        use.blockLabel = newLabel;
        replaced = true;
      }
    }
  }
  if (!replaced) {
    if (!blockCanFallThrough(block)) {
      throw BackendError("failed to split machine CFG edge");
    }
    appendJump(block, newLabel);
  }
}

std::vector<MachineInstr> buildPhiCopies(MachineFunction &mf,
                                         MachineBasicBlock &insertionBlock,
                                         const MachineBasicBlock &phiBlock,
                                         const std::string &predLabel) {
  struct CopyPair {
    MachineReg dst;
    MachineReg src;
  };

  std::vector<MachineInstr> copies;
  std::vector<CopyPair> pendingCopies;
  for (const MachineInstr &phi : phiBlock.instrs) {
    if (phi.opcode != Opcode::PHI) {
      continue;
    }
    if (phi.defs.empty()) {
      throw BackendError("malformed PHI without def");
    }
    for (std::size_t i = 0; i + 1 < phi.uses.size(); i += 2) {
      const MachineOperand &value = phi.uses[i];
      const MachineOperand &pred = phi.uses[i + 1];
      if (pred.kind != OperandKind::Block || pred.blockLabel != predLabel) {
        continue;
      }
      if (value.kind != OperandKind::ValueRef) {
        throw BackendError("malformed PHI incoming value");
      }
      MachineReg src =
          valueRegInBlock(mf, insertionBlock, copies, value.valueRef);
      pendingCopies.push_back({phi.defs.front().regValue, src});
    }
  }

  std::vector<CopyPair> tempCopies;
  tempCopies.reserve(pendingCopies.size());
  for (const CopyPair &copy : pendingCopies) {
    MachineReg temp = mf.createVReg(copy.dst.rc);
    copies.push_back(makeCopy(temp, copy.src));
    tempCopies.push_back({copy.dst, temp});
  }
  for (const CopyPair &copy : tempCopies) {
    copies.push_back(makeCopy(copy.dst, copy.src));
  }
  return copies;
}

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

void rebuildCFG(MachineFunction &mf) {
  for (MachineBasicBlock &block : mf.blocks) {
    block.preds.clear();
    block.succs.clear();
  }

  for (std::size_t i = 0; i < mf.blocks.size(); ++i) {
    for (const MachineInstr &instr : mf.blocks[i].instrs) {
      if (!isTerminatorInstr(instr)) {
        continue;
      }
      for (const MachineOperand &use : instr.uses) {
        if (use.kind == OperandKind::Block) {
          addCFGEdge(mf, static_cast<int>(i), use.blockLabel);
        }
      }
    }
    if (blockCanFallThrough(mf.blocks[i]) && i + 1 < mf.blocks.size()) {
      addCFGEdge(mf, static_cast<int>(i), mf.blocks[i + 1].label);
    }
  }
}

bool cleanupMachineCFG(MachineFunction &mf) {
  bool changed = false;
  bool localChanged = true;
  rebuildCFG(mf);

  while (localChanged) {
    localChanged = false;
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

void eliminatePhiNodes(MachineFunction &mf) {
  rebuildCFG(mf);

  for (std::size_t blockIndex = 0; blockIndex < mf.blocks.size(); ++blockIndex) {
    bool hasPhi = false;
    for (const MachineInstr &instr : mf.blocks[blockIndex].instrs) {
      if (instr.opcode == Opcode::PHI) {
        hasPhi = true;
        break;
      }
    }
    if (!hasPhi) {
      continue;
    }

    const std::string blockLabel = mf.blocks[blockIndex].label;
    const std::vector<int> preds = mf.blocks[blockIndex].preds;
    for (int predIndex : preds) {
      const std::string predLabel = mf.blocks[predIndex].label;
      if (mf.blocks[predIndex].succs.size() <= 1) {
        auto copies = buildPhiCopies(mf, mf.blocks[predIndex],
                                     mf.blocks[blockIndex], predLabel);
        insertBeforeTerminators(mf.blocks[predIndex], std::move(copies));
        continue;
      }

      MachineBasicBlock split;
      split.label = predLabel + "_to_" + blockLabel + "_phi";
      auto copies = buildPhiCopies(mf, split, mf.blocks[blockIndex], predLabel);
      split.instrs.insert(split.instrs.end(), copies.begin(), copies.end());
      appendJump(split, blockLabel);

      replaceBranchTarget(mf.blocks[predIndex], blockLabel, split.label);
      mf.blocks.push_back(std::move(split));
    }

    MachineBasicBlock &phiBlock = mf.blocks[blockIndex];
    phiBlock.instrs.erase(
        std::remove_if(phiBlock.instrs.begin(), phiBlock.instrs.end(),
                       [](const MachineInstr &instr) {
                         return instr.opcode == Opcode::PHI;
                       }),
        phiBlock.instrs.end());
    rebuildCFG(mf);
  }
}

void lowerSelectPseudo(MachineFunction &mf) {
  int selectId = 0;
  for (std::size_t blockIndex = 0; blockIndex < mf.blocks.size(); ++blockIndex) {
    MachineBasicBlock &block = mf.blocks[blockIndex];
    auto selectIt = std::find_if(block.instrs.begin(), block.instrs.end(),
                                 [](const MachineInstr &instr) {
                                   return instr.opcode == Opcode::SELECT;
                                 });
    if (selectIt == block.instrs.end()) {
      continue;
    }
    if (selectIt->defs.empty() || selectIt->uses.size() < 3) {
      throw BackendError("malformed SELECT pseudo");
    }

    MachineInstr select = *selectIt;
    std::vector<MachineInstr> tail(selectIt + 1, block.instrs.end());
    block.instrs.erase(selectIt, block.instrs.end());

    const std::string base =
        block.label + ".select" + std::to_string(selectId++);
    MachineBasicBlock trueBlock;
    trueBlock.label = base + ".true";
    MachineBasicBlock falseBlock;
    falseBlock.label = base + ".false";
    MachineBasicBlock doneBlock;
    doneBlock.label = base + ".done";
    retargetPhiIncomingPreds(mf, block.label, doneBlock.label);

    MachineInstr branch;
    branch.opcode = Opcode::BNEZ;
    branch.uses.push_back(select.uses[0]);
    branch.uses.push_back(MachineOperand::block(trueBlock.label));
    branch.terminator = true;
    block.instrs.push_back(std::move(branch));
    appendJump(block, falseBlock.label);

    trueBlock.instrs.push_back(makeParallelCopy(select.defs.front().regValue,
                                                select.uses[1].regValue));
    appendJump(trueBlock, doneBlock.label);

    falseBlock.instrs.push_back(makeParallelCopy(select.defs.front().regValue,
                                                 select.uses[2].regValue));
    appendJump(falseBlock, doneBlock.label);

    doneBlock.instrs = std::move(tail);

    auto insertPos = mf.blocks.begin() + static_cast<long>(blockIndex + 1);
    insertPos = mf.blocks.insert(insertPos, std::move(doneBlock));
    insertPos = mf.blocks.insert(insertPos, std::move(falseBlock));
    mf.blocks.insert(insertPos, std::move(trueBlock));
  }
  rebuildCFG(mf);
}

} // namespace riscv
