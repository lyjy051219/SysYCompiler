#include "aarch64/opt/MachineOptPasses.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace aarch64 {
namespace {

bool sameRegister(const MachineReg &lhs, const MachineReg &rhs) {
  return lhs.physical == rhs.physical && lhs.id == rhs.id && lhs.rc == rhs.rc &&
         lhs.phys == rhs.phys;
}

bool hasSingleRegDefUse(const MachineInstr &instr) {
  return instr.defs.size() == 1 && instr.uses.size() == 1 &&
         instr.defs.front().kind == OperandKind::Reg &&
         instr.uses.front().kind == OperandKind::Reg;
}

bool hasSingleRegDef(const MachineInstr &instr) {
  return instr.defs.size() == 1 && instr.defs.front().kind == OperandKind::Reg;
}

bool isRegUse(const MachineInstr &instr, std::size_t index) {
  return index < instr.uses.size() &&
         instr.uses[index].kind == OperandKind::Reg;
}

bool isZeroReg(const MachineOperand &operand) {
  return operand.kind == OperandKind::Reg && operand.regValue.physical &&
         operand.regValue.phys == PhysReg::X0;
}

bool isSafeToRewrite(const MachineInstr &instr) {
  return !instr.sideEffects && !instr.terminator &&
         instr.implicitDefs.empty() && instr.implicitUses.empty() &&
         hasSingleRegDef(instr);
}

MachineInstr makeCopyFrom(const MachineInstr &instr,
                          MachineOperand sourceOperand) {
  MachineInstr copy;
  copy.opcode = Opcode::COPY;
  copy.defs = instr.defs;
  copy.uses.push_back(std::move(sourceOperand));
  copy.comment = instr.comment;
  return copy;
}

MachineOperand zeroForDef(const MachineInstr &instr) {
  return MachineOperand::reg(
      MachineReg::preg(PhysReg::X0, instr.defs.front().regValue.rc));
}

bool isRedundantCopy(const MachineInstr &instr) {
  if (instr.sideEffects || instr.terminator) {
    return false;
  }
  if (instr.opcode != Opcode::COPY && instr.opcode != Opcode::F_COPY &&
      instr.opcode != Opcode::PARALLEL_COPY) {
    return false;
  }
  return hasSingleRegDefUse(instr) &&
         sameRegister(instr.defs.front().regValue, instr.uses.front().regValue);
}

bool foldAddImmediateZero(const MachineInstr &instr, MachineInstr &folded) {
  if (!isSafeToRewrite(instr) ||
      (instr.opcode != Opcode::ADDI && instr.opcode != Opcode::ADDIW &&
       instr.opcode != Opcode::ORI && instr.opcode != Opcode::XORI &&
       instr.opcode != Opcode::SLLI && instr.opcode != Opcode::SLLIW &&
       instr.opcode != Opcode::SRAI && instr.opcode != Opcode::SRAIW &&
       instr.opcode != Opcode::SRLIW) ||
      instr.uses.size() != 2 || !isRegUse(instr, 0) ||
      instr.uses[1].kind != OperandKind::Imm || instr.uses[1].immValue != 0) {
    return false;
  }
  folded = makeCopyFrom(instr, instr.uses[0]);
  return true;
}

bool foldBinaryZeroIdentity(const MachineInstr &instr, MachineInstr &folded) {
  if (!isSafeToRewrite(instr) || instr.uses.size() != 2 ||
      !isRegUse(instr, 0) || !isRegUse(instr, 1)) {
    return false;
  }

  if (instr.opcode == Opcode::ADD || instr.opcode == Opcode::ADDW ||
      instr.opcode == Opcode::OR || instr.opcode == Opcode::XOR) {
    if (isZeroReg(instr.uses[0])) {
      folded = makeCopyFrom(instr, instr.uses[1]);
      return true;
    }
    if (isZeroReg(instr.uses[1])) {
      folded = makeCopyFrom(instr, instr.uses[0]);
      return true;
    }
  }

  if ((instr.opcode == Opcode::SUB || instr.opcode == Opcode::SUBW ||
       instr.opcode == Opcode::SLL || instr.opcode == Opcode::SLLW ||
       instr.opcode == Opcode::SRAW || instr.opcode == Opcode::SRLW) &&
      isZeroReg(instr.uses[1])) {
    folded = makeCopyFrom(instr, instr.uses[0]);
    return true;
  }

  if (instr.opcode == Opcode::AND) {
    if (isZeroReg(instr.uses[0])) {
      folded = makeCopyFrom(instr, zeroForDef(instr));
      return true;
    }
    if (isZeroReg(instr.uses[1])) {
      folded = makeCopyFrom(instr, zeroForDef(instr));
      return true;
    }
  }

  if (instr.opcode == Opcode::MUL || instr.opcode == Opcode::MULW) {
    if (isZeroReg(instr.uses[0]) || isZeroReg(instr.uses[1])) {
      folded = makeCopyFrom(instr, zeroForDef(instr));
      return true;
    }
  }

  if ((instr.opcode == Opcode::SLL || instr.opcode == Opcode::SLLW ||
       instr.opcode == Opcode::SRAW || instr.opcode == Opcode::SRLW) &&
      isZeroReg(instr.uses[0])) {
    folded = makeCopyFrom(instr, zeroForDef(instr));
    return true;
  }

  return false;
}

bool foldSameOperand(const MachineInstr &instr, MachineInstr &folded) {
  if (!isSafeToRewrite(instr) || instr.uses.size() != 2 ||
      !isRegUse(instr, 0) || !isRegUse(instr, 1) ||
      !sameRegister(instr.uses[0].regValue, instr.uses[1].regValue)) {
    return false;
  }

  switch (instr.opcode) {
  case Opcode::AND:
  case Opcode::OR:
    folded = makeCopyFrom(instr, instr.uses[0]);
    return true;
  case Opcode::SUB:
  case Opcode::SUBW:
  case Opcode::XOR:
  case Opcode::SLT:
  case Opcode::SLTU:
    folded = makeCopyFrom(instr, zeroForDef(instr));
    return true;
  default:
    return false;
  }
}

bool foldZeroIdentity(const MachineInstr &instr, MachineInstr &folded) {
  return foldAddImmediateZero(instr, folded) ||
         foldBinaryZeroIdentity(instr, folded) ||
         foldSameOperand(instr, folded);
}

bool isRedundantAddZero(const MachineInstr &instr) {
  if (instr.sideEffects || instr.terminator || instr.opcode != Opcode::ADDI) {
    return false;
  }
  if (instr.defs.size() != 1 || instr.uses.size() != 2 ||
      instr.defs.front().kind != OperandKind::Reg ||
      instr.uses.front().kind != OperandKind::Reg ||
      instr.uses[1].kind != OperandKind::Imm || instr.uses[1].immValue != 0) {
    return false;
  }
  return sameRegister(instr.defs.front().regValue, instr.uses.front().regValue);
}

bool isRemovablePostRAInstr(const MachineInstr &instr) {
  if (!instr.sideEffects && !instr.terminator && instr.opcode == Opcode::NOP) {
    return true;
  }
  return isRedundantCopy(instr) || isRedundantAddZero(instr);
}

bool isVirtualRegOperand(const MachineOperand &operand) {
  return operand.kind == OperandKind::Reg && !operand.regValue.physical;
}

MachineReg
resolveReplacement(MachineReg reg,
                   const std::map<uint32_t, MachineReg> &replacements) {
  while (!reg.physical) {
    auto found = replacements.find(reg.id);
    if (found == replacements.end()) {
      break;
    }
    MachineReg next = found->second;
    if (!next.physical && next.id == reg.id) {
      break;
    }
    reg = next;
  }
  return reg;
}

bool rewriteUseReplacements(
    MachineInstr &instr, const std::map<uint32_t, MachineReg> &replacements) {
  bool changed = false;
  for (MachineOperand &use : instr.uses) {
    if (isVirtualRegOperand(use)) {
      MachineReg resolved = resolveReplacement(use.regValue, replacements);
      if (!sameRegister(use.regValue, resolved)) {
        use.regValue = resolved;
        changed = true;
      }
    }
  }
  return changed;
}

bool operandAllowedForCSE(const MachineOperand &operand) {
  switch (operand.kind) {
  case OperandKind::Reg:
    return !operand.regValue.physical || operand.regValue.phys == PhysReg::X0;
  case OperandKind::Imm:
  case OperandKind::FrameIndex:
  case OperandKind::Global:
    return true;
  case OperandKind::Block:
  case OperandKind::ValueRef:
    return false;
  }
  return false;
}

std::string regKey(const MachineReg &reg) {
  if (reg.physical) {
    return "p:" + std::to_string(static_cast<int>(reg.phys)) + ":" +
           std::to_string(static_cast<int>(reg.rc));
  }
  return "v:" + std::to_string(reg.id) + ":" +
         std::to_string(static_cast<int>(reg.rc));
}

std::string operandKey(const MachineOperand &operand) {
  switch (operand.kind) {
  case OperandKind::Reg:
    return regKey(operand.regValue);
  case OperandKind::Imm:
    return "i:" + std::to_string(operand.immValue);
  case OperandKind::FrameIndex:
    return "f:" + std::to_string(operand.frameIndexValue);
  case OperandKind::Global:
    return "g:" + operand.symbol;
  case OperandKind::Block:
    return "b:" + operand.blockLabel;
  case OperandKind::ValueRef:
    return "value";
  }
  return "?";
}

bool isCommutativeOpcode(Opcode opcode) {
  switch (opcode) {
  case Opcode::ADD:
  case Opcode::ADDW:
  case Opcode::MUL:
  case Opcode::MULW:
  case Opcode::AND:
  case Opcode::OR:
  case Opcode::XOR:
  case Opcode::FADD_S:
  case Opcode::FMUL_S:
    return true;
  default:
    return false;
  }
}

bool isLoadOpcode(Opcode opcode) {
  return opcode == Opcode::LBU || opcode == Opcode::LW ||
         opcode == Opcode::LD || opcode == Opcode::FLW;
}

bool isPureCSEOpcode(Opcode opcode) {
  switch (opcode) {
  case Opcode::LI:
  case Opcode::LA:
  case Opcode::ADD:
  case Opcode::ADDW:
  case Opcode::SUB:
  case Opcode::SUBW:
  case Opcode::AND:
  case Opcode::OR:
  case Opcode::XOR:
  case Opcode::SLL:
  case Opcode::SLLW:
  case Opcode::SRLW:
  case Opcode::SRAW:
  case Opcode::SLT:
  case Opcode::SLTU:
  case Opcode::SEQZ:
  case Opcode::SNEZ:
  case Opcode::MUL:
  case Opcode::MULW:
  case Opcode::ADDI:
  case Opcode::ADDIW:
  case Opcode::ANDI:
  case Opcode::ORI:
  case Opcode::XORI:
  case Opcode::SLLI:
  case Opcode::SLLIW:
  case Opcode::SRLIW:
  case Opcode::SRAI:
  case Opcode::SRAIW:
  case Opcode::FADD_S:
  case Opcode::FSUB_S:
  case Opcode::FMUL_S:
  // ARM64 专用符号扩展：纯函数，可参与 CSE。地址计算里 `sext(index)` 常被
  // 中端为同一索引重复生成，纳入 CSE 后其下游的 SLLI/ADD 地址链也随之合并，
  // 消除数组访问里成片重复的地址物化（crypto 家族尤其明显）。
  case Opcode::SXTW:
    return true;
  default:
    return false;
  }
}

bool isLocalCSECandidate(const MachineInstr &instr) {
  if (instr.sideEffects || instr.terminator || instr.implicitDefs.size() != 0 ||
      instr.implicitUses.size() != 0 || instr.defs.size() != 1 ||
      !isVirtualRegOperand(instr.defs.front())) {
    return false;
  }
  if (!isPureCSEOpcode(instr.opcode)) {
    return false;
  }
  return std::all_of(instr.uses.begin(), instr.uses.end(),
                     operandAllowedForCSE);
}

std::string cseKey(const MachineInstr &instr) {
  std::vector<std::string> uses;
  uses.reserve(instr.uses.size());
  for (const MachineOperand &use : instr.uses) {
    uses.push_back(operandKey(use));
  }
  if (isCommutativeOpcode(instr.opcode) && uses.size() == 2) {
    std::sort(uses.begin(), uses.end());
  }

  std::string key =
      std::to_string(static_cast<int>(instr.opcode)) + ":" +
      std::to_string(static_cast<int>(instr.defs.front().regValue.rc));
  for (const std::string &use : uses) {
    key += "|" + use;
  }
  return key;
}

using CSETable = std::map<std::string, MachineReg>;
using CSEKeyDependencies = std::map<std::string, std::set<std::string>>;
using CSERegisterDependencies = std::map<std::string, std::set<std::string>>;
constexpr std::size_t kMaxLocalCSEEntries = 64;

std::string cseDependencyKey(const MachineReg &reg) {
  if (reg.physical) {
    return "p:" + std::to_string(static_cast<int>(reg.phys));
  }
  return "v:" + std::to_string(reg.id);
}

void removeCSEKey(const std::string &key, CSETable &exprs, CSETable &loads,
                  CSEKeyDependencies &keyDependencies,
                  CSERegisterDependencies &registerDependencies) {
  exprs.erase(key);
  loads.erase(key);

  auto keyFound = keyDependencies.find(key);
  if (keyFound == keyDependencies.end()) {
    return;
  }
  for (const std::string &reg : keyFound->second) {
    auto regFound = registerDependencies.find(reg);
    if (regFound == registerDependencies.end()) {
      continue;
    }
    regFound->second.erase(key);
    if (regFound->second.empty()) {
      registerDependencies.erase(regFound);
    }
  }
  keyDependencies.erase(keyFound);
}

void clearCSETables(CSETable &exprs, CSETable &loads,
                    CSEKeyDependencies &keyDependencies,
                    CSERegisterDependencies &registerDependencies) {
  exprs.clear();
  loads.clear();
  keyDependencies.clear();
  registerDependencies.clear();
}

bool isMemoryStoreOpcode(Opcode opcode) {
  return opcode == Opcode::SB || opcode == Opcode::SW || opcode == Opcode::SD ||
         opcode == Opcode::FSW;
}

void clearCSELoads(CSETable &loads, CSETable &exprs,
                   CSEKeyDependencies &keyDependencies,
                   CSERegisterDependencies &registerDependencies) {
  const std::vector<std::string> keys = [&] {
    std::vector<std::string> result;
    result.reserve(loads.size());
    for (const auto &[key, reg] : loads) {
      (void)reg;
      result.push_back(key);
    }
    return result;
  }();
  for (const std::string &key : keys) {
    removeCSEKey(key, exprs, loads, keyDependencies, registerDependencies);
  }
}

void recordCSEKey(const std::string &key, const MachineInstr &instr,
                  CSEKeyDependencies &keyDependencies,
                  CSERegisterDependencies &registerDependencies) {
  auto &dependencies = keyDependencies[key];
  for (const MachineOperand &use : instr.uses) {
    if (use.kind != OperandKind::Reg) {
      continue;
    }
    const std::string dependency = cseDependencyKey(use.regValue);
    dependencies.insert(dependency);
    registerDependencies[dependency].insert(key);
  }
}

void invalidateCSERegister(const MachineReg &reg, CSETable &exprs,
                           CSETable &loads, CSEKeyDependencies &keyDependencies,
                           CSERegisterDependencies &registerDependencies,
                           std::map<uint32_t, MachineReg> &replacements) {
  auto dependency = registerDependencies.find(cseDependencyKey(reg));
  if (dependency != registerDependencies.end()) {
    const std::vector<std::string> keys(dependency->second.begin(),
                                        dependency->second.end());
    for (const std::string &key : keys) {
      removeCSEKey(key, exprs, loads, keyDependencies, registerDependencies);
    }
  }

  if (!reg.physical) {
    replacements.erase(reg.id);
  }
  for (auto it = replacements.begin(); it != replacements.end();) {
    if (sameRegister(it->second, reg)) {
      it = replacements.erase(it);
    } else {
      ++it;
    }
  }
}

void invalidateCSEDefinitions(const MachineInstr &instr, CSETable &exprs,
                              CSETable &loads,
                              CSEKeyDependencies &keyDependencies,
                              CSERegisterDependencies &registerDependencies,
                              std::map<uint32_t, MachineReg> &replacements) {
  for (const MachineOperand &def : instr.defs) {
    if (def.kind == OperandKind::Reg) {
      invalidateCSERegister(def.regValue, exprs, loads, keyDependencies,
                            registerDependencies, replacements);
    }
  }
  for (PhysReg def : instr.implicitDefs) {
    invalidateCSERegister(MachineReg::preg(def, RegClass::GPR64), exprs, loads,
                          keyDependencies, registerDependencies, replacements);
    invalidateCSERegister(MachineReg::preg(def, RegClass::FPR32), exprs, loads,
                          keyDependencies, registerDependencies, replacements);
  }
}

std::map<uint32_t, std::set<std::size_t>>
collectUseBlocks(const MachineFunction &mf) {
  std::map<uint32_t, std::set<std::size_t>> useBlocks;
  for (std::size_t blockIndex = 0; blockIndex < mf.blocks.size();
       ++blockIndex) {
    for (const MachineInstr &instr : mf.blocks[blockIndex].instrs) {
      for (const MachineOperand &use : instr.uses) {
        if (isVirtualRegOperand(use)) {
          useBlocks[use.regValue.id].insert(blockIndex);
        }
      }
    }
  }
  return useBlocks;
}

bool allUsesInBlock(
    uint32_t regId, std::size_t blockIndex,
    const std::map<uint32_t, std::set<std::size_t>> &useBlocks) {
  auto found = useBlocks.find(regId);
  if (found == useBlocks.end()) {
    return true;
  }
  return found->second.size() == 1 && found->second.count(blockIndex) != 0;
}

std::map<uint32_t, std::size_t>
collectVirtualUseCounts(const MachineFunction &mf) {
  std::map<uint32_t, std::size_t> counts;
  for (const MachineBasicBlock &block : mf.blocks) {
    for (const MachineInstr &instr : block.instrs) {
      for (const MachineOperand &use : instr.uses) {
        if (isVirtualRegOperand(use)) {
          ++counts[use.regValue.id];
        }
      }
    }
  }
  return counts;
}

std::map<uint32_t, std::size_t>
collectVirtualDefCounts(const MachineFunction &mf) {
  std::map<uint32_t, std::size_t> counts;
  for (const MachineBasicBlock &block : mf.blocks) {
    for (const MachineInstr &instr : block.instrs) {
      for (const MachineOperand &def : instr.defs) {
        if (isVirtualRegOperand(def)) {
          ++counts[def.regValue.id];
        }
      }
    }
  }
  return counts;
}

bool hasSingleVirtualUse(const MachineOperand &def,
                         const std::map<uint32_t, std::size_t> &useCounts) {
  if (!isVirtualRegOperand(def)) {
    return false;
  }
  auto found = useCounts.find(def.regValue.id);
  return found != useCounts.end() && found->second == 1;
}

bool hasSingleVirtualDef(const MachineOperand &def,
                         const std::map<uint32_t, std::size_t> &defCounts) {
  if (!isVirtualRegOperand(def)) {
    return false;
  }
  auto found = defCounts.find(def.regValue.id);
  return found != defCounts.end() && found->second == 1;
}

bool isBranchOnZeroReg(const MachineInstr &instr) {
  return (instr.opcode == Opcode::BEQZ || instr.opcode == Opcode::BNEZ) &&
         instr.uses.size() == 2 && instr.uses[0].kind == OperandKind::Reg &&
         instr.uses[1].kind == OperandKind::Block;
}

bool isZeroCompare(const MachineInstr &instr) {
  return (instr.opcode == Opcode::SEQZ || instr.opcode == Opcode::SNEZ) &&
         instr.defs.size() == 1 && instr.uses.size() == 1 &&
         instr.defs[0].kind == OperandKind::Reg &&
         instr.uses[0].kind == OperandKind::Reg && !instr.sideEffects &&
         !instr.terminator && instr.implicitDefs.empty() &&
         instr.implicitUses.empty();
}

bool isFoldableCompareProducer(const MachineInstr &instr) {
  return (instr.opcode == Opcode::SLT || instr.opcode == Opcode::XOR ||
          instr.opcode == Opcode::XORI) &&
         instr.defs.size() == 1 && instr.defs[0].kind == OperandKind::Reg &&
         !instr.sideEffects && !instr.terminator &&
         instr.implicitDefs.empty() && instr.implicitUses.empty();
}

bool isBinaryRegCompareProducer(const MachineInstr &instr) {
  return (instr.opcode == Opcode::SLT || instr.opcode == Opcode::XOR) &&
         instr.uses.size() == 2 && instr.uses[0].kind == OperandKind::Reg &&
         instr.uses[1].kind == OperandKind::Reg &&
         isFoldableCompareProducer(instr);
}

bool isXoriZeroProducer(const MachineInstr &instr) {
  return instr.opcode == Opcode::XORI && instr.uses.size() == 2 &&
         instr.uses[0].kind == OperandKind::Reg &&
         instr.uses[1].kind == OperandKind::Imm &&
         instr.uses[1].immValue == 0 && isFoldableCompareProducer(instr);
}

bool branchUsesDef(const MachineInstr &branch, const MachineInstr &defInstr) {
  return isBranchOnZeroReg(branch) &&
         sameRegister(branch.uses[0].regValue, defInstr.defs[0].regValue);
}

bool zeroCompareUsesDef(const MachineInstr &zeroCompare,
                        const MachineInstr &defInstr) {
  return isZeroCompare(zeroCompare) &&
         sameRegister(zeroCompare.uses[0].regValue, defInstr.defs[0].regValue);
}

Opcode branchOpcodeForSlt(Opcode branchOpcode) {
  return branchOpcode == Opcode::BNEZ ? Opcode::BLT : Opcode::BGE;
}

Opcode branchOpcodeForXor(Opcode branchOpcode) {
  return branchOpcode == Opcode::BNEZ ? Opcode::BNE : Opcode::BEQ;
}

Opcode branchOpcodeForZeroCompare(Opcode zeroOpcode, Opcode branchOpcode,
                                  Opcode nonZeroBranch, Opcode zeroBranch) {
  const bool branchOnConditionTrue = branchOpcode == Opcode::BNEZ;
  const bool conditionIsNonZero = zeroOpcode == Opcode::SNEZ;
  return branchOnConditionTrue == conditionIsNonZero ? nonZeroBranch
                                                     : zeroBranch;
}

MachineInstr makeRegRegBranch(Opcode opcode, const MachineOperand &lhs,
                              const MachineOperand &rhs,
                              const MachineOperand &target) {
  MachineInstr branch;
  branch.opcode = opcode;
  branch.uses.push_back(lhs);
  branch.uses.push_back(rhs);
  branch.uses.push_back(target);
  branch.terminator = true;
  return branch;
}

MachineInstr makeRegZeroBranch(Opcode opcode, const MachineOperand &reg,
                               const MachineOperand &target) {
  MachineInstr branch;
  branch.opcode = opcode;
  branch.uses.push_back(reg);
  branch.uses.push_back(target);
  branch.terminator = true;
  return branch;
}

bool foldCompareAndBranch(const MachineInstr &cmp, const MachineInstr &branch,
                          const std::map<uint32_t, std::size_t> &useCounts,
                          MachineInstr &folded) {
  if (!isFoldableCompareProducer(cmp) || !branchUsesDef(branch, cmp) ||
      !hasSingleVirtualUse(cmp.defs[0], useCounts)) {
    return false;
  }

  if (cmp.opcode == Opcode::SLT && isBinaryRegCompareProducer(cmp)) {
    folded = makeRegRegBranch(branchOpcodeForSlt(branch.opcode), cmp.uses[0],
                              cmp.uses[1], branch.uses[1]);
    return true;
  }

  if (cmp.opcode == Opcode::XOR && isBinaryRegCompareProducer(cmp)) {
    folded = makeRegRegBranch(branchOpcodeForXor(branch.opcode), cmp.uses[0],
                              cmp.uses[1], branch.uses[1]);
    return true;
  }

  if (isXoriZeroProducer(cmp)) {
    folded = makeRegZeroBranch(branch.opcode, cmp.uses[0], branch.uses[1]);
    return true;
  }

  return false;
}

bool foldZeroCompareAndBranch(const MachineInstr &zeroCompare,
                              const MachineInstr &branch,
                              const std::map<uint32_t, std::size_t> &useCounts,
                              MachineInstr &folded) {
  if (!isZeroCompare(zeroCompare) || !branchUsesDef(branch, zeroCompare) ||
      !hasSingleVirtualUse(zeroCompare.defs[0], useCounts)) {
    return false;
  }

  const Opcode opcode = branchOpcodeForZeroCompare(
      zeroCompare.opcode, branch.opcode, Opcode::BNEZ, Opcode::BEQZ);
  folded = makeRegZeroBranch(opcode, zeroCompare.uses[0], branch.uses[1]);
  return true;
}

bool foldCompareZeroCompareAndBranch(
    const MachineInstr &cmp, const MachineInstr &zeroCompare,
    const MachineInstr &branch,
    const std::map<uint32_t, std::size_t> &useCounts, MachineInstr &folded) {
  if (!isFoldableCompareProducer(cmp) ||
      !zeroCompareUsesDef(zeroCompare, cmp) ||
      !branchUsesDef(branch, zeroCompare) ||
      !hasSingleVirtualUse(cmp.defs[0], useCounts) ||
      !hasSingleVirtualUse(zeroCompare.defs[0], useCounts)) {
    return false;
  }

  if (cmp.opcode == Opcode::SLT && isBinaryRegCompareProducer(cmp)) {
    const Opcode opcode = branchOpcodeForZeroCompare(
        zeroCompare.opcode, branch.opcode, Opcode::BLT, Opcode::BGE);
    folded = makeRegRegBranch(opcode, cmp.uses[0], cmp.uses[1], branch.uses[1]);
    return true;
  }

  if (cmp.opcode == Opcode::XOR && isBinaryRegCompareProducer(cmp)) {
    const Opcode opcode = branchOpcodeForZeroCompare(
        zeroCompare.opcode, branch.opcode, Opcode::BNE, Opcode::BEQ);
    folded = makeRegRegBranch(opcode, cmp.uses[0], cmp.uses[1], branch.uses[1]);
    return true;
  }

  if (isXoriZeroProducer(cmp)) {
    const Opcode opcode = branchOpcodeForZeroCompare(
        zeroCompare.opcode, branch.opcode, Opcode::BNEZ, Opcode::BEQZ);
    folded = makeRegZeroBranch(opcode, cmp.uses[0], branch.uses[1]);
    return true;
  }

  return false;
}

constexpr std::size_t kMaxCompareBranchGap = 4;

bool definesRegister(const MachineInstr &instr, const MachineReg &reg) {
  return std::any_of(
      instr.defs.begin(), instr.defs.end(), [&](const MachineOperand &def) {
        return def.kind == OperandKind::Reg && sameRegister(def.regValue, reg);
      });
}

bool gapPreservesCompareInputs(const std::vector<MachineInstr> &instrs,
                               std::size_t producerIndex,
                               std::size_t branchIndex) {
  const MachineInstr &producer = instrs[producerIndex];
  for (std::size_t i = producerIndex + 1; i < branchIndex; ++i) {
    for (const MachineOperand &use : producer.uses) {
      if (use.kind == OperandKind::Reg &&
          definesRegister(instrs[i], use.regValue)) {
        return false;
      }
    }
  }
  return true;
}

bool foldSeparatedCompareAndBranch(
    std::vector<MachineInstr> &instrs, std::size_t branchIndex,
    const std::map<uint32_t, std::size_t> &useCounts) {
  if (branchIndex >= instrs.size() || !isBranchOnZeroReg(instrs[branchIndex])) {
    return false;
  }

  const MachineInstr branch = instrs[branchIndex];
  const MachineReg condition = branch.uses[0].regValue;
  std::size_t cursor = branchIndex;
  std::size_t gapSize = 0;
  while (cursor > 0) {
    const std::size_t candidateIndex = cursor - 1;
    const MachineInstr &candidate = instrs[candidateIndex];
    if (definesRegister(candidate, condition)) {
      if (gapSize == 0 ||
          !gapPreservesCompareInputs(instrs, candidateIndex, branchIndex)) {
        return false;
      }

      MachineInstr folded;
      if (!foldCompareAndBranch(candidate, branch, useCounts, folded) &&
          !foldZeroCompareAndBranch(candidate, branch, useCounts, folded)) {
        return false;
      }
      instrs[branchIndex] = std::move(folded);
      instrs.erase(instrs.begin() + static_cast<long>(candidateIndex));
      return true;
    }

    if (gapSize == kMaxCompareBranchGap || !isLocalCSECandidate(candidate)) {
      return false;
    }
    ++gapSize;
    cursor = candidateIndex;
  }
  return false;
}

bool isCopyPropagationOpcode(Opcode opcode) {
  return opcode == Opcode::COPY || opcode == Opcode::F_COPY;
}

bool copySourceAllowed(const MachineReg &dst, const MachineReg &src) {
  if (!src.physical) {
    return src.rc == dst.rc;
  }
  return src.phys == PhysReg::X0 && dst.rc != RegClass::FPR32;
}

bool isCopyPropagationCandidate(
    const MachineInstr &instr,
    const std::map<uint32_t, std::size_t> &defCounts) {
  if (!isCopyPropagationOpcode(instr.opcode) || instr.sideEffects ||
      instr.terminator || !instr.implicitDefs.empty() ||
      !instr.implicitUses.empty() || instr.defs.size() != 1 ||
      instr.uses.size() != 1 || instr.defs[0].kind != OperandKind::Reg ||
      instr.uses[0].kind != OperandKind::Reg ||
      !hasSingleVirtualDef(instr.defs[0], defCounts)) {
    return false;
  }
  if (isVirtualRegOperand(instr.uses[0]) &&
      !hasSingleVirtualDef(instr.uses[0], defCounts)) {
    return false;
  }
  return copySourceAllowed(instr.defs[0].regValue, instr.uses[0].regValue);
}

void killCopyReplacementsForDef(const MachineReg &def,
                                std::map<uint32_t, MachineReg> &replacements) {
  if (!def.physical) {
    replacements.erase(def.id);
  }
  for (auto it = replacements.begin(); it != replacements.end();) {
    if (sameRegister(it->second, def)) {
      it = replacements.erase(it);
    } else {
      ++it;
    }
  }
}

bool isLoadAddressOfGlobal(const MachineInstr &instr, std::string &symbol) {
  if (instr.opcode != Opcode::LA || instr.defs.size() != 1 ||
      instr.defs[0].kind != OperandKind::Reg || instr.uses.size() != 1 ||
      instr.uses[0].kind != OperandKind::Global || instr.sideEffects ||
      instr.terminator || !instr.implicitDefs.empty() ||
      !instr.implicitUses.empty()) {
    return false;
  }
  symbol = instr.uses[0].symbol;
  return true;
}

MachineInstr makeLoadAddress(MachineReg dst, const std::string &symbol) {
  MachineInstr instr;
  instr.opcode = Opcode::LA;
  instr.defs.push_back(MachineOperand::reg(dst));
  instr.uses.push_back(MachineOperand::global(symbol));
  return instr;
}

MachineInstr makeCopy(MachineReg dst, MachineReg src,
                      const std::string &comment = "") {
  MachineInstr instr;
  instr.opcode = dst.rc == RegClass::FPR32 ? Opcode::F_COPY : Opcode::COPY;
  instr.defs.push_back(MachineOperand::reg(dst));
  instr.uses.push_back(MachineOperand::reg(src));
  instr.comment = comment;
  return instr;
}

int64_t abs64(int64_t value) { return value < 0 ? -value : value; }

bool fitsSigned12(int64_t value) { return value >= -2048 && value <= 2047; }

std::string addressCombineGroupKey(const MachineReg &base,
                                   uint64_t baseVersion) {
  return regKey(base) + "#" + std::to_string(baseVersion);
}

bool knownAddressConstant(const MachineOperand &operand,
                          const std::map<std::string, int64_t> &constants,
                          int64_t &value) {
  if (operand.kind != OperandKind::Reg) {
    return false;
  }
  if (operand.regValue.physical) {
    if (operand.regValue.phys == PhysReg::X0 &&
        operand.regValue.rc == RegClass::GPR64) {
      value = 0;
      return true;
    }
    return false;
  }
  if (operand.regValue.rc != RegClass::GPR64) {
    return false;
  }
  auto found = constants.find(regKey(operand.regValue));
  if (found == constants.end()) {
    return false;
  }
  value = found->second;
  return true;
}

struct AddressCombineInfo {
  MachineReg reg;
  MachineReg base;
  int64_t offset = 0;
  uint64_t baseVersion = 0;
  std::string groupKey;
  std::size_t defIndex = 0;
};

bool isAddressCombineDef(const MachineInstr &instr,
                         const std::map<std::string, int64_t> &constants,
                         const std::map<std::string, uint64_t> &versions,
                         std::size_t defIndex, AddressCombineInfo &info) {
  if (instr.sideEffects || instr.terminator || instr.defs.size() != 1 ||
      instr.defs[0].kind != OperandKind::Reg ||
      instr.defs[0].regValue.rc != RegClass::GPR64) {
    return false;
  }

  const MachineReg dst = instr.defs[0].regValue;

  if (instr.opcode == Opcode::ADDI && instr.uses.size() == 2 &&
      instr.uses[0].kind == OperandKind::Reg &&
      instr.uses[0].regValue.rc == RegClass::GPR64 &&
      instr.uses[1].kind == OperandKind::Imm &&
      !sameRegister(dst, instr.uses[0].regValue)) {
    info.reg = dst;
    info.base = instr.uses[0].regValue;
    info.offset = instr.uses[1].immValue;
    info.baseVersion = [&] {
      auto found = versions.find(regKey(info.base));
      return found == versions.end() ? uint64_t{0} : found->second;
    }();
    info.groupKey = addressCombineGroupKey(info.base, info.baseVersion);
    info.defIndex = defIndex;
    return true;
  }

  if (instr.opcode == Opcode::ADD && instr.uses.size() == 2) {
    int64_t constant = 0;
    int64_t otherConstant = 0;
    if (knownAddressConstant(instr.uses[0], constants, constant) &&
        instr.uses[1].kind == OperandKind::Reg &&
        instr.uses[1].regValue.rc == RegClass::GPR64 &&
        !knownAddressConstant(instr.uses[1], constants, otherConstant) &&
        !sameRegister(dst, instr.uses[1].regValue)) {
      info.reg = dst;
      info.base = instr.uses[1].regValue;
      info.offset = constant;
    } else if (knownAddressConstant(instr.uses[1], constants, constant) &&
               instr.uses[0].kind == OperandKind::Reg &&
               instr.uses[0].regValue.rc == RegClass::GPR64 &&
               !knownAddressConstant(instr.uses[0], constants, otherConstant) &&
               !sameRegister(dst, instr.uses[0].regValue)) {
      info.reg = dst;
      info.base = instr.uses[0].regValue;
      info.offset = constant;
    } else {
      return false;
    }

    info.baseVersion = [&] {
      auto found = versions.find(regKey(info.base));
      return found == versions.end() ? uint64_t{0} : found->second;
    }();
    info.groupKey = addressCombineGroupKey(info.base, info.baseVersion);
    info.defIndex = defIndex;
    return true;
  }

  return false;
}

bool rewriteAddressUse(
    MachineInstr &instr, std::size_t baseIndex, std::size_t offsetIndex,
    const std::map<std::string, AddressCombineInfo> &activeCandidates,
    const std::map<std::string, uint64_t> &versions) {
  if (baseIndex >= instr.uses.size() || offsetIndex >= instr.uses.size() ||
      instr.uses[baseIndex].kind != OperandKind::Reg ||
      instr.uses[offsetIndex].kind != OperandKind::Imm) {
    return false;
  }

  const MachineReg baseReg = instr.uses[baseIndex].regValue;
  const std::string baseKey = regKey(baseReg);
  auto candidateIt = activeCandidates.find(baseKey);
  if (candidateIt == activeCandidates.end()) {
    return false;
  }

  const AddressCombineInfo &candidate = candidateIt->second;
  const int64_t effectiveOffset =
      candidate.offset + instr.uses[offsetIndex].immValue;

  // 优先尝试把当前使用折到更早的共同基址 pivot 上。
  bool havePivot = false;
  MachineReg pivotReg;
  int64_t pivotOffset = 0;
  int64_t pivotScore = 0;
  std::size_t pivotDefIndex = 0;
  for (const auto &[pivotKey, pivot] : activeCandidates) {
    if (pivotKey == baseKey || pivot.groupKey != candidate.groupKey) {
      continue;
    }
    const int64_t candidateOffset = effectiveOffset - pivot.offset;
    if (!fitsSigned12(candidateOffset)) {
      continue;
    }
    const int64_t score = abs64(candidateOffset);
    if (!havePivot || score < pivotScore ||
        (score == pivotScore && pivot.defIndex < pivotDefIndex) ||
        (score == pivotScore && pivot.defIndex == pivotDefIndex &&
         regKey(pivot.reg) < regKey(pivotReg))) {
      havePivot = true;
      pivotReg = pivot.reg;
      pivotOffset = candidateOffset;
      pivotScore = score;
      pivotDefIndex = pivot.defIndex;
    }
  }

  if (havePivot) {
    instr.uses[baseIndex] = MachineOperand::reg(pivotReg);
    instr.uses[offsetIndex] = MachineOperand::imm(pivotOffset);
    return true;
  }

  auto foundVersion = versions.find(regKey(candidate.base));
  const uint64_t currentVersion =
      foundVersion == versions.end() ? uint64_t{0} : foundVersion->second;
  if (currentVersion == candidate.baseVersion &&
      fitsSigned12(effectiveOffset)) {
    instr.uses[baseIndex] = MachineOperand::reg(candidate.base);
    instr.uses[offsetIndex] = MachineOperand::imm(effectiveOffset);
    return true;
  }

  return false;
}

void bumpPhysicalVersions(const std::vector<PhysReg> &regs,
                          std::map<std::string, uint64_t> &versions) {
  for (PhysReg reg : regs) {
    ++versions[regKey(MachineReg::preg(reg, RegClass::GPR64))];
    ++versions[regKey(MachineReg::preg(reg, RegClass::FPR32))];
  }
}

void invalidateDefinedRegisters(
    const MachineInstr &instr,
    std::map<std::string, AddressCombineInfo> &activeCandidates,
    std::map<std::string, int64_t> &constants,
    std::map<std::string, uint64_t> &versions) {
  for (const MachineOperand &def : instr.defs) {
    if (def.kind != OperandKind::Reg) {
      continue;
    }
    const std::string key = regKey(def.regValue);
    activeCandidates.erase(key);
    constants.erase(key);
    ++versions[key];
  }

  for (PhysReg reg : instr.implicitDefs) {
    ++versions[regKey(MachineReg::preg(reg, RegClass::GPR64))];
    ++versions[regKey(MachineReg::preg(reg, RegClass::FPR32))];
  }
}

} // namespace

bool runBranchCompareFolding(MachineFunction &mf) {
  bool changed = false;
  const auto useCounts = collectVirtualUseCounts(mf);

  for (MachineBasicBlock &block : mf.blocks) {
    for (std::size_t i = 0; i < block.instrs.size();) {
      if (foldSeparatedCompareAndBranch(block.instrs, i, useCounts)) {
        changed = true;
        --i;
        continue;
      }
      ++i;
    }

    std::vector<MachineInstr> rewritten;
    rewritten.reserve(block.instrs.size());

    for (std::size_t i = 0; i < block.instrs.size();) {
      MachineInstr folded;
      if (i + 2 < block.instrs.size() &&
          foldCompareZeroCompareAndBranch(block.instrs[i], block.instrs[i + 1],
                                          block.instrs[i + 2], useCounts,
                                          folded)) {
        rewritten.push_back(std::move(folded));
        i += 3;
        changed = true;
        continue;
      }

      if (i + 1 < block.instrs.size() &&
          foldCompareAndBranch(block.instrs[i], block.instrs[i + 1], useCounts,
                               folded)) {
        rewritten.push_back(std::move(folded));
        i += 2;
        changed = true;
        continue;
      }

      if (i + 1 < block.instrs.size() &&
          foldZeroCompareAndBranch(block.instrs[i], block.instrs[i + 1],
                                   useCounts, folded)) {
        rewritten.push_back(std::move(folded));
        i += 2;
        changed = true;
        continue;
      }

      rewritten.push_back(std::move(block.instrs[i]));
      ++i;
    }

    block.instrs = std::move(rewritten);
  }

  return changed;
}

bool runPreRAGlobalAddressReuse(MachineFunction &mf) {
  if (mf.blocks.empty()) {
    return false;
  }

  std::map<std::string, std::size_t> counts;
  for (const MachineBasicBlock &block : mf.blocks) {
    for (const MachineInstr &instr : block.instrs) {
      std::string symbol;
      if (isLoadAddressOfGlobal(instr, symbol)) {
        ++counts[symbol];
      }
    }
  }

  std::size_t maxCount = 0;
  for (const auto &[symbol, count] : counts) {
    (void)symbol;
    maxCount = std::max(maxCount, count);
  }
  if (counts.size() > 3 && maxCount > 8) {
    return false;
  }

  std::vector<std::pair<std::string, std::size_t>> candidates;
  for (const auto &[symbol, count] : counts) {
    if (count >= 2) {
      candidates.push_back({symbol, count});
    }
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const auto &lhs, const auto &rhs) {
              if (lhs.second != rhs.second) {
                return lhs.second > rhs.second;
              }
              return lhs.first < rhs.first;
            });
  constexpr std::size_t kMaxCachedGlobals = 3;
  if (candidates.size() > kMaxCachedGlobals) {
    candidates.resize(kMaxCachedGlobals);
  }
  if (candidates.empty()) {
    return false;
  }

  std::map<std::string, MachineReg> cachedRegs;
  std::vector<MachineInstr> prefix;
  prefix.reserve(candidates.size());
  for (const auto &[symbol, count] : candidates) {
    (void)count;
    MachineReg reg = mf.createVReg(RegClass::GPR64);
    cachedRegs.emplace(symbol, reg);
    prefix.push_back(makeLoadAddress(reg, symbol));
  }

  MachineBasicBlock &entry = mf.blocks.front();
  std::vector<MachineInstr> entryInstrs;
  entryInstrs.reserve(prefix.size() + entry.instrs.size());
  for (MachineInstr &instr : prefix) {
    entryInstrs.push_back(std::move(instr));
  }
  for (MachineInstr &instr : entry.instrs) {
    entryInstrs.push_back(std::move(instr));
  }
  entry.instrs = std::move(entryInstrs);

  for (MachineBasicBlock &block : mf.blocks) {
    for (MachineInstr &instr : block.instrs) {
      std::string symbol;
      if (!isLoadAddressOfGlobal(instr, symbol)) {
        continue;
      }
      auto cached = cachedRegs.find(symbol);
      if (cached == cachedRegs.end() ||
          sameRegister(instr.defs[0].regValue, cached->second)) {
        continue;
      }
      instr = makeCopy(instr.defs[0].regValue, cached->second, instr.comment);
    }
  }

  return true;
}

bool runPreRAAddressCombine(MachineFunction &mf) {
  if (mf.blocks.empty()) {
    return false;
  }

  bool changed = false;

  for (MachineBasicBlock &block : mf.blocks) {
    std::map<std::string, AddressCombineInfo> activeCandidates;
    std::map<std::string, int64_t> constants;
    std::map<std::string, uint64_t> versions;
    std::size_t instrIndex = 0;

    std::vector<MachineInstr> rewritten;
    rewritten.reserve(block.instrs.size());

    for (const MachineInstr &original : block.instrs) {
      MachineInstr instr = original;
      AddressCombineInfo candidateInfo;
      const bool hasCandidate = isAddressCombineDef(
          original, constants, versions, instrIndex, candidateInfo);

      bool localChanged = false;
      if (isLoadOpcode(instr.opcode)) {
        localChanged =
            rewriteAddressUse(instr, 0, 1, activeCandidates, versions);
      } else if (isMemoryStoreOpcode(instr.opcode)) {
        localChanged =
            rewriteAddressUse(instr, 1, 2, activeCandidates, versions);
      }
      changed = changed || localChanged;

      rewritten.push_back(std::move(instr));

      invalidateDefinedRegisters(original, activeCandidates, constants,
                                 versions);
      if (original.opcode == Opcode::CALL) {
        bumpPhysicalVersions(callerSavedRegs(RegClass::GPR64), versions);
        bumpPhysicalVersions(callerSavedRegs(RegClass::FPR32), versions);
      }

      for (const MachineOperand &def : original.defs) {
        if (def.kind != OperandKind::Reg) {
          continue;
        }
        const std::string key = regKey(def.regValue);
        if (original.opcode == Opcode::LI && original.defs.size() == 1 &&
            original.uses.size() == 1 &&
            original.uses[0].kind == OperandKind::Imm &&
            def.regValue.rc == RegClass::GPR64) {
          constants[key] = original.uses[0].immValue;
        } else if (original.opcode == Opcode::COPY &&
                   original.defs.size() == 1 && original.uses.size() == 1 &&
                   def.regValue.rc == RegClass::GPR64) {
          int64_t value = 0;
          if (knownAddressConstant(original.uses[0], constants, value)) {
            constants[key] = value;
          } else {
            constants.erase(key);
          }
        } else {
          constants.erase(key);
        }
      }

      if (hasCandidate) {
        activeCandidates[regKey(original.defs[0].regValue)] = candidateInfo;
      }

      ++instrIndex;
    }

    block.instrs = std::move(rewritten);
  }

  return changed;
}

bool runPreRACopyPropagation(MachineFunction &mf) {
  bool changed = false;
  const auto useBlocks = collectUseBlocks(mf);
  const auto defCounts = collectVirtualDefCounts(mf);

  for (std::size_t blockIndex = 0; blockIndex < mf.blocks.size();
       ++blockIndex) {
    MachineBasicBlock &block = mf.blocks[blockIndex];
    std::map<uint32_t, MachineReg> replacements;
    std::vector<MachineInstr> rewritten;
    rewritten.reserve(block.instrs.size());

    for (MachineInstr instr : block.instrs) {
      changed = rewriteUseReplacements(instr, replacements) || changed;

      for (const MachineOperand &def : instr.defs) {
        if (def.kind == OperandKind::Reg) {
          killCopyReplacementsForDef(def.regValue, replacements);
        }
      }

      if (isCopyPropagationCandidate(instr, defCounts)) {
        const MachineReg dst = instr.defs[0].regValue;
        const MachineReg src =
            resolveReplacement(instr.uses[0].regValue, replacements);
        replacements[dst.id] = src;
        if (sameRegister(dst, src) ||
            allUsesInBlock(dst.id, blockIndex, useBlocks)) {
          changed = true;
          continue;
        }
      }

      rewritten.push_back(std::move(instr));
    }

    block.instrs = std::move(rewritten);
  }

  return changed;
}

bool runPreRALocalCSE(MachineFunction &mf) {
  bool changed = false;
  const auto useBlocks = collectUseBlocks(mf);

  for (std::size_t blockIndex = 0; blockIndex < mf.blocks.size();
       ++blockIndex) {
    MachineBasicBlock &block = mf.blocks[blockIndex];
    std::map<uint32_t, MachineReg> replacements;
    CSETable exprs;
    CSETable loads;
    CSEKeyDependencies keyDependencies;
    CSERegisterDependencies registerDependencies;
    std::vector<MachineInstr> rewritten;
    rewritten.reserve(block.instrs.size());

    for (MachineInstr instr : block.instrs) {
      rewriteUseReplacements(instr, replacements);

      if (instr.opcode == Opcode::CALL ||
          (instr.sideEffects && !isMemoryStoreOpcode(instr.opcode))) {
        clearCSETables(exprs, loads, keyDependencies, registerDependencies);
      } else if (instr.sideEffects) {
        // Stores invalidate memory-derived CSE entries, but cannot change a
        // pure ALU expression whose operands are virtual registers or X0.
        clearCSELoads(loads, exprs, keyDependencies, registerDependencies);
      }
      invalidateCSEDefinitions(instr, exprs, loads, keyDependencies,
                               registerDependencies, replacements);

      if (isLocalCSECandidate(instr)) {
        const std::string key = cseKey(instr);
        auto &table = isLoadOpcode(instr.opcode) ? loads : exprs;
        auto found = table.find(key);
        const uint32_t defId = instr.defs.front().regValue.id;
        if (found != table.end() &&
            allUsesInBlock(defId, blockIndex, useBlocks)) {
          replacements[defId] = found->second;
          changed = true;
          continue;
        }
        if (found == table.end() &&
            exprs.size() + loads.size() >= kMaxLocalCSEEntries) {
          rewritten.push_back(std::move(instr));
          continue;
        }
        if (table.emplace(key, instr.defs.front().regValue).second) {
          recordCSEKey(key, instr, keyDependencies, registerDependencies);
        }
      }

      rewritten.push_back(std::move(instr));
    }

    block.instrs = std::move(rewritten);
  }

  return changed;
}

bool runPostRAPeephole(MachineFunction &mf) {
  bool changed = false;
  for (MachineBasicBlock &block : mf.blocks) {
    std::vector<MachineInstr> rewritten;
    rewritten.reserve(block.instrs.size());

    for (const MachineInstr &instr : block.instrs) {
      if (isRemovablePostRAInstr(instr)) {
        changed = true;
        continue;
      }

      MachineInstr folded;
      if (foldZeroIdentity(instr, folded)) {
        changed = true;
        if (!isRemovablePostRAInstr(folded)) {
          rewritten.push_back(std::move(folded));
        }
        continue;
      }

      rewritten.push_back(instr);
    }

    block.instrs = std::move(rewritten);
  }
  return changed;
}

} // namespace aarch64
