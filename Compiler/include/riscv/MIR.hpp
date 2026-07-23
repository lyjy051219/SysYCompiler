#pragma once

#include "Registers.hpp"
#include <cstdint>
#include <iosfwd>
#include <string>
#include <unordered_map>
#include <vector>

class Function;
class BasicBlock;
class Value;
class Variable;

namespace riscv {

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
  NOP, COPY, F_COPY, FMV_W_X, FMV_X_W, FCVT_D_S, FMV_X_D, LI, LA, ADD,
  ADDW, ADDI, ADDIW,
  SUB, SUBW,
  MUL, MULW, DIV, DIVW, DIVUW, REM, REMW, REMUW, AND, OR, XOR, ANDI, ORI,
  XORI, SLL, SLLW, SRAW, SRLW, SLLI, SLLIW, SRAI, SRAIW, SRLIW, SLT, SLTU,
  SEQZ, SNEZ, FEQ_S, FLT_S, FLE_S, FADD_S, FSUB_S, FMUL_S,
  FDIV_S, FCVT_W_S, FCVT_S_W, LBU, SB, LW, SW, LD, SD, FLW, FSW, GEP,
  MEMCPY_INLINE, MEMSET_INLINE, CALL, RET, BEQZ, BNEZ, BEQ, BNE, BLT, BGE, J,
  PHI, SELECT, PARALLEL_COPY
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

struct MachineFunction {
  std::string name;
  Function *irFunction = nullptr;
  RegClass returnClass = RegClass::GPR32;
  bool returnsValue = false;
  std::vector<MachineBasicBlock> blocks;
  std::vector<StackObject> stackObjects;
  std::unordered_map<Value *, MachineReg> valueRegs;
  std::unordered_map<Value *, int> valueStackObjects;
  std::vector<PhysReg> usedCalleeSaved;
  int64_t maxOutgoingArgs = 0;
  int64_t frameSize = 0;
  uint32_t nextVReg = 1;

  MachineReg createVReg(RegClass rc);
  int createStackObject(int64_t size, int64_t align, std::string name);
};

struct GlobalObject {
  std::string name;
  Variable *ir = nullptr;
  bool constant = false;
  int64_t size = 0;
  int64_t align = 4;
};

struct MachineModule {
  std::vector<GlobalObject> globals;
  std::vector<MachineFunction> functions;
};

void dumpMachineFunction(const MachineFunction &mf, std::ostream &out);
void validateNoInvalidVRegs(const MachineFunction &mf);

} // namespace riscv
