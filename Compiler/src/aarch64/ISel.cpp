// ISel.cpp - AArch64 指令选择与 IR 到 MIR 的 lowering。
//
// 本文件把中端的线性 SSA IR 逐条翻译为目标相关的机器指令(MIR)。核心职责:
//   - 为每个 IR Value 分配虚拟寄存器,并选择正确的寄存器类(GPR32/GPR64/FPR32);
//   - 把 IR 二元/比较/类型转换/访存/GEP/调用/分支/PHI/SELECT 等指令 lower 为
//     AArch64 指令,尽量在选择期完成立即数折叠、常量除法强度削减等本地优化;
//   - 处理函数参数按调用约定的入参落位,以及 memcpy/memset 等内建的展开。
//
// 命名与位宽约定:本后端的 MIR 沿用一套与 RISC-V 同名的操作码集合,SysY 的整数
// 类型是 i32,算术使用带 W 后缀的 word 指令(ADDW/SUBW/MULW/DIVW 等),在
// 32 位宽度上运算以保持 i32 语义;指针是 64 位,使用不带 W 的整型指令。浮点为
// f32,使用 *_S 系列单精度指令。带 I 后缀的是立即数形式(ADDI/SLLIW 等)。
//
// 本文件只做选择与本地折叠;跨块优化、寄存器分配、栈帧展开由后续 pass 完成。
#include "aarch64/ISel.hpp"

#include "aarch64/Frame.hpp"
#include "aarch64/Target.hpp"
#include "aarch64/opt/MachinePassBuilder.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <unordered_map>
#include <utility>

namespace aarch64 {
namespace {

const char *instTypeName(Instruction::InstType type);

// 判断是否为布尔类型(i1),布尔在本后端占 1 字节、用 LBU/SB 访存。
bool isBoolType(const std::shared_ptr<IRType> &type) {
  auto intType = std::dynamic_pointer_cast<INTType>(type);
  return intType && intType->getIsBool();
}

// 返回类型在内存中的字节大小。标量按固定宽度(bool 1、int 4、float 4、指针 8),
// 数组递归为元素数乘元素大小；其余类型回退到类型自身报告的 getSize。
int64_t sizeOfType(const std::shared_ptr<IRType> &type) {
  if (!type) {
    return 0;
  }

  switch (type->getTypeSystem()) {
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

// 返回类型的自然对齐字节数。数组的对齐取元素对齐,标量对齐等于其宽度。
int64_t alignOfType(const std::shared_ptr<IRType> &type) {
  if (!type) {
    return 1;
  }

  switch (type->getTypeSystem()) {
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

// 空指针或 IR_VOID 都视为 void(用于判断调用/返回是否产生结果值)。
bool isVoidType(const std::shared_ptr<IRType> &type) {
  return !type || type->getTypeSystem() == IR_VOID;
}

// 是否为浮点类型。
bool isFloatType(const std::shared_ptr<IRType> &type) {
  return type && type->getTypeSystem() == IR_FLOAT;
}

// 是否为指针类型。
bool isPointerType(const std::shared_ptr<IRType> &type) {
  return type && type->getTypeSystem() == IR_POINTER;
}

// 生成类型的简短可读名,仅用于错误信息(如 f32/i32/pointer)。
std::string scalarTypeName(const std::shared_ptr<IRType> &type) {
  if (!type) {
    return "<missing>";
  }
  if (type->getTypeSystem() == IR_FLOAT) {
    return "f32";
  }
  if (type->getTypeSystem() == IR_INT) {
    return "i" + std::to_string(type->getSize() * 8);
  }
  if (type->getTypeSystem() == IR_POINTER) {
    return "pointer";
  }
  return "non-scalar";
}

// 为 IR 类型选择机器寄存器类:浮点用 FPR32,指针用 GPR64,整数用 GPR32。
// 这是决定后续所有指令位宽(W/非 W)与拷贝操作码的关键。
RegClass regClassForType(const std::shared_ptr<IRType> &type) {
  if (!type) {
    throw BackendError("missing IR type for register class selection");
  }
  switch (type->getTypeSystem()) {
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

// 立即数是否落在有符号 12 位范围 [-2048, 2047]。
// 决定能否使用 ADDI/ANDI 等立即数形式而无需先 LI 到寄存器。
bool fitsSigned12(int64_t value) { return value >= -2048 && value <= 2047; }

// 表示一个可用于访存/取地址的地址:要么是寄存器基址加偏移(Reg),要么是
// 尚未展开的栈帧对象加偏移(FrameIndex,栈布局 pass 之后才变成实际的 sp 偏移)。
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

// 常量除法/取模复用的键:同一基本块内、同一被除数 lhs、同一除数 divisor 的
// 除法商可被后续同参取模复用,避免重复展开 magic 乘法序列。
struct DivRemKey {
  Value *lhs = nullptr;
  int64_t divisor = 0;
  BasicBlock *block = nullptr;

  bool operator==(const DivRemKey &other) const {
    return lhs == other.lhs && divisor == other.divisor &&
           block == other.block;
  }
};

// DivRemKey 的哈希:混合 lhs/divisor/block 三个字段(boost 风格的 hash_combine)。
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

// lowering 过程中跨指令共享的状态:各 Value 的地址、各 IR 块的标签、以及
// 当前块内的除法商复用表。除法复用只在块内有效,因此进入新块要清表。
struct LoweringState {
  // 后端选项快照:lowerCall 据此查询 A/B 开关(BACKEND_DISABLE_PASSES=tail-call
  // 时关闭尾调用降低)。
  const BackendOptions *options = nullptr;
  std::unordered_map<Value *, Address> addresses;
  std::unordered_map<BasicBlock *, std::string> blockLabels;
  std::unordered_map<DivRemKey, MachineReg, DivRemKeyHash> divRemQuotients;
  BasicBlock *divRemBlock = nullptr;

  // 切换到新的基本块时清空除法商复用表:复用的商寄存器只在其定义块内活跃,
  // 跨块复用会破坏 SSA/活跃性假设,故按块隔离。
  void beginDivRemBlock(BasicBlock *block) {
    if (divRemBlock != block) {
      divRemQuotients.clear();
      divRemBlock = block;
    }
  }
};

// 把标识符里非字母数字下划线的字符替换为下划线,得到可用作汇编标签的片段。
std::string sanitizeLabelPart(const std::string &name) {
  std::string result;
  result.reserve(name.size());
  for (unsigned char ch : name) {
    result.push_back(std::isalnum(ch) || ch == '_' ? static_cast<char>(ch)
                                                   : '_');
  }
  return result.empty() ? "fn" : result;
}

// 生成基本块的汇编标签:入口块(index 0)直接用函数名作为符号,其余块用
// 本地标签 .L<函数名>.bb<编号>,保证不同函数间不冲突。
std::string blockLabel(Function &function, int index) {
  if (index == 0) {
    return function.getIdent();
  }
  return ".L" + sanitizeLabelPart(function.getIdent()) + ".bb" +
         std::to_string(index);
}

// 取用户指令的第 index 个操作数,越界返回 nullptr。
Value *useValue(User &user, std::size_t index) {
  auto &uses = user.getUserList();
  if (index >= uses.size()) {
    return nullptr;
  }
  return uses[index]->getValue();
}

// 同 useValue,但操作数缺失时抛出后端错误(用于必须存在的操作数)。
Value *requiredUseValue(User &user, std::size_t index,
                        const std::string &context) {
  Value *value = useValue(user, index);
  if (!value) {
    throw BackendError("missing operand while lowering " + context);
  }
  return value;
}

// 尝试把一个 Value 求值为编译期整数常量,写入 out。除直接的整数/布尔/空指针/
// undef 常量外,还会有限深度(depth<4)地折叠形如 add i32 0, 65535 这类被中端
// 物化为普通整数指令的常量表达式。这样指令选择仍能用上立即数折叠与常量除法。
// 注意:该函数只“观察”表达式,不改动 IR,原指令仍会被正常 lower。
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
      (value->getIRType()->getTypeSystem() == IR_INT ||
       value->getIRType()->getTypeSystem() == IR_POINTER)) {
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
      value->getIRType()->getTypeSystem() != IR_INT) {
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
    // 有符号除法:除数为 0 或 INT_MIN/-1(会溢出)时拒绝折叠,交给运行期。
    if (rhs == 0 ||
        (lhs == std::numeric_limits<int32_t>::min() && rhs == -1)) {
      return false;
    }
    out = lhs / rhs;
    return true;
  case BinaryInst::UDIV:
    // 无符号除法:除数为 0 拒绝折叠。
    if (rhsBits == 0) {
      return false;
    }
    out = asI32(lhsBits / rhsBits);
    return true;
  case BinaryInst::MOD:
    // 有符号取模:同样排除除零与 INT_MIN/-1 的情况。
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
    // 移位量必须在 [0, 32) 内,否则是未定义行为,拒绝折叠。
    if (rhs < 0 || rhs >= 32) {
      return false;
    }
    out = asI32(lhsBits << static_cast<unsigned>(rhs));
    return true;
  case BinaryInst::LSHR:
    // 逻辑右移:对无符号位模式移位。
    if (rhs < 0 || rhs >= 32) {
      return false;
    }
    out = asI32(lhsBits >> static_cast<unsigned>(rhs));
    return true;
  case BinaryInst::ASHR:
    // 算术右移:对有符号值移位以保留符号位。
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

// intConstantImpl 的对外入口,从 depth 0 开始求值。
bool intConstant(Value *value, int64_t &out) {
  return intConstantImpl(value, out, 0);
}

// 判断是否为正的 2 的幂(用于乘/除/取模的移位强度削减)。
bool positivePowerOfTwo(int64_t value) {
  return value > 0 && (value & (value - 1)) == 0;
}

// 计算 2 的幂的以 2 为底的对数,即移位量。
int log2PowerOfTwo(int64_t value) {
  int shift = 0;
  while (value > 1) {
    value >>= 1;
    ++shift;
  }
  return shift;
}

// 有符号常量除法的“魔数”参数:被除数乘以 multiplier 取高位,再右移 shift 位;
// addDividend 为真时需在移位前把被除数加回(处理 multiplier 为负的溢出补偿)。
struct SignedDivMagic {
  int64_t multiplier = 0;
  int shift = 0;
  bool addDividend = false;
};

// 按 Granlund-Montgomery 算法为正除数计算 32 位有符号除法魔数常量。
// 迭代寻找满足精度的最小移位 p,使得 floor(2^p / d) 的舍入乘子能用高位乘法
// 精确复现 x/d(向零取整)。magicBits 取 32 位并按需转为负乘子(此时 addDividend
// 为真,需要把被除数加回以补偿有符号高位乘法的偏差)。shift 返回 p-32。
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

// 追加一条“寄存器-寄存器”形式的机器指令:def 有效时作为定义,uses 为源寄存器。
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

// 追加一条寄存器拷贝:目标为浮点寄存器时用 F_COPY,否则用 COPY。
void appendCopyInstr(MachineBasicBlock &mbb, MachineReg dst, MachineReg src) {
  appendRegInstr(mbb, dst.rc == RegClass::FPR32 ? Opcode::F_COPY
                                                : Opcode::COPY,
                 dst, {src});
}

// 把整数常量物化到寄存器。0 直接复用零寄存器 x0(免一条 LI),否则发一条 LI。
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

// 把 float 常量物化到浮点寄存器:先把其位模式作为整数 LI 到 GPR,再用 FMV_W_X
// 按位搬入 FPR32(避免依赖浮点常量池)。
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

// 构造有符号 2 的幂除法所需的“偏置后被除数”:有符号向零取整需要先把负数
// 加上 (2^shift - 1) 的偏置,使算术右移得到向零(而非向下)取整的商。
//   divisor==2 时偏置就是符号位本身(SRLIW 31),直接取最高位;
//   否则先算术右移 31 得到全 0 或全 1 的符号掩码,再逻辑右移 32-shift 得到
//   低 shift 位为符号的偏置量,加到被除数上。
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

// 有符号 除以 2 的幂:除以 1 只需拷贝;否则对偏置后的被除数算术右移 log2(divisor)。
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

// 已知商时求有符号 2 的幂取模:rem = lhs - (quotient << log2(divisor))。
// 除以 1 时余数恒为 0。
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

// 无预备商时求有符号 2 的幂取模。若 -divisor 能放进 12 位立即数,可用更短的
// 序列:对偏置后被除数 ANDI -divisor 得到向下对齐的倍数,再从 lhs 减去;这里
// 利用 -divisor 恰是清除低 shift 位的掩码。否则退化为先算商再算余数。
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

// 判断同一被除数 lhs 在同一块内是否还存在一个除数相同的有符号除法(排除当前
// 取模指令 remainder)。用于lower 取模时判定是否值得预先算出并缓存商供复用。
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

// 有符号 除以正常量(非 2 的幂)的 magic 序列:
//   product = lhs * magic(32×32→64 有符号长乘 SMULL),high = product >> 32(取高 32 位),
//   若 magic 为负则 high += lhs(补偿),再算术右移 shift 位,最后减去被除数
//   的符号位(SRAIW 31,负数得 -1)得到向零取整的商。
void lowerSignedDivByPositiveConstant(MachineFunction &mf,
                                      MachineBasicBlock &mbb, MachineReg dst,
                                      MachineReg lhs, int64_t divisor) {
  const SignedDivMagic magic = signedDivMagicForPositiveDivisor(divisor);
  MachineReg magicReg = materializeInt(mf, mbb, magic.multiplier,
                                       RegClass::GPR64);

  MachineReg product = mf.createVReg(RegClass::GPR64);
  appendRegInstr(mbb, Opcode::SMULL, product, {lhs, magicReg});

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

// 已知商时求除以正常量的取模:rem = lhs - quotient * divisor。
void lowerSignedRemByPositiveConstant(MachineFunction &mf,
                                      MachineBasicBlock &mbb, MachineReg dst,
                                      MachineReg lhs, int64_t divisor,
                                      MachineReg quotient) {
  MachineReg divisorReg = materializeInt(mf, mbb, divisor, RegClass::GPR32);
  MachineReg scaled = mf.createVReg(RegClass::GPR32);
  appendRegInstr(mbb, Opcode::MULW, scaled, {quotient, divisorReg});
  appendRegInstr(mbb, Opcode::SUBW, dst, {lhs, scaled});
}

// 无预备商时求除以正常量的取模:先算商再据此算余数。
void lowerSignedRemByPositiveConstant(MachineFunction &mf,
                                      MachineBasicBlock &mbb, MachineReg dst,
                                      MachineReg lhs, int64_t divisor) {
  MachineReg quotient = mf.createVReg(RegClass::GPR32);
  lowerSignedDivByPositiveConstant(mf, mbb, quotient, lhs, divisor);
  lowerSignedRemByPositiveConstant(mf, mbb, dst, lhs, divisor, quotient);
}

// 在块内除法商复用表里查 (lhs, divisor, block) 对应的商寄存器。
bool findDivRemQuotient(LoweringState &state, Value *lhs, int64_t divisor,
                        BasicBlock *block, MachineReg &quotient) {
  auto found = state.divRemQuotients.find(DivRemKey{lhs, divisor, block});
  if (found == state.divRemQuotients.end()) {
    return false;
  }
  quotient = found->second;
  return true;
}

// 把新算出的商登记进复用表,供同块内相同 (lhs, divisor) 的除法/取模复用。
void rememberDivRemQuotient(LoweringState &state, Value *lhs,
                            int64_t divisor, BasicBlock *block,
                            MachineReg quotient) {
  state.divRemQuotients[DivRemKey{lhs, divisor, block}] = quotient;
}

// 当右操作数是正的 2 的幂常量时,把乘/除/取模强度削减为移位或掩码操作。
// 返回 true 表示已处理该指令。乘法用左移,无符号除用逻辑右移,无符号取模用
// AND (divisor-1) 掩码;有符号除/取模走前述带偏置的序列,并在可行时复用商。
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
        // 商已在本块算过,直接据其求余数。
        lowerSignedRemByPowerOfTwo(mf, mbb, dst, left, rhsValue, quotient);
      } else if (hasMatchingSignedDivUse(lhs, rhsValue, mbb.irBlock, &inst)) {
        // 本块内还有同参除法会用到商:预先算出商并缓存,余数复用之,省一遍。
        quotient = mf.createVReg(RegClass::GPR32);
        lowerSignedDivByPowerOfTwo(mf, mbb, quotient, left, rhsValue);
        rememberDivRemQuotient(state, lhs, rhsValue, mbb.irBlock, quotient);
        lowerSignedRemByPowerOfTwo(mf, mbb, dst, left, rhsValue, quotient);
      } else {
        // 无复用价值:走不显式产生商的短序列(ANDI 掩码路径)。
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

// 当右操作数是大于 1 的非 2 的幂正常量时,用 magic 乘法把有符号除/取模 lower 为
// 无除法指令的序列;商在块内缓存以便同参的除法与取模共享。返回 true 表示已处理。
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

// 尝试把一个操作数为立即数的整数二元运算 lower 为立即数形式指令(ADDIW/ANDI/ORI/
// XORI/SLLIW/SRAIW/SRLIW)。仅当立即数落在 12 位范围、移位量落在 [0,32) 时成立。
// 加/与/或/异或满足交换律,故也处理左操作数为常量的情形。返回 true 表示已处理。
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
  // 左操作数为常量:仅对满足交换律的加/与/或/异或成立(减法与移位不可交换)。
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

// 结果类型是否为标量(int 或 float)。
bool isScalarResultType(const std::shared_ptr<IRType> &type) {
  return type && (type->getTypeSystem() == IR_INT || type->getTypeSystem() == IR_FLOAT);
}

// 取或建指令结果的虚拟寄存器:同一指令只分配一次,后续查表复用。
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

// 取得一个 Value 当前所在的机器寄存器,必要时按需物化:
//   - 已映射的直接返回;undef 物化为 0;整数/浮点常量就地物化;
//   - 指令结果返回其(可能新建的)结果寄存器;其余情形报错。
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
    detail += " type=" + std::to_string(value->getIRType()->getTypeSystem());
  }
  if (auto *instruction = dynamic_cast<Instruction *>(value)) {
    detail += " inst=" + std::string(instTypeName(instruction->instType));
  }
  throw BackendError("unsupported operand " + detail +
                     " while lowering function " + mf.name);
}

// 取指针类型 value 的被指向类型,非指针则报错。
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

// 把 Address 的基址转成机器操作数:FrameIndex 型给出帧对象引用,否则给出寄存器。
MachineOperand addressBaseOperand(const Address &address) {
  if (address.kind == Address::Kind::FrameIndex) {
    return MachineOperand::frameIndex(address.frameIndex);
  }
  return MachineOperand::reg(address.reg);
}

// 追加一条“定义 + 基址 + 立即数”形式的指令(如 ADDI/各类移位立即数指令、
// 也复用于以帧对象为基址的取址)。
void appendImmInstr(MachineBasicBlock &mbb, Opcode opcode, MachineReg def,
                    MachineOperand base, int64_t imm) {
  MachineInstr instr;
  instr.opcode = opcode;
  instr.defs.push_back(MachineOperand::reg(def));
  instr.uses.push_back(std::move(base));
  instr.uses.push_back(MachineOperand::imm(imm));
  mbb.instrs.push_back(std::move(instr));
}

// 用 LA 伪指令把全局符号地址加载到 GPR64。
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

// 把常量偏移加到寄存器基址上:偏移 0 直接返回基址;偏移可放进 12 位用 ADDI;
// 否则先把偏移物化到寄存器再 ADD。
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

// 把一个 Address 物化成保存最终地址的寄存器:寄存器基址走 addImmediateToReg;
// 帧对象基址用 ADDI(帧对象操作数,offset),待栈布局 pass 展开为真实 sp 偏移。
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

// 求一个指针 Value 对应的 Address(基址加偏移),优先复用已缓存的地址或寄存器:
//   - state.addresses / valueRegs 命中直接返回;
//   - 全局变量用 LA 取符号地址;空指针物化为 0;
//   - 其余指针类型取其寄存器作为基址并缓存。用于 load/store/GEP/调用取址。
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

// 按结果类型选择 load 操作码:float 用 FLW,指针用 LD,bool 用 LBU(零扩展),
// 其余整数用 LW。
Opcode loadOpcodeForType(const std::shared_ptr<IRType> &type) {
  if (isFloatType(type)) {
    return Opcode::FLW;
  }
  if (isPointerType(type)) {
    return Opcode::LD;
  }
  if (type && type->getTypeSystem() == IR_INT) {
    if (isBoolType(type)) {
      return Opcode::LBU;
    }
    return Opcode::LW;
  }
  throw BackendError("unsupported load result type");
}

// 按被存值类型选择 store 操作码:float 用 FSW,指针用 SD,bool 用 SB,其余用 SW。
Opcode storeOpcodeForType(const std::shared_ptr<IRType> &type) {
  if (isFloatType(type)) {
    return Opcode::FSW;
  }
  if (isPointerType(type)) {
    return Opcode::SD;
  }
  if (type && type->getTypeSystem() == IR_INT) {
    if (isBoolType(type)) {
      return Opcode::SB;
    }
    return Opcode::SW;
  }
  throw BackendError("unsupported store value type");
}

// 追加一条 load:操作数为(基址, 立即偏移),定义为 dst。
void appendLoad(MachineBasicBlock &mbb, Opcode opcode, MachineReg dst,
                const Address &address) {
  MachineInstr instr;
  instr.opcode = opcode;
  instr.defs.push_back(MachineOperand::reg(dst));
  instr.uses.push_back(addressBaseOperand(address));
  instr.uses.push_back(MachineOperand::imm(address.offset));
  mbb.instrs.push_back(std::move(instr));
}

// 追加一条 store:操作数为(源值, 基址, 立即偏移),并标记有副作用(不可被 CSE/DCE)。
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

// GEP 逐维推进类型并返回该维的元素步长(字节)。第一个索引作用在指针指向的
// 对象整体(步长为该类型大小),后续索引进入数组/指针的元素层。
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

// 把一个动态索引乘以步长后累加到当前偏移寄存器上。步长为 1 免乘;为 2 的幂用
// 左移;否则物化步长再 SMULL(32×32→64 有符号长乘,索引先 SXTW 为 64 位)。
// currentOffset 无效表示这是第一个动态项,直接返回。
MachineReg addScaledIndex(MachineFunction &mf, MachineBasicBlock &mbb,
                          MachineReg currentOffset, Value *index,
                          int64_t scale) {
  MachineReg indexReg = valueReg(mf, mbb, index);
  // ARM64 地址计算按 64 位进行:i32 索引先符号扩展到 GPR64,
  // 避免 lsl/mul/add 出现 w/x 混宽,也保证负索引语义正确。
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
      appendRegInstr(mbb, Opcode::SMULL, scaled, {index64, scaleReg});
    }
  }

  if (!currentOffset.isValid()) {
    return scaled;
  }

  MachineReg sum = mf.createVReg(RegClass::GPR64);
  appendRegInstr(mbb, Opcode::ADD, sum, {currentOffset, scaled});
  return sum;
}

// 取被调用者的符号名(内建函数或普通函数),间接调用返回空串。
std::string calleeName(Value *callee) {
  if (auto *builtin = dynamic_cast<BuildInFunction *>(callee)) {
    return builtin->getIdent();
  }
  if (auto *function = dynamic_cast<Function *>(callee)) {
    return function->getIdent();
  }
  return {};
}

// 查一个 IR 块对应的汇编标签(lowering 前已为所有块预登记),缺失即报错。
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

// 按调用约定把函数形参接入到虚拟寄存器:整数/指针依次用 a0-a7,浮点用
// fa0-fa7,寄存器用尽后从栈上按 8 字节槽依次加载(标记 incoming_arg)。这些拷贝/
// 加载发在入口块,建立形参 Value 到寄存器的映射供后续指令引用。
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

// 把 IR 指令类型枚举转成可读名字,仅用于诊断信息。
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

// 判断二元操作是否为比较类(lowering 路径与算术分开走)。
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

// lower 整数比较为布尔结果(0/1)。等于/不等归约为 XOR 后测零(SEQZ/SNEZ),小于/
// 大于用 SLT 交换操作数,LE/GE 用 SLT 取反。等于/不等且右操作数是 12 位常量时
// 用 XORI 省一次物化。见函数体内关于比较寄存器类的说明。
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

// lower 浮点比较。相等用 FEQ_S;不等取 FEQ_S 再 SEQZ;小于/大于用 FLT_S 交换操作数;
// LE/GE 用 FLE_S。结果落在 GPR32 布尔寄存器。
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

// 二元指令 lowering 总入口。先判浮点/整数;比较走比较 lowering;整数则依次尝试立即数折叠、
// 2 的幂强度削减、非幂正常量的 magic 除法,均不命中再落到通用寄存器-寄存器指令。
// 浮点用 *_S 指令,整数算术用带 W 的字指令,逻辑/移位按操作码选择。
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

// lower return。有返回值时按类别拷入 a0 / fa0,并在 RET 指令上记录隐式使用该
// 返回寄存器(供活跃性分析知道返回值在此被读取)。
void lowerRet(MachineFunction &mf, MachineBasicBlock &mbb, RetInst &inst) {
  // 尾调用已把本块出口换为 TAILCALL(终结指令,直接 b 到被调函数),紧邻的
  // ret 不再生成任何代码:返回值 COPY 与 epilogue 均被跳过——被调函数将把
  // 结果直接带给本函数的调用者。
  if (!mbb.instrs.empty() && mbb.instrs.back().opcode == Opcode::TAILCALL) {
    return;
  }
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

// lower 类型转换指令:
//   FPTOSI/SITOFP 用浮点-整数转换指令;
//   TRUNC/ZEXT 到 i1 用 AND 1 只保留最低位(布尔归一化);
//   SEXT 从 i1 到整数:先 AND 1,再用 0 减去(0 或 -1)得到全 0/全 1 的符号扩展。
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

// lower alloca:在机器函数上按对象大小/对齐创建一个栈对象,并把该指令的地址
// 记为对应帧对象(FrameIndex 基址,偏移 0),后续取址/访存直接引用。
void lowerAlloca(MachineFunction &mf, LoweringState &state,
                 AllocaInst &inst) {
  auto objectType = pointeeType(&inst, "alloca instruction");
  const int frameIndex = mf.createStackObject(typeSizeBytes(objectType),
                                              typeAlignBytes(objectType),
                                              inst.getIdent());
  mf.valueStackObjects[&inst] = frameIndex;
  state.addresses[&inst] = Address::frameIndexBase(frameIndex);
}

// lower load:按结果类型选 load 操作码,从指针地址读入结果寄存器。
void lowerLoad(MachineFunction &mf, MachineBasicBlock &mbb,
               LoweringState &state, LoadInst &inst) {
  Value *ptr = requiredUseValue(inst, 0, "load instruction");
  MachineReg dst =
      getOrCreateResultReg(mf, inst, regClassForType(inst.getIRType()));
  appendLoad(mbb, loadOpcodeForType(inst.getIRType()), dst,
             addressForValue(mf, mbb, state, ptr, "load instruction"));
}

// lower store:按被存值类型选 store 操作码,把值写到指针地址。
void lowerStore(MachineFunction &mf, MachineBasicBlock &mbb,
                LoweringState &state, StoreInst &inst) {
  Value *value = requiredUseValue(inst, 0, "store instruction");
  Value *ptr = requiredUseValue(inst, 1, "store instruction");
  appendStore(mbb, storeOpcodeForType(value->getIRType()),
              valueReg(mf, mbb, value),
              addressForValue(mf, mbb, state, ptr, "store instruction"));
}

// lower bitcast。指针到指针只是地址别名,直接复用源地址(偏移 0 时连寄存器一起
// 别名），不发指令。标量到标量要求同宽：浮点与整数互转用 FMV_X_W / FMV_W_X 按位搬，
// 同类则退化为拷贝;其余组合报错。
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

// lower getelementptr。从基址地址出发,逐个索引累加偏移:常量索引折进常量偏移
// constOffset,动态索引乘步长后累加到 dynamicOffset 寄存器。
//   - 全为常量索引:直接把 constOffset 折进基址并物化最终地址;
//   - 有动态索引:先物化(基址+常量偏移),再 ADD 动态偏移。
// existingDst 处理该 GEP 结果寄存器已被 PHI 等预分配的情形,保持定义寄存器不变。
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

// 仅识别严格的整数/布尔常量(不做表达式折叠),写入 out。
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

// 判断一个初始值(可能是数组 Initializer 递归结构)是否整块为全零字节。用于把
// 从全零常量源拷贝识别成对目标的清零。空初始化视为全零;超出元素数则不成立。
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
  if (type->getTypeSystem() == IR_INT) {
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

// 计算 offset += index*scale,带溢出检查;任一乘/加会溢出 int64 即返回 false。
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

// 顺着 bitcast/常量索引的 GEP 链回溯一个指针,判断它是否恒等于“某个全局常量
// base 加编译期常量 offset”。全为常量索引才能追踪;有限深度(depth<16)防环。
// 用于把从常量对象的 memcpy 识别为可静态判定内容(如全零)的拷贝。
bool traceConstantPointer(Value *value, Variable *&base, int64_t &offset,
                          unsigned depth = 0) {
  if (!value || depth >= 16) {
    return false;
  }
  if (auto *variable = dynamic_cast<Variable *>(value)) {
    if (variable->vartag != Variable::GlobalConstant) {
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

// 判断 [src, src+size) 这段是否落在某个全局常量对象内且内容全为零字节。要求
// 追踪到常量 base、区间在对象范围内,且对象初始值全零。用于把 memcpy 化为清零。
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

// memcpy 内建的第 4 个参数是 isVolatile 标志,这里判断其为非 volatile(可优化)。
bool isNonVolatileMemcpy(CallInst &inst) {
  int64_t isVolatile = 0;
  return exactIntegerConstant(useValue(inst, 4), isVolatile) && isVolatile == 0;
}

// 为把清零展开成自然宽度 store 做准备:目标必须是本函数的、非固定的栈对象,且
// 区间在对象内。因帧布局尚未运行,可提前提升该对象的对齐请求,使随后选出的
// 8/4 字节 store 都对齐安全。对齐不兼容或越界时返回 false 放弃优化。
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

// 把对目标区间的清零展开为一串把 x0 写入的 store:按当前绝对偏移的对齐情况
// 贪心选择 8/4/1 字节宽度(SD/SW/SB),逐段前进直到写满 size 字节。
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

// 计算两个对齐字节数的最大公约数(负数按绝对值处理,结果至少为 1)。
int64_t gcdAlignment(int64_t a, int64_t b) {
  auto magnitude = [](int64_t v) -> uint64_t {
    return v < 0 ? static_cast<uint64_t>(-(v + 1)) + 1u
                 : static_cast<uint64_t>(v);
  };
  return static_cast<int64_t>(std::gcd(magnitude(a), magnitude(b)));
}

// 静态证明一个指针值的最小对齐字节数。返回 1 表示只能证明字节对齐,展开
// memcpy/memset 时不得使用 ldr/str 等自然宽度访存。证明来源(按强度递减):
//   - alloca/全局对象:对象类型自然对齐(栈帧/数据段布局保证);
//   - GEP:基址对齐与常量偏移、动态索引步长的最大公约数(动态索引取任意整数,
//     index*scale 必为 scale 的倍数,故参与 gcd 是安全下界);
//   - bitcast(指针→指针):物理地址不变,沿用源指针对齐;
//   - 其余指针来源(形参、load/PHI/SELECT 结果等):按指针所指类型的 ABI 自然
//     对齐。本 IR 没有 inttoptr/任意地址算术,类型契约即对齐契约;
//   - 非常规来源(空指针、undef、深度超限)一律保守返回 1。
int64_t provenPointerAlignment(Value *value, unsigned depth = 0) {
  if (!value || !isPointerType(value->getIRType()) || depth >= 16) {
    return 1;
  }
  if (auto *alloca = dynamic_cast<AllocaInst *>(value)) {
    return alignOfType(pointeeType(alloca, "memcpy/memset alignment"));
  }
  if (auto *variable = dynamic_cast<Variable *>(value)) {
    auto type = variable->getIRType();
    if (isPointerType(type)) {
      return alignOfType(pointeeType(variable, "memcpy/memset alignment"));
    }
    return alignOfType(type);
  }
  if (auto *gep = dynamic_cast<GetElementPtrInst *>(value)) {
    Value *base = useValue(*gep, 0);
    if (!base || !isPointerType(base->getIRType())) {
      return 1;
    }
    int64_t alignment = provenPointerAlignment(base, depth + 1);
    int64_t constOffset = 0;
    auto currentType = pointeeType(base, "memcpy/memset alignment");
    auto &uses = gep->getUserList();
    for (std::size_t i = 1; i < uses.size(); ++i) {
      int64_t scale = 0;
      currentType = advanceGepType(currentType, i == 1, scale);
      if (scale <= 0) {
        return 1;
      }
      int64_t index = 0;
      if (exactIntegerConstant(uses[i]->getValue(), index)) {
        if (!addScaledOffset(index, scale, constOffset)) {
          return 1;
        }
      } else {
        alignment = gcdAlignment(alignment, scale);
      }
    }
    return gcdAlignment(alignment, constOffset);
  }
  if (auto *bitcast = dynamic_cast<BitCastInst *>(value)) {
    Value *source = useValue(*bitcast, 0);
    if (source && isPointerType(source->getIRType())) {
      return provenPointerAlignment(source, depth + 1);
    }
    return 1;
  }
  if (dynamic_cast<ConstPtr *>(value) || dynamic_cast<UndefValue *>(value)) {
    return 1;
  }
  return alignOfType(pointeeType(value, "memcpy/memset alignment"));
}

// 地址整体可证明对齐 = 指针来源对齐 与 地址偏移/栈对象请求对齐 的公约数。
// 同时覆盖 FrameIndex 基址与 GEP/bitcast 叠加出的常量偏移。
int64_t provenAddressAlignment(MachineFunction &mf, const Address &address,
                               Value *value) {
  int64_t alignment = gcdAlignment(provenPointerAlignment(value),
                                    address.offset);
  if (address.kind == Address::Kind::FrameIndex && address.frameIndex >= 0 &&
      address.frameIndex < static_cast<int>(mf.stackObjects.size())) {
    alignment =
        gcdAlignment(alignment, mf.stackObjects[address.frameIndex].align);
  }
  return alignment;
}

// 把内存内建 lower 为对 libc(memcpy/memset/memmove)的普通调用。先把三个实参全部
// 物化到临时寄存器,再统一拷入 a0-a2,以维持并行拷贝语义(避免某源恰被分到 a0-a2
// 时被提前覆盖)。第 3 个参数是字节数:常量直接物化,32 位动态值零扩展到 64 位。
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
          argument->getIRType()->getTypeSystem() != IR_INT) {
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

// 校验内存内建是非 volatile(第 4 个参数为 0),否则不支持并报错。
void requireNonVolatileMemoryIntrinsic(CallInst &inst,
                                       const std::string &name) {
  Value *volatileValue = requiredUseValue(inst, 4, name);
  int64_t isVolatile = 0;
  if (!intConstant(volatileValue, isVolatile) || isVolatile != 0) {
    throw BackendError("unsupported volatile memory intrinsic " + name);
  }
}

// lower memcpy。按大小分三档:
//   - 大小非常量:退化为 libc memcpy 调用;
//   - 常量且不超过阈值、源为全零常量、目标可安全清零:直接展开为清零 store;
//   - 超过阈值(256 字节):发一条内联循环 MEMCPY_INLINE 伪指令;
//   - 否则按 4 字节字循环加尾部字节复制,内联展开。
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
  // 只有目标与源都静态证明至少 4 字节对齐时,才允许 ldr/str 字循环;任一无法
  // 证明时整体退化为纯字节循环,与 RISC-V 侧保持一致的对齐防御策略。
  const int64_t copyAlign =
      std::min(provenAddressAlignment(mf, dstAddress, dst),
               provenAddressAlignment(mf, srcAddress, src));
  if (size > kLoopMemcpyThreshold) {
    MachineReg dstReg = materializeAddress(mf, mbb, dstAddress);
    MachineReg srcReg = materializeAddress(mf, mbb, srcAddress);
    MachineInstr loop;
    loop.opcode = Opcode::MEMCPY_INLINE;
    loop.uses.push_back(MachineOperand::reg(dstReg));
    loop.uses.push_back(MachineOperand::reg(srcReg));
    loop.uses.push_back(MachineOperand::imm(size));
    // 第 4 个操作数为可证明对齐字节数,AsmPrinter 据此决定是否启用字循环。
    loop.uses.push_back(MachineOperand::imm(copyAlign));
    loop.implicitDefs = {PhysReg::T3, PhysReg::T4, PhysReg::T5, PhysReg::T6};
    loop.sideEffects = true;
    mbb.instrs.push_back(std::move(loop));
    return;
  }

  int64_t offset = 0;
  if (copyAlign >= 4) {
    for (; offset + 4 <= size; offset += 4) {
      Address loadAddress = srcAddress;
      Address storeAddress = dstAddress;
      loadAddress.offset += offset;
      storeAddress.offset += offset;

      MachineReg tmp = mf.createVReg(RegClass::GPR32);
      appendLoad(mbb, Opcode::LW, tmp, loadAddress);
      appendStore(mbb, Opcode::SW, tmp, storeAddress);
    }
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

// lower memset。字节值与大小都是常量且字节在 [0,255] 时才内联,否则退化为 libc
// memset。内联时把字节复制成 0x01010101 的 32 位字重复模式:超阈值发 MEMSET_INLINE
// 循环伪指令,否则按 4 字节字循环加尾部字节写入展开。
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
  const int64_t dstAlign = provenAddressAlignment(mf, dstAddress, dst);
  const bool wordAligned = dstAlign >= 4;
  if (size > kLoopMemsetThreshold) {
    MachineReg dstReg = materializeAddress(mf, mbb, dstAddress);
    MachineInstr loop;
    loop.opcode = Opcode::MEMSET_INLINE;
    loop.uses.push_back(MachineOperand::reg(dstReg));
    loop.uses.push_back(MachineOperand::imm(byte));
    loop.uses.push_back(MachineOperand::imm(size));
    // 第 4 个操作数为可证明对齐字节数,AsmPrinter 据此决定是否启用字循环。
    loop.uses.push_back(MachineOperand::imm(dstAlign));
    loop.implicitDefs = {PhysReg::T3, PhysReg::T5, PhysReg::T6};
    loop.sideEffects = true;
    mbb.instrs.push_back(std::move(loop));
    return;
  }

  const uint32_t repeated = static_cast<uint32_t>(byte) * 0x01010101u;
  MachineReg word = materializeInt(mf, mbb, static_cast<int64_t>(repeated),
                                   RegClass::GPR32);
  int64_t offset = 0;
  if (wordAligned) {
    for (; offset + 4 <= size; offset += 4) {
      Address address = dstAddress;
      address.offset += offset;
      appendStore(mbb, Opcode::SW, word, address);
    }
  }
  for (; offset < size; ++offset) {
    Address address = dstAddress;
    address.offset += offset;
    appendStore(mbb, Opcode::SB, word, address);
  }
}

void lowerUnsupported(MachineFunction &mf, MachineBasicBlock &mbb,
                      Instruction &inst);

// lower call。先识别 memcpy/memset/memmove 内建走各自的特化路径;其余按 RV 调用
// 约定传参:整数/指针用 a0-a7,浮点用 fa0-fa7,溢出的参数按 8 字节槽压栈(栈大小
// 16 字节对齐)。先物化所有实参再落位以保并行拷贝语义;调用前后调整 sp;有返回值
// 时从 a0/fa0 拷回结果寄存器。满足尾调用门槛时改为 TAILCALL 出口。
//
// 判断调用是否可安全按尾调用降低。中端 markTailCalls(TRE)是标记唯一生成点，
// 但标记在后继 pass 中不保值(UnifyFuncExits 会把 call;ret 改写为
// call;br@unified_exit，FunctionMemoization 会清标记)，这里在消费点重新验证：
//   - IR 形状:call 紧邻 ret，且返回值形状精确匹配(void→void；非 void→ret 的
//     操作数就是该 call，无 PHI/位宽转换包装)；
//   - 实参闭包:任一实参的操作数闭包中不得出现 AllocaInst(与 TRE 同判据，
//     防止尾跳回收本帧后实参里的栈地址失效；Global2Local 等后期 pass 可能
//     在标记之后引入栈对象，故消费点必须重验)。
bool isTailCallCandidate(CallInst &inst) {
  if (!inst.isTailCall()) {
    return false;
  }
  // 形状:call 必须是块内最后一条有效指令,紧随其后是 ret。
  Instruction *next = inst.getNext();
  auto *ret = dynamic_cast<RetInst *>(next);
  if (!ret) {
    return false;
  }
  Value *retValue = ret->getReturnValue();
  if (retValue == nullptr) {
    // void 返回:call 也必须返回 void。
    if (!isVoidType(inst.getIRType())) {
      return false;
    }
  } else if (retValue != &inst) {
    // 非 void 返回:必须精确返回该 call 的结果。
    return false;
  }
  // 实参闭包 alloca 检查:反向追溯每个实参的操作数闭包。
  std::unordered_set<Value *> visited;
  std::vector<Value *> worklist;
  auto &uses = inst.getUserList();
  for (std::size_t i = 1; i < uses.size(); ++i) {
    Value *arg = uses[i]->getValue();
    if (arg) {
      worklist.push_back(arg);
    }
  }
  while (!worklist.empty()) {
    Value *curr = worklist.back();
    worklist.pop_back();
    if (!curr || visited.count(curr)) {
      continue;
    }
    visited.insert(curr);
    if (dynamic_cast<AllocaInst *>(curr)) {
      return false;
    }
    if (auto *operandUser = dynamic_cast<User *>(curr)) {
      for (const auto &use : operandUser->getUserList()) {
        if (use && use->getValue()) {
          worklist.push_back(use->getValue());
        }
      }
    }
  }
  return true;
}

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

  // 描述一个实参的落位方案:传值类别、走寄存器还是压栈、栈偏移或目标物理寄存器。
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
  // 尾调用判定:中端标记 + 消费点形状重验 + 首版门槛(实参全部走寄存器,
  // 无栈参,规避出参区与帧回收的交互)+ A/B 开关(disable:tail-call)。
  // 有栈参时降级为普通调用,正确性不受影响。
  const bool tailCall =
      isTailCallCandidate(inst) &&
      (state.options == nullptr ||
       !machinePassDisabled(*state.options, "tail-call")) &&
      stackBytes == 0;
  // 外传栈参数改用帧底部预留的固定区(FrameLowering 按 maxOutgoingArgs 预留),
  // 不在调用点动态下调 sp。动态下调会让活跃跨调用点、相对 sp 寻址的溢出值在 sp
  // 变动后取到错误偏移,大量栈参数时导致崩溃。此处记录本函数最大外传字节数。
  // 尾调用无栈参且不再返回,不占用出参区。
  if (!tailCall && stackBytes > mf.maxOutgoingArgs) {
    mf.maxOutgoingArgs = stackBytes;
  }
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

  // 外传栈参数直接写入帧底部预留区 [sp, sp+stackBytes),不改动 sp。
  // 同时收集寄存器参数占用的物理寄存器,供下方标记为 CALL 的隐式使用。
  std::vector<PhysReg> argPhysRegs;
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
      argPhysRegs.push_back(move.phys);
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

  if (tailCall) {
    // 尾调用:实参 COPY 已就位,出口换为不写返回地址寄存器 x30 的 TAILCALL
    // (AsmPrinter 展开为:恢复 x30/callee-saved + 回收栈帧 + b 直接跳转)。
    // 返回值由被调函数直接带给本函数的调用者,不生成结果 COPY,也不走本函数
    // 的 epilogue。implicitUses 与 CALL 一致,防止分配器把实参 COPY 当死码删除。
    MachineInstr tail;
    tail.opcode = Opcode::TAILCALL;
    tail.uses.push_back(MachineOperand::global(name));
    for (PhysReg reg : argPhysRegs) {
      tail.implicitUses.push_back(reg);
    }
    tail.sideEffects = true;
    tail.terminator = true;
    mbb.instrs.push_back(std::move(tail));
    return;
  }

  MachineInstr call;
  call.opcode = Opcode::CALL;
  call.uses.push_back(MachineOperand::global(name));
  call.implicitDefs.push_back(PhysReg::RA);
  // 标记参数寄存器为隐式使用:否则寄存器分配器会认为 COPY 到 a0-a7/fa0-fa7 后
  // 这些寄存器即死,可能在存放栈参数时将其当作临时复用,覆盖已就绪的参数值。
  for (PhysReg reg : argPhysRegs) {
    call.implicitUses.push_back(reg);
  }
  call.sideEffects = true;
  mbb.instrs.push_back(std::move(call));

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

// lower 分支。无条件跳转发一条 J;条件分支把条件寄存器非零则跳到真块(BNEZ),
// 随后紧跟一条无条件 J 跳到假块。均标记为终结指令。
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

// lower PHI 为一条 MIR 层的 PHI 伪指令,操作数交替为(前驱来值, 前驱块标签),
// 由后续的 PHI 消除/寄存器分配处理成实际拷贝。
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

// lower select 为一条 SELECT 伪指令(操作数:条件、真值、假值),条件寄存器
// 非零取真值否则取假值,具体展开留给后续 pass。
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

// 遇到本后端不支持的 IR 指令时统一抛出后端错误。
void lowerUnsupported(MachineFunction &mf, MachineBasicBlock &mbb,
                      Instruction &inst) {
  (void)mbb;
  throw BackendError("unsupported IR instruction " +
                     std::string(instTypeName(inst.instType)) +
                     " while lowering function " + mf.name);
}

// 单条 IR 指令的lowering 分派:按动态类型/指令类型路由到对应的 lower* 函数,
// 类型转换类归到 lowerCast,未覆盖的落到 lowerUnsupported。
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

IRLowering::IRLowering(Module &module, const BackendOptions &options)
    : module_(module), options_(options) {}

// lower 整个模块:先处理全局对象,再逐个函数 lower 为机器函数。
MachineModule IRLowering::lowerModule() {
  MachineModule mm;
  lowerGlobals(mm);

  for (auto &function : module_.getFunctions()) {
    if (function) {
      mm.functions.push_back(lowerFunction(*function));
    }
  }

  // 给每个机器函数写入模块级全局对象快照（符号名 -> GlobalObject），
  // 供机器层 pass（如 machineLICM）做全局符号的在界/常量性查询。
  for (MachineFunction &function : mm.functions) {
    for (const GlobalObject &object : mm.globals) {
      function.globals.emplace(object.name, object);
    }
  }

  return mm;
}

// lower 全局变量为机器模块的全局对象,记录名字/是否常量/大小/对齐等布局信息。
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
    object.constant = (var->vartag == Variable::GlobalConstant);
    object.size = typeSizeBytes(objectType);
    object.align = typeAlignBytes(objectType);
    mm.globals.push_back(std::move(object));
  }
}

// lower 单个函数:建立机器基本块与标签、登记块标签映射、接入形参,再逐块逐指令
// lowering;空函数体补一个只含 RET 的入口块。最后运行栈帧布局与帧对象展开。
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
  state.options = &options_;
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
