#include "../../../include/riscv/Passes.hpp"

#include "../../../include/lib/CFG.hpp"
#include "../../../include/riscv/Target.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace riscv {
namespace {

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
    throw BackendError("failed to split machine CFG edge");
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

void MachineFunctionPassManager::addPass(Pass pass) {
  if (pass) {
    passes_.push_back(pass);
  }
}

bool MachineFunctionPassManager::run(MachineFunction &mf) const {
  bool changed = false;
  for (Pass pass : passes_) {
    changed = pass(mf) || changed;
  }
  return changed;
}

void rebuildCFG(MachineFunction &mf) {
  for (MachineBasicBlock &block : mf.blocks) {
    block.preds.clear();
    block.succs.clear();
  }

  for (std::size_t i = 0; i < mf.blocks.size(); ++i) {
    for (const MachineInstr &instr : mf.blocks[i].instrs) {
      if (!isTerminator(instr.opcode)) {
        continue;
      }
      for (const MachineOperand &use : instr.uses) {
        if (use.kind == OperandKind::Block) {
          addCFGEdge(mf, static_cast<int>(i), use.blockLabel);
        }
      }
    }
  }
}

void cleanupMachineCFG(MachineFunction &mf) {
  (void)mf;
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

void runPreRAOptimizations(MachineFunction &mf) {
  MachineFunctionPassManager pipeline;
  (void)pipeline.run(mf);
}

void runPostRAOptimizations(MachineFunction &mf) {
  MachineFunctionPassManager pipeline;
  pipeline.addPass(runPostRAPeephole);
  (void)pipeline.run(mf);
}

} // namespace riscv
