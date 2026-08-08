#include "riscv/Passes.hpp"

#include "lib/CFG.hpp"
#include "riscv/Target.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace riscv {
// 本文件实现若干机器级 pass 的公共正文:重建 CFG、消除 PHI(把 SSA 的 PHI
// lower 为前驱块中的并行拷贝,必要时拆分关键边)、以及降级 SELECT 伪指令
// (提供无分支算术实现与基于分支的实现两条路径)。位于指令选择之后、寄存器
// 分配之前(消 PHI 与降 SELECT 都要求操作数仍是虚拟寄存器)。此为 RISC-V 与
// ARM64 两后端相同的实现,两后端各保留一份相同实现。

namespace {

// 前置声明:文件内相互调用的辅助函数。
void appendJump(MachineBasicBlock &block, const std::string &label);
void retargetPhiIncomingPreds(MachineFunction &mf, const std::string &oldLabel,
                              const std::string &newLabel);
int findBlockIndex(const MachineFunction &mf, const std::string &label);

// 判断操作码是否为终结指令(返回、条件分支、无条件跳转)。
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

// J 与 RET 不会顺序落入下一块。
bool isNoFallthroughTerminator(Opcode opcode) {
  return opcode == Opcode::RET || opcode == Opcode::J;
}

// 指令是否为终结指令:显式 terminator 标记或操作码属于终结类。
bool isTerminatorInstr(const MachineInstr &instr) {
  return instr.terminator || isTerminator(instr.opcode);
}

// 块是否可顺序落入下一块:自块尾找首条终结指令按类型判定,无终结指令视为可落入。
bool blockCanFallThrough(const MachineBasicBlock &block) {
  for (auto it = block.instrs.rbegin(); it != block.instrs.rend(); ++it) {
    if (isTerminatorInstr(*it)) {
      return !isNoFallthroughTerminator(it->opcode);
    }
  }
  return true;
}

// 判断 IR 类型是否为浮点。
bool isFloatType(const std::shared_ptr<IRType> &type) {
  return type && type->getTypeSystem() == IR_FLOAT;
}

// 判断 IR 类型是否为指针。
bool isPointerType(const std::shared_ptr<IRType> &type) {
  return type && type->getTypeSystem() == IR_POINTER;
}

// 由 IR 值类型推断应放入的寄存器类:浮点归 FPR32,指针归 GPR64,整型归 GPR32。
// 类型缺失或不支持时抛出后端错误。
RegClass regClassForValue(Value *value) {
  if (!value || !value->getIRType()) {
    throw BackendError("missing value type while running machine pass");
  }
  switch (value->getIRType()->getTypeSystem()) {
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

// 按标签线性查找块下标,未找到返回 -1。
int findBlockIndex(const MachineFunction &mf, const std::string &label) {
  for (std::size_t i = 0; i < mf.blocks.size(); ++i) {
    if (mf.blocks[i].label == label) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

// 在 CFG 中加入 from 块到标签 toLabel 对应块的边,并对称维护后继/前驱表(去重)。
// 目标标签不存在时抛错。
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

// 在块末尾追加一条到 label 的无条件跳转 J。
void appendJump(MachineBasicBlock &block, const std::string &label) {
  MachineInstr jump;
  jump.opcode = Opcode::J;
  jump.uses.push_back(MachineOperand::block(label));
  jump.terminator = true;
  block.instrs.push_back(std::move(jump));
}

// 返回块中首条终结指令的下标;没有则返回指令数(表示末尾)。用于在终结指令前插入。
std::size_t firstTerminatorIndex(const MachineBasicBlock &block) {
  for (std::size_t i = 0; i < block.instrs.size(); ++i) {
    if (block.instrs[i].terminator || isTerminator(block.instrs[i].opcode)) {
      return i;
    }
  }
  return block.instrs.size();
}

// 把一批指令插入到块的终结指令之前(消 PHI 时把并行拷贝放在跳转前)。
void insertBeforeTerminators(MachineBasicBlock &block,
                             std::vector<MachineInstr> copies) {
  const std::size_t pos = firstTerminatorIndex(block);
  block.instrs.insert(block.instrs.begin() + static_cast<long>(pos),
                      copies.begin(), copies.end());
}

// 把一个常量 IR 值物化到寄存器,必要时发射 LI/浮点搬运指令并追加到 instrs。
// 整型/布尔/空指针/整型或指针的未定义值:值为 0 时直接复用零寄存器 X0,否则 LI;
// 浮点常量:先把其位模式 LI 到 GPR,再 FMV_W_X 搬到 FPR;浮点未定义值按 0 处理。
// 遇到无法物化的传入值抛出后端错误。返回持有该常量的寄存器。
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
             (value->getIRType()->getTypeSystem() == IR_INT ||
              value->getIRType()->getTypeSystem() == IR_POINTER)) {
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
    detail += " type=" + std::to_string(value->getIRType()->getTypeSystem());
  }
  if (auto *instruction = dynamic_cast<Instruction *>(value)) {
    detail +=
        " inst=" + std::to_string(static_cast<int>(instruction->instType));
  }
  throw BackendError("unsupported PHI constant incoming value " + detail);
}

// 把一个指针类 IR 值物化到寄存器:栈对象取其帧地址(ADDI 帧索引+0);
// 全局变量取其符号地址(LA);其余(常量指针等)退回常量物化。
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

// 取 IR 值在当前块可用的寄存器:已在 valueRegs 中映射则直接返回;否则按类型
// 现场物化(指针走指针物化,其余走常量物化)。物化产生的指令写入 instrs。
MachineReg valueRegInBlock(MachineFunction &mf, MachineBasicBlock &block,
                           std::vector<MachineInstr> &instrs, Value *value) {
  (void)block;
  auto mapped = mf.valueRegs.find(value);
  if (mapped != mf.valueRegs.end()) {
    return mapped->second;
  }
  if (value && value->getIRType() &&
      value->getIRType()->getTypeSystem() == IR_POINTER) {
    return materializePointerValue(mf, instrs, value);
  }
  return materializeConstant(mf, instrs, value);
}

// 比较两个机器寄存器是否完全相同。
bool sameMachineReg(const MachineReg &lhs, const MachineReg &rhs) {
  return lhs.physical == rhs.physical && lhs.id == rhs.id && lhs.rc == rhs.rc &&
         lhs.phys == rhs.phys;
}

// 判断寄存器是否为通用寄存器类(GPR32 或 GPR64)。
bool isGPR(MachineReg reg) {
  return reg.rc == RegClass::GPR32 || reg.rc == RegClass::GPR64;
}

// 判断寄存器当前是否"已知取值为 0/1 的布尔":零寄存器 X0 恒为布尔;否则查
// 已知布尔集合。无分支 SELECT 降级需要 cond 确为 0/1 才能用作掩码来源。
bool isKnownBoolean(const std::vector<MachineReg> &known,
                    const MachineReg &reg) {
  if (reg.physical && reg.phys == PhysReg::X0) {
    return true;
  }
  return std::find_if(known.begin(), known.end(), [&](const MachineReg &item) {
           return sameMachineReg(item, reg);
         }) != known.end();
}

// 从已知布尔集合中移除某寄存器(其值被重新定义,不再确定是布尔)。
void forgetKnownBoolean(std::vector<MachineReg> &known, const MachineReg &reg) {
  known.erase(std::remove_if(known.begin(), known.end(),
                             [&](const MachineReg &item) {
                               return sameMachineReg(item, reg);
                             }),
              known.end());
}

// 一条指令写入的所有寄存器都要从已知布尔集合中剔除,包括显式定义与隐式定义。
// 隐式定义无法确定寄存器类,故对 GPR64/GPR32/FPR32 三种表示都尝试剔除。
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

// 判断指令第 index 个使用端是否存在且为寄存器操作数。
bool isRegUse(const MachineInstr &instr, std::size_t index) {
  return index < instr.uses.size() &&
         instr.uses[index].kind == OperandKind::Reg;
}

// 判断指令是否产生一个取值必为 0/1 的布尔结果(用于扩充已知布尔集合)。
// 要求:恰有一个 GPR 定义、无副作用、非终结。比较类指令(SEQZ/SNEZ/SLT/浮点比较)
// 天然产生布尔;LI 立即数为 0/1;COPY 源是已知布尔;位运算在操作数均为已知布尔
// 且立即数为 0/1 时仍是布尔。
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

// 构造一条形如 dst = op(operands...) 的寄存器型指令(所有操作数均为寄存器)。
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

// 在已降级指令序列中逆向查找定义 trueValue 的 ADD/ADDW,判断其是否形如
// trueValue = falseValue + increment(即 SELECT 两分支相差一个增量)。命中则
// 通过 increment 与 addOpcode 输出,供无分支降级用 mask 选择增量的优化路径。
// 遇到定义 trueValue 但不符合加法模式的指令即返回 false。
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

// 统计每个虚拟寄存器在全函数中被用作使用端的次数。供判断某条件是否只有单一
// 使用(从而可能折叠进分支)使用。
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

// 判断 SELECT 的条件是否由一条可折叠进后续分支的比较指令产生:该条件是仅用一次
// 的虚拟寄存器,且其生产者是刚发射的、无副作用/无隐式操作数的比较类指令。
// 若成立,则保留 SELECT(交给后续 compare-fold),而不做无分支展开。
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

// 在已降级指令序列中倒序查找定义某寄存器的最后一条指令。
// 命中返回指向该指令的逆向迭代器;未找到返回 rend()。虚拟寄存器是单赋值
// (SSA),故块内任意位置的定义即为唯一定义,不存在被覆盖问题。
std::vector<MachineInstr>::const_reverse_iterator
findLastDefInLowered(const std::vector<MachineInstr> &lowered,
                     const MachineReg &reg) {
  for (auto it = lowered.rbegin(); it != lowered.rend(); ++it) {
    if (it->defs.size() == 1 && it->defs.front().kind == OperandKind::Reg &&
        sameMachineReg(it->defs.front().regValue, reg)) {
      return it;
    }
  }
  return lowered.rend();
}

// 判断 SELECT 是否为最小/最大形态:条件由 SLT/SLTU(a, b) 产生,且两分支值
// 恰为 a 与 b(任一顺序),即 dst = (a<b) ? a : b 或 (a<b) ? b : a。
// 该形态的掩码展开固定 4 条纯算术指令、无分支无跳转,在热循环(数组下标
// 夹取 max(i,0)、min/max 归约)中实测收益明确,列入白名单形态。
bool isMinMaxShape(const std::vector<MachineInstr> &lowered,
                   const MachineReg &cond, const MachineReg &trueValue,
                   const MachineReg &falseValue) {
  auto producer = findLastDefInLowered(lowered, cond);
  if (producer == lowered.rend() ||
      (producer->opcode != Opcode::SLT && producer->opcode != Opcode::SLTU) ||
      producer->uses.size() != 2 || !isRegUse(*producer, 0) ||
      !isRegUse(*producer, 1)) {
    return false;
  }
  const MachineReg a = producer->uses[0].regValue;
  const MachineReg b = producer->uses[1].regValue;
  return (sameMachineReg(trueValue, a) && sameMachineReg(falseValue, b)) ||
         (sameMachineReg(trueValue, b) && sameMachineReg(falseValue, a));
}

// 无分支 SELECT 降级:把 dst = SELECT(cond, trueValue, falseValue) 用算术+掩码
// 实现,避免生成条件分支。仅当结果为整型 GPR、cond 是已知布尔(0/1)、两分支
// 寄存器类一致时适用。掩码 mask = 0 - cond(cond=1 得全 1,cond=0 得全 0):
//   - 若两分支相差一个增量(true = false + inc),则 dst = false + (inc & mask);
//   - 否则用异或差值法 dst = false ^ ((true ^ false) & mask);
//   - 若条件可折叠进分支则保留原 SELECT 交给后续处理。
// P1#7 整改(默认关 + 白名单):RISC-V 无 csel,掩码展开固定 3~4 条指令且形成
// 串行依赖链,qemu/分支友好的分支实现往往更短,全量掩码展开实测负收益。
// 故掩码展开默认只对白名单形态开启:
//   A. 条件累加(true = false + inc):3 条指令,热循环实测大赢(matmul 2.2x);
//   B. 最小/最大(min/max):仅当条件不能折叠进分支时启用,4 条指令无跳转;
// 其余形态一律保留 SELECT 交给后续分支实现。设置环境变量
// BACKEND_ENABLE_BRANCHLESS_SELECT_ALL=1 可恢复旧的"全量展开"行为(A/B 用)。
// 逐块重建指令序列,同时维护已知布尔集合。返回是否发生过改写。
bool lowerBranchlessSelects(MachineFunction &mf) {
  bool changed = false;
  // 恢复旧行为的环境开关(仅用于 A/B 对照,非默认路径)。
  const bool aggressive =
      std::getenv("BACKEND_ENABLE_BRANCHLESS_SELECT_ALL") != nullptr;
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
        // 仅整型 GPR、条件确为 0/1、两分支寄存器类与 dst 一致时才走无分支路径。
        if (isGPR(dst) && dst.rc != RegClass::FPR32 &&
            isKnownBoolean(knownBooleans, cond) && trueValue.rc == dst.rc &&
            falseValue.rc == dst.rc) {
          const MachineReg zero = MachineReg::preg(PhysReg::X0, dst.rc);
          MachineReg increment;
          Opcode addOpcode = Opcode::ADDW;
          if (findAddWithFalseValue(lowered, trueValue, falseValue, increment,
                                    addOpcode)) {
            // 两分支相差 increment:mask = 0-cond,dst = false + (increment & mask)。
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
                                                       useCounts) &&
                     (aggressive ||
                      isMinMaxShape(lowered, cond, trueValue, falseValue))) {
            // 通用异或差值法:mask = 0-cond,dst = false ^ ((true ^ false) & mask)。
            // 默认仅白名单形态 B(min/max)启用;aggressive 时恢复旧全量展开。
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
            // 条件更适合折叠进后续分支:保留原 SELECT,不做无分支展开。
            forgetKnownBoolean(knownBooleans, dst);
            lowered.push_back(instr);
            continue;
          }
          forgetKnownBoolean(knownBooleans, dst);
          changed = true;
          continue;
        }
        // 不满足无分支条件:原样保留 SELECT。
        forgetKnownBoolean(knownBooleans, dst);
        lowered.push_back(instr);
        continue;
      }

      // 普通指令:先按其定义更新已知布尔集合,保留指令;若它本身产生布尔则登记。
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

// 构造一条并行拷贝伪指令 dst <- src(消 PHI 时先用它表达同时发生的赋值语义)。
MachineInstr makeParallelCopy(MachineReg dst, MachineReg src) {
  MachineInstr copy;
  copy.opcode = Opcode::PARALLEL_COPY;
  copy.defs.push_back(MachineOperand::reg(dst));
  copy.uses.push_back(MachineOperand::reg(src));
  return copy;
}

// 构造一条串行拷贝指令 dst <- src(按寄存器类选 F_COPY/COPY),标记为 phiCopy
// 以便寄存器合并阶段识别其来自 PHI 下降。
MachineInstr makeCopy(MachineReg dst, MachineReg src) {
  MachineInstr copy;
  copy.opcode = dst.rc == RegClass::FPR32 ? Opcode::F_COPY : Opcode::COPY;
  copy.defs.push_back(MachineOperand::reg(dst));
  copy.uses.push_back(MachineOperand::reg(src));
  copy.phiCopy = true;
  return copy;
}

// 把块中终结指令里指向 oldLabel 的分支目标改为 newLabel(拆分关键边时用)。
// 若该块没有任何显式指向 oldLabel 的分支(靠落入到达),则在其能落入的前提下
// 追加一条到 newLabel 的跳转;否则说明边无法安全拆分而抛错。
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

// 为某前驱 predLabel 生成消 PHI 所需的拷贝指令序列。
// 输入:phiBlock 中的各 PHI,insertionBlock 是拷贝将被插入的块。
// 关键步骤:先收集该前驱对应的 (目标寄存器 <- 传入值) 拷贝对(必要时物化常量);
// 去掉自拷贝;再把并行拷贝语义串行化——每轮挑一个"其目标不再作为任何未完成
// 拷贝的源"的拷贝先发射(拓扑排序);当只剩相互依赖的环时,引入临时寄存器保存
// 环中某个源的旧值,把对它的引用改为临时寄存器以打破环。若发生过打破环,则给
// 这批串行拷贝打上合并屏障,防止寄存器合并阶段把旋转链重新合出并行环。
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
  // 收集该前驱在各 PHI 中对应的 (PHI 结果 <- 传入值) 拷贝对。
  for (const MachineInstr &phi : phiBlock.instrs) {
    if (phi.opcode != Opcode::PHI) {
      continue;
    }
    if (phi.defs.empty()) {
      throw BackendError("malformed PHI without def");
    }
    // PHI 的 uses 按 (值, 前驱块) 成对排列,只取前驱为 predLabel 的那一对。
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
    // 找一个"目标不再被任何未完成拷贝当作源"的拷贝,它可以安全先发射。
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

// 把所有 PHI 传入前驱中等于 oldLabel 的改为 newLabel(降 SELECT 拆块后修正前驱)。
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

// 从头重建整个函数的 CFG 前驱/后继表(对外接口)。
// 关键步骤:先清空所有块的 preds/succs;再逐块扫描终结指令的块标签操作数加边;
// 若块可顺序落入且存在下一块,则补一条落入边。
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

// 消除 PHI 节点(对外接口):把 SSA 的 PHI lower 为前驱侧的拷贝,并删除 PHI。
// 关键步骤:先重建 CFG;对每个含 PHI 的块,遍历其前驱:
//   - 若前驱只有单一后继(边非关键),直接把拷贝插入前驱终结指令前;
//   - 否则该边为关键边,新建一个中转块放置拷贝并跳到本块,再把前驱分支改指
//     中转块(拆分关键边,避免拷贝污染其他后继路径);
// 处理完删除本块所有 PHI 并重建 CFG。
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
      // 前驱唯一后继:边非关键,拷贝直接插入前驱终结指令之前。
      if (mf.blocks[predIndex].succs.size() <= 1) {
        auto copies = buildPhiCopies(mf, mf.blocks[predIndex],
                                     mf.blocks[blockIndex], predLabel);
        insertBeforeTerminators(mf.blocks[predIndex], std::move(copies));
        continue;
      }

      // 关键边:新建中转块承载拷贝并跳到 PHI 块,再把前驱分支改指中转块。
      MachineBasicBlock split;
      split.label = predLabel + "_to_" + blockLabel + "_phi";
      auto copies = buildPhiCopies(mf, split, mf.blocks[blockIndex], predLabel);
      split.instrs.insert(split.instrs.end(), copies.begin(), copies.end());
      appendJump(split, blockLabel);

      replaceBranchTarget(mf.blocks[predIndex], blockLabel, split.label);
      mf.blocks.push_back(std::move(split));
    }

    MachineBasicBlock &phiBlock = mf.blocks[blockIndex];
    // 拷贝已放到各前驱侧,PHI 本身可以删除;删后重建 CFG(新增了中转块)。
    phiBlock.instrs.erase(std::remove_if(phiBlock.instrs.begin(),
                                         phiBlock.instrs.end(),
                                         [](const MachineInstr &instr) {
                                           return instr.opcode == Opcode::PHI;
                                         }),
                          phiBlock.instrs.end());
    rebuildCFG(mf);
  }
}

// 降级 SELECT 伪指令(对外接口)。
// branchlessOnly 为真时只做无分支算术展开并返回。否则用控制流实现:把含 SELECT
// 的块在该 SELECT 处切开,生成 true/false/done 三个块——条件真跳 true 块、假跳
// false 块,两块各把对应源并行拷贝到结果后汇合到 done 块;done 块承接原块 SELECT
// 之后的剩余指令。每块每次处理一个 SELECT,新块插入到当前块之后;最后重建 CFG。
// 返回是否发生过改写。
bool lowerSelectPseudo(MachineFunction &mf, bool branchlessOnly) {
  if (branchlessOnly) {
    return lowerBranchlessSelects(mf);
  }

  int selectId = 0;
  bool changed = false;
  for (std::size_t blockIndex = 0; blockIndex < mf.blocks.size();
       ++blockIndex) {
    MachineBasicBlock &block = mf.blocks[blockIndex];
    // 在块中找首条 SELECT;没有则跳过。
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
    // 把 SELECT 及其后的指令从原块切下:后续指令留给 done 块,原块以分支收尾。
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
    // 原块被替换为控制流入口,原指向原块的 PHI 前驱应改指汇合块 done。
    retargetPhiIncomingPreds(mf, block.label, doneBlock.label);

    // 原块尾部:条件非零跳 true 块,否则落入/跳 false 块。
    MachineInstr branch;
    branch.opcode = Opcode::BNEZ;
    branch.uses.push_back(select.uses[0]);
    branch.uses.push_back(MachineOperand::block(trueBlock.label));
    branch.terminator = true;
    block.instrs.push_back(std::move(branch));
    appendJump(block, falseBlock.label);

    // true/false 块各把对应源拷贝到结果寄存器,然后跳到 done 块汇合。
    trueBlock.instrs.push_back(makeParallelCopy(select.defs.front().regValue,
                                                select.uses[1].regValue));
    appendJump(trueBlock, doneBlock.label);

    falseBlock.instrs.push_back(makeParallelCopy(select.defs.front().regValue,
                                                 select.uses[2].regValue));
    appendJump(falseBlock, doneBlock.label);

    doneBlock.instrs = std::move(tail);

    // 依次把 done、false、true 插到当前块之后(倒序插入使最终顺序为 true/false/done)。
    auto insertPos = mf.blocks.begin() + static_cast<long>(blockIndex + 1);
    insertPos = mf.blocks.insert(insertPos, std::move(doneBlock));
    insertPos = mf.blocks.insert(insertPos, std::move(falseBlock));
    mf.blocks.insert(insertPos, std::move(trueBlock));
  }
  rebuildCFG(mf);
  return changed;
}

} // namespace riscv
