#pragma once

#include "Registers.hpp"
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

class Function;
class BasicBlock;
class Value;
class Variable;

namespace riscv {

// 逻辑向量元素类型。I1 是比较产生的 lane mask；maskGranuleBits 记录每个
// mask lane 所对应的数据 lane 位宽，避免把 <4 x i1> 错当成 4-bit 寄存器。
enum class MachineVectorElementKind : uint8_t { Invalid, I1, I32, F32 };

struct MachineVectorType {
  MachineVectorElementKind element = MachineVectorElementKind::Invalid;
  uint16_t lanes = 0;
  uint16_t maskGranuleBits = 0;

  bool isValid() const noexcept;
  bool isMask() const noexcept;
  unsigned elementBits() const noexcept;
  unsigned logicalBits() const noexcept;
  unsigned dataWidthBits() const noexcept;
  std::string str() const;
  bool operator==(const MachineVectorType &other) const noexcept;
  bool operator!=(const MachineVectorType &other) const noexcept {
    return !(*this == other);
  }
};

struct MachineReg {
  bool physical = false;
  uint32_t id = 0;
  RegClass rc = RegClass::GPR64;
  PhysReg phys = PhysReg::NoReg;

  static MachineReg vreg(uint32_t id, RegClass rc);
  static MachineReg preg(PhysReg reg, RegClass rc);
  bool isValid() const;
  std::string str() const;
};

enum class Opcode {
  NOP, COPY, F_COPY, FMV_W_X, FMV_X_W, LI, LA, ADD, ADDW, ADDI, ADDIW,
  SUB, SUBW,
  MUL, MULW, DIV, DIVW, DIVUW, REM, REMW, REMUW, AND, OR, XOR, ANDI, ORI,
  XORI, SLL, SLLW, SRAW, SRLW, SLLI, SLLIW, SRAI, SRAIW, SRLIW, SLT, SLTU,
  SEQZ, SNEZ, FEQ_S, FLT_S, FLE_S, FADD_S, FSUB_S, FMUL_S,
  FDIV_S, FMADD_S, FMSUB_S, FNMADD_S, FNMSUB_S, FCVT_W_S, FCVT_S_W, LBU, SB, LW, SW, LD, SD, FLW, FSW, GEP,
  MEMCPY_INLINE, MEMSET_INLINE, CALL, RET, BEQZ, BNEZ, BEQ, BNE, BLT, BGE, J,
  PHI, SELECT, PARALLEL_COPY,
  // 与最终 RISC-V SIMD 指令集无关的语义 pseudo。它们必须在寄存器分配
  // 前由目标 VectorLegalize/ISel 消除；AsmPrinter 永远不会直接打印它们。
  G_VUNDEF, G_VCONST, G_VCOPY, G_VLOAD, G_VSTORE,
  G_VADD, G_VSUB, G_VMUL, G_VDIV, G_VUDIV, G_VREM, G_VUREM,
  G_VAND, G_VOR, G_VXOR, G_VSHL, G_VLSHR, G_VASHR,
  G_VFADD, G_VFSUB, G_VFMUL, G_VFDIV, G_VFREM,
  G_VICMP, G_VFCMP, G_VSELECT,
  G_VEXTRACT, G_VINSERT, G_VSHUFFLE,
  G_VTRUNC, G_VZEXT, G_VSEXT, G_VFPTOSI, G_VSITOFP, G_VBITCAST
};

enum class OperandKind { Reg, Imm, FrameIndex, Global, Block, ValueRef };

struct MachineOperand {
  OperandKind kind = OperandKind::Reg;
  MachineReg regValue;
  int64_t immValue = 0;
  int frameIndexValue = -1;
  std::string symbol;
  std::string blockLabel;
  Value *valueRef = nullptr;
  // 向量 PHI 被标量化后，ValueRef 记录取第几个 lane；标量默认 -1。
  int valueLane = -1;

  static MachineOperand reg(MachineReg reg);
  static MachineOperand imm(int64_t value);
  static MachineOperand frameIndex(int index);
  static MachineOperand global(std::string symbol);
  static MachineOperand block(std::string label);
  static MachineOperand value(Value *value);
};

struct MachineInstr {
  Opcode opcode = Opcode::NOP;
  std::vector<MachineOperand> defs;
  std::vector<MachineOperand> uses;
  std::vector<PhysReg> implicitDefs;
  std::vector<PhysReg> implicitUses;
  bool sideEffects = false;
  bool terminator = false;
  // Set on COPYs emitted while lowering PHI incoming values.  Register
  // allocation uses this provenance to distinguish loop-carried transfers
  // from ordinary copies and from mutually exclusive diamond edges.
  bool phiCopy = false;
  // Some COPY instructions are ordering barriers rather than ordinary value
  // transfers.  In particular, the temporary and rotations used to serialize
  // a cyclic PHI bundle must not be undone by register coalescing.
  bool copyCoalescingBarrier = false;
  // 访存元数据由通用向量 pseudo 保留到目标合法化阶段。alignment=0 表示
  // 未知；不能据此选择要求自然对齐的硬件指令。
  bool mayLoad = false;
  bool mayStore = false;
  uint32_t memorySize = 0;
  uint32_t memoryAlignment = 0;
  std::string comment;
};

struct StackObject {
  int index = -1;
  int64_t size = 0;
  int64_t align = 1;
  int64_t offset = 0;
  bool fixed = false;
  std::string name;
};

struct MachineBasicBlock {
  std::string label;
  BasicBlock *irBlock = nullptr;
  std::vector<MachineInstr> instrs;
  std::vector<int> preds;
  std::vector<int> succs;
};

struct GlobalObject {
  std::string name;
  Variable *ir = nullptr;
  bool constant = false;
  int64_t size = 0;
  int64_t align = 4;
  bool requiresVectorLowering = false;
};

struct MachineFunction {
  std::string name;
  Function *irFunction = nullptr;
  RegClass returnClass = RegClass::GPR32;
  bool returnsValue = false;
  std::vector<MachineBasicBlock> blocks;
  std::vector<StackObject> stackObjects;
  std::unordered_map<Value *, MachineReg> valueRegs;
  // VectorLegalize 把向量 SSA 值标量化后，Value -> lane 标量寄存器组。
  std::unordered_map<Value *, std::vector<MachineReg>> valueVectorLanes;
  // GenericVector 虚拟寄存器的精确逻辑类型。标量寄存器不进入此表。
  std::unordered_map<uint32_t, MachineVectorType> vectorVRegTypes;
  std::unordered_map<Value *, int> valueStackObjects;
  // 模块级全局对象快照（符号名 -> 尺寸/常量性），lowerModule 时按
  // MachineModule::globals 填充，std::map 保证确定性遍历序。
  // 供机器层 pass（如 machineLICM 的 load 在界/常量性查询）使用，避免依赖 IR 层。
  std::map<std::string, GlobalObject> globals;
  std::vector<PhysReg> usedCalleeSaved;
  int64_t maxOutgoingArgs = 0;
  int64_t frameSize = 0;
  uint32_t nextVReg = 1;

  MachineReg createVReg(RegClass rc);
  MachineReg createVectorVReg(const MachineVectorType &type);
  MachineReg createVRegLike(const MachineReg &prototype);
  const MachineVectorType *getVectorType(const MachineReg &reg) const;
  int createStackObject(int64_t size, int64_t align, std::string name);
};

struct MachineModule {
  std::vector<GlobalObject> globals;
  std::vector<MachineFunction> functions;
};

void dumpMachineFunction(const MachineFunction &mf, std::ostream &out);
void validateNoInvalidVRegs(const MachineFunction &mf);
bool isGenericVectorOpcode(Opcode opcode) noexcept;
bool containsGenericVectorMIR(const MachineFunction &function) noexcept;
bool containsGenericVectorMIR(const MachineModule &module) noexcept;
void validateGenericVectorMIR(const MachineModule &module);

} // namespace riscv
