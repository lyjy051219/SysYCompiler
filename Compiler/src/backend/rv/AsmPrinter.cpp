#include "../../../include/riscv/AsmPrinter.hpp"

#include "../../../include/lib/CFG.hpp"
#include "../../../include/riscv/Target.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <ostream>
#include <string>

namespace riscv {
namespace {

bool isBoolType(const std::shared_ptr<IRType> &type) {
  auto intType = std::dynamic_pointer_cast<INTType>(type);
  return intType && intType->getIsBool();
}

bool isPointerType(const std::shared_ptr<IRType> &type) {
  return type && type->getEnum() == IR_POINTER;
}

int64_t sizeOfType(const std::shared_ptr<IRType> &type) {
  if (!type) {
    return 0;
  }

  switch (type->getEnum()) {
  case IR_INT:
    return isBoolType(type) ? 1 : 4;
  case IR_FLOAT:
    return 4;
  case IR_POINTER:
    return 8;
  case IR_ARRAY: {
    auto arrayType = std::dynamic_pointer_cast<ARRAYType>(type);
    return static_cast<int64_t>(arrayType->getElementCount()) *
           sizeOfType(arrayType->getElementType());
  }
  default:
    return static_cast<int64_t>(type->getSize());
  }
}

int p2Align(int64_t align) {
  align = std::max<int64_t>(1, align);
  int power = 0;
  int64_t value = 1;
  while (value < align) {
    value <<= 1;
    ++power;
  }
  return power;
}

bool fitsSigned12(int64_t value) { return value >= -2048 && value <= 2047; }

constexpr PhysReg kReservedMemcpyTmp = PhysReg::T3;
constexpr PhysReg kReservedMemcpySrc = PhysReg::T4;
constexpr PhysReg kReservedMemcpyDst = PhysReg::T5;
constexpr PhysReg kReservedGprScratch = PhysReg::T6;

void emitSpRelative(std::ostream &out, const char *mnemonic, PhysReg reg,
                    int64_t offset) {
  if (fitsSigned12(offset)) {
    out << "    " << mnemonic << ' ' << regName(reg) << ", " << offset
        << "(sp)\n";
    return;
  }
  out << "    li " << regName(kReservedGprScratch) << ", " << offset << '\n';
  out << "    add " << regName(kReservedGprScratch) << ", sp, "
      << regName(kReservedGprScratch) << '\n';
  out << "    " << mnemonic << ' ' << regName(reg) << ", 0("
      << regName(kReservedGprScratch) << ")\n";
}

constexpr std::array<PhysReg, 11> kGprScratchRegs{
    PhysReg::T0, PhysReg::T1, PhysReg::T2, PhysReg::A0,
    PhysReg::A1, PhysReg::A2, PhysReg::A3, PhysReg::A4,
    PhysReg::A5, PhysReg::A6, PhysReg::A7};

constexpr std::array<PhysReg, 8> kFprScratchRegs{
    PhysReg::FT0, PhysReg::FT1, PhysReg::FT2, PhysReg::FT3,
    PhysReg::FT4, PhysReg::FT5, PhysReg::FT6, PhysReg::FT7};

constexpr std::array<PhysReg, 11> kNonLeafGprScratchRegs{
    PhysReg::S1, PhysReg::S2,  PhysReg::S3, PhysReg::S4,
    PhysReg::S5, PhysReg::S6,  PhysReg::S7, PhysReg::S8,
    PhysReg::S9, PhysReg::S10, PhysReg::S11};

constexpr std::array<PhysReg, 12> kNonLeafFprScratchRegs{
    PhysReg::FS0, PhysReg::FS1, PhysReg::FS2,  PhysReg::FS3,
    PhysReg::FS4, PhysReg::FS5, PhysReg::FS6,  PhysReg::FS7,
    PhysReg::FS8, PhysReg::FS9, PhysReg::FS10, PhysReg::FS11};

bool functionContainsCall(const MachineFunction &function) {
  for (const MachineBasicBlock &block : function.blocks) {
    for (const MachineInstr &instr : block.instrs) {
      if (instr.opcode == Opcode::CALL) {
        return true;
      }
    }
  }
  return false;
}

bool isFprVirtualReg(const MachineOperand &operand) {
  return operand.kind == OperandKind::Reg && !operand.regValue.physical &&
         operand.regValue.rc == RegClass::FPR32;
}

bool isGprVirtualReg(const MachineOperand &operand) {
  return operand.kind == OperandKind::Reg && !operand.regValue.physical &&
         (operand.regValue.rc == RegClass::GPR32 ||
          operand.regValue.rc == RegClass::GPR64);
}

bool functionUsesGprVirtualRegs(const MachineFunction &function) {
  for (const MachineBasicBlock &block : function.blocks) {
    for (const MachineInstr &instr : block.instrs) {
      for (const MachineOperand &def : instr.defs) {
        if (isGprVirtualReg(def)) {
          return true;
        }
      }
      for (const MachineOperand &use : instr.uses) {
        if (isGprVirtualReg(use)) {
          return true;
        }
      }
    }
  }
  return false;
}

bool functionUsesFprVirtualRegs(const MachineFunction &function) {
  for (const MachineBasicBlock &block : function.blocks) {
    for (const MachineInstr &instr : block.instrs) {
      for (const MachineOperand &def : instr.defs) {
        if (isFprVirtualReg(def)) {
          return true;
        }
      }
      for (const MachineOperand &use : instr.uses) {
        if (isFprVirtualReg(use)) {
          return true;
        }
      }
    }
  }
  return false;
}

bool isNonLeafFprScratchReg(PhysReg reg) {
  return std::find(kNonLeafFprScratchRegs.begin(), kNonLeafFprScratchRegs.end(),
                   reg) != kNonLeafFprScratchRegs.end();
}

bool isNonLeafFprScratchOperand(const MachineOperand &operand) {
  return operand.kind == OperandKind::Reg && operand.regValue.physical &&
         isNonLeafFprScratchReg(operand.regValue.phys);
}

bool functionUsesNonLeafFprScratchRegs(const MachineFunction &function) {
  for (const MachineBasicBlock &block : function.blocks) {
    for (const MachineInstr &instr : block.instrs) {
      for (const MachineOperand &def : instr.defs) {
        if (isNonLeafFprScratchOperand(def)) {
          return true;
        }
      }
      for (const MachineOperand &use : instr.uses) {
        if (isNonLeafFprScratchOperand(use)) {
          return true;
        }
      }
    }
  }
  return false;
}

bool functionNeedsFprTempSaves(const MachineFunction &function) {
  return functionUsesFprVirtualRegs(function) ||
         functionUsesNonLeafFprScratchRegs(function);
}

std::vector<PhysReg> usedCalleeSavedRegs(const MachineFunction &function,
                                         bool wantFloat) {
  std::vector<PhysReg> regs;
  for (PhysReg reg : function.usedCalleeSaved) {
    const RegisterInfo &info = getRegisterInfo(reg);
    if (info.calleeSaved && info.isFloat == wantFloat) {
      regs.push_back(reg);
    }
  }
  return regs;
}

std::vector<PhysReg> gprCalleeSavedRegsToSave(const MachineFunction &function) {
  std::vector<PhysReg> regs = usedCalleeSavedRegs(function, false);
  if (!regs.empty() || !functionContainsCall(function)) {
    return regs;
  }
  if (function.usedCalleeSaved.empty() && functionUsesGprVirtualRegs(function)) {
    return std::vector<PhysReg>(kNonLeafGprScratchRegs.begin(),
                                kNonLeafGprScratchRegs.end());
  }
  return regs;
}

std::vector<PhysReg> fprCalleeSavedRegsToSave(const MachineFunction &function) {
  std::vector<PhysReg> regs = usedCalleeSavedRegs(function, true);
  if (!regs.empty() || !functionContainsCall(function)) {
    return regs;
  }
  if (function.usedCalleeSaved.empty() && functionNeedsFprTempSaves(function)) {
    return std::vector<PhysReg>(kNonLeafFprScratchRegs.begin(),
                                kNonLeafFprScratchRegs.end());
  }
  return regs;
}

std::shared_ptr<IRType> pointeeType(const GlobalObject &global) {
  if (!global.ir) {
    return nullptr;
  }

  auto pointerType =
      std::dynamic_pointer_cast<POINTERType>(global.ir->getIRType());
  if (!pointerType) {
    return global.ir->getIRType();
  }
  return pointerType->getPointerType();
}

bool isZeroInitializer(Value *value, const std::shared_ptr<IRType> &type) {
  if (!value) {
    return true;
  }
  if (isPointerType(type)) {
    return dynamic_cast<ConstPtr *>(value) != nullptr;
  }
  if (isBoolType(type)) {
    if (auto boolValue = dynamic_cast<ConstBool *>(value)) {
      return !boolValue->getVal();
    }
    if (auto intValue = dynamic_cast<ConstInt *>(value)) {
      return intValue->getVal() == 0;
    }
  }
  if (auto intValue = dynamic_cast<ConstInt *>(value)) {
    return intValue->getVal() == 0;
  }
  if (auto floatValue = dynamic_cast<ConstFloat *>(value)) {
    return floatValue->getVal() == 0.0f;
  }
  if (auto boolValue = dynamic_cast<ConstBool *>(value)) {
    return !boolValue->getVal();
  }

  auto init = dynamic_cast<Initializer *>(value);
  if (!init) {
    return false;
  }
  if (init->empty()) {
    return true;
  }

  auto arrayType = std::dynamic_pointer_cast<ARRAYType>(type);
  if (!arrayType) {
    for (Value *element : *init) {
      if (!isZeroInitializer(element,
                             element ? element->getIRType() : nullptr)) {
        return false;
      }
    }
    return true;
  }

  auto elementType = arrayType->getElementType();
  const int count = arrayType->getElementCount();
  for (int i = 0; i < count && i < static_cast<int>(init->size()); ++i) {
    if (!isZeroInitializer((*init)[i], elementType)) {
      return false;
    }
  }
  return true;
}

void emitZeroBytes(std::ostream &out, int64_t size) {
  if (size > 0) {
    out << "    .zero " << size << '\n';
  }
}

void emitZeroForType(std::ostream &out, const std::shared_ptr<IRType> &type) {
  emitZeroBytes(out, sizeOfType(type));
}

void emitDataValue(std::ostream &out, Value *value,
                   const std::shared_ptr<IRType> &type) {
  if (auto arrayType = std::dynamic_pointer_cast<ARRAYType>(type)) {
    auto init = dynamic_cast<Initializer *>(value);
    if (!init) {
      emitZeroForType(out, type);
      return;
    }

    auto elementType = arrayType->getElementType();
    const int count = arrayType->getElementCount();
    int i = 0;
    for (; i < count && i < static_cast<int>(init->size()); ++i) {
      emitDataValue(out, (*init)[i], elementType);
    }
    if (i < count) {
      emitZeroBytes(out,
                    static_cast<int64_t>(count - i) * sizeOfType(elementType));
    }
    return;
  }

  if (isPointerType(type)) {
    if (!value || dynamic_cast<ConstPtr *>(value)) {
      emitZeroForType(out, type);
      return;
    }
    throw BackendError("unsupported pointer global initializer");
  }

  if (isBoolType(type)) {
    if (auto boolValue = dynamic_cast<ConstBool *>(value)) {
      out << "    .byte " << (boolValue->getVal() ? 1 : 0) << '\n';
      return;
    }
    if (auto intValue = dynamic_cast<ConstInt *>(value)) {
      out << "    .byte " << (intValue->getVal() != 0 ? 1 : 0) << '\n';
      return;
    }
    emitZeroForType(out, type);
    return;
  }

  if (auto init = dynamic_cast<Initializer *>(value)) {
    if (init->empty()) {
      emitZeroForType(out, type);
      return;
    }
    emitDataValue(out, init->front(), type);
    return;
  }

  if (auto intValue = dynamic_cast<ConstInt *>(value)) {
    out << "    .word " << intValue->getVal() << '\n';
    return;
  }
  if (auto boolValue = dynamic_cast<ConstBool *>(value)) {
    out << "    .word " << (boolValue->getVal() ? 1 : 0) << '\n';
    return;
  }
  if (auto floatValue = dynamic_cast<ConstFloat *>(value)) {
    uint32_t bits = 0;
    float raw = floatValue->getVal();
    std::memcpy(&bits, &raw, sizeof(bits));
    out << "    .word " << bits << '\n';
    return;
  }

  emitZeroForType(out, type);
}

} // namespace

AsmPrinter::AsmPrinter(std::ostream &out) : out_(out) {}

void AsmPrinter::emitModule(const MachineModule &module) {
  out_ << "# myCompiler RV64GC backend\n";
  out_ << "    .attribute arch, \"rv64gc\"\n";
  out_ << "    .option rvc\n";
  for (const GlobalObject &global : module.globals) {
    emitGlobal(global);
  }
  out_ << "    .text\n";
  for (const MachineFunction &function : module.functions) {
    emitFunction(function);
  }
}

void AsmPrinter::emitGlobal(const GlobalObject &global) {
  auto type = pointeeType(global);
  Value *init = global.ir ? global.ir->getUsee() : nullptr;
  const bool zero = isZeroInitializer(init, type);

  if (zero) {
    out_ << "    .section .bss\n";
  } else if (global.constant) {
    out_ << "    .section .rodata\n";
  } else {
    out_ << "    .section .data\n";
  }

  out_ << "    .globl " << global.name << '\n';
  out_ << "    .p2align " << p2Align(global.align) << '\n';
  out_ << global.name << ":\n";
  if (zero) {
    emitZeroBytes(out_, global.size);
  } else {
    emitDataValue(out_, init, type);
  }
  out_ << "    .size " << global.name << ", " << global.size << '\n';
}

void AsmPrinter::emitFunction(const MachineFunction &function) {
  prepareVirtualRegTemps(function);
  currentFunction_ = &function;
  const bool hasCall = functionContainsCall(function);
  const std::vector<PhysReg> savedGprs = gprCalleeSavedRegsToSave(function);
  const std::vector<PhysReg> savedFprs = fprCalleeSavedRegsToSave(function);

  out_ << "    .globl " << function.name << '\n';
  out_ << "    .type " << function.name << ", @function\n";
  out_ << function.name << ":\n";
  emitStackAdjust(-function.frameSize);
  if (hasCall || !savedGprs.empty() || !savedFprs.empty()) {
    int64_t offset = function.frameSize;
    if (hasCall) {
      offset -= 8;
      emitSpRelative(out_, "sd", PhysReg::RA, offset);
    }
    for (PhysReg reg : savedGprs) {
      offset -= 8;
      emitSpRelative(out_, "sd", reg, offset);
    }
    for (PhysReg reg : savedFprs) {
      offset -= 8;
      emitSpRelative(out_, "fsd", reg, offset);
    }
  }

  if (function.blocks.empty()) {
    out_ << "    ret\n";
  } else {
    bool firstBlock = true;
    for (const MachineBasicBlock &block : function.blocks) {
      if (!firstBlock && !block.label.empty()) {
        out_ << block.label << ":\n";
      }
      for (const MachineInstr &instr : block.instrs) {
        emitInstr(instr);
        releaseInstrUses(instr);
        releaseDeadDefs(instr);
      }
      firstBlock = false;
    }
  }

  out_ << "    .size " << function.name << ", .-" << function.name << '\n';
  currentFunction_ = nullptr;
  gprVRegMap_.clear();
  fprVRegMap_.clear();
  vregUseCounts_.clear();
  vregDefCounts_.clear();
  freeGprTemps_.clear();
  freeFprTemps_.clear();
}

void AsmPrinter::emitInstr(const MachineInstr &instr) {
  switch (instr.opcode) {
  case Opcode::COPY:
    if (instr.defs.empty() || instr.uses.empty()) {
      throw BackendError("malformed COPY instruction");
    }
    out_ << "    mv " << operandReg(instr.defs.front()) << ", "
         << operandReg(instr.uses.front()) << '\n';
    return;
  case Opcode::F_COPY:
    if (instr.defs.empty() || instr.uses.empty()) {
      throw BackendError("malformed F_COPY instruction");
    }
    out_ << "    fmv.s " << operandReg(instr.defs.front()) << ", "
         << operandReg(instr.uses.front()) << '\n';
    return;
  case Opcode::PARALLEL_COPY:
    if (instr.defs.empty() || instr.uses.empty()) {
      throw BackendError("malformed PARALLEL_COPY instruction");
    }
    if (instr.defs.front().regValue.rc == RegClass::FPR32) {
      out_ << "    fmv.s " << operandReg(instr.defs.front()) << ", "
           << operandReg(instr.uses.front()) << '\n';
    } else {
      out_ << "    mv " << operandReg(instr.defs.front()) << ", "
           << operandReg(instr.uses.front()) << '\n';
    }
    return;
  case Opcode::FMV_W_X:
    if (instr.defs.empty() || instr.uses.empty()) {
      throw BackendError("malformed FMV_W_X instruction");
    }
    out_ << "    fmv.w.x " << operandReg(instr.defs.front()) << ", "
         << operandReg(instr.uses.front()) << '\n';
    return;
  case Opcode::LI:
    if (instr.defs.empty() || instr.uses.empty() ||
        instr.uses.front().kind != OperandKind::Imm) {
      throw BackendError("malformed LI instruction");
    }
    out_ << "    li " << operandReg(instr.defs.front()) << ", "
         << instr.uses.front().immValue << '\n';
    return;
  case Opcode::LA:
    if (instr.defs.empty() || instr.uses.empty() ||
        instr.uses.front().kind != OperandKind::Global) {
      throw BackendError("malformed LA instruction");
    }
    emitLoadAddress(instr.defs.front(), instr.uses.front().symbol);
    return;
  case Opcode::ADDI:
  case Opcode::ADDIW:
  case Opcode::ANDI:
  case Opcode::ORI:
  case Opcode::XORI:
  case Opcode::SLLI:
  case Opcode::SLLIW:
  case Opcode::SRAI:
  case Opcode::SRAIW:
  case Opcode::SRLIW:
    if (instr.defs.empty() || instr.uses.size() < 2 ||
        instr.uses[1].kind != OperandKind::Imm) {
      throw BackendError("malformed immediate instruction");
    }
    {
      const char *mnemonic = "";
      switch (instr.opcode) {
      case Opcode::ADDI:
        mnemonic = "addi";
        break;
      case Opcode::ADDIW:
        mnemonic = "addiw";
        break;
      case Opcode::ANDI:
        mnemonic = "andi";
        break;
      case Opcode::ORI:
        mnemonic = "ori";
        break;
      case Opcode::XORI:
        mnemonic = "xori";
        break;
      case Opcode::SLLI:
        mnemonic = "slli";
        break;
      case Opcode::SLLIW:
        mnemonic = "slliw";
        break;
      case Opcode::SRAI:
        mnemonic = "srai";
        break;
      case Opcode::SRAIW:
        mnemonic = "sraiw";
        break;
      case Opcode::SRLIW:
        mnemonic = "srliw";
        break;
      default:
        break;
      }

      int64_t imm = instr.uses[1].immValue;
      auto [base, scratch] = memoryBaseReg(instr.uses[0], imm);
      if (!fitsSigned12(imm)) {
        throw BackendError("immediate operand out of signed 12-bit range");
      }
      out_ << "    " << mnemonic << ' ' << operandReg(instr.defs.front())
           << ", " << base << ", " << imm << '\n';
      releaseScratchGpr(scratch);
    }
    return;
  case Opcode::ADD:
  case Opcode::ADDW:
  case Opcode::SUB:
  case Opcode::SUBW:
  case Opcode::MUL:
  case Opcode::MULW:
  case Opcode::DIV:
  case Opcode::DIVW:
  case Opcode::DIVUW:
  case Opcode::REM:
  case Opcode::REMW:
  case Opcode::REMUW:
  case Opcode::AND:
  case Opcode::OR:
  case Opcode::XOR:
  case Opcode::SLL:
  case Opcode::SLLW:
  case Opcode::SRAW:
  case Opcode::SRLW:
  case Opcode::SLT:
  case Opcode::SLTU:
  case Opcode::FEQ_S:
  case Opcode::FLT_S:
  case Opcode::FLE_S:
  case Opcode::FADD_S:
  case Opcode::FSUB_S:
  case Opcode::FMUL_S:
  case Opcode::FDIV_S:
    if (instr.defs.empty() || instr.uses.size() < 2) {
      throw BackendError("malformed binary instruction");
    }
    {
      const char *mnemonic = "";
      switch (instr.opcode) {
      case Opcode::ADD:
        mnemonic = "add";
        break;
      case Opcode::ADDW:
        mnemonic = "addw";
        break;
      case Opcode::SUB:
        mnemonic = "sub";
        break;
      case Opcode::SUBW:
        mnemonic = "subw";
        break;
      case Opcode::MUL:
        mnemonic = "mul";
        break;
      case Opcode::MULW:
        mnemonic = "mulw";
        break;
      case Opcode::DIV:
        mnemonic = "div";
        break;
      case Opcode::DIVW:
        mnemonic = "divw";
        break;
      case Opcode::DIVUW:
        mnemonic = "divuw";
        break;
      case Opcode::REM:
        mnemonic = "rem";
        break;
      case Opcode::REMW:
        mnemonic = "remw";
        break;
      case Opcode::REMUW:
        mnemonic = "remuw";
        break;
      case Opcode::AND:
        mnemonic = "and";
        break;
      case Opcode::OR:
        mnemonic = "or";
        break;
      case Opcode::XOR:
        mnemonic = "xor";
        break;
      case Opcode::SLL:
        mnemonic = "sll";
        break;
      case Opcode::SLLW:
        mnemonic = "sllw";
        break;
      case Opcode::SRAW:
        mnemonic = "sraw";
        break;
      case Opcode::SRLW:
        mnemonic = "srlw";
        break;
      case Opcode::SLT:
        mnemonic = "slt";
        break;
      case Opcode::SLTU:
        mnemonic = "sltu";
        break;
      case Opcode::FEQ_S:
        mnemonic = "feq.s";
        break;
      case Opcode::FLT_S:
        mnemonic = "flt.s";
        break;
      case Opcode::FLE_S:
        mnemonic = "fle.s";
        break;
      case Opcode::FADD_S:
        mnemonic = "fadd.s";
        break;
      case Opcode::FSUB_S:
        mnemonic = "fsub.s";
        break;
      case Opcode::FMUL_S:
        mnemonic = "fmul.s";
        break;
      case Opcode::FDIV_S:
        mnemonic = "fdiv.s";
        break;
      default:
        break;
      }
      out_ << "    " << mnemonic << ' ' << operandReg(instr.defs.front())
           << ", " << operandReg(instr.uses[0]) << ", "
           << operandReg(instr.uses[1]) << '\n';
    }
    return;
  case Opcode::SEQZ:
  case Opcode::SNEZ:
    if (instr.defs.empty() || instr.uses.empty()) {
      throw BackendError("malformed zero-compare instruction");
    }
    out_ << "    " << (instr.opcode == Opcode::SEQZ ? "seqz" : "snez") << ' '
         << operandReg(instr.defs.front()) << ", "
         << operandReg(instr.uses.front()) << '\n';
    return;
  case Opcode::FCVT_W_S:
  case Opcode::FCVT_S_W:
    if (instr.defs.empty() || instr.uses.empty()) {
      throw BackendError("malformed float conversion instruction");
    }
    if (instr.opcode == Opcode::FCVT_W_S) {
      out_ << "    fcvt.w.s " << operandReg(instr.defs.front()) << ", "
           << operandReg(instr.uses.front()) << ", rtz\n";
    } else {
      out_ << "    fcvt.s.w " << operandReg(instr.defs.front()) << ", "
           << operandReg(instr.uses.front()) << '\n';
    }
    return;
  case Opcode::LBU:
  case Opcode::SB:
  case Opcode::LW:
  case Opcode::SW:
  case Opcode::LD:
  case Opcode::SD:
  case Opcode::FLW:
  case Opcode::FSW:
    emitMemoryInstr(instr);
    return;
  case Opcode::MEMCPY_INLINE: {
    if (instr.uses.size() < 3 || instr.uses[2].kind != OperandKind::Imm) {
      throw BackendError("malformed MEMCPY_INLINE instruction");
    }
    const std::string dst = operandReg(instr.uses[0]);
    const std::string src = operandReg(instr.uses[1]);
    const int64_t size = instr.uses[2].immValue;
    if (size < 0) {
      throw BackendError("negative MEMCPY_INLINE size");
    }

    const int id = pcrelId_++;
    const std::string wordLoop = ".Lmemcpy" + std::to_string(id) + ".loopw";
    const std::string byteLoop = ".Lmemcpy" + std::to_string(id) + ".loopb";
    const std::string byteStart = ".Lmemcpy" + std::to_string(id) + ".bytes";
    const std::string done = ".Lmemcpy" + std::to_string(id) + ".done";
    const int64_t wordCount = size / 4;
    const int64_t byteCount = size % 4;

    out_ << "    mv " << regName(kReservedMemcpyDst) << ", " << dst << '\n';
    out_ << "    mv " << regName(kReservedMemcpySrc) << ", " << src << '\n';
    out_ << "    li " << regName(kReservedGprScratch) << ", " << wordCount
         << '\n';
    out_ << "    beqz " << regName(kReservedGprScratch) << ", " << byteStart
         << '\n';
    out_ << wordLoop << ":\n";
    out_ << "    lw " << regName(kReservedMemcpyTmp) << ", 0("
         << regName(kReservedMemcpySrc) << ")\n";
    out_ << "    sw " << regName(kReservedMemcpyTmp) << ", 0("
         << regName(kReservedMemcpyDst) << ")\n";
    out_ << "    addi " << regName(kReservedMemcpySrc) << ", "
         << regName(kReservedMemcpySrc) << ", 4\n";
    out_ << "    addi " << regName(kReservedMemcpyDst) << ", "
         << regName(kReservedMemcpyDst) << ", 4\n";
    out_ << "    addi " << regName(kReservedGprScratch) << ", "
         << regName(kReservedGprScratch) << ", -1\n";
    out_ << "    bnez " << regName(kReservedGprScratch) << ", " << wordLoop
         << '\n';
    out_ << byteStart << ":\n";
    out_ << "    li " << regName(kReservedGprScratch) << ", " << byteCount
         << '\n';
    out_ << "    beqz " << regName(kReservedGprScratch) << ", " << done << '\n';
    out_ << byteLoop << ":\n";
    out_ << "    lbu " << regName(kReservedMemcpyTmp) << ", 0("
         << regName(kReservedMemcpySrc) << ")\n";
    out_ << "    sb " << regName(kReservedMemcpyTmp) << ", 0("
         << regName(kReservedMemcpyDst) << ")\n";
    out_ << "    addi " << regName(kReservedMemcpySrc) << ", "
         << regName(kReservedMemcpySrc) << ", 1\n";
    out_ << "    addi " << regName(kReservedMemcpyDst) << ", "
         << regName(kReservedMemcpyDst) << ", 1\n";
    out_ << "    addi " << regName(kReservedGprScratch) << ", "
         << regName(kReservedGprScratch) << ", -1\n";
    out_ << "    bnez " << regName(kReservedGprScratch) << ", " << byteLoop
         << '\n';
    out_ << done << ":\n";
    return;
  }
  case Opcode::RET:
    if (currentFunction_) {
      const bool containsCall = functionContainsCall(*currentFunction_);
      const std::vector<PhysReg> savedGprs =
          gprCalleeSavedRegsToSave(*currentFunction_);
      const std::vector<PhysReg> savedFprs =
          fprCalleeSavedRegsToSave(*currentFunction_);
      int64_t offset = currentFunction_->frameSize;
      if (containsCall) {
        offset -= 8;
        emitSpRelative(out_, "ld", PhysReg::RA, offset);
      }
      for (PhysReg reg : savedGprs) {
        offset -= 8;
        emitSpRelative(out_, "ld", reg, offset);
      }
      for (PhysReg reg : savedFprs) {
        offset -= 8;
        emitSpRelative(out_, "fld", reg, offset);
      }
    }
    if (currentFunction_) {
      emitStackAdjust(currentFunction_->frameSize);
    }
    out_ << "    ret\n";
    return;
  case Opcode::CALL:
    if (instr.uses.empty() || instr.uses.front().kind != OperandKind::Global) {
      throw BackendError("malformed CALL instruction");
    }
    out_ << "    call " << instr.uses.front().symbol << '\n';
    return;
  case Opcode::BNEZ:
    if (instr.uses.size() < 2 || instr.uses[1].kind != OperandKind::Block) {
      throw BackendError("malformed BNEZ instruction");
    }
    out_ << "    bnez " << operandReg(instr.uses[0]) << ", "
         << instr.uses[1].blockLabel << '\n';
    return;
  case Opcode::BEQZ:
    if (instr.uses.size() < 2 || instr.uses[1].kind != OperandKind::Block) {
      throw BackendError("malformed BEQZ instruction");
    }
    out_ << "    beqz " << operandReg(instr.uses[0]) << ", "
         << instr.uses[1].blockLabel << '\n';
    return;
  case Opcode::BEQ:
  case Opcode::BNE:
  case Opcode::BLT:
  case Opcode::BGE:
    if (instr.uses.size() < 3 || instr.uses[2].kind != OperandKind::Block) {
      throw BackendError("malformed conditional branch instruction");
    }
    switch (instr.opcode) {
    case Opcode::BEQ:
      out_ << "    beq ";
      break;
    case Opcode::BNE:
      out_ << "    bne ";
      break;
    case Opcode::BLT:
      out_ << "    blt ";
      break;
    case Opcode::BGE:
      out_ << "    bge ";
      break;
    default:
      break;
    }
    out_ << operandReg(instr.uses[0]) << ", " << operandReg(instr.uses[1])
         << ", " << instr.uses[2].blockLabel << '\n';
    return;
  case Opcode::J:
    if (instr.uses.empty() || instr.uses.front().kind != OperandKind::Block) {
      throw BackendError("malformed J instruction");
    }
    out_ << "    j " << instr.uses.front().blockLabel << '\n';
    return;
  case Opcode::NOP:
    out_ << "    nop\n";
    return;
  default:
    throw BackendError("unsupported temporary MIR opcode " +
                       std::to_string(static_cast<int>(instr.opcode)));
  }
}

void AsmPrinter::prepareVirtualRegTemps(const MachineFunction &function) {
  gprVRegMap_.clear();
  fprVRegMap_.clear();
  vregUseCounts_.clear();
  vregDefCounts_.clear();
  if (functionContainsCall(function)) {
    freeGprTemps_.assign(kNonLeafGprScratchRegs.rbegin(),
                         kNonLeafGprScratchRegs.rend());
  } else {
    freeGprTemps_.assign(kGprScratchRegs.rbegin(), kGprScratchRegs.rend());
  }
  if (functionContainsCall(function) && functionUsesFprVirtualRegs(function)) {
    freeFprTemps_.assign(kNonLeafFprScratchRegs.rbegin(),
                         kNonLeafFprScratchRegs.rend());
  } else {
    freeFprTemps_.assign(kFprScratchRegs.rbegin(), kFprScratchRegs.rend());
  }

  for (const MachineBasicBlock &block : function.blocks) {
    for (const MachineInstr &instr : block.instrs) {
      for (const MachineOperand &use : instr.uses) {
        if (use.kind != OperandKind::Reg || use.regValue.physical) {
          continue;
        }
        ++vregUseCounts_[use.regValue.id];
      }
      for (const MachineOperand &def : instr.defs) {
        if (def.kind != OperandKind::Reg || def.regValue.physical) {
          continue;
        }
        ++vregDefCounts_[def.regValue.id];
        if (instr.opcode == Opcode::PARALLEL_COPY) {
          ++vregDefCounts_[def.regValue.id];
        }
      }
    }
  }
}

void AsmPrinter::releaseInstrUses(const MachineInstr &instr) {
  for (const MachineOperand &use : instr.uses) {
    releaseOperandUse(use);
  }
}

void AsmPrinter::releaseDeadDefs(const MachineInstr &instr) {
  for (const MachineOperand &def : instr.defs) {
    if (def.kind != OperandKind::Reg || def.regValue.physical) {
      continue;
    }
    auto defs = vregDefCounts_.find(def.regValue.id);
    if (defs != vregDefCounts_.end() && defs->second > 1) {
      continue;
    }
    auto remainingUses = vregUseCounts_.find(def.regValue.id);
    if (remainingUses == vregUseCounts_.end() || remainingUses->second <= 0) {
      releaseVirtualReg(def.regValue);
    }
  }
}

void AsmPrinter::releaseOperandUse(const MachineOperand &operand) {
  if (operand.kind != OperandKind::Reg || operand.regValue.physical) {
    return;
  }

  auto remainingUses = vregUseCounts_.find(operand.regValue.id);
  if (remainingUses == vregUseCounts_.end()) {
    return;
  }

  --remainingUses->second;
  if (remainingUses->second <= 0) {
    vregUseCounts_.erase(remainingUses);
    auto defs = vregDefCounts_.find(operand.regValue.id);
    if (defs != vregDefCounts_.end() && defs->second > 1) {
      return;
    }
    releaseVirtualReg(operand.regValue);
  }
}

void AsmPrinter::releaseVirtualReg(MachineReg reg) {
  if (reg.rc == RegClass::FPR32) {
    auto mapped = fprVRegMap_.find(reg.id);
    if (mapped == fprVRegMap_.end()) {
      return;
    }
    freeFprTemps_.push_back(mapped->second);
    fprVRegMap_.erase(mapped);
    return;
  }

  auto mapped = gprVRegMap_.find(reg.id);
  if (mapped == gprVRegMap_.end()) {
    return;
  }
  freeGprTemps_.push_back(mapped->second);
  gprVRegMap_.erase(mapped);
}

PhysReg AsmPrinter::acquireScratchGpr() {
  if (freeGprTemps_.empty()) {
    throw BackendError("RISC-V GPR scratch register pool exhausted");
  }
  PhysReg reg = freeGprTemps_.back();
  freeGprTemps_.pop_back();
  return reg;
}

void AsmPrinter::releaseScratchGpr(PhysReg reg) {
  if (reg == PhysReg::NoReg) {
    return;
  }
  freeGprTemps_.push_back(reg);
}

void AsmPrinter::emitLoadAddress(const MachineOperand &dst,
                                 const std::string &symbol) {
  const std::string reg = operandReg(dst);
  const int id = pcrelId_++;
  out_ << ".Lpcrel" << id << ":\n";
  out_ << "    auipc " << reg << ", %pcrel_hi(" << symbol << ")\n";
  out_ << "    addi " << reg << ", " << reg << ", %pcrel_lo(.Lpcrel" << id
       << ")\n";
}

void AsmPrinter::emitStackAdjust(int64_t amount) {
  if (amount == 0) {
    return;
  }

  if (fitsSigned12(amount)) {
    out_ << "    addi sp, sp, " << amount << '\n';
    return;
  }

  out_ << "    li " << regName(kReservedGprScratch) << ", " << amount << '\n';
  out_ << "    add sp, sp, " << regName(kReservedGprScratch) << '\n';
}

void AsmPrinter::emitMemoryInstr(const MachineInstr &instr) {
  const bool isStore =
      instr.opcode == Opcode::SB || instr.opcode == Opcode::SW ||
      instr.opcode == Opcode::SD || instr.opcode == Opcode::FSW;
  const char *mnemonic = "";
  switch (instr.opcode) {
  case Opcode::LBU:
    mnemonic = "lbu";
    break;
  case Opcode::SB:
    mnemonic = "sb";
    break;
  case Opcode::LW:
    mnemonic = "lw";
    break;
  case Opcode::SW:
    mnemonic = "sw";
    break;
  case Opcode::LD:
    mnemonic = "ld";
    break;
  case Opcode::SD:
    mnemonic = "sd";
    break;
  case Opcode::FLW:
    mnemonic = "flw";
    break;
  case Opcode::FSW:
    mnemonic = "fsw";
    break;
  default:
    throw BackendError("unsupported memory instruction");
  }

  if (isStore) {
    if (instr.uses.size() < 3 || instr.uses[2].kind != OperandKind::Imm) {
      throw BackendError("malformed store instruction");
    }
    int64_t offset = instr.uses[2].immValue;
    auto [base, scratch] = memoryBaseReg(instr.uses[1], offset);
    if (!fitsSigned12(offset)) {
      throw BackendError("store offset out of signed 12-bit range");
    }
    out_ << "    " << mnemonic << ' ' << operandReg(instr.uses[0]) << ", "
         << offset << '(' << base << ")\n";
    releaseScratchGpr(scratch);
    return;
  }

  if (instr.defs.empty() || instr.uses.size() < 2 ||
      instr.uses[1].kind != OperandKind::Imm) {
    throw BackendError("malformed load instruction");
  }
  int64_t offset = instr.uses[1].immValue;
  auto [base, scratch] = memoryBaseReg(instr.uses[0], offset);
  if (!fitsSigned12(offset)) {
    throw BackendError("load offset out of signed 12-bit range");
  }
  out_ << "    " << mnemonic << ' ' << operandReg(instr.defs.front()) << ", "
       << offset << '(' << base << ")\n";
  releaseScratchGpr(scratch);
}

std::pair<std::string, PhysReg>
AsmPrinter::memoryBaseReg(const MachineOperand &base, int64_t &offset) {
  std::string baseReg;
  if (base.kind == OperandKind::Reg) {
    baseReg = operandReg(base);
  } else if (base.kind == OperandKind::FrameIndex) {
    if (!currentFunction_) {
      throw BackendError("frame-index operand without active function");
    }

    const int frameIndex = base.frameIndexValue;
    if (frameIndex < 0 ||
        frameIndex >= static_cast<int>(currentFunction_->stackObjects.size())) {
      throw BackendError("invalid frame-index operand");
    }

    offset += currentFunction_->stackObjects[frameIndex].offset;
    baseReg = regName(PhysReg::SP);
  } else {
    throw BackendError("expected register or frame-index memory base");
  }

  if (fitsSigned12(offset)) {
    return {baseReg, PhysReg::NoReg};
  }

  PhysReg scratch = kReservedGprScratch;
  const std::string scratchReg = regName(scratch);
  out_ << "    li " << scratchReg << ", " << offset << '\n';
  out_ << "    add " << scratchReg << ", " << baseReg << ", " << scratchReg
       << '\n';
  offset = 0;
  return {scratchReg, PhysReg::NoReg};
}

PhysReg AsmPrinter::assignVirtualReg(MachineReg reg) {
  if (reg.rc == RegClass::FPR32) {
    auto existing = fprVRegMap_.find(reg.id);
    if (existing != fprVRegMap_.end()) {
      return existing->second;
    }
    if (freeFprTemps_.empty()) {
      throw BackendError("RISC-V FPR temporary register pool exhausted");
    }
    PhysReg phys = freeFprTemps_.back();
    freeFprTemps_.pop_back();
    fprVRegMap_.emplace(reg.id, phys);
    return phys;
  }

  if (reg.rc == RegClass::GPR32 || reg.rc == RegClass::GPR64) {
    auto existing = gprVRegMap_.find(reg.id);
    if (existing != gprVRegMap_.end()) {
      return existing->second;
    }
    if (freeGprTemps_.empty()) {
      throw BackendError("RISC-V GPR temporary register pool exhausted");
    }
    PhysReg phys = freeGprTemps_.back();
    freeGprTemps_.pop_back();
    gprVRegMap_.emplace(reg.id, phys);
    return phys;
  }

  throw BackendError("unsupported virtual register class");
}

std::string AsmPrinter::operandReg(const MachineOperand &operand) {
  if (operand.kind != OperandKind::Reg) {
    throw BackendError("expected register operand");
  }
  const MachineReg reg = operand.regValue;
  if (!reg.isValid()) {
    throw BackendError("invalid register operand");
  }
  if (reg.physical) {
    return reg.str();
  }
  return regName(assignVirtualReg(reg));
}

} // namespace riscv
