#include "../../../include/riscv/ISel.hpp"

#include "../../../include/riscv/Frame.hpp"
#include "../../../include/riscv/Target.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace riscv {
namespace {

const char *instTypeName(Instruction::InstType type);

bool isBoolType(const std::shared_ptr<IRType> &type) {
  auto intType = std::dynamic_pointer_cast<INTType>(type);
  return intType && intType->getIsBool();
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

int64_t alignOfType(const std::shared_ptr<IRType> &type) {
  if (!type) {
    return 1;
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
    return std::max<int64_t>(1, alignOfType(arrayType->getElementType()));
  }
  default:
    return std::max<int64_t>(1, static_cast<int64_t>(type->getSize()));
  }
}

bool isVoidType(const std::shared_ptr<IRType> &type) {
  return !type || type->getEnum() == IR_VOID;
}

bool isFloatType(const std::shared_ptr<IRType> &type) {
  return type && type->getEnum() == IR_FLOAT;
}

bool isPointerType(const std::shared_ptr<IRType> &type) {
  return type && type->getEnum() == IR_POINTER;
}

RegClass regClassForType(const std::shared_ptr<IRType> &type) {
  if (!type) {
    throw BackendError("missing IR type for register class selection");
  }
  switch (type->getEnum()) {
  case IR_FLOAT:
    return RegClass::FPR32;
  case IR_POINTER:
    return RegClass::GPR64;
  case IR_INT:
    return RegClass::GPR32;
  default:
    throw BackendError("unsupported IR type for register class selection");
  }
}

bool fitsSigned12(int64_t value) { return value >= -2048 && value <= 2047; }

struct Address {
  enum class Kind { Reg, FrameIndex };

  Kind kind = Kind::Reg;
  MachineReg reg;
  int frameIndex = -1;
  int64_t offset = 0;

  static Address regBase(MachineReg reg, int64_t offset = 0) {
    Address address;
    address.kind = Kind::Reg;
    address.reg = reg;
    address.offset = offset;
    return address;
  }

  static Address frameIndexBase(int frameIndex, int64_t offset = 0) {
    Address address;
    address.kind = Kind::FrameIndex;
    address.frameIndex = frameIndex;
    address.offset = offset;
    return address;
  }
};

struct LoweringState {
  std::unordered_map<Value *, Address> addresses;
  std::unordered_map<BasicBlock *, std::string> blockLabels;
};

std::string sanitizeLabelPart(const std::string &name) {
  std::string result;
  result.reserve(name.size());
  for (unsigned char ch : name) {
    result.push_back(std::isalnum(ch) || ch == '_' ? static_cast<char>(ch)
                                                   : '_');
  }
  return result.empty() ? "fn" : result;
}

std::string blockLabel(Function &function, int index) {
  if (index == 0) {
    return function.getIdent();
  }
  return ".L" + sanitizeLabelPart(function.getIdent()) + ".bb" +
         std::to_string(index);
}

Value *useValue(User &user, std::size_t index) {
  auto &uses = user.getUserList();
  if (index >= uses.size()) {
    return nullptr;
  }
  return uses[index]->getValue();
}

Value *requiredUseValue(User &user, std::size_t index,
                        const std::string &context) {
  Value *value = useValue(user, index);
  if (!value) {
    throw BackendError("missing operand while lowering " + context);
  }
  return value;
}

bool intConstant(Value *value, int64_t &out) {
  if (auto *intValue = dynamic_cast<ConstInt *>(value)) {
    out = intValue->getVal();
    return true;
  }
  if (auto *boolValue = dynamic_cast<ConstBool *>(value)) {
    out = boolValue->getVal() ? 1 : 0;
    return true;
  }
  if (dynamic_cast<ConstPtr *>(value)) {
    out = 0;
    return true;
  }
  if (dynamic_cast<UndefValue *>(value) && value->getIRType() &&
      (value->getIRType()->getEnum() == IR_INT ||
       value->getIRType()->getEnum() == IR_POINTER)) {
    out = 0;
    return true;
  }
  return false;
}

void appendRegInstr(MachineBasicBlock &mbb, Opcode opcode, MachineReg def,
                    std::initializer_list<MachineReg> uses) {
  MachineInstr instr;
  instr.opcode = opcode;
  if (def.isValid()) {
    instr.defs.push_back(MachineOperand::reg(def));
  }
  for (MachineReg use : uses) {
    instr.uses.push_back(MachineOperand::reg(use));
  }
  mbb.instrs.push_back(std::move(instr));
}

MachineReg materializeInt(MachineFunction &mf, MachineBasicBlock &mbb,
                          int64_t value, RegClass rc) {
  MachineReg reg = mf.createVReg(rc);
  MachineInstr instr;
  instr.opcode = Opcode::LI;
  instr.defs.push_back(MachineOperand::reg(reg));
  instr.uses.push_back(MachineOperand::imm(value));
  mbb.instrs.push_back(std::move(instr));
  return reg;
}

MachineReg materializeFloat(MachineFunction &mf, MachineBasicBlock &mbb,
                            float value) {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  MachineReg bitsReg =
      materializeInt(mf, mbb, static_cast<int64_t>(bits), RegClass::GPR64);
  MachineReg floatReg = mf.createVReg(RegClass::FPR32);
  appendRegInstr(mbb, Opcode::FMV_W_X, floatReg, {bitsReg});
  return floatReg;
}

bool isScalarResultType(const std::shared_ptr<IRType> &type) {
  return type && (type->getEnum() == IR_INT || type->getEnum() == IR_FLOAT);
}

MachineReg getOrCreateResultReg(MachineFunction &mf, Instruction &inst,
                                RegClass rc) {
  auto existing = mf.valueRegs.find(&inst);
  if (existing != mf.valueRegs.end()) {
    return existing->second;
  }
  MachineReg reg = mf.createVReg(rc);
  mf.valueRegs[&inst] = reg;
  return reg;
}

MachineReg valueReg(MachineFunction &mf, MachineBasicBlock &mbb,
                    Value *value) {
  if (!value) {
    throw BackendError("missing value while lowering function " + mf.name);
  }

  auto mapped = mf.valueRegs.find(value);
  if (mapped != mf.valueRegs.end()) {
    return mapped->second;
  }

  if (dynamic_cast<UndefValue *>(value)) {
    if (isFloatType(value->getIRType())) {
      return materializeFloat(mf, mbb, 0.0f);
    }
    return materializeInt(mf, mbb, 0, regClassForType(value->getIRType()));
  }

  int64_t intValue = 0;
  if (intConstant(value, intValue)) {
    return materializeInt(mf, mbb, intValue, regClassForType(value->getIRType()));
  }
  if (auto *floatValue = dynamic_cast<ConstFloat *>(value)) {
    return materializeFloat(mf, mbb, floatValue->getVal());
  }
  if (auto *instruction = dynamic_cast<Instruction *>(value)) {
    if (isScalarResultType(instruction->getIRType())) {
      return getOrCreateResultReg(
          mf, *instruction, regClassForType(instruction->getIRType()));
    }
    if (dynamic_cast<GetElementPtrInst *>(instruction)) {
      return getOrCreateResultReg(mf, *instruction, RegClass::GPR64);
    }
  }

  std::string detail = value->getIdent();
  if (detail.empty()) {
    detail = "<unnamed>";
  }
  if (value->getIRType()) {
    detail += " type=" + std::to_string(value->getIRType()->getEnum());
  }
  if (auto *instruction = dynamic_cast<Instruction *>(value)) {
    detail += " inst=" + std::string(instTypeName(instruction->instType));
  }
  throw BackendError("unsupported operand " + detail +
                     " while lowering function " + mf.name);
}

std::shared_ptr<IRType> pointeeType(Value *value, const std::string &context) {
  if (!value) {
    throw BackendError("missing pointer while lowering " + context);
  }
  auto pointerType = std::dynamic_pointer_cast<POINTERType>(value->getIRType());
  if (!pointerType) {
    throw BackendError("expected pointer while lowering " + context);
  }
  return pointerType->getPointerType();
}

MachineOperand addressBaseOperand(const Address &address) {
  if (address.kind == Address::Kind::FrameIndex) {
    return MachineOperand::frameIndex(address.frameIndex);
  }
  return MachineOperand::reg(address.reg);
}

void appendImmInstr(MachineBasicBlock &mbb, Opcode opcode, MachineReg def,
                    MachineOperand base, int64_t imm) {
  MachineInstr instr;
  instr.opcode = opcode;
  instr.defs.push_back(MachineOperand::reg(def));
  instr.uses.push_back(std::move(base));
  instr.uses.push_back(MachineOperand::imm(imm));
  mbb.instrs.push_back(std::move(instr));
}

MachineReg materializeGlobalAddress(MachineFunction &mf, MachineBasicBlock &mbb,
                                    const std::string &symbol) {
  MachineReg reg = mf.createVReg(RegClass::GPR64);
  MachineInstr instr;
  instr.opcode = Opcode::LA;
  instr.defs.push_back(MachineOperand::reg(reg));
  instr.uses.push_back(MachineOperand::global(symbol));
  mbb.instrs.push_back(std::move(instr));
  return reg;
}

MachineReg addImmediateToReg(MachineFunction &mf, MachineBasicBlock &mbb,
                             MachineReg base, int64_t offset) {
  if (offset == 0) {
    return base;
  }
  if (fitsSigned12(offset)) {
    MachineReg dst = mf.createVReg(RegClass::GPR64);
    appendImmInstr(mbb, Opcode::ADDI, dst, MachineOperand::reg(base), offset);
    return dst;
  }

  MachineReg offsetReg = materializeInt(mf, mbb, offset, RegClass::GPR64);
  MachineReg dst = mf.createVReg(RegClass::GPR64);
  appendRegInstr(mbb, Opcode::ADD, dst, {base, offsetReg});
  return dst;
}

MachineReg materializeAddress(MachineFunction &mf, MachineBasicBlock &mbb,
                              const Address &address) {
  if (address.kind == Address::Kind::Reg) {
    return addImmediateToReg(mf, mbb, address.reg, address.offset);
  }

  MachineReg dst = mf.createVReg(RegClass::GPR64);
  appendImmInstr(mbb, Opcode::ADDI, dst,
                 MachineOperand::frameIndex(address.frameIndex),
                 address.offset);
  return dst;
}

Address addressForValue(MachineFunction &mf, MachineBasicBlock &mbb,
                        LoweringState &state, Value *value,
                        const std::string &context) {
  if (!value) {
    throw BackendError("missing address while lowering " + context);
  }

  auto existingAddress = state.addresses.find(value);
  if (existingAddress != state.addresses.end()) {
    return existingAddress->second;
  }

  if (auto existingReg = mf.valueRegs.find(value);
      existingReg != mf.valueRegs.end()) {
    return Address::regBase(existingReg->second);
  }

  if (auto *global = dynamic_cast<Variable *>(value); global &&
                                                     global->isGlobal()) {
    MachineReg reg = materializeGlobalAddress(mf, mbb, global->getIdent());
    return Address::regBase(reg);
  }

  if (dynamic_cast<ConstPtr *>(value)) {
    MachineReg reg = materializeInt(mf, mbb, 0, RegClass::GPR64);
    return Address::regBase(reg);
  }

  if (isPointerType(value->getIRType())) {
    MachineReg reg = valueReg(mf, mbb, value);
    Address address = Address::regBase(reg);
    state.addresses[value] = address;
    return address;
  }

  throw BackendError("unsupported address operand while lowering " + context);
}

Opcode loadOpcodeForType(const std::shared_ptr<IRType> &type) {
  if (isFloatType(type)) {
    return Opcode::FLW;
  }
  if (isPointerType(type)) {
    return Opcode::LD;
  }
  if (type && type->getEnum() == IR_INT) {
    if (isBoolType(type)) {
      return Opcode::LBU;
    }
    return Opcode::LW;
  }
  throw BackendError("unsupported load result type");
}

Opcode storeOpcodeForType(const std::shared_ptr<IRType> &type) {
  if (isFloatType(type)) {
    return Opcode::FSW;
  }
  if (isPointerType(type)) {
    return Opcode::SD;
  }
  if (type && type->getEnum() == IR_INT) {
    if (isBoolType(type)) {
      return Opcode::SB;
    }
    return Opcode::SW;
  }
  throw BackendError("unsupported store value type");
}

void appendLoad(MachineBasicBlock &mbb, Opcode opcode, MachineReg dst,
                const Address &address) {
  MachineInstr instr;
  instr.opcode = opcode;
  instr.defs.push_back(MachineOperand::reg(dst));
  instr.uses.push_back(addressBaseOperand(address));
  instr.uses.push_back(MachineOperand::imm(address.offset));
  mbb.instrs.push_back(std::move(instr));
}

void appendStore(MachineBasicBlock &mbb, Opcode opcode, MachineReg src,
                 const Address &address) {
  MachineInstr instr;
  instr.opcode = opcode;
  instr.uses.push_back(MachineOperand::reg(src));
  instr.uses.push_back(addressBaseOperand(address));
  instr.uses.push_back(MachineOperand::imm(address.offset));
  instr.sideEffects = true;
  mbb.instrs.push_back(std::move(instr));
}

std::shared_ptr<IRType> advanceGepType(const std::shared_ptr<IRType> &type,
                                       bool firstIndex,
                                       int64_t &scale) {
  if (!type) {
    scale = 0;
    return nullptr;
  }

  if (firstIndex) {
    scale = typeSizeBytes(type);
    return type;
  }

  if (auto arrayType = std::dynamic_pointer_cast<ARRAYType>(type)) {
    auto elementType = arrayType->getElementType();
    scale = typeSizeBytes(elementType);
    return elementType;
  }

  if (auto pointerType = std::dynamic_pointer_cast<POINTERType>(type)) {
    auto elementType = pointerType->getPointerType();
    scale = typeSizeBytes(elementType);
    return elementType;
  }

  scale = typeSizeBytes(type);
  return type;
}

MachineReg addScaledIndex(MachineFunction &mf, MachineBasicBlock &mbb,
                          MachineReg currentOffset, Value *index,
                          int64_t scale) {
  MachineReg indexReg = valueReg(mf, mbb, index);
  MachineReg scaled = indexReg;
  if (scale != 1) {
    MachineReg scaleReg = materializeInt(mf, mbb, scale, RegClass::GPR64);
    scaled = mf.createVReg(RegClass::GPR64);
    appendRegInstr(mbb, Opcode::MUL, scaled, {indexReg, scaleReg});
  }

  if (!currentOffset.isValid()) {
    return scaled;
  }

  MachineReg sum = mf.createVReg(RegClass::GPR64);
  appendRegInstr(mbb, Opcode::ADD, sum, {currentOffset, scaled});
  return sum;
}

std::string calleeName(Value *callee) {
  if (auto *builtin = dynamic_cast<BuildInFunction *>(callee)) {
    return builtin->getIdent();
  }
  if (auto *function = dynamic_cast<Function *>(callee)) {
    return function->getIdent();
  }
  return {};
}

std::string labelForBlock(LoweringState &state, BasicBlock *block,
                          const std::string &context) {
  if (!block) {
    throw BackendError("missing block while lowering " + context);
  }
  auto found = state.blockLabels.find(block);
  if (found == state.blockLabels.end()) {
    throw BackendError("unknown block while lowering " + context);
  }
  return found->second;
}

void lowerFunctionParams(MachineFunction &mf, MachineBasicBlock &entry,
                         Function &function) {
  constexpr PhysReg gprArgs[] = {PhysReg::A0, PhysReg::A1, PhysReg::A2,
                                 PhysReg::A3, PhysReg::A4, PhysReg::A5,
                                 PhysReg::A6, PhysReg::A7};
  constexpr PhysReg fprArgs[] = {PhysReg::FA0, PhysReg::FA1, PhysReg::FA2,
                                 PhysReg::FA3, PhysReg::FA4, PhysReg::FA5,
                                 PhysReg::FA6, PhysReg::FA7};

  std::size_t nextGpr = 0;
  std::size_t nextFpr = 0;
  int64_t stackOffset = 0;
  for (auto &param : function.getParams()) {
    RegClass rc = regClassForType(param->getIRType());
    MachineReg dst = mf.createVReg(rc);
    mf.valueRegs[param.get()] = dst;

    if (rc == RegClass::FPR32) {
      if (nextFpr < std::size(fprArgs)) {
        appendRegInstr(entry, Opcode::F_COPY, dst,
                       {MachineReg::preg(fprArgs[nextFpr++], rc)});
      } else {
        appendLoad(entry, Opcode::FLW, dst,
                   Address::regBase(
                       MachineReg::preg(PhysReg::SP, RegClass::GPR64),
                       stackOffset));
        entry.instrs.back().comment = "incoming_arg";
        stackOffset += 8;
      }
      continue;
    }
    if (nextGpr < std::size(gprArgs)) {
      appendRegInstr(entry, Opcode::COPY, dst,
                     {MachineReg::preg(gprArgs[nextGpr++], RegClass::GPR64)});
    } else {
      appendLoad(entry, isPointerType(param->getIRType()) ? Opcode::LD
                                                          : Opcode::LW,
                 dst,
                 Address::regBase(
                     MachineReg::preg(PhysReg::SP, RegClass::GPR64),
                     stackOffset));
      entry.instrs.back().comment = "incoming_arg";
      stackOffset += 8;
    }
  }
}

const char *instTypeName(Instruction::InstType type) {
  switch (type) {
  case Instruction::RET:
    return "RET";
  case Instruction::BR:
    return "BR";
  case Instruction::ALLOCA:
    return "ALLOCA";
  case Instruction::STORE:
    return "STORE";
  case Instruction::LOAD:
    return "LOAD";
  case Instruction::GEP:
    return "GEP";
  case Instruction::ADD:
    return "ADD";
  case Instruction::FADD:
    return "FADD";
  case Instruction::SUB:
    return "SUB";
  case Instruction::FSUB:
    return "FSUB";
  case Instruction::MUL:
    return "MUL";
  case Instruction::FMUL:
    return "FMUL";
  case Instruction::SDIV:
    return "SDIV";
  case Instruction::UDIV:
    return "UDIV";
  case Instruction::FDIV:
    return "FDIV";
  case Instruction::SREM:
    return "SREM";
  case Instruction::UREM:
    return "UREM";
  case Instruction::FREM:
    return "FREM";
  case Instruction::AND:
    return "AND";
  case Instruction::OR:
    return "OR";
  case Instruction::XOR:
    return "XOR";
  case Instruction::BinaryUnknown:
    return "BinaryUnknown";
  case Instruction::ICMP:
    return "ICMP";
  case Instruction::FCMP:
    return "FCMP";
  case Instruction::SHL:
    return "SHL";
  case Instruction::LSHR:
    return "LSHR";
  case Instruction::ASHR:
    return "ASHR";
  case Instruction::SELECT:
    return "SELECT";
  case Instruction::PHI:
    return "PHI";
  case Instruction::CALL:
    return "CALL";
  case Instruction::TRUNC:
    return "TRUNC";
  case Instruction::ZEXT:
    return "ZEXT";
  case Instruction::SEXT:
    return "SEXT";
  case Instruction::FPTOSI:
    return "FPTOSI";
  case Instruction::SITOFP:
    return "SITOFP";
  case Instruction::BITCAST:
    return "BITCAST";
  }
  return "<unknown>";
}

bool isComparison(BinaryInst::Operation op) {
  switch (op) {
  case BinaryInst::E:
  case BinaryInst::NE:
  case BinaryInst::GE:
  case BinaryInst::L:
  case BinaryInst::LE:
  case BinaryInst::G:
    return true;
  default:
    return false;
  }
}

void lowerIntCompare(MachineFunction &mf, MachineBasicBlock &mbb,
                     BinaryInst &inst, Value *lhs, Value *rhs) {
  MachineReg left = valueReg(mf, mbb, lhs);
  MachineReg right = valueReg(mf, mbb, rhs);
  MachineReg dst = getOrCreateResultReg(mf, inst, RegClass::GPR32);
  MachineReg tmp;

  switch (inst.getOp()) {
  case BinaryInst::E:
    tmp = mf.createVReg(RegClass::GPR32);
    appendRegInstr(mbb, Opcode::XOR, tmp, {left, right});
    appendRegInstr(mbb, Opcode::SEQZ, dst, {tmp});
    break;
  case BinaryInst::NE:
    tmp = mf.createVReg(RegClass::GPR32);
    appendRegInstr(mbb, Opcode::XOR, tmp, {left, right});
    appendRegInstr(mbb, Opcode::SNEZ, dst, {tmp});
    break;
  case BinaryInst::L:
    appendRegInstr(mbb, Opcode::SLT, dst, {left, right});
    break;
  case BinaryInst::G:
    appendRegInstr(mbb, Opcode::SLT, dst, {right, left});
    break;
  case BinaryInst::LE:
    tmp = mf.createVReg(RegClass::GPR32);
    appendRegInstr(mbb, Opcode::SLT, tmp, {right, left});
    appendRegInstr(mbb, Opcode::SEQZ, dst, {tmp});
    break;
  case BinaryInst::GE:
    tmp = mf.createVReg(RegClass::GPR32);
    appendRegInstr(mbb, Opcode::SLT, tmp, {left, right});
    appendRegInstr(mbb, Opcode::SEQZ, dst, {tmp});
    break;
  default:
    throw BackendError("unsupported integer comparison");
  }

  mf.valueRegs[&inst] = dst;
}

void lowerFloatCompare(MachineFunction &mf, MachineBasicBlock &mbb,
                       BinaryInst &inst, Value *lhs, Value *rhs) {
  MachineReg left = valueReg(mf, mbb, lhs);
  MachineReg right = valueReg(mf, mbb, rhs);
  MachineReg dst = getOrCreateResultReg(mf, inst, RegClass::GPR32);
  MachineReg tmp;

  switch (inst.getOp()) {
  case BinaryInst::E:
    appendRegInstr(mbb, Opcode::FEQ_S, dst, {left, right});
    break;
  case BinaryInst::NE:
    tmp = mf.createVReg(RegClass::GPR32);
    appendRegInstr(mbb, Opcode::FEQ_S, tmp, {left, right});
    appendRegInstr(mbb, Opcode::SEQZ, dst, {tmp});
    break;
  case BinaryInst::L:
    appendRegInstr(mbb, Opcode::FLT_S, dst, {left, right});
    break;
  case BinaryInst::G:
    appendRegInstr(mbb, Opcode::FLT_S, dst, {right, left});
    break;
  case BinaryInst::LE:
    appendRegInstr(mbb, Opcode::FLE_S, dst, {left, right});
    break;
  case BinaryInst::GE:
    appendRegInstr(mbb, Opcode::FLE_S, dst, {right, left});
    break;
  default:
    throw BackendError("unsupported floating-point comparison");
  }

  mf.valueRegs[&inst] = dst;
}

void lowerBinary(MachineFunction &mf, MachineBasicBlock &mbb,
                 BinaryInst &inst) {
  Value *lhs = requiredUseValue(inst, 0, "binary instruction");
  Value *rhs = requiredUseValue(inst, 1, "binary instruction");
  const bool floatOperands = lhs && isFloatType(lhs->getIRType());

  if (isComparison(inst.getOp())) {
    if (floatOperands) {
      lowerFloatCompare(mf, mbb, inst, lhs, rhs);
    } else {
      lowerIntCompare(mf, mbb, inst, lhs, rhs);
    }
    return;
  }

  Opcode opcode = Opcode::NOP;
  if (floatOperands) {
    switch (inst.getOp()) {
    case BinaryInst::ADD:
      opcode = Opcode::FADD_S;
      break;
    case BinaryInst::SUB:
      opcode = Opcode::FSUB_S;
      break;
    case BinaryInst::MUL:
      opcode = Opcode::FMUL_S;
      break;
    case BinaryInst::DIV:
      opcode = Opcode::FDIV_S;
      break;
    default:
      throw BackendError("unsupported floating-point binary operation");
    }
  } else {
    switch (inst.getOp()) {
    case BinaryInst::ADD:
      opcode = Opcode::ADDW;
      break;
    case BinaryInst::SUB:
      opcode = Opcode::SUBW;
      break;
    case BinaryInst::MUL:
      opcode = Opcode::MULW;
      break;
    case BinaryInst::DIV:
      opcode = Opcode::DIVW;
      break;
    case BinaryInst::UDIV:
      opcode = Opcode::DIVUW;
      break;
    case BinaryInst::MOD:
      opcode = Opcode::REMW;
      break;
    case BinaryInst::UREM:
      opcode = Opcode::REMUW;
      break;
    case BinaryInst::AND:
      opcode = Opcode::AND;
      break;
    case BinaryInst::OR:
      opcode = Opcode::OR;
      break;
    case BinaryInst::XOR:
      opcode = Opcode::XOR;
      break;
    case BinaryInst::SHL:
      opcode = Opcode::SLLW;
      break;
    case BinaryInst::ASHR:
      opcode = Opcode::SRAW;
      break;
    case BinaryInst::LSHR:
      opcode = Opcode::SRLW;
      break;
    default:
      throw BackendError("unsupported integer binary operation");
    }
  }

  MachineReg dst =
      getOrCreateResultReg(mf, inst, regClassForType(inst.getIRType()));
  appendRegInstr(mbb, opcode, dst, {valueReg(mf, mbb, lhs),
                                    valueReg(mf, mbb, rhs)});
}

void lowerRet(MachineFunction &mf, MachineBasicBlock &mbb, RetInst &inst) {
  Value *retValue = useValue(inst, 0);
  if (retValue) {
    MachineReg src = valueReg(mf, mbb, retValue);
    RegClass rc = regClassForType(retValue->getIRType());
    if (rc == RegClass::FPR32) {
      appendRegInstr(mbb, Opcode::F_COPY,
                     MachineReg::preg(PhysReg::FA0, RegClass::FPR32), {src});
    } else {
      appendRegInstr(mbb, Opcode::COPY,
                     MachineReg::preg(PhysReg::A0, RegClass::GPR64), {src});
    }
  }

  MachineInstr ret;
  ret.opcode = Opcode::RET;
  ret.terminator = true;
  if (retValue) {
    ret.implicitUses.push_back(isFloatType(retValue->getIRType()) ? PhysReg::FA0
                                                                  : PhysReg::A0);
  }
  mbb.instrs.push_back(std::move(ret));
}

void lowerCast(MachineFunction &mf, MachineBasicBlock &mbb, Instruction &inst) {
  Value *srcValue = useValue(inst, 0);
  if (!srcValue) {
    throw BackendError("missing operand while lowering cast instruction");
  }
  MachineReg src = valueReg(mf, mbb, srcValue);
  MachineReg dst =
      getOrCreateResultReg(mf, inst, regClassForType(inst.getIRType()));

  switch (inst.instType) {
  case Instruction::FPTOSI:
    appendRegInstr(mbb, Opcode::FCVT_W_S, dst, {src});
    break;
  case Instruction::SITOFP:
    appendRegInstr(mbb, Opcode::FCVT_S_W, dst, {src});
    break;
  case Instruction::TRUNC:
  case Instruction::ZEXT: {
    MachineReg one = materializeInt(mf, mbb, 1, RegClass::GPR32);
    appendRegInstr(mbb, Opcode::AND, dst, {src, one});
    break;
  }
  case Instruction::SEXT: {
    MachineReg one = materializeInt(mf, mbb, 1, RegClass::GPR32);
    MachineReg masked = mf.createVReg(RegClass::GPR32);
    appendRegInstr(mbb, Opcode::AND, masked, {src, one});
    appendRegInstr(mbb, Opcode::SUBW, dst,
                   {MachineReg::preg(PhysReg::X0, RegClass::GPR32), masked});
    break;
  }
  default:
    throw BackendError("unsupported cast instruction");
  }
}

void lowerAlloca(MachineFunction &mf, LoweringState &state,
                 AllocaInst &inst) {
  auto objectType = pointeeType(&inst, "alloca instruction");
  const int frameIndex = mf.createStackObject(typeSizeBytes(objectType),
                                              typeAlignBytes(objectType),
                                              inst.getIdent());
  mf.valueStackObjects[&inst] = frameIndex;
  state.addresses[&inst] = Address::frameIndexBase(frameIndex);
}

void lowerLoad(MachineFunction &mf, MachineBasicBlock &mbb,
               LoweringState &state, LoadInst &inst) {
  Value *ptr = requiredUseValue(inst, 0, "load instruction");
  MachineReg dst =
      getOrCreateResultReg(mf, inst, regClassForType(inst.getIRType()));
  appendLoad(mbb, loadOpcodeForType(inst.getIRType()), dst,
             addressForValue(mf, mbb, state, ptr, "load instruction"));
}

void lowerStore(MachineFunction &mf, MachineBasicBlock &mbb,
                LoweringState &state, StoreInst &inst) {
  Value *value = requiredUseValue(inst, 0, "store instruction");
  Value *ptr = requiredUseValue(inst, 1, "store instruction");
  appendStore(mbb, storeOpcodeForType(value->getIRType()),
              valueReg(mf, mbb, value),
              addressForValue(mf, mbb, state, ptr, "store instruction"));
}

void lowerBitCast(MachineFunction &mf, MachineBasicBlock &mbb,
                  LoweringState &state, BitCastInst &inst) {
  Value *srcValue = requiredUseValue(inst, 0, "bitcast instruction");
  if (isPointerType(srcValue->getIRType()) && isPointerType(inst.getIRType())) {
    Address address =
        addressForValue(mf, mbb, state, srcValue, "bitcast instruction");
    state.addresses[&inst] = address;
    if (address.kind == Address::Kind::Reg && address.offset == 0) {
      mf.valueRegs[&inst] = address.reg;
    }
    return;
  }

  MachineReg src = valueReg(mf, mbb, srcValue);
  MachineReg dst =
      getOrCreateResultReg(mf, inst, regClassForType(inst.getIRType()));
  appendRegInstr(mbb, Opcode::COPY, dst, {src});
}

void lowerGep(MachineFunction &mf, MachineBasicBlock &mbb,
              LoweringState &state, GetElementPtrInst &inst) {
  Value *base = requiredUseValue(inst, 0, "getelementptr instruction");
  auto existingDst = mf.valueRegs.find(&inst);
  Address baseAddress =
      addressForValue(mf, mbb, state, base, "getelementptr instruction");
  int64_t constOffset = baseAddress.offset;
  MachineReg dynamicOffset;
  auto currentType = pointeeType(base, "getelementptr instruction");

  auto &uses = inst.getUserList();
  for (std::size_t i = 1; i < uses.size(); ++i) {
    Value *index = uses[i]->getValue();
    int64_t scale = 0;
    currentType = advanceGepType(currentType, i == 1, scale);
    if (scale <= 0) {
      throw BackendError("unsupported zero-sized getelementptr component");
    }

    int64_t constantIndex = 0;
    if (intConstant(index, constantIndex)) {
      constOffset += constantIndex * scale;
      continue;
    }

    dynamicOffset = addScaledIndex(mf, mbb, dynamicOffset, index, scale);
  }

  if (!dynamicOffset.isValid()) {
    baseAddress.offset = constOffset;
    MachineReg materialized = materializeAddress(mf, mbb, baseAddress);
    if (existingDst != mf.valueRegs.end()) {
      if (materialized.id != existingDst->second.id ||
          materialized.physical != existingDst->second.physical ||
          materialized.phys != existingDst->second.phys) {
        appendRegInstr(mbb, Opcode::COPY, existingDst->second, {materialized});
      }
      state.addresses[&inst] = Address::regBase(existingDst->second);
      return;
    }
    mf.valueRegs[&inst] = materialized;
    state.addresses[&inst] = baseAddress;
    return;
  }

  baseAddress.offset = constOffset;
  MachineReg baseReg = materializeAddress(mf, mbb, baseAddress);
  MachineReg dst = existingDst != mf.valueRegs.end()
                       ? existingDst->second
                       : mf.createVReg(RegClass::GPR64);
  appendRegInstr(mbb, Opcode::ADD, dst, {baseReg, dynamicOffset});
  mf.valueRegs[&inst] = dst;
  state.addresses[&inst] = Address::regBase(dst);
}

void lowerMemcpy(MachineFunction &mf, MachineBasicBlock &mbb,
                 LoweringState &state, CallInst &inst,
                 const std::string &name) {
  constexpr int64_t kLoopMemcpyThreshold = 256;
  Value *dst = requiredUseValue(inst, 1, name);
  Value *src = requiredUseValue(inst, 2, name);
  Value *sizeValue = requiredUseValue(inst, 3, name);

  int64_t size = 0;
  if (!intConstant(sizeValue, size) || size < 0) {
    throw BackendError("unsupported " + name +
                       " size; only non-negative constant sizes are lowered");
  }

  Address dstAddress = addressForValue(mf, mbb, state, dst, name);
  Address srcAddress = addressForValue(mf, mbb, state, src, name);
  if (size > kLoopMemcpyThreshold) {
    MachineReg dstReg = materializeAddress(mf, mbb, dstAddress);
    MachineReg srcReg = materializeAddress(mf, mbb, srcAddress);
    MachineInstr loop;
    loop.opcode = Opcode::MEMCPY_INLINE;
    loop.uses.push_back(MachineOperand::reg(dstReg));
    loop.uses.push_back(MachineOperand::reg(srcReg));
    loop.uses.push_back(MachineOperand::imm(size));
    loop.implicitDefs = {PhysReg::T3, PhysReg::T4, PhysReg::T5};
    loop.sideEffects = true;
    mbb.instrs.push_back(std::move(loop));
    return;
  }

  int64_t offset = 0;
  for (; offset + 4 <= size; offset += 4) {
    Address loadAddress = srcAddress;
    Address storeAddress = dstAddress;
    loadAddress.offset += offset;
    storeAddress.offset += offset;

    MachineReg tmp = mf.createVReg(RegClass::GPR32);
    appendLoad(mbb, Opcode::LW, tmp, loadAddress);
    appendStore(mbb, Opcode::SW, tmp, storeAddress);
  }
  for (; offset < size; ++offset) {
    Address loadAddress = srcAddress;
    Address storeAddress = dstAddress;
    loadAddress.offset += offset;
    storeAddress.offset += offset;

    MachineReg tmp = mf.createVReg(RegClass::GPR32);
    appendLoad(mbb, Opcode::LBU, tmp, loadAddress);
    appendStore(mbb, Opcode::SB, tmp, storeAddress);
  }
}

void lowerUnsupported(MachineFunction &mf, MachineBasicBlock &mbb,
                      Instruction &inst);

void lowerCall(MachineFunction &mf, MachineBasicBlock &mbb,
               LoweringState &state, CallInst &inst) {
  Value *callee = requiredUseValue(inst, 0, "call instruction");
  std::string name = calleeName(callee);
  if (name == "llvm.memcpy.p0.p0.i32" ||
      name == "llvm.memcpy.p0i8.p0i8.i32") {
    lowerMemcpy(mf, mbb, state, inst, name);
    return;
  }

  if (name.empty()) {
    throw BackendError("unsupported indirect call");
  }

  constexpr PhysReg gprArgs[] = {PhysReg::A0, PhysReg::A1, PhysReg::A2,
                                 PhysReg::A3, PhysReg::A4, PhysReg::A5,
                                 PhysReg::A6, PhysReg::A7};
  constexpr PhysReg fprArgs[] = {PhysReg::FA0, PhysReg::FA1, PhysReg::FA2,
                                 PhysReg::FA3, PhysReg::FA4, PhysReg::FA5,
                                 PhysReg::FA6, PhysReg::FA7};

  struct ArgMove {
    Value *value = nullptr;
    RegClass rc = RegClass::GPR32;
    bool stack = false;
    int64_t stackOffset = 0;
    PhysReg phys = PhysReg::NoReg;
  };

  std::vector<ArgMove> argMoves;
  std::size_t nextGpr = 0;
  std::size_t nextFpr = 0;
  int64_t stackBytes = 0;
  auto &uses = inst.getUserList();
  for (std::size_t i = 1; i < uses.size(); ++i) {
    Value *arg = uses[i]->getValue();
    ArgMove move;
    move.value = arg;
    move.rc = regClassForType(arg->getIRType());
    if (move.rc == RegClass::FPR32) {
      if (nextFpr < std::size(fprArgs)) {
        move.phys = fprArgs[nextFpr++];
      } else {
        move.stack = true;
      }
    } else if (nextGpr < std::size(gprArgs)) {
      move.phys = gprArgs[nextGpr++];
    } else {
      move.stack = true;
    }

    if (move.stack) {
      move.stackOffset = stackBytes;
      stackBytes += 8;
    }
    argMoves.push_back(move);
  }

  stackBytes = ((stackBytes + 15) / 16) * 16;
  std::vector<MachineReg> argRegs;
  argRegs.reserve(argMoves.size());
  for (const ArgMove &move : argMoves) {
    if (isPointerType(move.value->getIRType())) {
      argRegs.push_back(materializeAddress(
          mf, mbb, addressForValue(mf, mbb, state, move.value, "call argument")));
    } else {
      argRegs.push_back(valueReg(mf, mbb, move.value));
    }
  }

  if (stackBytes > 0) {
    appendImmInstr(mbb, Opcode::ADDI,
                   MachineReg::preg(PhysReg::SP, RegClass::GPR64),
                   MachineOperand::reg(
                       MachineReg::preg(PhysReg::SP, RegClass::GPR64)),
                   -stackBytes);
  }

  for (std::size_t i = 0; i < argMoves.size(); ++i) {
    const ArgMove &move = argMoves[i];
    if (!move.stack) {
      MachineReg dst =
          MachineReg::preg(move.phys, move.rc == RegClass::FPR32
                                          ? RegClass::FPR32
                                          : RegClass::GPR64);
      appendRegInstr(mbb, move.rc == RegClass::FPR32 ? Opcode::F_COPY
                                                     : Opcode::COPY,
                     dst, {argRegs[i]});
      continue;
    }

    Address stackSlot = Address::regBase(
        MachineReg::preg(PhysReg::SP, RegClass::GPR64), move.stackOffset);
    Opcode storeOpcode = Opcode::SW;
    if (move.rc == RegClass::FPR32) {
      storeOpcode = Opcode::FSW;
    } else if (isPointerType(move.value->getIRType())) {
      storeOpcode = Opcode::SD;
    }
    appendStore(mbb, storeOpcode, argRegs[i], stackSlot);
  }

  MachineInstr call;
  call.opcode = Opcode::CALL;
  call.uses.push_back(MachineOperand::global(name));
  call.implicitDefs.push_back(PhysReg::RA);
  call.sideEffects = true;
  mbb.instrs.push_back(std::move(call));

  if (stackBytes > 0) {
    appendImmInstr(mbb, Opcode::ADDI,
                   MachineReg::preg(PhysReg::SP, RegClass::GPR64),
                   MachineOperand::reg(
                       MachineReg::preg(PhysReg::SP, RegClass::GPR64)),
                   stackBytes);
  }

  if (!isVoidType(inst.getIRType())) {
    RegClass rc = regClassForType(inst.getIRType());
    MachineReg dst = getOrCreateResultReg(mf, inst, rc);
    appendRegInstr(mbb, rc == RegClass::FPR32 ? Opcode::F_COPY : Opcode::COPY,
                   dst,
                   {MachineReg::preg(rc == RegClass::FPR32 ? PhysReg::FA0
                                                           : PhysReg::A0,
                                      rc == RegClass::FPR32 ? RegClass::FPR32
                                                           : RegClass::GPR64)});
  }
}

void lowerBranch(MachineFunction &mf, MachineBasicBlock &mbb,
                 LoweringState &state, BrInst &inst) {
  auto &uses = inst.getUserList();
  if (uses.size() == 1) {
    auto *target = dynamic_cast<BasicBlock *>(uses[0]->getValue());
    MachineInstr jump;
    jump.opcode = Opcode::J;
    jump.uses.push_back(
        MachineOperand::block(labelForBlock(state, target, "branch")));
    jump.terminator = true;
    mbb.instrs.push_back(std::move(jump));
    return;
  }

  if (uses.size() != 3) {
    throw BackendError("malformed branch instruction");
  }

  Value *cond = uses[0]->getValue();
  auto *trueBlock = dynamic_cast<BasicBlock *>(uses[1]->getValue());
  auto *falseBlock = dynamic_cast<BasicBlock *>(uses[2]->getValue());

  MachineInstr branch;
  branch.opcode = Opcode::BNEZ;
  branch.uses.push_back(MachineOperand::reg(valueReg(mf, mbb, cond)));
  branch.uses.push_back(
      MachineOperand::block(labelForBlock(state, trueBlock, "branch")));
  branch.terminator = true;
  mbb.instrs.push_back(std::move(branch));

  MachineInstr jump;
  jump.opcode = Opcode::J;
  jump.uses.push_back(
      MachineOperand::block(labelForBlock(state, falseBlock, "branch")));
  jump.terminator = true;
  mbb.instrs.push_back(std::move(jump));
}

void lowerPhi(MachineFunction &mf, MachineBasicBlock &mbb,
              LoweringState &state, PhiInst &inst) {
  MachineReg dst =
      getOrCreateResultReg(mf, inst, regClassForType(inst.getIRType()));

  MachineInstr phi;
  phi.opcode = Opcode::PHI;
  phi.defs.push_back(MachineOperand::reg(dst));
  for (unsigned i = 0; i < inst.getNumIncomingValues(); ++i) {
    phi.uses.push_back(MachineOperand::value(inst.getIncomingValue(i)));
    phi.uses.push_back(MachineOperand::block(
        labelForBlock(state, inst.getIncomingBlock(i), "phi instruction")));
  }
  mbb.instrs.push_back(std::move(phi));
}

void lowerSelect(MachineFunction &mf, MachineBasicBlock &mbb, SelectInst &inst) {
  Value *cond = requiredUseValue(inst, 0, "select instruction");
  Value *trueValue = requiredUseValue(inst, 1, "select instruction");
  Value *falseValue = requiredUseValue(inst, 2, "select instruction");

  MachineReg dst =
      getOrCreateResultReg(mf, inst, regClassForType(inst.getIRType()));
  MachineInstr select;
  select.opcode = Opcode::SELECT;
  select.defs.push_back(MachineOperand::reg(dst));
  select.uses.push_back(MachineOperand::reg(valueReg(mf, mbb, cond)));
  select.uses.push_back(MachineOperand::reg(valueReg(mf, mbb, trueValue)));
  select.uses.push_back(MachineOperand::reg(valueReg(mf, mbb, falseValue)));
  mbb.instrs.push_back(std::move(select));
}

void lowerUnsupported(MachineFunction &mf, MachineBasicBlock &mbb,
                      Instruction &inst) {
  (void)mbb;
  throw BackendError("unsupported IR instruction " +
                     std::string(instTypeName(inst.instType)) +
                     " while lowering function " + mf.name);
}

void lowerInstruction(MachineFunction &mf, MachineBasicBlock &mbb,
                      LoweringState &state, Instruction &inst) {
  if (auto *branch = dynamic_cast<BrInst *>(&inst)) {
    lowerBranch(mf, mbb, state, *branch);
    return;
  }
  if (auto *phi = dynamic_cast<PhiInst *>(&inst)) {
    lowerPhi(mf, mbb, state, *phi);
    return;
  }
  if (auto *ret = dynamic_cast<RetInst *>(&inst)) {
    lowerRet(mf, mbb, *ret);
    return;
  }
  if (auto *binary = dynamic_cast<BinaryInst *>(&inst)) {
    lowerBinary(mf, mbb, *binary);
    return;
  }
  if (auto *alloca = dynamic_cast<AllocaInst *>(&inst)) {
    lowerAlloca(mf, state, *alloca);
    return;
  }
  if (auto *load = dynamic_cast<LoadInst *>(&inst)) {
    lowerLoad(mf, mbb, state, *load);
    return;
  }
  if (auto *store = dynamic_cast<StoreInst *>(&inst)) {
    lowerStore(mf, mbb, state, *store);
    return;
  }
  if (auto *gep = dynamic_cast<GetElementPtrInst *>(&inst)) {
    lowerGep(mf, mbb, state, *gep);
    return;
  }
  if (auto *call = dynamic_cast<CallInst *>(&inst)) {
    lowerCall(mf, mbb, state, *call);
    return;
  }
  if (auto *bitcast = dynamic_cast<BitCastInst *>(&inst)) {
    lowerBitCast(mf, mbb, state, *bitcast);
    return;
  }
  if (auto *select = dynamic_cast<SelectInst *>(&inst)) {
    lowerSelect(mf, mbb, *select);
    return;
  }

  switch (inst.instType) {
  case Instruction::TRUNC:
  case Instruction::ZEXT:
  case Instruction::SEXT:
  case Instruction::FPTOSI:
  case Instruction::SITOFP:
    lowerCast(mf, mbb, inst);
    return;
  default:
    lowerUnsupported(mf, mbb, inst);
    return;
  }
}

} // namespace

IRLowering::IRLowering(Module &module) : module_(module) {}

MachineModule IRLowering::lowerModule() {
  MachineModule mm;
  lowerGlobals(mm);

  for (auto &function : module_.getFunctions()) {
    if (function) {
      mm.functions.push_back(lowerFunction(*function));
    }
  }

  return mm;
}

void IRLowering::lowerGlobals(MachineModule &mm) {
  for (auto &var : module_.getGlobalVar()) {
    if (!var) {
      continue;
    }

    auto pointerType = std::dynamic_pointer_cast<POINTERType>(var->getIRType());
    std::shared_ptr<IRType> objectType =
        pointerType ? pointerType->getPointerType() : var->getIRType();

    GlobalObject object;
    object.name = var->getIdent();
    object.ir = var.get();
    object.constant = (var->vartag == Variable::Constant);
    object.size = typeSizeBytes(objectType);
    object.align = typeAlignBytes(objectType);
    mm.globals.push_back(std::move(object));
  }
}

MachineFunction IRLowering::lowerFunction(Function &function) {
  MachineFunction mf;
  mf.name = function.getIdent();
  mf.irFunction = &function;
  mf.returnsValue = !isVoidType(function.getIRType());
  mf.returnClass = mf.returnsValue ? regClassForType(function.getIRType())
                                   : RegClass::GPR32;

  int blockIndex = 0;
  for (BasicBlock *irBlock : function) {
    MachineBasicBlock block;
    block.label = blockLabel(function, blockIndex++);
    block.irBlock = irBlock;
    mf.blocks.push_back(std::move(block));
  }

  if (mf.blocks.empty()) {
    MachineBasicBlock entry;
    entry.label = function.getIdent();
    MachineInstr ret;
    ret.opcode = Opcode::RET;
    ret.terminator = true;
    entry.instrs.push_back(std::move(ret));
    mf.blocks.push_back(std::move(entry));
    return mf;
  }

  LoweringState state;
  for (const MachineBasicBlock &mbb : mf.blocks) {
    if (mbb.irBlock) {
      state.blockLabels[mbb.irBlock] = mbb.label;
    }
  }
  lowerFunctionParams(mf, mf.blocks.front(), function);
  for (MachineBasicBlock &mbb : mf.blocks) {
    if (!mbb.irBlock) {
      continue;
    }
    for (Instruction *inst : *mbb.irBlock) {
      lowerInstruction(mf, mbb, state, *inst);
    }
  }

  FrameLowering frameLowering;
  frameLowering.assignFrameLayout(mf);
  frameLowering.expandFrameIndices(mf);

  return mf;
}

} // namespace riscv
