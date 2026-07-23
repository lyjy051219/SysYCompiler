#include "riscv/Passes.hpp"

#include "lib/CFG.hpp"
#include "riscv/Target.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace riscv {
namespace {

void appendJump(MachineBasicBlock &block, const std::string &label);
void retargetPhiIncomingPreds(MachineFunction &mf, const std::string &oldLabel,
                              const std::string &newLabel);
int findBlockIndex(const MachineFunction &mf, const std::string &label);

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
    detail +=
        " inst=" + std::to_string(static_cast<int>(instruction->instType));
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

  if (auto *global = dynamic_cast<Variable *>(value);
      global && global->isGlobal()) {
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
  if (value && value->getIRType() &&
      value->getIRType()->getEnum() == IR_POINTER) {
    return materializePointerValue(mf, instrs, value);
  }
  return materializeConstant(mf, instrs, value);
}

bool sameMachineReg(const MachineReg &lhs, const MachineReg &rhs) {
  return lhs.physical == rhs.physical && lhs.id == rhs.id && lhs.rc == rhs.rc &&
         lhs.phys == rhs.phys;
}

bool isGPR(MachineReg reg) {
  return reg.rc == RegClass::GPR32 || reg.rc == RegClass::GPR64;
}

bool isKnownBoolean(const std::vector<MachineReg> &known,
                    const MachineReg &reg) {
  if (reg.physical && reg.phys == PhysReg::X0) {
    return true;
  }
  return std::find_if(known.begin(), known.end(), [&](const MachineReg &item) {
           return sameMachineReg(item, reg);
         }) != known.end();
}

void forgetKnownBoolean(std::vector<MachineReg> &known, const MachineReg &reg) {
  known.erase(std::remove_if(known.begin(), known.end(),
                             [&](const MachineReg &item) {
                               return sameMachineReg(item, reg);
                             }),
              known.end());
}

void forgetDefinedRegisters(std::vector<MachineReg> &known,
                            const MachineInstr &instr) {
  for (const MachineOperand &def : instr.defs) {
    if (def.kind == OperandKind::Reg) {
      forgetKnownBoolean(known, def.regValue);
    }
  }
  for (PhysReg def : instr.implicitDefs) {
    MachineReg physical = MachineReg::preg(def, RegClass::GPR64);
    forgetKnownBoolean(known, physical);
    physical.rc = RegClass::GPR32;
    forgetKnownBoolean(known, physical);
    physical.rc = RegClass::FPR32;
    forgetKnownBoolean(known, physical);
  }
}

bool isRegUse(const MachineInstr &instr, std::size_t index) {
  return index < instr.uses.size() &&
         instr.uses[index].kind == OperandKind::Reg;
}

bool producesKnownBoolean(const MachineInstr &instr,
                          const std::vector<MachineReg> &known) {
  if (instr.defs.size() != 1 || instr.defs.front().kind != OperandKind::Reg ||
      !isGPR(instr.defs.front().regValue) || instr.sideEffects ||
      instr.terminator) {
    return false;
  }

  switch (instr.opcode) {
  case Opcode::SEQZ:
  case Opcode::SNEZ:
  case Opcode::SLT:
  case Opcode::SLTU:
  case Opcode::FEQ_S:
  case Opcode::FLT_S:
  case Opcode::FLE_S:
    return true;
  case Opcode::LI:
    return instr.uses.size() == 1 &&
           instr.uses.front().kind == OperandKind::Imm &&
           (instr.uses.front().immValue == 0 ||
            instr.uses.front().immValue == 1);
  case Opcode::COPY:
    return isRegUse(instr, 0) && isKnownBoolean(known, instr.uses[0].regValue);
  case Opcode::ANDI:
    return instr.uses.size() == 2 && isRegUse(instr, 0) &&
           instr.uses[1].kind == OperandKind::Imm &&
           (instr.uses[1].immValue == 0 || instr.uses[1].immValue == 1);
  case Opcode::ORI:
  case Opcode::XORI:
    return instr.uses.size() == 2 && isRegUse(instr, 0) &&
           instr.uses[1].kind == OperandKind::Imm &&
           (instr.uses[1].immValue == 0 || instr.uses[1].immValue == 1) &&
           isKnownBoolean(known, instr.uses[0].regValue);
  case Opcode::AND:
  case Opcode::OR:
  case Opcode::XOR:
    return isRegUse(instr, 0) && isRegUse(instr, 1) &&
           isKnownBoolean(known, instr.uses[0].regValue) &&
           isKnownBoolean(known, instr.uses[1].regValue);
  default:
    return false;
  }
}

MachineInstr makeRegInstr(Opcode opcode, MachineReg dst,
                          std::initializer_list<MachineReg> operands) {
  MachineInstr instr;
  instr.opcode = opcode;
  instr.defs.push_back(MachineOperand::reg(dst));
  for (MachineReg operand : operands) {
    instr.uses.push_back(MachineOperand::reg(operand));
  }
  return instr;
}

bool findAddWithFalseValue(const std::vector<MachineInstr> &lowered,
                           const MachineReg &trueValue,
                           const MachineReg &falseValue, MachineReg &increment,
                           Opcode &addOpcode) {
  for (auto it = lowered.rbegin(); it != lowered.rend(); ++it) {
    const MachineInstr &candidate = *it;
    if (candidate.defs.size() != 1 ||
        candidate.defs.front().kind != OperandKind::Reg ||
        !sameMachineReg(candidate.defs.front().regValue, trueValue)) {
      continue;
    }
    if ((candidate.opcode != Opcode::ADD && candidate.opcode != Opcode::ADDW) ||
        candidate.uses.size() != 2 || !isRegUse(candidate, 0) ||
        !isRegUse(candidate, 1)) {
      return false;
    }
    if (sameMachineReg(candidate.uses[0].regValue, falseValue)) {
      increment = candidate.uses[1].regValue;
    } else if (sameMachineReg(candidate.uses[1].regValue, falseValue)) {
      increment = candidate.uses[0].regValue;
    } else {
      return false;
    }
    addOpcode = candidate.opcode;
    return increment.rc == falseValue.rc;
  }
  return false;
}

std::map<uint32_t, std::size_t>
collectVirtualUseCounts(const MachineFunction &mf) {
  std::map<uint32_t, std::size_t> counts;
  for (const MachineBasicBlock &block : mf.blocks) {
    for (const MachineInstr &instr : block.instrs) {
      for (const MachineOperand &use : instr.uses) {
        if (use.kind == OperandKind::Reg && !use.regValue.physical) {
          ++counts[use.regValue.id];
        }
      }
    }
  }
  return counts;
}

bool conditionProducerFoldsIntoBranch(
    const std::vector<MachineInstr> &lowered, const MachineReg &condition,
    const std::map<uint32_t, std::size_t> &useCounts) {
  if (condition.physical) {
    return false;
  }
  auto count = useCounts.find(condition.id);
  if (count == useCounts.end() || count->second != 1) {
    return false;
  }

  if (lowered.empty()) {
    return false;
  }
  const MachineInstr &producer = lowered.back();
  if (producer.defs.size() != 1 ||
      producer.defs.front().kind != OperandKind::Reg ||
      !sameMachineReg(producer.defs.front().regValue, condition) ||
      producer.sideEffects || producer.terminator ||
      !producer.implicitDefs.empty() || !producer.implicitUses.empty()) {
    return false;
  }

  // Keep this SELECT decision deliberately local. The later compare-folder may
  // cross a small proven-safe gap, but reproducing that legality scan here
  // would make the lowering cost model depend on instructions not yet emitted.
  switch (producer.opcode) {
  case Opcode::SLT:
  case Opcode::XOR:
    return producer.uses.size() == 2 && isRegUse(producer, 0) &&
           isRegUse(producer, 1);
  case Opcode::SEQZ:
  case Opcode::SNEZ:
    return producer.uses.size() == 1 && isRegUse(producer, 0);
  case Opcode::XORI:
    return producer.uses.size() == 2 && isRegUse(producer, 0) &&
           producer.uses[1].kind == OperandKind::Imm &&
           producer.uses[1].immValue == 0;
  default:
    return false;
  }
}

bool lowerBranchlessSelects(MachineFunction &mf) {
  bool changed = false;
  const auto useCounts = collectVirtualUseCounts(mf);
  for (MachineBasicBlock &block : mf.blocks) {
    std::vector<MachineReg> knownBooleans;
    std::vector<MachineInstr> lowered;
    lowered.reserve(block.instrs.size());

    for (const MachineInstr &instr : block.instrs) {
      if (instr.opcode == Opcode::SELECT && instr.defs.size() == 1 &&
          instr.defs.front().kind == OperandKind::Reg &&
          instr.uses.size() == 3 && isRegUse(instr, 0) && isRegUse(instr, 1) &&
          isRegUse(instr, 2)) {
        const MachineReg dst = instr.defs.front().regValue;
        const MachineReg cond = instr.uses[0].regValue;
        const MachineReg trueValue = instr.uses[1].regValue;
        const MachineReg falseValue = instr.uses[2].regValue;
        if (isGPR(dst) && dst.rc != RegClass::FPR32 &&
            isKnownBoolean(knownBooleans, cond) && trueValue.rc == dst.rc &&
            falseValue.rc == dst.rc) {
          const MachineReg zero = MachineReg::preg(PhysReg::X0, dst.rc);
          MachineReg increment;
          Opcode addOpcode = Opcode::ADDW;
          if (findAddWithFalseValue(lowered, trueValue, falseValue, increment,
                                    addOpcode)) {
            const MachineReg mask = mf.createVReg(dst.rc);
            const MachineReg selected = mf.createVReg(dst.rc);
            lowered.push_back(makeRegInstr(
                dst.rc == RegClass::GPR32 ? Opcode::SUBW : Opcode::SUB, mask,
                {zero, cond}));
            lowered.push_back(
                makeRegInstr(Opcode::AND, selected, {increment, mask}));
            lowered.push_back(
                makeRegInstr(addOpcode, dst, {falseValue, selected}));
          } else if (!conditionProducerFoldsIntoBranch(lowered, cond,
                                                       useCounts)) {
            const MachineReg mask = mf.createVReg(dst.rc);
            const MachineReg difference = mf.createVReg(dst.rc);
            const MachineReg selected = mf.createVReg(dst.rc);
            lowered.push_back(makeRegInstr(
                dst.rc == RegClass::GPR32 ? Opcode::SUBW : Opcode::SUB, mask,
                {zero, cond}));
            lowered.push_back(
                makeRegInstr(Opcode::XOR, difference, {trueValue, falseValue}));
            lowered.push_back(
                makeRegInstr(Opcode::AND, selected, {difference, mask}));
            lowered.push_back(
                makeRegInstr(Opcode::XOR, dst, {selected, falseValue}));
          } else {
            forgetKnownBoolean(knownBooleans, dst);
            lowered.push_back(instr);
            continue;
          }
          forgetKnownBoolean(knownBooleans, dst);
          changed = true;
          continue;
        }
        forgetKnownBoolean(knownBooleans, dst);
        lowered.push_back(instr);
        continue;
      }

      forgetDefinedRegisters(knownBooleans, instr);
      lowered.push_back(instr);
      if (producesKnownBoolean(instr, knownBooleans) &&
          instr.defs.front().kind == OperandKind::Reg) {
        knownBooleans.push_back(instr.defs.front().regValue);
      }
    }
    block.instrs = std::move(lowered);
  }
  return changed;
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
  copy.phiCopy = true;
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

  auto SameReg = [](const MachineReg &A, const MachineReg &B) {
    return A.physical == B.physical && A.id == B.id && A.rc == B.rc &&
           A.phys == B.phys;
  };

  pendingCopies.erase(
      std::remove_if(pendingCopies.begin(), pendingCopies.end(),
                     [&](const CopyPair &Copy) {
                       return SameReg(Copy.dst, Copy.src);
                     }),
      pendingCopies.end());

  const std::size_t serializedCopyBegin = copies.size();
  bool hasCycle = false;
  while (!pendingCopies.empty()) {
    auto Ready = pendingCopies.end();
    for (auto It = pendingCopies.begin(); It != pendingCopies.end(); ++It) {
      const bool DestinationStillNeeded =
          std::any_of(pendingCopies.begin(), pendingCopies.end(),
                      [&](const CopyPair &Other) {
                        return SameReg(It->dst, Other.src);
                      });
      if (!DestinationStillNeeded) {
        Ready = It;
        break;
      }
    }

    if (Ready != pendingCopies.end()) {
      copies.push_back(makeCopy(Ready->dst, Ready->src));
      pendingCopies.erase(Ready);
      continue;
    }

    // A remaining cycle needs one saved old value. Replacing every use of the
    // chosen source breaks the cycle without penalizing acyclic copy chains.
    hasCycle = true;
    const MachineReg CycleSource = pendingCopies.front().src;
    MachineReg Temp = mf.createVReg(CycleSource.rc);
    copies.push_back(makeCopy(Temp, CycleSource));
    for (CopyPair &Copy : pendingCopies) {
      if (SameReg(Copy.src, CycleSource))
        Copy.src = Temp;
    }
  }

  if (hasCycle) {
    // Keep the whole serialized bundle intact.  Marking only the temporary
    // save is insufficient: coalescing one of the rotations can recreate the
    // parallel-copy cycle that the temporary was introduced to break.
    for (std::size_t index = serializedCopyBegin; index < copies.size();
         ++index) {
      if (copies[index].opcode == Opcode::COPY ||
          copies[index].opcode == Opcode::F_COPY) {
        copies[index].copyCoalescingBarrier = true;
      }
    }
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

void eliminatePhiNodes(MachineFunction &mf) {
  rebuildCFG(mf);

  for (std::size_t blockIndex = 0; blockIndex < mf.blocks.size();
       ++blockIndex) {
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
    phiBlock.instrs.erase(std::remove_if(phiBlock.instrs.begin(),
                                         phiBlock.instrs.end(),
                                         [](const MachineInstr &instr) {
                                           return instr.opcode == Opcode::PHI;
                                         }),
                          phiBlock.instrs.end());
    rebuildCFG(mf);
  }
}

bool lowerSelectPseudo(MachineFunction &mf, bool branchlessOnly) {
  if (branchlessOnly) {
    return lowerBranchlessSelects(mf);
  }

  int selectId = 0;
  bool changed = false;
  for (std::size_t blockIndex = 0; blockIndex < mf.blocks.size();
       ++blockIndex) {
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
    changed = true;
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
  return changed;
}

} // namespace riscv
