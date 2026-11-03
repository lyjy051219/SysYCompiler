// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

// MIR的核心数据结构实现。
// 定义:
// MachineReg(虚拟/物理)
// MachineOperand(寄存器/立即数/栈帧下标/全局符号/基本块/值引用)
// Mfunc的虚寄存器与栈对象
// 以及调试用的部分MIR打印
#include "riscv/MIR.hpp"

#include "riscv/Target.hpp"

#include <ostream>
#include <string>
#include <unordered_map>
#include <utility>

namespace riscv {

namespace {

// 把 MIR 操作码枚举转成可读名字,仅供调试使用
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
  }
  return "<unknown>";
}

// 单个操作数打印
//不同种类采用不同前缀便于辨认
// 寄存器直接打印名字,立即数打印数值,栈帧下标写作 fi#N,
// 全局符号加 @ 前缀,基本块标签加 % 前缀,值引用写作 value@指针。
void dumpOperand(const MachineOperand &operand, std::ostream &out) {
  switch (operand.kind) {
  case OperandKind::Reg:
    out << operand.regValue.str();
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

// 校验寄存器类operand:非寄存器直接跳过,寄存器则要求其内容有效,
// 否则抛出 BackendError 并带上mf名
void validateOperand(const MachineOperand &operand, const MachineFunction &mf) {
  if (operand.kind != OperandKind::Reg) {
    return;
  }

  if (!operand.regValue.isValid()) {
    throw BackendError("invalid register operand in machine function " +
                       mf.name);
  }
}

// 保证是有效的具体物理寄存器，排除隐式物理寄存器(指令的 implicit def/use)
void validateImplicitPhysReg(PhysReg reg, const MachineFunction &mf) {
  if (reg == PhysReg::NoReg || !isValidPhysReg(reg)) {
    throw BackendError("invalid implicit physical register in machine function " +
                       mf.name);
  }
}

} // namespace

// 构造虚拟寄存器
//physical=false,带自增 id 和寄存器类标识,尚未绑定物理寄存器
MachineReg MachineReg::vreg(uint32_t id, RegClass rc) {
  MachineReg reg;
  reg.physical = false;
  reg.id = id;
  reg.rc = rc;
  reg.phys = PhysReg::NoReg;
  return reg;
}

// 构造物理寄存器
// physical=true,直接绑定具体 PhysReg,id 置零
MachineReg MachineReg::preg(PhysReg reg, RegClass rc) {
  MachineReg machineReg;
  machineReg.physical = true;
  machineReg.id = 0;
  machineReg.rc = rc;
  machineReg.phys = reg;
  return machineReg;
}

// 寄存器类须合法,且物理寄存器需为具体有效寄存器
// 虚拟寄存器需 id 非零
bool MachineReg::isValid() const {
  if (!isValidRegClass(rc)) {
    return false;
  }
  return physical ? phys != PhysReg::NoReg && isValidPhysReg(phys) : id != 0;
}

// 物理寄存器输出 ABI 名(如 a0),
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

// 按操作数种类分别构造 MachineOperand，寄存器/立即数/栈帧下标/全局符号/基本块标签
// 寄存器operand
MachineOperand MachineOperand::reg(MachineReg reg) {
  MachineOperand operand{};
  operand.kind = OperandKind::Reg;
  operand.regValue = reg;
  return operand;
}

// 立即数operand
MachineOperand MachineOperand::imm(int64_t value) {
  MachineOperand operand{};
  operand.kind = OperandKind::Imm;
  operand.immValue = value;
  return operand;
}

// 栈帧下标operand(引用某个栈对象,后续在frame lowering时替换成 sp 相对偏移)
MachineOperand MachineOperand::frameIndex(int index) {
  MachineOperand operand{};
  operand.kind = OperandKind::FrameIndex;
  operand.frameIndexValue = index;
  return operand;
}

// 全局符号operand(全局变量/函数名)。
MachineOperand MachineOperand::global(std::string symbol) {
  MachineOperand operand{};
  operand.kind = OperandKind::Global;
  operand.symbol = std::move(symbol);
  return operand;
}

// 基本块标签operand(分支目标)
MachineOperand MachineOperand::block(std::string label) {
  MachineOperand operand{};
  operand.kind = OperandKind::Block;
  operand.blockLabel = std::move(label);
  return operand;
}

// 中端值引用操作数(lowering 过程中用于回指原IR Value)
MachineOperand MachineOperand::value(Value *value) {
  MachineOperand operand{};
  operand.kind = OperandKind::ValueRef;
  operand.valueRef = value;
  return operand;
}

// 为mf分配一个新虚拟寄存器,id 由 nextVReg 自增
MachineReg MachineFunction::createVReg(RegClass rc) {
  return MachineReg::vreg(nextVReg++, rc);
}

MachineReg MachineFunction::createVRegLike(
    const MachineReg &prototype) {
  return createVReg(prototype.rc);
}

// 在mf中记录一个栈对象(局部变量/溢出槽),记录大小/对齐/名字,
// 返回其在 stackObjects 中的下标,该下标即 FrameIndex 操作数用的编号
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

// (调试用)mf打印，逐块打印标签,
// 逐指令打印操作码,再依次列出 defs: 和 uses: 操作数,末尾附带注释(# ...)
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
          dumpOperand(def, out);
        }
      }
      if (!instr.uses.empty()) {
        out << " uses:";
        for (const MachineOperand &use : instr.uses) {
          out << ' ';
          dumpOperand(use, out);
        }
      }
      if (!instr.comment.empty()) {
        out << " # " << instr.comment;
      }
      out << '\n';
    }
  }
}

// 遍历机器函数的所有指令,校验其 def/use 寄存器操作数与隐式物理寄存器均有效,
// 用于 pass 之间做检查(见 MachineVerifyPass)。
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

} // namespace riscv
