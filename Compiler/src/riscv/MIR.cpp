// MIR.cpp —— 机器 IR(MIR)的核心数据结构实现。
// 定义机器寄存器 MachineReg(虚拟/物理)、机器操作数 MachineOperand(寄存器/
// 立即数/栈帧下标/全局符号/基本块/值引用)、机器函数的虚寄存器与栈对象工厂,
// 以及调试用的文本转储和虚寄存器有效性校验。这层是纯文本可读的调试打印,
// 与最终 RISC-V 汇编发射(AsmPrinter)不同,只用于开发期观察 MIR。
#include "riscv/MIR.hpp"

#include "riscv/Target.hpp"

#include <limits>
#include <ostream>
#include <string>
#include <unordered_map>
#include <utility>

namespace riscv {

bool MachineVectorType::isValid() const noexcept {
  if (lanes == 0) {
    return false;
  }
  switch (element) {
  case MachineVectorElementKind::I1:
    return maskGranuleBits == 32;
  case MachineVectorElementKind::I32:
  case MachineVectorElementKind::F32:
    return maskGranuleBits == 0;
  case MachineVectorElementKind::Invalid:
    return false;
  }
  return false;
}

bool MachineVectorType::isMask() const noexcept {
  return element == MachineVectorElementKind::I1;
}

unsigned MachineVectorType::elementBits() const noexcept {
  switch (element) {
  case MachineVectorElementKind::I1:
    return 1;
  case MachineVectorElementKind::I32:
  case MachineVectorElementKind::F32:
    return 32;
  case MachineVectorElementKind::Invalid:
    return 0;
  }
  return 0;
}

unsigned MachineVectorType::logicalBits() const noexcept {
  const unsigned Bits = elementBits();
  if (Bits == 0 || lanes > std::numeric_limits<unsigned>::max() / Bits) {
    return 0;
  }
  return static_cast<unsigned>(lanes) * Bits;
}

unsigned MachineVectorType::dataWidthBits() const noexcept {
  const unsigned Granule = isMask() ? maskGranuleBits : elementBits();
  if (Granule == 0 || lanes > std::numeric_limits<unsigned>::max() / Granule) {
    return 0;
  }
  return static_cast<unsigned>(lanes) * Granule;
}

std::string MachineVectorType::str() const {
  const char *ElementName = "invalid";
  switch (element) {
  case MachineVectorElementKind::I1:
    ElementName = "i1";
    break;
  case MachineVectorElementKind::I32:
    ElementName = "i32";
    break;
  case MachineVectorElementKind::F32:
    ElementName = "f32";
    break;
  case MachineVectorElementKind::Invalid:
    break;
  }
  return "<" + std::to_string(lanes) + "x" + ElementName + ">";
}

bool MachineVectorType::operator==(
    const MachineVectorType &other) const noexcept {
  return element == other.element && lanes == other.lanes &&
         maskGranuleBits == other.maskGranuleBits;
}

namespace {

// 把 MIR 操作码枚举转成可读名字,仅供调试转储使用(非最终汇编助记符)。
const char *opcodeName(Opcode opcode) {
  switch (opcode) {
  case Opcode::NOP:
    return "NOP";
  case Opcode::COPY:
    return "COPY";
  case Opcode::F_COPY:
    return "F_COPY";
  case Opcode::FMV_W_X:
    return "FMV_W_X";
  case Opcode::FMV_X_W:
    return "FMV_X_W";
  case Opcode::LI:
    return "LI";
  case Opcode::LA:
    return "LA";
  case Opcode::ADD:
    return "ADD";
  case Opcode::ADDW:
    return "ADDW";
  case Opcode::ADDI:
    return "ADDI";
  case Opcode::ADDIW:
    return "ADDIW";
  case Opcode::SUB:
    return "SUB";
  case Opcode::SUBW:
    return "SUBW";
  case Opcode::MUL:
    return "MUL";
  case Opcode::MULW:
    return "MULW";
  case Opcode::DIV:
    return "DIV";
  case Opcode::DIVW:
    return "DIVW";
  case Opcode::DIVUW:
    return "DIVUW";
  case Opcode::REM:
    return "REM";
  case Opcode::REMW:
    return "REMW";
  case Opcode::REMUW:
    return "REMUW";
  case Opcode::AND:
    return "AND";
  case Opcode::OR:
    return "OR";
  case Opcode::XOR:
    return "XOR";
  case Opcode::ANDI:
    return "ANDI";
  case Opcode::ORI:
    return "ORI";
  case Opcode::XORI:
    return "XORI";
  case Opcode::SLL:
    return "SLL";
  case Opcode::SLLW:
    return "SLLW";
  case Opcode::SRAW:
    return "SRAW";
  case Opcode::SRLW:
    return "SRLW";
  case Opcode::SLLI:
    return "SLLI";
  case Opcode::SLLIW:
    return "SLLIW";
  case Opcode::SRAI:
    return "SRAI";
  case Opcode::SRAIW:
    return "SRAIW";
  case Opcode::SRLIW:
    return "SRLIW";
  case Opcode::SLT:
    return "SLT";
  case Opcode::SLTU:
    return "SLTU";
  case Opcode::SEQZ:
    return "SEQZ";
  case Opcode::SNEZ:
    return "SNEZ";
  case Opcode::FEQ_S:
    return "FEQ_S";
  case Opcode::FLT_S:
    return "FLT_S";
  case Opcode::FLE_S:
    return "FLE_S";
  case Opcode::FADD_S:
    return "FADD_S";
  case Opcode::FSUB_S:
    return "FSUB_S";
  case Opcode::FMUL_S:
    return "FMUL_S";
  case Opcode::FDIV_S:
    return "FDIV_S";
  case Opcode::FCVT_W_S:
    return "FCVT_W_S";
  case Opcode::FCVT_S_W:
    return "FCVT_S_W";
  case Opcode::LBU:
    return "LBU";
  case Opcode::SB:
    return "SB";
  case Opcode::LW:
    return "LW";
  case Opcode::SW:
    return "SW";
  case Opcode::LD:
    return "LD";
  case Opcode::SD:
    return "SD";
  case Opcode::FLW:
    return "FLW";
  case Opcode::FSW:
    return "FSW";
  case Opcode::GEP:
    return "GEP";
  case Opcode::MEMCPY_INLINE:
    return "MEMCPY_INLINE";
  case Opcode::MEMSET_INLINE:
    return "MEMSET_INLINE";
  case Opcode::CALL:
    return "CALL";
  case Opcode::RET:
    return "RET";
  case Opcode::BEQZ:
    return "BEQZ";
  case Opcode::BNEZ:
    return "BNEZ";
  case Opcode::BEQ:
    return "BEQ";
  case Opcode::BNE:
    return "BNE";
  case Opcode::BLT:
    return "BLT";
  case Opcode::BGE:
    return "BGE";
  case Opcode::J:
    return "J";
  case Opcode::PHI:
    return "PHI";
  case Opcode::SELECT:
    return "SELECT";
  case Opcode::PARALLEL_COPY:
    return "PARALLEL_COPY";
  case Opcode::G_VUNDEF:
    return "G_VUNDEF";
  case Opcode::G_VCONST:
    return "G_VCONST";
  case Opcode::G_VCOPY:
    return "G_VCOPY";
  case Opcode::G_VLOAD:
    return "G_VLOAD";
  case Opcode::G_VSTORE:
    return "G_VSTORE";
  case Opcode::G_VADD:
    return "G_VADD";
  case Opcode::G_VSUB:
    return "G_VSUB";
  case Opcode::G_VMUL:
    return "G_VMUL";
  case Opcode::G_VDIV:
    return "G_VDIV";
  case Opcode::G_VUDIV:
    return "G_VUDIV";
  case Opcode::G_VREM:
    return "G_VREM";
  case Opcode::G_VUREM:
    return "G_VUREM";
  case Opcode::G_VAND:
    return "G_VAND";
  case Opcode::G_VOR:
    return "G_VOR";
  case Opcode::G_VXOR:
    return "G_VXOR";
  case Opcode::G_VSHL:
    return "G_VSHL";
  case Opcode::G_VLSHR:
    return "G_VLSHR";
  case Opcode::G_VASHR:
    return "G_VASHR";
  case Opcode::G_VFADD:
    return "G_VFADD";
  case Opcode::G_VFSUB:
    return "G_VFSUB";
  case Opcode::G_VFMUL:
    return "G_VFMUL";
  case Opcode::G_VFDIV:
    return "G_VFDIV";
  case Opcode::G_VFREM:
    return "G_VFREM";
  case Opcode::G_VICMP:
    return "G_VICMP";
  case Opcode::G_VFCMP:
    return "G_VFCMP";
  case Opcode::G_VSELECT:
    return "G_VSELECT";
  case Opcode::G_VEXTRACT:
    return "G_VEXTRACT";
  case Opcode::G_VINSERT:
    return "G_VINSERT";
  case Opcode::G_VSHUFFLE:
    return "G_VSHUFFLE";
  case Opcode::G_VTRUNC:
    return "G_VTRUNC";
  case Opcode::G_VZEXT:
    return "G_VZEXT";
  case Opcode::G_VSEXT:
    return "G_VSEXT";
  case Opcode::G_VFPTOSI:
    return "G_VFPTOSI";
  case Opcode::G_VSITOFP:
    return "G_VSITOFP";
  case Opcode::G_VBITCAST:
    return "G_VBITCAST";
  }
  return "<unknown>";
}

// 转储单个操作数到文本流。不同种类采用不同前缀便于辨认:
// 寄存器直接打印名字,立即数打印数值,栈帧下标写作 fi#N,
// 全局符号加 @ 前缀,基本块标签加 % 前缀,值引用写作 value@指针。
void dumpOperand(const MachineOperand &operand, const MachineFunction &mf,
                 std::ostream &out) {
  switch (operand.kind) {
  case OperandKind::Reg:
    out << operand.regValue.str();
    if (const MachineVectorType *Type =
            mf.getVectorType(operand.regValue)) {
      out << ':' << Type->str();
    }
    break;
  case OperandKind::Imm:
    out << operand.immValue;
    break;
  case OperandKind::FrameIndex:
    out << "fi#" << operand.frameIndexValue;
    break;
  case OperandKind::Global:
    out << '@' << operand.symbol;
    break;
  case OperandKind::Block:
    out << '%' << operand.blockLabel;
    break;
  case OperandKind::ValueRef:
    out << "value@" << operand.valueRef;
    break;
  }
}

// 校验寄存器类操作数:非寄存器操作数直接跳过,寄存器则要求其内容有效,
// 否则抛出 BackendError 并带上所在机器函数名。
void validateOperand(const MachineOperand &operand, const MachineFunction &mf) {
  if (operand.kind != OperandKind::Reg) {
    return;
  }

  if (!operand.regValue.isValid()) {
    throw BackendError("invalid register operand in machine function " +
                       mf.name);
  }
  const MachineVectorType *VectorType = mf.getVectorType(operand.regValue);
  if (operand.regValue.rc == RegClass::GenericVector) {
    if (operand.regValue.physical || !VectorType || !VectorType->isValid()) {
      throw BackendError("untyped generic vector register in machine function " +
                         mf.name);
    }
  } else if (!operand.regValue.physical && VectorType) {
    throw BackendError("scalar register has vector type in machine function " +
                       mf.name);
  }
}

// 校验隐式物理寄存器(指令的 implicit def/use):必须是有效的具体物理寄存器。
void validateImplicitPhysReg(PhysReg reg, const MachineFunction &mf) {
  if (reg == PhysReg::NoReg || !isValidPhysReg(reg)) {
    throw BackendError("invalid implicit physical register in machine function " +
                       mf.name);
  }
}

} // namespace

// 构造一个虚拟寄存器:physical=false,由自增 id 和寄存器类标识,尚未绑定物理寄存器。
MachineReg MachineReg::vreg(uint32_t id, RegClass rc) {
  MachineReg reg;
  reg.physical = false;
  reg.id = id;
  reg.rc = rc;
  reg.phys = PhysReg::NoReg;
  return reg;
}

// 构造一个物理寄存器:physical=true,直接绑定具体 PhysReg,id 置零。
MachineReg MachineReg::preg(PhysReg reg, RegClass rc) {
  MachineReg machineReg;
  machineReg.physical = true;
  machineReg.id = 0;
  machineReg.rc = rc;
  machineReg.phys = reg;
  return machineReg;
}

// 判断机器寄存器是否有效:寄存器类合法,且物理寄存器需为具体有效寄存器、
// 虚拟寄存器需 id 非零。
bool MachineReg::isValid() const {
  if (!isValidRegClass(rc)) {
    return false;
  }
  if (rc == RegClass::GenericVector) {
    return !physical && id != 0;
  }
  return physical ? phys != PhysReg::NoReg && isValidPhysReg(phys) : id != 0;
}

// 机器寄存器的文本形式:物理寄存器输出 ABI 名(如 a0),
// 虚拟寄存器输出 %v<id>,id 为零视为非法输出 %invalid。
std::string MachineReg::str() const {
  if (physical) {
    return regName(phys);
  }
  if (id == 0) {
    return "%invalid";
  }
  return "%v" + std::to_string(id);
}

// 以下六个静态工厂按操作数种类分别构造 MachineOperand。
// 寄存器操作数。
MachineOperand MachineOperand::reg(MachineReg reg) {
  MachineOperand operand{};
  operand.kind = OperandKind::Reg;
  operand.regValue = reg;
  return operand;
}

// 立即数操作数。
MachineOperand MachineOperand::imm(int64_t value) {
  MachineOperand operand{};
  operand.kind = OperandKind::Imm;
  operand.immValue = value;
  return operand;
}

// 栈帧下标操作数(引用某个栈对象,后续在frame lowering时替换成 sp 相对偏移)。
MachineOperand MachineOperand::frameIndex(int index) {
  MachineOperand operand{};
  operand.kind = OperandKind::FrameIndex;
  operand.frameIndexValue = index;
  return operand;
}

// 全局符号操作数(全局变量/函数名)。
MachineOperand MachineOperand::global(std::string symbol) {
  MachineOperand operand{};
  operand.kind = OperandKind::Global;
  operand.symbol = std::move(symbol);
  return operand;
}

// 基本块标签操作数(分支目标)。
MachineOperand MachineOperand::block(std::string label) {
  MachineOperand operand{};
  operand.kind = OperandKind::Block;
  operand.blockLabel = std::move(label);
  return operand;
}

// 中端值引用操作数(lowering 过程中用于回指原 IR Value)。
MachineOperand MachineOperand::value(Value *value) {
  MachineOperand operand{};
  operand.kind = OperandKind::ValueRef;
  operand.valueRef = value;
  return operand;
}

// 为机器函数分配一个新虚拟寄存器,id 由 nextVReg 自增保证唯一。
MachineReg MachineFunction::createVReg(RegClass rc) {
  if (rc == RegClass::GenericVector) {
    throw BackendError(
        "generic vector vreg requires an explicit MachineVectorType");
  }
  return MachineReg::vreg(nextVReg++, rc);
}

MachineReg MachineFunction::createVectorVReg(
    const MachineVectorType &type) {
  if (!type.isValid()) {
    throw BackendError("invalid MachineVectorType in machine function " + name);
  }
  MachineReg Reg = MachineReg::vreg(nextVReg++, RegClass::GenericVector);
  vectorVRegTypes.emplace(Reg.id, type);
  return Reg;
}

MachineReg MachineFunction::createVRegLike(
    const MachineReg &prototype) {
  if (prototype.rc != RegClass::GenericVector) {
    return createVReg(prototype.rc);
  }
  const MachineVectorType *Type = getVectorType(prototype);
  if (!Type) {
    throw BackendError("missing vector type for vreg clone in machine function " +
                       name);
  }
  return createVectorVReg(*Type);
}

const MachineVectorType *MachineFunction::getVectorType(
    const MachineReg &reg) const {
  if (reg.physical || reg.rc != RegClass::GenericVector || reg.id == 0) {
    return nullptr;
  }
  auto Found = vectorVRegTypes.find(reg.id);
  return Found == vectorVRegTypes.end() ? nullptr : &Found->second;
}

// 在机器函数中登记一个栈对象(局部变量/溢出槽),记录大小/对齐/名字,
// 返回其在 stackObjects 中的下标,该下标即 FrameIndex 操作数所引用的编号。
int MachineFunction::createStackObject(int64_t size, int64_t align,
                                       std::string name) {
  const int index = static_cast<int>(stackObjects.size());
  StackObject object;
  object.index = index;
  object.size = size;
  object.align = align;
  object.name = std::move(name);
  stackObjects.push_back(std::move(object));
  return index;
}

// 将整个机器函数转储为可读文本(调试用)。逐块打印标签,
// 逐指令打印操作码,再依次列出 defs: 和 uses: 操作数,末尾附带注释(# ...)。
void dumpMachineFunction(const MachineFunction &mf, std::ostream &out) {
  out << "machine-function " << mf.name << '\n';
  for (const MachineBasicBlock &block : mf.blocks) {
    out << block.label << ":\n";
    for (const MachineInstr &instr : block.instrs) {
      out << "  " << opcodeName(instr.opcode);
      if (!instr.defs.empty()) {
        out << " defs:";
        for (const MachineOperand &def : instr.defs) {
          out << ' ';
          dumpOperand(def, mf, out);
        }
      }
      if (!instr.uses.empty()) {
        out << " uses:";
        for (const MachineOperand &use : instr.uses) {
          out << ' ';
          dumpOperand(use, mf, out);
        }
      }
      if (!instr.comment.empty()) {
        out << " # " << instr.comment;
      }
      if (instr.mayLoad || instr.mayStore) {
        out << " [mem size=" << instr.memorySize
            << " align=" << instr.memoryAlignment << ']';
      }
      out << '\n';
    }
  }
}

// 遍历机器函数的所有指令,校验其 def/use 寄存器操作数与隐式物理寄存器均有效,
// 常在关键 pass 之间作为完整性检查(见 MachineVerifyPass)。
void validateNoInvalidVRegs(const MachineFunction &mf) {
  for (const MachineBasicBlock &block : mf.blocks) {
    for (const MachineInstr &instr : block.instrs) {
      for (const MachineOperand &def : instr.defs) {
        validateOperand(def, mf);
      }
      for (const MachineOperand &use : instr.uses) {
        validateOperand(use, mf);
      }
      for (PhysReg reg : instr.implicitDefs) {
        validateImplicitPhysReg(reg, mf);
      }
      for (PhysReg reg : instr.implicitUses) {
        validateImplicitPhysReg(reg, mf);
      }
    }
  }
}

bool isGenericVectorOpcode(Opcode opcode) noexcept {
  switch (opcode) {
  case Opcode::G_VUNDEF:
  case Opcode::G_VCONST:
  case Opcode::G_VCOPY:
  case Opcode::G_VLOAD:
  case Opcode::G_VSTORE:
  case Opcode::G_VADD:
  case Opcode::G_VSUB:
  case Opcode::G_VMUL:
  case Opcode::G_VDIV:
  case Opcode::G_VUDIV:
  case Opcode::G_VREM:
  case Opcode::G_VUREM:
  case Opcode::G_VAND:
  case Opcode::G_VOR:
  case Opcode::G_VXOR:
  case Opcode::G_VSHL:
  case Opcode::G_VLSHR:
  case Opcode::G_VASHR:
  case Opcode::G_VFADD:
  case Opcode::G_VFSUB:
  case Opcode::G_VFMUL:
  case Opcode::G_VFDIV:
  case Opcode::G_VFREM:
  case Opcode::G_VICMP:
  case Opcode::G_VFCMP:
  case Opcode::G_VSELECT:
  case Opcode::G_VEXTRACT:
  case Opcode::G_VINSERT:
  case Opcode::G_VSHUFFLE:
  case Opcode::G_VTRUNC:
  case Opcode::G_VZEXT:
  case Opcode::G_VSEXT:
  case Opcode::G_VFPTOSI:
  case Opcode::G_VSITOFP:
  case Opcode::G_VBITCAST:
    return true;
  default:
    return false;
  }
}

bool containsGenericVectorMIR(const MachineFunction &function) noexcept {
  if (!function.vectorVRegTypes.empty()) {
    return true;
  }
  for (const MachineBasicBlock &Block : function.blocks) {
    for (const MachineInstr &Instr : Block.instrs) {
      if (isGenericVectorOpcode(Instr.opcode)) {
        return true;
      }
      for (const MachineOperand &Def : Instr.defs) {
        if (Def.kind == OperandKind::Reg &&
            Def.regValue.rc == RegClass::GenericVector) {
          return true;
        }
      }
      for (const MachineOperand &Use : Instr.uses) {
        if (Use.kind == OperandKind::Reg &&
            Use.regValue.rc == RegClass::GenericVector) {
          return true;
        }
      }
    }
  }
  return false;
}

bool containsGenericVectorMIR(const MachineModule &module) noexcept {
  for (const GlobalObject &Global : module.globals) {
    if (Global.requiresVectorLowering) {
      return true;
    }
  }
  for (const MachineFunction &Function : module.functions) {
    if (containsGenericVectorMIR(Function)) {
      return true;
    }
  }
  return false;
}

void validateGenericVectorMIR(const MachineModule &module) {
  auto RequireVectorOperand = [](const MachineFunction &Function,
                                 const MachineOperand &Operand,
                                 const char *Context)
      -> const MachineVectorType & {
    if (Operand.kind != OperandKind::Reg) {
      throw BackendError(std::string(Context) +
                         " requires a vector register operand");
    }
    const MachineVectorType *Type = Function.getVectorType(Operand.regValue);
    if (!Type || !Type->isValid()) {
      throw BackendError(std::string(Context) +
                         " has an untyped vector register");
    }
    return *Type;
  };
  auto RequireSameType = [](const MachineVectorType &Left,
                            const MachineVectorType &Right,
                            const char *Context) {
    if (Left != Right) {
      throw BackendError(std::string(Context) +
                         " has mismatched vector shapes");
    }
  };
  auto IsVectorBinary = [](Opcode Op) {
    switch (Op) {
    case Opcode::G_VADD:
    case Opcode::G_VSUB:
    case Opcode::G_VMUL:
    case Opcode::G_VDIV:
    case Opcode::G_VUDIV:
    case Opcode::G_VREM:
    case Opcode::G_VUREM:
    case Opcode::G_VAND:
    case Opcode::G_VOR:
    case Opcode::G_VXOR:
    case Opcode::G_VSHL:
    case Opcode::G_VLSHR:
    case Opcode::G_VASHR:
    case Opcode::G_VFADD:
    case Opcode::G_VFSUB:
    case Opcode::G_VFMUL:
    case Opcode::G_VFDIV:
    case Opcode::G_VFREM:
      return true;
    default:
      return false;
    }
  };
  auto IsVectorCast = [](Opcode Op) {
    switch (Op) {
    case Opcode::G_VTRUNC:
    case Opcode::G_VZEXT:
    case Opcode::G_VSEXT:
    case Opcode::G_VFPTOSI:
    case Opcode::G_VSITOFP:
    case Opcode::G_VBITCAST:
      return true;
    default:
      return false;
    }
  };

  for (const MachineFunction &Function : module.functions) {
    validateNoInvalidVRegs(Function);
    for (const auto &Entry : Function.vectorVRegTypes) {
      if (Entry.first == 0 || !Entry.second.isValid()) {
        throw BackendError("invalid vector vreg type table in machine function " +
                           Function.name);
      }
    }
    for (const MachineBasicBlock &Block : Function.blocks) {
      for (const MachineInstr &Instr : Block.instrs) {
        // PHI 暂时保留为目标无关伪指令；除此以外，GenericVector 只能出现
        // 在 G_V* 上，避免它被标量 COPY/分支/调用路径悄悄解释为 GPR。
        if (!isGenericVectorOpcode(Instr.opcode) &&
            Instr.opcode != Opcode::PHI) {
          for (const MachineOperand &Def : Instr.defs) {
            if (Def.kind == OperandKind::Reg &&
                Def.regValue.rc == RegClass::GenericVector) {
              throw BackendError(
                  "generic vector register reached a scalar MIR opcode");
            }
          }
          for (const MachineOperand &Use : Instr.uses) {
            if (Use.kind == OperandKind::Reg &&
                Use.regValue.rc == RegClass::GenericVector) {
              throw BackendError(
                  "generic vector register reached a scalar MIR opcode");
            }
          }
        }
        if (!isGenericVectorOpcode(Instr.opcode)) {
          continue;
        }
        if (Instr.opcode == Opcode::G_VUNDEF ||
            Instr.opcode == Opcode::G_VCONST) {
          if (Instr.defs.size() != 1) {
            throw BackendError("malformed vector constant pseudo");
          }
          const MachineVectorType &Def =
              RequireVectorOperand(Function, Instr.defs[0], "vector constant");
          if ((Instr.opcode == Opcode::G_VUNDEF && !Instr.uses.empty()) ||
              (Instr.opcode == Opcode::G_VCONST &&
               Instr.uses.size() != Def.lanes)) {
            throw BackendError("malformed vector constant lanes");
          }
          for (const MachineOperand &Use : Instr.uses) {
            if (Use.kind != OperandKind::Imm) {
              throw BackendError("vector constant lane must be immediate bits");
            }
          }
          continue;
        }
        if (Instr.opcode == Opcode::G_VCOPY) {
          if (Instr.defs.size() != 1 || Instr.uses.size() != 1) {
            throw BackendError("malformed vector copy pseudo");
          }
          RequireSameType(
              RequireVectorOperand(Function, Instr.defs[0], "vector copy"),
              RequireVectorOperand(Function, Instr.uses[0], "vector copy"),
              "vector copy");
          continue;
        }
        if (Instr.opcode == Opcode::G_VLOAD ||
            Instr.opcode == Opcode::G_VSTORE) {
          const bool IsLoad = Instr.opcode == Opcode::G_VLOAD;
          const std::size_t VectorUseIndex = IsLoad ? 0 : 0;
          if ((IsLoad && (Instr.defs.size() != 1 || Instr.uses.size() != 2)) ||
              (!IsLoad && (!Instr.defs.empty() || Instr.uses.size() != 3))) {
            throw BackendError("malformed vector memory pseudo");
          }
          const MachineVectorType &Type = IsLoad
              ? RequireVectorOperand(Function, Instr.defs[0], "vector load")
              : RequireVectorOperand(Function, Instr.uses[VectorUseIndex],
                                     "vector store");
          if (Type.isMask() || Instr.memorySize != Type.dataWidthBits() / 8U ||
              Instr.memoryAlignment == 0 || Instr.mayLoad != IsLoad ||
              Instr.mayStore == IsLoad) {
            throw BackendError("invalid vector memory metadata");
          }
          const std::size_t BaseIndex = IsLoad ? 0 : 1;
          if (Instr.uses[BaseIndex].kind != OperandKind::Reg &&
              Instr.uses[BaseIndex].kind != OperandKind::FrameIndex) {
            throw BackendError("vector memory base must be address-like");
          }
          if (Instr.uses.back().kind != OperandKind::Imm) {
            throw BackendError("vector memory offset must be immediate");
          }
          if (Instr.uses[BaseIndex].kind == OperandKind::Reg &&
              Instr.uses[BaseIndex].regValue.rc != RegClass::GPR64) {
            throw BackendError("vector memory register base must be GPR64");
          }
          continue;
        }
        if (IsVectorBinary(Instr.opcode)) {
          if (Instr.defs.size() != 1 || Instr.uses.size() != 2) {
            throw BackendError("malformed vector binary pseudo");
          }
          const MachineVectorType &Def =
              RequireVectorOperand(Function, Instr.defs[0], "vector binary");
          RequireSameType(Def, RequireVectorOperand(
                                   Function, Instr.uses[0], "vector binary"),
                          "vector binary");
          RequireSameType(Def, RequireVectorOperand(
                                   Function, Instr.uses[1], "vector binary"),
                          "vector binary");
          const bool IsFloating = Instr.opcode == Opcode::G_VFADD ||
                                  Instr.opcode == Opcode::G_VFSUB ||
                                  Instr.opcode == Opcode::G_VFMUL ||
                                  Instr.opcode == Opcode::G_VFDIV ||
                                  Instr.opcode == Opcode::G_VFREM;
          const bool IsMaskLogic = Def.isMask() &&
                                   (Instr.opcode == Opcode::G_VAND ||
                                    Instr.opcode == Opcode::G_VOR ||
                                    Instr.opcode == Opcode::G_VXOR);
          if ((IsFloating &&
               Def.element != MachineVectorElementKind::F32) ||
              (!IsFloating && !IsMaskLogic &&
               Def.element != MachineVectorElementKind::I32)) {
            throw BackendError("vector binary opcode/type mismatch");
          }
          continue;
        }
        if (Instr.opcode == Opcode::G_VICMP ||
            Instr.opcode == Opcode::G_VFCMP) {
          if (Instr.defs.size() != 1 || Instr.uses.size() != 3 ||
              Instr.uses[2].kind != OperandKind::Imm) {
            throw BackendError("malformed vector compare pseudo");
          }
          const MachineVectorType &Mask =
              RequireVectorOperand(Function, Instr.defs[0], "vector compare");
          const MachineVectorType &Left =
              RequireVectorOperand(Function, Instr.uses[0], "vector compare");
          const MachineVectorType &Right =
              RequireVectorOperand(Function, Instr.uses[1], "vector compare");
          RequireSameType(Left, Right, "vector compare");
          const MachineVectorElementKind ExpectedElement =
              Instr.opcode == Opcode::G_VFCMP
                  ? MachineVectorElementKind::F32
                  : MachineVectorElementKind::I32;
          const bool IsIntegerMaskCompare =
              Instr.opcode == Opcode::G_VICMP && Left.isMask();
          const unsigned ExpectedMaskGranule =
              Left.isMask() ? Left.maskGranuleBits : Left.elementBits();
          if (!Mask.isMask() || Mask.lanes != Left.lanes ||
              Mask.maskGranuleBits != ExpectedMaskGranule ||
              (Left.element != ExpectedElement && !IsIntegerMaskCompare)) {
            throw BackendError("vector compare mask shape mismatch");
          }
          continue;
        }
        if (Instr.opcode == Opcode::G_VSELECT) {
          if (Instr.defs.size() != 1 || Instr.uses.size() != 3) {
            throw BackendError("malformed vector select pseudo");
          }
          const MachineVectorType &Def =
              RequireVectorOperand(Function, Instr.defs[0], "vector select");
          RequireSameType(Def, RequireVectorOperand(
                                   Function, Instr.uses[1], "vector select"),
                          "vector select");
          RequireSameType(Def, RequireVectorOperand(
                                   Function, Instr.uses[2], "vector select"),
                          "vector select");
          if (Instr.uses[0].kind != OperandKind::Reg) {
            throw BackendError("vector select condition must be a register");
          }
          if (Instr.uses[0].regValue.rc == RegClass::GenericVector) {
            const MachineVectorType &Mask = RequireVectorOperand(
                Function, Instr.uses[0], "vector select mask");
            const unsigned ExpectedMaskGranule =
                Def.isMask() ? Def.maskGranuleBits : Def.elementBits();
            if (!Mask.isMask() || Mask.lanes != Def.lanes ||
                Mask.maskGranuleBits != ExpectedMaskGranule) {
              throw BackendError("vector select mask shape mismatch");
            }
          } else if (Instr.uses[0].regValue.rc != RegClass::GPR32) {
            throw BackendError("whole-vector select condition must be GPR32");
          }
          continue;
        }
        if (Instr.opcode == Opcode::G_VEXTRACT) {
          if (Instr.defs.size() != 1 || Instr.uses.size() != 2 ||
              Instr.defs[0].kind != OperandKind::Reg) {
            throw BackendError("malformed vector extract pseudo");
          }
          const MachineVectorType &Source = RequireVectorOperand(
              Function, Instr.uses[0], "vector extract");
          const RegClass Expected = Source.element == MachineVectorElementKind::F32
                                        ? RegClass::FPR32
                                        : RegClass::GPR32;
          if (Instr.defs[0].regValue.rc != Expected ||
              (Instr.uses[1].kind != OperandKind::Imm &&
               (Instr.uses[1].kind != OperandKind::Reg ||
                Instr.uses[1].regValue.rc != RegClass::GPR32))) {
            throw BackendError("invalid vector extract operand classes");
          }
          continue;
        }
        if (Instr.opcode == Opcode::G_VINSERT) {
          if (Instr.defs.size() != 1 || Instr.uses.size() != 3 ||
              Instr.uses[1].kind != OperandKind::Reg) {
            throw BackendError("malformed vector insert pseudo");
          }
          const MachineVectorType &Def =
              RequireVectorOperand(Function, Instr.defs[0], "vector insert");
          RequireSameType(Def, RequireVectorOperand(
                                   Function, Instr.uses[0], "vector insert"),
                          "vector insert");
          const RegClass Expected = Def.element == MachineVectorElementKind::F32
                                        ? RegClass::FPR32
                                        : RegClass::GPR32;
          if (Instr.uses[1].regValue.rc != Expected ||
              (Instr.uses[2].kind != OperandKind::Imm &&
               (Instr.uses[2].kind != OperandKind::Reg ||
                Instr.uses[2].regValue.rc != RegClass::GPR32))) {
            throw BackendError("invalid vector insert operand classes");
          }
          continue;
        }
        if (Instr.opcode == Opcode::G_VSHUFFLE) {
          if (Instr.defs.size() != 1 || Instr.uses.size() < 2) {
            throw BackendError("malformed vector shuffle pseudo");
          }
          const MachineVectorType &Def =
              RequireVectorOperand(Function, Instr.defs[0], "vector shuffle");
          const MachineVectorType &Left = RequireVectorOperand(
              Function, Instr.uses[0], "vector shuffle");
          const MachineVectorType &Right = RequireVectorOperand(
              Function, Instr.uses[1], "vector shuffle");
          RequireSameType(Left, Right, "vector shuffle inputs");
          if (Def.element != Left.element ||
              Def.maskGranuleBits != Left.maskGranuleBits ||
              Instr.uses.size() != static_cast<std::size_t>(Def.lanes) + 2U) {
            throw BackendError("vector shuffle result shape mismatch");
          }
          for (std::size_t Index = 2; Index < Instr.uses.size(); ++Index) {
            const MachineOperand &Mask = Instr.uses[Index];
            if (Mask.kind != OperandKind::Imm || Mask.immValue < -1 ||
                Mask.immValue >= static_cast<int64_t>(Left.lanes) * 2) {
              throw BackendError("invalid vector shuffle mask lane");
            }
          }
          continue;
        }
        if (IsVectorCast(Instr.opcode)) {
          if (Instr.defs.size() != 1 || Instr.uses.size() != 1) {
            throw BackendError("malformed vector cast pseudo");
          }
          const MachineVectorType &Def =
              RequireVectorOperand(Function, Instr.defs[0], "vector cast");
          const MachineVectorType &Source =
              RequireVectorOperand(Function, Instr.uses[0], "vector cast");
          if (Def.lanes != Source.lanes) {
            throw BackendError("vector cast changes lane count");
          }
          bool LegalCast = false;
          switch (Instr.opcode) {
          case Opcode::G_VTRUNC:
            LegalCast = Source.element == MachineVectorElementKind::I32 &&
                        Def.element == MachineVectorElementKind::I1;
            break;
          case Opcode::G_VZEXT:
          case Opcode::G_VSEXT:
            LegalCast = Source.element == MachineVectorElementKind::I1 &&
                        Def.element == MachineVectorElementKind::I32;
            break;
          case Opcode::G_VFPTOSI:
            LegalCast = Source.element == MachineVectorElementKind::F32 &&
                        Def.element == MachineVectorElementKind::I32;
            break;
          case Opcode::G_VSITOFP:
            LegalCast = Source.element == MachineVectorElementKind::I32 &&
                        Def.element == MachineVectorElementKind::F32;
            break;
          case Opcode::G_VBITCAST:
            LegalCast = Source.logicalBits() == Def.logicalBits();
            break;
          default:
            break;
          }
          if (!LegalCast) {
            throw BackendError("vector cast opcode/type mismatch");
          }
          continue;
        }
        throw BackendError("unvalidated generic vector opcode");
      }
    }
  }
}

} // namespace riscv
