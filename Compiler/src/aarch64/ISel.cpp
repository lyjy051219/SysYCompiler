#include "aarch64/ISel.hpp"

#include "aarch64/Frame.hpp"
#include "aarch64/Target.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace aarch64 {
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

std::string scalarTypeName(const std::shared_ptr<IRType> &type) {
  if (!type) {
    return "<missing>";
  }
  if (type->getEnum() == IR_FLOAT) {
    return "f32";
  }
  if (type->getEnum() == IR_INT) {
    return "i" + std::to_string(type->getSize() * 8);
  }
  if (type->getEnum() == IR_POINTER) {
    return "pointer";
  }
  return "non-scalar";
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

struct DivRemKey {
  Value *lhs = nullptr;
  int64_t divisor = 0;
  BasicBlock *block = nullptr;

  bool operator==(const DivRemKey &other) const {
    return lhs == other.lhs && divisor == other.divisor &&
           block == other.block;
  }
};

struct DivRemKeyHash {
  std::size_t operator()(const DivRemKey &key) const {
    std::size_t hash = std::hash<Value *>{}(key.lhs);
    hash ^= std::hash<int64_t>{}(key.divisor) + 0x9e3779b9 +
            (hash << 6) + (hash >> 2);
    hash ^= std::hash<BasicBlock *>{}(key.block) + 0x9e3779b9 +
            (hash << 6) + (hash >> 2);
    return hash;
  }
};

struct LoweringState {
  std::unordered_map<Value *, Address> addresses;
  std::unordered_map<BasicBlock *, std::string> blockLabels;
  std::unordered_map<DivRemKey, MachineReg, DivRemKeyHash> divRemQuotients;
  BasicBlock *divRemBlock = nullptr;

  void beginDivRemBlock(BasicBlock *block) {
    if (divRemBlock != block) {
      divRemQuotients.clear();
      divRemBlock = block;
    }
  }
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

bool intConstantImpl(Value *value, int64_t &out, unsigned depth) {
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

  // Some middle-end transformations materialize a constant as an otherwise
  // ordinary integer instruction (for example, add i32 0, 65535).  Look
  // through a small, acyclic expression tree so the instruction selector can
  // still use immediate and constant-divisor lowering.  The original
  // instruction is still lowered normally; this helper only observes it.
  auto *binary = dynamic_cast<BinaryInst *>(value);
  if (!binary || depth >= 4 || !value->getIRType() ||
      value->getIRType()->getEnum() != IR_INT) {
    return false;
  }

  auto &operands = binary->getUserList();
  if (operands.size() != 2 || !operands[0] || !operands[1]) {
    return false;
  }

  int64_t lhsValue = 0;
  int64_t rhsValue = 0;
  if (!intConstantImpl(operands[0]->getValue(), lhsValue, depth + 1) ||
      !intConstantImpl(operands[1]->getValue(), rhsValue, depth + 1)) {
    return false;
  }

  // Integer instructions in this backend operate on i32 values.  Normalize
  // operands before folding so signed division/remainder and shifts follow
  // the same width as the eventual machine instruction.
  const int64_t lhs = static_cast<int32_t>(lhsValue);
  const int64_t rhs = static_cast<int32_t>(rhsValue);
  const uint32_t lhsBits = static_cast<uint32_t>(lhs);
  const uint32_t rhsBits = static_cast<uint32_t>(rhs);

  auto asI32 = [](uint32_t bits) -> int64_t {
    return static_cast<int64_t>(static_cast<int32_t>(bits));
  };

  switch (binary->getOp()) {
  case BinaryInst::ADD:
    out = static_cast<int32_t>(lhs + rhs);
    return true;
  case BinaryInst::SUB:
    out = static_cast<int32_t>(lhs - rhs);
    return true;
  case BinaryInst::MUL:
    out = static_cast<int32_t>(lhs * rhs);
    return true;
  case BinaryInst::DIV:
    if (rhs == 0 ||
        (lhs == std::numeric_limits<int32_t>::min() && rhs == -1)) {
      return false;
    }
    out = lhs / rhs;
    return true;
  case BinaryInst::UDIV:
    if (rhsBits == 0) {
      return false;
    }
    out = asI32(lhsBits / rhsBits);
    return true;
  case BinaryInst::MOD:
    if (rhs == 0 ||
        (lhs == std::numeric_limits<int32_t>::min() && rhs == -1)) {
      return false;
    }
    out = lhs % rhs;
    return true;
  case BinaryInst::UREM:
    if (rhsBits == 0) {
      return false;
    }
    out = asI32(lhsBits % rhsBits);
    return true;
  case BinaryInst::AND:
    out = asI32(lhsBits & rhsBits);
    return true;
  case BinaryInst::OR:
    out = asI32(lhsBits | rhsBits);
    return true;
  case BinaryInst::XOR:
    out = asI32(lhsBits ^ rhsBits);
    return true;
  case BinaryInst::SHL:
    if (rhs < 0 || rhs >= 32) {
      return false;
    }
    out = asI32(lhsBits << static_cast<unsigned>(rhs));
    return true;
  case BinaryInst::LSHR:
    if (rhs < 0 || rhs >= 32) {
      return false;
    }
    out = asI32(lhsBits >> static_cast<unsigned>(rhs));
    return true;
  case BinaryInst::ASHR:
    if (rhs < 0 || rhs >= 32) {
      return false;
    }
    out = static_cast<int32_t>(lhs) >> static_cast<unsigned>(rhs);
    return true;
  case BinaryInst::E:
    out = lhs == rhs ? 1 : 0;
    return true;
  case BinaryInst::NE:
    out = lhs != rhs ? 1 : 0;
    return true;
  case BinaryInst::GE:
    out = lhs >= rhs ? 1 : 0;
    return true;
  case BinaryInst::L:
    out = lhs < rhs ? 1 : 0;
    return true;
  case BinaryInst::LE:
    out = lhs <= rhs ? 1 : 0;
    return true;
  case BinaryInst::G:
    out = lhs > rhs ? 1 : 0;
    return true;
  }

  return false;
}

bool intConstant(Value *value, int64_t &out) {
  return intConstantImpl(value, out, 0);
}

bool positivePowerOfTwo(int64_t value) {
  return value > 0 && (value & (value - 1)) == 0;
}

int log2PowerOfTwo(int64_t value) {
  int shift = 0;
  while (value > 1) {
    value >>= 1;
    ++shift;
  }
  return shift;
}

struct SignedDivMagic {
  int64_t multiplier = 0;
  int shift = 0;
  bool addDividend = false;
};

SignedDivMagic signedDivMagicForPositiveDivisor(int64_t divisor) {
  const uint64_t ad = static_cast<uint64_t>(divisor);
  const uint64_t two31 = 1ULL << 31;
  const uint64_t anc = two31 - 1 - (two31 - 1) % ad;
  int p = 31;
  uint64_t q1 = two31 / anc;
  uint64_t r1 = two31 - q1 * anc;
  uint64_t q2 = two31 / ad;
  uint64_t r2 = two31 - q2 * ad;

  while (true) {
    ++p;
    q1 <<= 1;
    r1 <<= 1;
    if (r1 >= anc) {
      ++q1;
      r1 -= anc;
    }
    q2 <<= 1;
    r2 <<= 1;
    if (r2 >= ad) {
      ++q2;
      r2 -= ad;
    }
    const uint64_t delta = ad - r2;
    if (q1 > delta || (q1 == delta && r1 != 0)) {
      break;
    }
  }

  const uint64_t magicBits = (q2 + 1) & 0xffffffffULL;
  int64_t multiplier = static_cast<int64_t>(magicBits);
  if (multiplier >= (1LL << 31)) {
    multiplier -= (1LL << 32);
  }
  return SignedDivMagic{multiplier, p - 32, multiplier < 0};
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

void appendCopyInstr(MachineBasicBlock &mbb, MachineReg dst, MachineReg src) {
  appendRegInstr(mbb, dst.rc == RegClass::FPR32 ? Opcode::F_COPY
                                                : Opcode::COPY,
                 dst, {src});
}

MachineReg materializeInt(MachineFunction &mf, MachineBasicBlock &mbb,
                          int64_t value, RegClass rc) {
  if (value == 0) {
    return MachineReg::preg(PhysReg::X0, rc);
  }
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

MachineReg getOrCreateResultReg(MachineFunction &mf, Instruction &inst,
                                RegClass rc);
MachineReg valueReg(MachineFunction &mf, MachineBasicBlock &mbb,
                    Value *value);
void appendImmInstr(MachineBasicBlock &mbb, Opcode opcode, MachineReg def,
                    MachineOperand base, int64_t imm);

MachineReg buildSignedPowerOfTwoAdjusted(MachineFunction &mf,
                                         MachineBasicBlock &mbb,
                                         MachineReg lhs, int64_t divisor) {
  const int shift = log2PowerOfTwo(divisor);
  MachineReg bias = mf.createVReg(RegClass::GPR32);
  if (divisor == 2) {
    appendImmInstr(mbb, Opcode::SRLIW, bias, MachineOperand::reg(lhs), 31);
  } else {
    MachineReg sign = mf.createVReg(RegClass::GPR32);
    appendImmInstr(mbb, Opcode::SRAIW, sign, MachineOperand::reg(lhs), 31);
    appendImmInstr(mbb, Opcode::SRLIW, bias, MachineOperand::reg(sign),
                   32 - shift);
  }

  MachineReg adjusted = mf.createVReg(RegClass::GPR32);
  appendRegInstr(mbb, Opcode::ADDW, adjusted, {lhs, bias});
  return adjusted;
}

void lowerSignedDivByPowerOfTwo(MachineFunction &mf, MachineBasicBlock &mbb,
                                MachineReg dst, MachineReg lhs,
                                int64_t divisor) {
  if (divisor == 1) {
    appendCopyInstr(mbb, dst, lhs);
    return;
  }

  MachineReg adjusted =
      buildSignedPowerOfTwoAdjusted(mf, mbb, lhs, divisor);
  appendImmInstr(mbb, Opcode::SRAIW, dst, MachineOperand::reg(adjusted),
                 log2PowerOfTwo(divisor));
}

void lowerSignedRemByPowerOfTwo(MachineFunction &mf, MachineBasicBlock &mbb,
                                MachineReg dst, MachineReg lhs,
                                int64_t divisor, MachineReg quotient) {
  if (divisor == 1) {
    appendCopyInstr(mbb, dst, MachineReg::preg(PhysReg::X0, RegClass::GPR32));
    return;
  }

  MachineReg scaled = mf.createVReg(RegClass::GPR32);
  appendImmInstr(mbb, Opcode::SLLIW, scaled, MachineOperand::reg(quotient),
                 log2PowerOfTwo(divisor));
  appendRegInstr(mbb, Opcode::SUBW, dst, {lhs, scaled});
}

void lowerSignedRemByPowerOfTwo(MachineFunction &mf, MachineBasicBlock &mbb,
                                MachineReg dst, MachineReg lhs,
                                int64_t divisor) {
  if (divisor == 1) {
    appendCopyInstr(mbb, dst, MachineReg::preg(PhysReg::X0, RegClass::GPR32));
    return;
  }
  if (fitsSigned12(-divisor)) {
    MachineReg adjusted =
        buildSignedPowerOfTwoAdjusted(mf, mbb, lhs, divisor);
    MachineReg multiple = mf.createVReg(RegClass::GPR32);
    appendImmInstr(mbb, Opcode::ANDI, multiple,
                   MachineOperand::reg(adjusted), -divisor);
    appendRegInstr(mbb, Opcode::SUBW, dst, {lhs, multiple});
    return;
  }

  MachineReg quotient = mf.createVReg(RegClass::GPR32);
  lowerSignedDivByPowerOfTwo(mf, mbb, quotient, lhs, divisor);
  lowerSignedRemByPowerOfTwo(mf, mbb, dst, lhs, divisor, quotient);
}

bool hasMatchingSignedDivUse(Value *lhs, int64_t divisor, BasicBlock *block,
                             const BinaryInst *remainder) {
  if (!lhs || !block)
    return false;
  for (auto It = lhs->getUseList().begin(); It != lhs->getUseList().end(); ++It) {
    Use *UseNode = *It;
    auto *Bin = UseNode ? dynamic_cast<BinaryInst *>(UseNode->getUser()) : nullptr;
    if (!Bin || Bin == remainder || Bin->getParent() != block ||
        Bin->getOp() != BinaryInst::DIV || Bin->getUserList().size() != 2 ||
        Bin->getUserList()[0]->getValue() != lhs) {
      continue;
    }
    int64_t OtherDivisor = 0;
    if (intConstant(Bin->getUserList()[1]->getValue(), OtherDivisor) &&
        OtherDivisor == divisor) {
      return true;
    }
  }
  return false;
}

void lowerSignedDivByPositiveConstant(MachineFunction &mf,
                                      MachineBasicBlock &mbb, MachineReg dst,
                                      MachineReg lhs, int64_t divisor) {
  const SignedDivMagic magic = signedDivMagicForPositiveDivisor(divisor);
  MachineReg magicReg = materializeInt(mf, mbb, magic.multiplier,
                                       RegClass::GPR64);

  MachineReg product = mf.createVReg(RegClass::GPR64);
  appendRegInstr(mbb, Opcode::MUL, product, {lhs, magicReg});

  MachineReg high = mf.createVReg(RegClass::GPR64);
  appendImmInstr(mbb, Opcode::SRAI, high, MachineOperand::reg(product), 32);

  MachineReg quotient = high;
  if (magic.addDividend) {
    quotient = mf.createVReg(RegClass::GPR32);
    appendRegInstr(mbb, Opcode::ADDW, quotient, {high, lhs});
  }

  if (magic.shift > 0) {
    MachineReg shifted = mf.createVReg(RegClass::GPR32);
    appendImmInstr(mbb, Opcode::SRAIW, shifted, MachineOperand::reg(quotient),
                   magic.shift);
    quotient = shifted;
  }

  MachineReg sign = mf.createVReg(RegClass::GPR32);
  appendImmInstr(mbb, Opcode::SRAIW, sign, MachineOperand::reg(lhs), 31);
  appendRegInstr(mbb, Opcode::SUBW, dst, {quotient, sign});
}

void lowerSignedRemByPositiveConstant(MachineFunction &mf,
                                      MachineBasicBlock &mbb, MachineReg dst,
                                      MachineReg lhs, int64_t divisor,
                                      MachineReg quotient) {
  MachineReg divisorReg = materializeInt(mf, mbb, divisor, RegClass::GPR32);
  MachineReg scaled = mf.createVReg(RegClass::GPR32);
  appendRegInstr(mbb, Opcode::MULW, scaled, {quotient, divisorReg});
  appendRegInstr(mbb, Opcode::SUBW, dst, {lhs, scaled});
}

void lowerSignedRemByPositiveConstant(MachineFunction &mf,
                                      MachineBasicBlock &mbb, MachineReg dst,
                                      MachineReg lhs, int64_t divisor) {
  MachineReg quotient = mf.createVReg(RegClass::GPR32);
  lowerSignedDivByPositiveConstant(mf, mbb, quotient, lhs, divisor);
  lowerSignedRemByPositiveConstant(mf, mbb, dst, lhs, divisor, quotient);
}

bool findDivRemQuotient(LoweringState &state, Value *lhs, int64_t divisor,
                        BasicBlock *block, MachineReg &quotient) {
  auto found = state.divRemQuotients.find(DivRemKey{lhs, divisor, block});
  if (found == state.divRemQuotients.end()) {
    return false;
  }
  quotient = found->second;
  return true;
}

void rememberDivRemQuotient(LoweringState &state, Value *lhs,
                            int64_t divisor, BasicBlock *block,
                            MachineReg quotient) {
  state.divRemQuotients[DivRemKey{lhs, divisor, block}] = quotient;
}

bool lowerIntegerPowerOfTwoOp(MachineFunction &mf, MachineBasicBlock &mbb,
                              LoweringState &state, BinaryInst &inst,
                              Value *lhs, Value *rhs) {
  int64_t rhsValue = 0;
  if (!intConstant(rhs, rhsValue) || !positivePowerOfTwo(rhsValue)) {
    return false;
  }

  MachineReg dst = getOrCreateResultReg(mf, inst, RegClass::GPR32);
  MachineReg left = valueReg(mf, mbb, lhs);
  const int shift = log2PowerOfTwo(rhsValue);

  switch (inst.getOp()) {
  case BinaryInst::MUL:
    if (rhsValue == 1) {
      appendCopyInstr(mbb, dst, left);
    } else {
      appendImmInstr(mbb, Opcode::SLLIW, dst, MachineOperand::reg(left),
                     shift);
    }
    return true;
  case BinaryInst::DIV:
    if (rhsValue > 1) {
      MachineReg quotient;
      if (findDivRemQuotient(state, lhs, rhsValue, mbb.irBlock, quotient)) {
        appendCopyInstr(mbb, dst, quotient);
      } else {
        lowerSignedDivByPowerOfTwo(mf, mbb, dst, left, rhsValue);
        rememberDivRemQuotient(state, lhs, rhsValue, mbb.irBlock, dst);
      }
    } else {
      lowerSignedDivByPowerOfTwo(mf, mbb, dst, left, rhsValue);
    }
    return true;
  case BinaryInst::UDIV:
    if (rhsValue == 1) {
      appendCopyInstr(mbb, dst, left);
    } else {
      appendImmInstr(mbb, Opcode::SRLIW, dst, MachineOperand::reg(left),
                     shift);
    }
    return true;
  case BinaryInst::MOD:
    if (rhsValue > 1) {
      MachineReg quotient;
      if (findDivRemQuotient(state, lhs, rhsValue, mbb.irBlock, quotient)) {
        lowerSignedRemByPowerOfTwo(mf, mbb, dst, left, rhsValue, quotient);
      } else if (hasMatchingSignedDivUse(lhs, rhsValue, mbb.irBlock, &inst)) {
        quotient = mf.createVReg(RegClass::GPR32);
        lowerSignedDivByPowerOfTwo(mf, mbb, quotient, left, rhsValue);
        rememberDivRemQuotient(state, lhs, rhsValue, mbb.irBlock, quotient);
        lowerSignedRemByPowerOfTwo(mf, mbb, dst, left, rhsValue, quotient);
      } else {
        lowerSignedRemByPowerOfTwo(mf, mbb, dst, left, rhsValue);
      }
    } else {
      lowerSignedRemByPowerOfTwo(mf, mbb, dst, left, rhsValue);
    }
    return true;
  case BinaryInst::UREM:
    if (rhsValue == 1) {
      appendCopyInstr(mbb, dst, MachineReg::preg(PhysReg::X0, RegClass::GPR32));
    } else {
      if (fitsSigned12(rhsValue - 1)) {
        appendImmInstr(mbb, Opcode::ANDI, dst, MachineOperand::reg(left),
                       rhsValue - 1);
      } else {
        MachineReg mask =
            materializeInt(mf, mbb, rhsValue - 1, RegClass::GPR32);
        appendRegInstr(mbb, Opcode::AND, dst, {left, mask});
      }
    }
    return true;
  default:
    return false;
  }
}

bool lowerIntegerPositiveConstantDivRem(MachineFunction &mf,
                                        MachineBasicBlock &mbb,
                                        LoweringState &state,
                                        BinaryInst &inst, Value *lhs,
                                        Value *rhs) {
  int64_t rhsValue = 0;
  if (!intConstant(rhs, rhsValue) || rhsValue <= 1 ||
      positivePowerOfTwo(rhsValue)) {
    return false;
  }

  MachineReg dst = getOrCreateResultReg(mf, inst, RegClass::GPR32);
  MachineReg left = valueReg(mf, mbb, lhs);
  switch (inst.getOp()) {
  case BinaryInst::DIV:
    {
      MachineReg quotient;
      if (findDivRemQuotient(state, lhs, rhsValue, mbb.irBlock, quotient)) {
        appendCopyInstr(mbb, dst, quotient);
      } else {
        lowerSignedDivByPositiveConstant(mf, mbb, dst, left, rhsValue);
        rememberDivRemQuotient(state, lhs, rhsValue, mbb.irBlock, dst);
      }
    }
    return true;
  case BinaryInst::MOD:
    {
      MachineReg quotient;
      if (!findDivRemQuotient(state, lhs, rhsValue, mbb.irBlock, quotient)) {
        quotient = mf.createVReg(RegClass::GPR32);
        lowerSignedDivByPositiveConstant(mf, mbb, quotient, left, rhsValue);
        rememberDivRemQuotient(state, lhs, rhsValue, mbb.irBlock, quotient);
      }
      lowerSignedRemByPositiveConstant(mf, mbb, dst, left, rhsValue,
                                       quotient);
    }
    return true;
  default:
    return false;
  }
}

bool lowerIntegerImmediateOp(MachineFunction &mf, MachineBasicBlock &mbb,
                             BinaryInst &inst, Value *lhs, Value *rhs) {
  int64_t rhsValue = 0;
  if (intConstant(rhs, rhsValue)) {
    MachineReg dst = getOrCreateResultReg(mf, inst, RegClass::GPR32);
    MachineReg left = valueReg(mf, mbb, lhs);
    switch (inst.getOp()) {
    case BinaryInst::ADD:
      if (fitsSigned12(rhsValue)) {
        appendImmInstr(mbb, Opcode::ADDIW, dst, MachineOperand::reg(left),
                       rhsValue);
        return true;
      }
      break;
    case BinaryInst::SUB:
      if (fitsSigned12(-rhsValue)) {
        appendImmInstr(mbb, Opcode::ADDIW, dst, MachineOperand::reg(left),
                       -rhsValue);
        return true;
      }
      break;
    case BinaryInst::AND:
      if (fitsSigned12(rhsValue)) {
        appendImmInstr(mbb, Opcode::ANDI, dst, MachineOperand::reg(left),
                       rhsValue);
        return true;
      }
      break;
    case BinaryInst::OR:
      if (fitsSigned12(rhsValue)) {
        appendImmInstr(mbb, Opcode::ORI, dst, MachineOperand::reg(left),
                       rhsValue);
        return true;
      }
      break;
    case BinaryInst::XOR:
      if (fitsSigned12(rhsValue)) {
        appendImmInstr(mbb, Opcode::XORI, dst, MachineOperand::reg(left),
                       rhsValue);
        return true;
      }
      break;
    case BinaryInst::SHL:
      if (rhsValue >= 0 && rhsValue < 32) {
        appendImmInstr(mbb, Opcode::SLLIW, dst, MachineOperand::reg(left),
                       rhsValue);
        return true;
      }
      break;
    case BinaryInst::ASHR:
      if (rhsValue >= 0 && rhsValue < 32) {
        appendImmInstr(mbb, Opcode::SRAIW, dst, MachineOperand::reg(left),
                       rhsValue);
        return true;
      }
      break;
    case BinaryInst::LSHR:
      if (rhsValue >= 0 && rhsValue < 32) {
        appendImmInstr(mbb, Opcode::SRLIW, dst, MachineOperand::reg(left),
                       rhsValue);
        return true;
      }
      break;
    default:
      break;
    }
  }

  int64_t lhsValue = 0;
  if (intConstant(lhs, lhsValue) && fitsSigned12(lhsValue) &&
      (inst.getOp() == BinaryInst::ADD || inst.getOp() == BinaryInst::AND ||
       inst.getOp() == BinaryInst::OR || inst.getOp() == BinaryInst::XOR)) {
    MachineReg dst = getOrCreateResultReg(mf, inst, RegClass::GPR32);
    MachineReg right = valueReg(mf, mbb, rhs);
    Opcode opcode = Opcode::ADDIW;
    if (inst.getOp() == BinaryInst::AND) {
      opcode = Opcode::ANDI;
    } else if (inst.getOp() == BinaryInst::OR) {
      opcode = Opcode::ORI;
    } else if (inst.getOp() == BinaryInst::XOR) {
      opcode = Opcode::XORI;
    }
    appendImmInstr(mbb, opcode, dst, MachineOperand::reg(right), lhsValue);
    return true;
  }

  return false;
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
    if (!isVoidType(instruction->getIRType())) {
      return getOrCreateResultReg(
          mf, *instruction, regClassForType(instruction->getIRType()));
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
  // ARM64 地址计算按 64 位进行：i32 索引先符号扩展到 GPR64，
  // 避免 lsl/mul/add 出现 w/x 混宽，也保证负索引语义正确。
  MachineReg index64 = indexReg;
  if (indexReg.rc != RegClass::GPR64) {
    index64 = mf.createVReg(RegClass::GPR64);
    appendRegInstr(mbb, Opcode::SXTW, index64, {indexReg});
  }
  MachineReg scaled = index64;
  if (scale != 1) {
    scaled = mf.createVReg(RegClass::GPR64);
    if (positivePowerOfTwo(scale)) {
      appendImmInstr(mbb, Opcode::SLLI, scaled, MachineOperand::reg(index64),
                     log2PowerOfTwo(scale));
    } else {
      MachineReg scaleReg = materializeInt(mf, mbb, scale, RegClass::GPR64);
      appendRegInstr(mbb, Opcode::MUL, scaled, {index64, scaleReg});
    }
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
  MachineReg dst = getOrCreateResultReg(mf, inst, RegClass::GPR32);
  MachineReg tmp;
  // Equality/inequality compares reduce to XOR followed by a zero test.  The
  // XOR temporary must stay in the same GPR class as the compared values:
  // pointers are GPR64 while ordinary SysY integers are GPR32.  The compare
  // result itself remains a boolean GPR32 regardless of operand width.
  const RegClass compareClass = left.rc;

  int64_t rhsValue = 0;
  if ((inst.getOp() == BinaryInst::E || inst.getOp() == BinaryInst::NE) &&
      intConstant(rhs, rhsValue) && fitsSigned12(rhsValue)) {
    tmp = mf.createVReg(compareClass);
    appendImmInstr(mbb, Opcode::XORI, tmp, MachineOperand::reg(left),
                   rhsValue);
    appendRegInstr(mbb, inst.getOp() == BinaryInst::E ? Opcode::SEQZ
                                                      : Opcode::SNEZ,
                   dst, {tmp});
    return;
  }

  MachineReg right = valueReg(mf, mbb, rhs);

  switch (inst.getOp()) {
  case BinaryInst::E:
    tmp = mf.createVReg(compareClass);
    appendRegInstr(mbb, Opcode::XOR, tmp, {left, right});
    appendRegInstr(mbb, Opcode::SEQZ, dst, {tmp});
    break;
  case BinaryInst::NE:
    tmp = mf.createVReg(compareClass);
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
                 LoweringState &state, BinaryInst &inst) {
  Value *lhs = requiredUseValue(inst, 0, "binary instruction");
  Value *rhs = requiredUseValue(inst, 1, "binary instruction");
  const bool floatOperands = lhs && isFloatType(lhs->getIRType());
  state.beginDivRemBlock(mbb.irBlock);

  if (isComparison(inst.getOp())) {
    if (floatOperands) {
      lowerFloatCompare(mf, mbb, inst, lhs, rhs);
    } else {
      lowerIntCompare(mf, mbb, inst, lhs, rhs);
    }
    return;
  }

  if (!floatOperands && lowerIntegerImmediateOp(mf, mbb, inst, lhs, rhs)) {
    return;
  }

  if (!floatOperands &&
      lowerIntegerPowerOfTwoOp(mf, mbb, state, inst, lhs, rhs)) {
    return;
  }

  if (!floatOperands &&
      lowerIntegerPositiveConstantDivRem(mf, mbb, state, inst, lhs, rhs)) {
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
  const std::shared_ptr<IRType> srcType = srcValue->getIRType();
  const std::shared_ptr<IRType> dstType = inst.getIRType();
  if (isPointerType(srcType) && isPointerType(dstType)) {
    Address address =
        addressForValue(mf, mbb, state, srcValue, "bitcast instruction");
    state.addresses[&inst] = address;
    if (address.kind == Address::Kind::Reg && address.offset == 0) {
      mf.valueRegs[&inst] = address.reg;
    }
    return;
  }

  const bool srcScalar = srcType && srcType->isScalar();
  const bool dstScalar = dstType && dstType->isScalar();
  if (!srcScalar || !dstScalar || sizeOfType(srcType) != sizeOfType(dstType)) {
    throw BackendError("unsupported bitcast from " + scalarTypeName(srcType) +
                       " to " + scalarTypeName(dstType));
  }

  MachineReg src = valueReg(mf, mbb, srcValue);
  MachineReg dst =
      getOrCreateResultReg(mf, inst, regClassForType(dstType));

  Opcode opcode;
  if (src.rc == RegClass::FPR32 && dst.rc == RegClass::GPR32) {
    opcode = Opcode::FMV_X_W;
  } else if (src.rc == RegClass::GPR32 && dst.rc == RegClass::FPR32) {
    opcode = Opcode::FMV_W_X;
  } else if (src.rc == RegClass::FPR32 && dst.rc == RegClass::FPR32) {
    opcode = Opcode::F_COPY;
  } else if (src.rc == RegClass::GPR32 && dst.rc == RegClass::GPR32) {
    opcode = Opcode::COPY;
  } else {
    throw BackendError("unsupported bitcast register classes from " +
                       scalarTypeName(srcType) + " to " +
                       scalarTypeName(dstType));
  }
  appendRegInstr(mbb, opcode, dst, {src});
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

bool exactIntegerConstant(Value *value, int64_t &out) {
  if (auto *intValue = dynamic_cast<ConstInt *>(value)) {
    out = intValue->getVal();
    return true;
  }
  if (auto *boolValue = dynamic_cast<ConstBool *>(value)) {
    out = boolValue->getVal() ? 1 : 0;
    return true;
  }
  return false;
}

bool hasAllZeroBytes(Value *value, const std::shared_ptr<IRType> &type) {
  if (!type) {
    return false;
  }
  if (!value) {
    return true;
  }

  if (auto *init = dynamic_cast<Initializer *>(value)) {
    if (init->empty()) {
      return true;
    }
    auto arrayType = std::dynamic_pointer_cast<ARRAYType>(type);
    if (!arrayType || arrayType->getElementCount() < 0 ||
        init->size() > static_cast<std::size_t>(arrayType->getElementCount())) {
      return false;
    }
    for (Value *element : *init) {
      if (!hasAllZeroBytes(element, arrayType->getElementType())) {
        return false;
      }
    }
    return true;
  }

  if (isPointerType(type)) {
    return dynamic_cast<ConstPtr *>(value) != nullptr;
  }
  if (isBoolType(type)) {
    if (auto *boolValue = dynamic_cast<ConstBool *>(value)) {
      return !boolValue->getVal();
    }
    if (auto *intValue = dynamic_cast<ConstInt *>(value)) {
      return intValue->getVal() == 0;
    }
    return false;
  }
  if (type->getEnum() == IR_INT) {
    if (auto *intValue = dynamic_cast<ConstInt *>(value)) {
      return intValue->getVal() == 0;
    }
    if (auto *boolValue = dynamic_cast<ConstBool *>(value)) {
      return !boolValue->getVal();
    }
    return false;
  }
  if (isFloatType(type)) {
    auto *floatValue = dynamic_cast<ConstFloat *>(value);
    if (!floatValue) {
      return false;
    }
    uint32_t bits = 0;
    float raw = floatValue->getVal();
    std::memcpy(&bits, &raw, sizeof(bits));
    return bits == 0;
  }
  return false;
}

bool addScaledOffset(int64_t index, int64_t scale, int64_t &offset) {
  if (scale <= 0 || index > std::numeric_limits<int64_t>::max() / scale ||
      index < std::numeric_limits<int64_t>::min() / scale) {
    return false;
  }
  const int64_t delta = index * scale;
  if ((delta > 0 && offset > std::numeric_limits<int64_t>::max() - delta) ||
      (delta < 0 && offset < std::numeric_limits<int64_t>::min() - delta)) {
    return false;
  }
  offset += delta;
  return true;
}

bool traceConstantPointer(Value *value, Variable *&base, int64_t &offset,
                          unsigned depth = 0) {
  if (!value || depth >= 16) {
    return false;
  }
  if (auto *variable = dynamic_cast<Variable *>(value)) {
    if (variable->vartag != Variable::Constant) {
      return false;
    }
    base = variable;
    offset = 0;
    return true;
  }
  if (auto *bitCast = dynamic_cast<BitCastInst *>(value)) {
    Value *source = useValue(*bitCast, 0);
    if (!source || !isPointerType(source->getIRType()) ||
        !isPointerType(bitCast->getIRType())) {
      return false;
    }
    return traceConstantPointer(source, base, offset, depth + 1);
  }

  auto *gep = dynamic_cast<GetElementPtrInst *>(value);
  if (!gep) {
    return false;
  }
  Value *pointer = useValue(*gep, 0);
  if (!traceConstantPointer(pointer, base, offset, depth + 1)) {
    return false;
  }
  auto pointerType =
      pointer ? std::dynamic_pointer_cast<POINTERType>(pointer->getIRType())
              : nullptr;
  if (!pointerType) {
    return false;
  }

  auto currentType = pointerType->getPointerType();
  auto &uses = gep->getUserList();
  for (std::size_t i = 1; i < uses.size(); ++i) {
    int64_t scale = 0;
    currentType = advanceGepType(currentType, i == 1, scale);
    int64_t index = 0;
    if (scale <= 0 || !exactIntegerConstant(uses[i]->getValue(), index) ||
        !addScaledOffset(index, scale, offset)) {
      return false;
    }
  }
  return true;
}

bool isAllZeroConstantSource(Value *src, int64_t size) {
  Variable *base = nullptr;
  int64_t offset = 0;
  if (size < 0 || !traceConstantPointer(src, base, offset) || !base) {
    return false;
  }

  auto pointerType = std::dynamic_pointer_cast<POINTERType>(base->getIRType());
  if (!pointerType) {
    return false;
  }
  auto objectType = pointerType->getPointerType();
  const int64_t objectSize = typeSizeBytes(objectType);
  if (objectSize < 0 || offset < 0 || offset > objectSize ||
      size > objectSize - offset) {
    return false;
  }
  return hasAllZeroBytes(base->getUsee(), objectType);
}

bool isNonVolatileMemcpy(CallInst &inst) {
  int64_t isVolatile = 0;
  return exactIntegerConstant(useValue(inst, 4), isVolatile) && isVolatile == 0;
}

bool prepareZeroMemcpyDestination(MachineFunction &mf, const Address &address,
                                  int64_t size) {
  if (address.kind != Address::Kind::FrameIndex || address.frameIndex < 0 ||
      address.frameIndex >= static_cast<int>(mf.stackObjects.size())) {
    return false;
  }
  StackObject &object = mf.stackObjects[address.frameIndex];
  if (object.fixed || object.size < 0 || address.offset < 0 ||
      address.offset > object.size || size < 0 ||
      size > object.size - address.offset) {
    return false;
  }

  // Frame layout has not run yet, so strengthening this object's requested
  // alignment makes every subsequently selected natural-width store safe.
  const int64_t requiredAlign = size >= 8 ? 8 : (size >= 4 ? 4 : 1);
  const int64_t currentAlign = std::max<int64_t>(1, object.align);
  if ((currentAlign <= requiredAlign && requiredAlign % currentAlign != 0) ||
      (currentAlign > requiredAlign && currentAlign % requiredAlign != 0)) {
    return false;
  }
  object.align = std::max(currentAlign, requiredAlign);
  return true;
}

void appendZeroMemcpyStores(MachineBasicBlock &mbb, const Address &dstAddress,
                            int64_t size) {
  const MachineReg zero64 = MachineReg::preg(PhysReg::X0, RegClass::GPR64);
  const MachineReg zero32 = MachineReg::preg(PhysReg::X0, RegClass::GPR32);

  int64_t offset = 0;
  while (offset < size) {
    const int64_t remaining = size - offset;
    const int64_t absoluteOffset = dstAddress.offset + offset;
    Opcode opcode = Opcode::SB;
    MachineReg zero = zero32;
    int64_t width = 1;
    if (remaining >= 8 && absoluteOffset % 8 == 0) {
      opcode = Opcode::SD;
      zero = zero64;
      width = 8;
    } else if (remaining >= 4 && absoluteOffset % 4 == 0) {
      opcode = Opcode::SW;
      width = 4;
    }

    Address storeAddress = dstAddress;
    storeAddress.offset += offset;
    appendStore(mbb, opcode, zero, storeAddress);
    offset += width;
  }
}

void lowerLibcMemoryCall(MachineFunction &mf, MachineBasicBlock &mbb,
                         LoweringState &state,
                         std::initializer_list<Value *> arguments,
                         const std::string &intrinsicName,
                         const std::string &libcName) {
  constexpr PhysReg gprArgs[] = {PhysReg::A0, PhysReg::A1, PhysReg::A2};
  if (arguments.size() != std::size(gprArgs)) {
    throw BackendError("malformed " + intrinsicName +
                       "; expected three memory arguments");
  }

  // Materialize every source before writing any ABI register.  This preserves
  // parallel-copy semantics when register allocation gives a source one of
  // a0-a2.
  std::vector<MachineReg> argumentRegs;
  argumentRegs.reserve(arguments.size());
  std::size_t argumentIndex = 0;
  for (Value *argument : arguments) {
    if (isPointerType(argument->getIRType())) {
      argumentRegs.push_back(materializeAddress(
          mf, mbb, addressForValue(mf, mbb, state, argument, intrinsicName)));
    } else if (argumentIndex == 2) {
      if (!argument->getIRType() ||
          argument->getIRType()->getEnum() != IR_INT) {
        throw BackendError("malformed " + intrinsicName +
                           "; memory size must be an integer");
      }

      int64_t constantSize = 0;
      if (intConstant(argument, constantSize)) {
        if (constantSize < 0) {
          throw BackendError("unsupported " + intrinsicName +
                             " size; expected a non-negative byte count");
        }
        argumentRegs.push_back(
            materializeInt(mf, mbb, constantSize, RegClass::GPR64));
      } else if (argument->getIRType()->getIntegerBitWidth(32) == 32) {
        MachineReg size = valueReg(mf, mbb, argument);
        MachineReg mask = materializeInt(
            mf, mbb, std::numeric_limits<uint32_t>::max(), RegClass::GPR64);
        MachineReg widened = mf.createVReg(RegClass::GPR64);
        appendRegInstr(mbb, Opcode::AND, widened, {size, mask});
        argumentRegs.push_back(widened);
      } else {
        throw BackendError("unsupported dynamic 64-bit size in " +
                           intrinsicName);
      }
    } else {
      argumentRegs.push_back(valueReg(mf, mbb, argument));
    }
    ++argumentIndex;
  }

  for (std::size_t i = 0; i < argumentRegs.size(); ++i) {
    appendRegInstr(mbb, Opcode::COPY,
                   MachineReg::preg(gprArgs[i], RegClass::GPR64),
                   {argumentRegs[i]});
  }

  MachineInstr call;
  call.opcode = Opcode::CALL;
  call.uses.push_back(MachineOperand::global(libcName));
  call.implicitUses.assign(std::begin(gprArgs), std::end(gprArgs));
  call.implicitDefs.push_back(PhysReg::RA);
  call.sideEffects = true;
  mbb.instrs.push_back(std::move(call));
}

void requireNonVolatileMemoryIntrinsic(CallInst &inst,
                                       const std::string &name) {
  Value *volatileValue = requiredUseValue(inst, 4, name);
  int64_t isVolatile = 0;
  if (!intConstant(volatileValue, isVolatile) || isVolatile != 0) {
    throw BackendError("unsupported volatile memory intrinsic " + name);
  }
}

void lowerMemcpy(MachineFunction &mf, MachineBasicBlock &mbb,
                 LoweringState &state, CallInst &inst,
                 const std::string &name) {
  constexpr int64_t kLoopMemcpyThreshold = 256;
  requireNonVolatileMemoryIntrinsic(inst, name);
  Value *dst = requiredUseValue(inst, 1, name);
  Value *src = requiredUseValue(inst, 2, name);
  Value *sizeValue = requiredUseValue(inst, 3, name);

  int64_t size = 0;
  const bool sizeIsConstant = intConstant(sizeValue, size);
  if (sizeIsConstant && size < 0) {
    throw BackendError("unsupported " + name +
                       " size; expected a non-negative byte count");
  }
  if (!sizeIsConstant) {
    lowerLibcMemoryCall(mf, mbb, state, {dst, src, sizeValue}, name, "memcpy");
    return;
  }

  Address dstAddress = addressForValue(mf, mbb, state, dst, name);
  if (size <= kLoopMemcpyThreshold && isNonVolatileMemcpy(inst) &&
      isAllZeroConstantSource(src, size) &&
      prepareZeroMemcpyDestination(mf, dstAddress, size)) {
    appendZeroMemcpyStores(mbb, dstAddress, size);
    return;
  }

  Address srcAddress = addressForValue(mf, mbb, state, src, name);
  if (size > kLoopMemcpyThreshold) {
    MachineReg dstReg = materializeAddress(mf, mbb, dstAddress);
    MachineReg srcReg = materializeAddress(mf, mbb, srcAddress);
    MachineInstr loop;
    loop.opcode = Opcode::MEMCPY_INLINE;
    loop.uses.push_back(MachineOperand::reg(dstReg));
    loop.uses.push_back(MachineOperand::reg(srcReg));
    loop.uses.push_back(MachineOperand::imm(size));
    loop.implicitDefs = {PhysReg::T3, PhysReg::T4, PhysReg::T5, PhysReg::T6};
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

void lowerMemset(MachineFunction &mf, MachineBasicBlock &mbb,
                 LoweringState &state, CallInst &inst,
                 const std::string &name) {
  constexpr int64_t kLoopMemsetThreshold = 256;
  requireNonVolatileMemoryIntrinsic(inst, name);
  Value *dst = requiredUseValue(inst, 1, name);
  Value *byteValue = requiredUseValue(inst, 2, name);
  Value *sizeValue = requiredUseValue(inst, 3, name);

  int64_t byte = 0;
  int64_t size = 0;
  const bool byteIsConst = intConstant(byteValue, byte);
  const bool sizeIsConst = intConstant(sizeValue, size);

  if (sizeIsConst && size < 0) {
    throw BackendError("unsupported " + name +
                       " size; expected a non-negative byte count");
  }
  if (!byteIsConst || !sizeIsConst || byte < 0 || byte > 255) {
    lowerLibcMemoryCall(mf, mbb, state, {dst, byteValue, sizeValue}, name,
                        "memset");
    return;
  }

  Address dstAddress = addressForValue(mf, mbb, state, dst, name);
  if (size > kLoopMemsetThreshold) {
    MachineReg dstReg = materializeAddress(mf, mbb, dstAddress);
    MachineInstr loop;
    loop.opcode = Opcode::MEMSET_INLINE;
    loop.uses.push_back(MachineOperand::reg(dstReg));
    loop.uses.push_back(MachineOperand::imm(byte));
    loop.uses.push_back(MachineOperand::imm(size));
    loop.implicitDefs = {PhysReg::T3, PhysReg::T5, PhysReg::T6};
    loop.sideEffects = true;
    mbb.instrs.push_back(std::move(loop));
    return;
  }

  const uint32_t repeated = static_cast<uint32_t>(byte) * 0x01010101u;
  MachineReg word = materializeInt(mf, mbb, static_cast<int64_t>(repeated),
                                   RegClass::GPR32);
  int64_t offset = 0;
  for (; offset + 4 <= size; offset += 4) {
    Address address = dstAddress;
    address.offset += offset;
    appendStore(mbb, Opcode::SW, word, address);
  }
  for (; offset < size; ++offset) {
    Address address = dstAddress;
    address.offset += offset;
    appendStore(mbb, Opcode::SB, word, address);
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
  if (name == "llvm.memset.p0.i32" || name == "llvm.memset.p0i8.i32" ||
      name == "llvm.memset.p0i8.i64" || name == "llvm.memset.p0.i64") {
    lowerMemset(mf, mbb, state, inst, name);
    return;
  }
  if (name == "llvm.memmove.p0.p0.i32" ||
      name == "llvm.memmove.p0i8.p0i8.i32") {
    requireNonVolatileMemoryIntrinsic(inst, name);
    Value *dst = requiredUseValue(inst, 1, name);
    Value *src = requiredUseValue(inst, 2, name);
    Value *size = requiredUseValue(inst, 3, name);
    lowerLibcMemoryCall(mf, mbb, state, {dst, src, size}, name, "memmove");
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
    lowerBinary(mf, mbb, state, *binary);
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

} // namespace aarch64
