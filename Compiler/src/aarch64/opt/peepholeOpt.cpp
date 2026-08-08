// peepholeOpt.cpp - AArch64 MIR 层的窥孔优化与本地(块内)清理 pass 集合。
//
// 本文件实现若干在寄存器分配前后运行的机器级优化,均以基本块为作用域:
//   - 比较+分支折叠:把 SLT/XOR/XORI 产生布尔再 BNEZ/BEQZ 的两三条指令折叠成
//     一条带条件的分支(BLT/BGE/BEQ/BNE 等),需满足布尔值只此一处使用;
//   - 全局地址复用:把同一全局的多次 LA 提到入口块共享一个寄存器;
//   - 地址合并:把 ADDI/ADD 形成的“基址+常量”重定位到共同 pivot,缩短访存偏移;
//   - 拷贝传播:顺着 COPY/F_COPY 把使用改写到源寄存器(函数级,跨块);
//   - 本地 CSE:块内消除重复的纯运算与 load,带完善的 invalidation 逻辑;
//   - RA 后窥孔:删冗余拷贝/加零/NOP,做零元恒等式折叠。
//
// 正确性关键点:改写只在“单次定义/单次使用、且不跨块”等前提下进行;CSE 对
// store/call/副作用指令按内存与寄存器依赖精确失效;地址合并用版本号跟踪基址
// 是否被重定义。相关判定见各辅助函数注释。SSA 特性在 RA 前成立,RA 后不成立,
// 故拷贝传播/CSE 只在 RA 前运行,纯清理类在 RA 后运行。
#include "aarch64/opt/MachineOptPasses.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace aarch64 {
namespace {

// 两个机器寄存器是否完全相同(物理标志、虚拟 id、寄存器类、物理编号全一致)。
bool sameRegister(const MachineReg &lhs, const MachineReg &rhs) {
  return lhs.physical == rhs.physical && lhs.id == rhs.id && lhs.rc == rhs.rc &&
         lhs.phys == rhs.phys;
}

// 指令恰有一个寄存器定义和一个寄存器使用(典型的拷贝形态)。
bool hasSingleRegDefUse(const MachineInstr &instr) {
  return instr.defs.size() == 1 && instr.uses.size() == 1 &&
         instr.defs.front().kind == OperandKind::Reg &&
         instr.uses.front().kind == OperandKind::Reg;
}

// 指令恰有一个寄存器定义。
bool hasSingleRegDef(const MachineInstr &instr) {
  return instr.defs.size() == 1 && instr.defs.front().kind == OperandKind::Reg;
}

// 第 index 个使用是否为寄存器操作数。
bool isRegUse(const MachineInstr &instr, std::size_t index) {
  return index < instr.uses.size() &&
         instr.uses[index].kind == OperandKind::Reg;
}

// 操作数是否为零寄存器 x0(物理寄存器且编号为 X0)。
bool isZeroReg(const MachineOperand &operand) {
  return operand.kind == OperandKind::Reg && operand.regValue.physical &&
         operand.regValue.phys == PhysReg::X0;
}

// 指令是否可安全改写:无副作用、非终结、无隐式定义/使用,且只有单个寄存器定义。
// 这些前提保证把它替换成拷贝不会丢失额外的效果或改变控制流。
bool isSafeToRewrite(const MachineInstr &instr) {
  return !instr.sideEffects && !instr.terminator &&
         instr.implicitDefs.empty() && instr.implicitUses.empty() &&
         hasSingleRegDef(instr);
}

// 用给定源操作数构造一条 COPY 指令,沿用原指令的定义与注释(用于零元恒等折叠)。
MachineInstr makeCopyFrom(const MachineInstr &instr,
                          MachineOperand sourceOperand) {
  MachineInstr copy;
  copy.opcode = Opcode::COPY;
  copy.defs = instr.defs;
  copy.uses.push_back(std::move(sourceOperand));
  copy.comment = instr.comment;
  return copy;
}

// 构造与定义寄存器同类的零寄存器操作数(用于结果恒为 0 的折叠,如 x AND 0)。
MachineOperand zeroForDef(const MachineInstr &instr) {
  return MachineOperand::reg(
      MachineReg::preg(PhysReg::X0, instr.defs.front().regValue.rc));
}

// 是否是自拷贝(COPY/F_COPY/PARALLEL_COPY 且源与目标同寄存器),可整条删除。
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

// 折叠“加/移位立即数 0”为拷贝:形如 ADDI/ADDIW/ORI/XORI/各移位立即数 rd, rs, 0
// 等价于 rd = rs,改写成 COPY。仅在可安全改写、且第二操作数是立即 0 时成立。
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

// 折叠含零寄存器的二元恒等式:
//   ADD/OR/XOR 有一操作数为 x0,结果等于另一操作数,折成拷贝;
//   SUB/移位 的第二操作数为 x0(减零/移零位)也折成拷贝其第一操作数;
//   AND 任一操作数为 x0,结果恒 0;MUL 任一操作数为 x0,结果恒 0;
//   移位 的第一操作数(被移位值)为 x0,结果恒 0。
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

  if (instr.opcode == Opcode::MUL || instr.opcode == Opcode::MULW ||
      instr.opcode == Opcode::SMULL) {
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

// 折叠两操作数为同一寄存器的运算:AND/OR x,x = x(折成拷贝);
// SUB/XOR/SLT/SLTU x,x = 0(折成拷贝零寄存器)。
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

// 零元/恒等折叠总入口:依次尝试立即数加零、含零寄存器恒等式、同操作数折叠。
bool foldZeroIdentity(const MachineInstr &instr, MachineInstr &folded) {
  return foldAddImmediateZero(instr, folded) ||
         foldBinaryZeroIdentity(instr, folded) ||
         foldSameOperand(instr, folded);
}

// 是否是自加零(ADDI rd, rd, 0),即定义与源为同一寄存器且立即数为 0,可删除。
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

// RA 后可直接删除的指令:无副作用的 NOP、自拷贝、自加零。
bool isRemovablePostRAInstr(const MachineInstr &instr) {
  if (!instr.sideEffects && !instr.terminator && instr.opcode == Opcode::NOP) {
    return true;
  }
  return isRedundantCopy(instr) || isRedundantAddZero(instr);
}

// 操作数是否为虚拟寄存器(未分配物理寄存器)。
bool isVirtualRegOperand(const MachineOperand &operand) {
  return operand.kind == OperandKind::Reg && !operand.regValue.physical;
}

// 沿拷贝传播的替换表把一个虚拟寄存器解析到其最终等价寄存器。逐级跟随直到无
// 后继或形成自映射,防止环。只对虚拟寄存器解析(物理寄存器视为终点)。
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

// 按替换表把指令的各虚拟寄存器使用改写为解析后的寄存器,返回是否有改动。
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

// 判断操作数能否作为 CSE 表达式的组成:虚拟寄存器或 x0、立即数、帧对象、全局
// 符号可参与;块标签、值引用会随上下文变化,不可参与。
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

// 把寄存器编码成 CSE 键的一部分(物理/虚拟前缀 + 编号 + 寄存器类)。
std::string regKey(const MachineReg &reg) {
  if (reg.physical) {
    return "p:" + std::to_string(static_cast<int>(reg.phys)) + ":" +
           std::to_string(static_cast<int>(reg.rc));
  }
  return "v:" + std::to_string(reg.id) + ":" +
         std::to_string(static_cast<int>(reg.rc));
}

// 把任意操作数编码成 CSE 键片段(按种类加前缀区分)。
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

// 操作码是否可交换(用于 CSE 键归一:两操作数排序后再入键,识别 a+b 与 b+a)。
bool isCommutativeOpcode(Opcode opcode) {
  switch (opcode) {
  case Opcode::ADD:
  case Opcode::ADDW:
  case Opcode::MUL:
  case Opcode::MULW:
  case Opcode::SMULL:
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

// 是否是 load 操作码(其 CSE 结果存独立表,遇 store/调用需按内存失效)。
bool isLoadOpcode(Opcode opcode) {
  return opcode == Opcode::LBU || opcode == Opcode::LW ||
         opcode == Opcode::LD || opcode == Opcode::FLW;
}

// 该操作码是否为可参与 CSE 的纯运算(无副作用、结果仅由操作数决定)。含常量
// 物化、算术/逻辑/移位、比较、以及 load 地址计算等;load 本身也纳入(另表管理)。
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
  case Opcode::SMULL:
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
  // ARM64 专用符号扩展:纯函数,可参与 CSE。地址计算里 `sext(index)` 常被
  // 中端为同一索引重复生成,纳入 CSE 后其下游的 SLLI/ADD 地址链也随之合并,
  // 消除数组访问里成片重复的地址物化。
  case Opcode::SXTW:
    return true;
  default:
    return false;
  }
}

// 指令是否是本地 CSE 候选:无副作用/终结/隐式操作数,恰一个虚拟寄存器定义,
// 操作码是纯运算,且所有使用都允许参与 CSE。
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

// 生成指令的 CSE 键:操作码 + 结果寄存器类 + 各使用键;可交换运算把两操作数
// 排序后入键,使 a op b 与 b op a 得到相同键。
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

// CSE 数据结构:表达式/load 结果表(键到结果寄存器)、以及双向依赖索引,
// 用于在某寄存器被重定义时快速失效所有依赖它的表项;上限限制表规模。
using CSETable = std::map<std::string, MachineReg>;
using CSEKeyDependencies = std::map<std::string, std::set<std::string>>;
using CSERegisterDependencies = std::map<std::string, std::set<std::string>>;
constexpr std::size_t kMaxLocalCSEEntries = 64;

// 依赖索引里表示一个寄存器的键(仅按物理/虚拟编号,不含寄存器类)。
std::string cseDependencyKey(const MachineReg &reg) {
  if (reg.physical) {
    return "p:" + std::to_string(static_cast<int>(reg.phys));
  }
  return "v:" + std::to_string(reg.id);
}

// 从所有 CSE 表结构中移除一个键:同时清掉它在寄存器依赖索引里的反向引用。
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

// 清空全部 CSE 表(遇到调用或非 store 副作用指令时整体失效)。
void clearCSETables(CSETable &exprs, CSETable &loads,
                    CSEKeyDependencies &keyDependencies,
                    CSERegisterDependencies &registerDependencies) {
  exprs.clear();
  loads.clear();
  keyDependencies.clear();
  registerDependencies.clear();
}

// 是否是内存 store 操作码。store 只会改变内存,故只需失效 load 表而非表达式表。
bool isMemoryStoreOpcode(Opcode opcode) {
  return opcode == Opcode::SB || opcode == Opcode::SW || opcode == Opcode::SD ||
         opcode == Opcode::FSW;
}

// 仅失效 load 表(store 之后 load 结果可能过期,但纯 ALU 表达式不受内存影响)。
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

// 登记一个 CSE 键对其各寄存器使用的依赖(双向索引),供后续按寄存器失效。
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

// 某寄存器被重定义时,失效所有依赖它的 CSE 表项,并从拷贝替换表里剔除以它为
// 键或为值的映射(避免把使用改写到已失效的等价寄存器)。
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

// 对一条指令的所有定义(含隐式定义,按两种寄存器类各失效一次)执行 CSE 失效。
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

// 统计每个虚拟寄存器被使用到的块集合(用于判断某定义的使用是否全在同一块内,
// 从而可安全地在块内做拷贝传播/CSE 后删除定义)。
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

// 某虚拟寄存器的使用是否全部落在给定块内(无使用也算满足)。
bool allUsesInBlock(
    uint32_t regId, std::size_t blockIndex,
    const std::map<uint32_t, std::set<std::size_t>> &useBlocks) {
  auto found = useBlocks.find(regId);
  if (found == useBlocks.end()) {
    return true;
  }
  return found->second.size() == 1 && found->second.count(blockIndex) != 0;
}

// 统计每个虚拟寄存器的使用次数(用于比较+分支折叠要求布尔值只此一处使用)。
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

// 统计每个虚拟寄存器的定义次数(拷贝传播要求源/目标均只有单一定义)。
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

// 该虚拟寄存器是否恰被使用一次。
bool hasSingleVirtualUse(const MachineOperand &def,
                         const std::map<uint32_t, std::size_t> &useCounts) {
  if (!isVirtualRegOperand(def)) {
    return false;
  }
  auto found = useCounts.find(def.regValue.id);
  return found != useCounts.end() && found->second == 1;
}

// 该虚拟寄存器是否恰被定义一次(SSA 前提下拷贝传播的正确性依据)。
bool hasSingleVirtualDef(const MachineOperand &def,
                         const std::map<uint32_t, std::size_t> &defCounts) {
  if (!isVirtualRegOperand(def)) {
    return false;
  }
  auto found = defCounts.find(def.regValue.id);
  return found != defCounts.end() && found->second == 1;
}

// 是否是对某寄存器的 BEQZ/BNEZ 条件分支(操作数为寄存器 + 目标块)。
bool isBranchOnZeroReg(const MachineInstr &instr) {
  return (instr.opcode == Opcode::BEQZ || instr.opcode == Opcode::BNEZ) &&
         instr.uses.size() == 2 && instr.uses[0].kind == OperandKind::Reg &&
         instr.uses[1].kind == OperandKind::Block;
}

// 是否是对结果测零的 SEQZ/SNEZ(可与后续分支进一步合并)。
bool isZeroCompare(const MachineInstr &instr) {
  return (instr.opcode == Opcode::SEQZ || instr.opcode == Opcode::SNEZ) &&
         instr.defs.size() == 1 && instr.uses.size() == 1 &&
         instr.defs[0].kind == OperandKind::Reg &&
         instr.uses[0].kind == OperandKind::Reg && !instr.sideEffects &&
         !instr.terminator && instr.implicitDefs.empty() &&
         instr.implicitUses.empty();
}

// 是否是可折进分支的比较产生者:SLT/XOR/XORI 且干净(无副作用/隐式操作数)。
bool isFoldableCompareProducer(const MachineInstr &instr) {
  return (instr.opcode == Opcode::SLT || instr.opcode == Opcode::XOR ||
          instr.opcode == Opcode::XORI) &&
         instr.defs.size() == 1 && instr.defs[0].kind == OperandKind::Reg &&
         !instr.sideEffects && !instr.terminator &&
         instr.implicitDefs.empty() && instr.implicitUses.empty();
}

// 是否是两个寄存器输入的 SLT/XOR 比较(可折成 BLT/BGE 或 BEQ/BNE)。
bool isBinaryRegCompareProducer(const MachineInstr &instr) {
  return (instr.opcode == Opcode::SLT || instr.opcode == Opcode::XOR) &&
         instr.uses.size() == 2 && instr.uses[0].kind == OperandKind::Reg &&
         instr.uses[1].kind == OperandKind::Reg &&
         isFoldableCompareProducer(instr);
}

// 是否是 XORI rd, rs, 0(即 rd = rs 的比较别名,可让分支直接对 rs 测零)。
bool isXoriZeroProducer(const MachineInstr &instr) {
  return instr.opcode == Opcode::XORI && instr.uses.size() == 2 &&
         instr.uses[0].kind == OperandKind::Reg &&
         instr.uses[1].kind == OperandKind::Imm &&
         instr.uses[1].immValue == 0 && isFoldableCompareProducer(instr);
}

// 分支的条件寄存器是否恰是 defInstr 定义的寄存器(判定二者是否可折叠)。
bool branchUsesDef(const MachineInstr &branch, const MachineInstr &defInstr) {
  return isBranchOnZeroReg(branch) &&
         sameRegister(branch.uses[0].regValue, defInstr.defs[0].regValue);
}

// 零比较的条件寄存器是否恰是 defInstr 定义的寄存器。
bool zeroCompareUsesDef(const MachineInstr &zeroCompare,
                        const MachineInstr &defInstr) {
  return isZeroCompare(zeroCompare) &&
         sameRegister(zeroCompare.uses[0].regValue, defInstr.defs[0].regValue);
}

// SLT+分支的目标分支操作码:BNEZ(结果非零即取真,即 a<b)对应 BLT,反之 BGE。
Opcode branchOpcodeForSlt(Opcode branchOpcode) {
  return branchOpcode == Opcode::BNEZ ? Opcode::BLT : Opcode::BGE;
}

// XOR+分支的目标分支操作码:BNEZ(异或非零即不等)对应 BNE,反之 BEQ。
Opcode branchOpcodeForXor(Opcode branchOpcode) {
  return branchOpcode == Opcode::BNEZ ? Opcode::BNE : Opcode::BEQ;
}

// 在中间还夹着一层 SEQZ/SNEZ 时,综合“分支取真/取假”与“比较测非零/测零”两重
// 极性选出最终分支操作码:两重极性同向取 nonZeroBranch,异向取 zeroBranch。
Opcode branchOpcodeForZeroCompare(Opcode zeroOpcode, Opcode branchOpcode,
                                  Opcode nonZeroBranch, Opcode zeroBranch) {
  const bool branchOnConditionTrue = branchOpcode == Opcode::BNEZ;
  const bool conditionIsNonZero = zeroOpcode == Opcode::SNEZ;
  return branchOnConditionTrue == conditionIsNonZero ? nonZeroBranch
                                                     : zeroBranch;
}

// 构造一条“寄存器-寄存器-目标”形式的比较分支(如 BLT/BGE/BEQ/BNE)。
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

// 构造一条“寄存器对零-目标”形式的分支(如 BEQZ/BNEZ)。
MachineInstr makeRegZeroBranch(Opcode opcode, const MachineOperand &reg,
                               const MachineOperand &target) {
  MachineInstr branch;
  branch.opcode = opcode;
  branch.uses.push_back(reg);
  branch.uses.push_back(target);
  branch.terminator = true;
  return branch;
}

// 折叠“比较 + 分支”两条指令:要求比较可折叠、分支用其结果、且该结果只此一处
// 使用(否则删掉比较会破坏其它使用)。SLT/XOR 折成寄存器比较分支,XORI 0 折成
// 对源寄存器的测零分支。产物写入 folded。
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

// 折叠“SEQZ/SNEZ + 分支”:综合两重极性得到直接对源寄存器测零的分支。要求零比较
// 结果只此一处使用。
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

// 折叠三段式“比较 + SEQZ/SNEZ + 分支”为一条比较分支。要求三者链式依赖、比较与
// 零比较结果各自只此一处使用;最终分支操作码由比较类型与两重极性共同决定。
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

// 比较与分支之间允许间隔的最大指令数(用于折叠相隔若干条的比较与分支)。
constexpr std::size_t kMaxCompareBranchGap = 4;

// 指令是否定义了给定寄存器。
bool definesRegister(const MachineInstr &instr, const MachineReg &reg) {
  return std::any_of(
      instr.defs.begin(), instr.defs.end(), [&](const MachineOperand &def) {
        return def.kind == OperandKind::Reg && sameRegister(def.regValue, reg);
      });
}

// 比较到分支之间的间隔指令是否都没有重定义比较的输入寄存器。若被重定义,把比较
// 下沉到分支处会读到不同的值,折叠不安全。
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

// 折叠与分支相隔若干条(不超过 kMaxCompareBranchGap)的比较。从分支往前扫,跳过
// 都是本地 CSE 候选(纯运算、可移动)的间隔指令,找到定义条件寄存器的比较后,
// 校验间隔未破坏比较输入,再折叠并删掉原比较指令。命中返回 true。
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

// 是否是可参与拷贝传播的拷贝操作码。
bool isCopyPropagationOpcode(Opcode opcode) {
  return opcode == Opcode::COPY || opcode == Opcode::F_COPY;
}

// 拷贝的源是否允许被传播到使用处:虚拟源要求与目标同寄存器类;物理源仅允许 x0
// 且目标非浮点(把 x0 传播进浮点使用不合法)。
bool copySourceAllowed(const MachineReg &dst, const MachineReg &src) {
  if (!src.physical) {
    return src.rc == dst.rc;
  }
  return src.phys == PhysReg::X0 && dst.rc != RegClass::FPR32;
}

// 指令是否是拷贝传播候选:单寄存器定义/使用的干净拷贝,目标唯一定义;若源是
// 虚拟寄存器也要求唯一定义(保证传播过去的值在使用点稳定);源类别合法。
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

// 某寄存器被重定义时清理拷贝替换表:删掉以它为键或以它为值的映射,避免继续把
// 使用改写到已过期的源。
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

// 识别一条干净的 LA rd, global(取全局符号地址),取出符号名。用于全局地址复用。
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

// 构造一条 LA rd, symbol。
MachineInstr makeLoadAddress(MachineReg dst, const std::string &symbol) {
  MachineInstr instr;
  instr.opcode = Opcode::LA;
  instr.defs.push_back(MachineOperand::reg(dst));
  instr.uses.push_back(MachineOperand::global(symbol));
  return instr;
}

// 构造一条拷贝,操作码按目标寄存器类选 F_COPY/COPY。
MachineInstr makeCopy(MachineReg dst, MachineReg src,
                      const std::string &comment = "") {
  MachineInstr instr;
  instr.opcode = dst.rc == RegClass::FPR32 ? Opcode::F_COPY : Opcode::COPY;
  instr.defs.push_back(MachineOperand::reg(dst));
  instr.uses.push_back(MachineOperand::reg(src));
  instr.comment = comment;
  return instr;
}

// 取绝对值(用于地址合并里比较偏移大小选 pivot)。
int64_t abs64(int64_t value) { return value < 0 ? -value : value; }

// 立即数是否落在有符号 12 位范围(能否直接作为访存偏移或 ADDI 立即数)。
bool fitsSigned12(int64_t value) { return value >= -2048 && value <= 2047; }

// 地址合并的分组键:相同基址寄存器 + 相同基址版本号归为一组,只有同组的候选
// 才能相互重定位(版本号保证基址在其间未被重定义)。
std::string addressCombineGroupKey(const MachineReg &base,
                                   uint64_t baseVersion) {
  return regKey(base) + "#" + std::to_string(baseVersion);
}

// 判断一个寄存器操作数是否是已知的地址常量:x0(GPR64)恒为 0;否则查由 LI/COPY
// 追踪来的常量表。用于把 ADD reg, const 识别为“基址 + 常量偏移”。
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

// 描述一个“基址 + 常量偏移”的地址计算候选:结果寄存器 reg、基址 base、偏移、
// 基址版本号、分组键、以及定义在块内的序号(用于挑选最早的 pivot)。
struct AddressCombineInfo {
  MachineReg reg;
  MachineReg base;
  int64_t offset = 0;
  uint64_t baseVersion = 0;
  std::string groupKey;
  std::size_t defIndex = 0;
};

// 识别一条把地址算成“基址 + 常量偏移”的 GPR64 定义,填充 AddressCombineInfo。
// 覆盖两种形态:ADDI base, imm;以及 ADD 中一操作数是已知常量、另一是基址寄存器。
// 排除目标与基址相同的自增(否则重定位会形成对自身的依赖)。
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

// 尝试重写一条访存指令的地址(基址 + 立即偏移),让它直接引用某个地址计算候选
// 的更早基址,从而省掉中间的地址加法。策略:
//   - 先在同组候选里找一个“pivot”(共同基址),使 (总偏移 - pivot 偏移) 仍能放进
//     12 位;偏移绝对值最小者优先,同分再按定义序号/寄存器键决胜,得到最短编码;
//   - 若无合适 pivot,则在基址版本未变且总偏移可放进 12 位时,直接折到原基址上。
// 命中改写并返回 true。
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

// 提升一组物理寄存器的版本号(两种寄存器类各一次)。调用会破坏调用者保存寄存器,
// 故调用点后需据此让相关地址候选失效。
void bumpPhysicalVersions(const std::vector<PhysReg> &regs,
                          std::map<std::string, uint64_t> &versions) {
  for (PhysReg reg : regs) {
    ++versions[regKey(MachineReg::preg(reg, RegClass::GPR64))];
    ++versions[regKey(MachineReg::preg(reg, RegClass::FPR32))];
  }
}

// 一条指令定义的寄存器被重写后:移除以它为键的地址候选与常量记录,并提升其版本
// 号(隐式定义的物理寄存器同样提升版本),使后续引用不会误用旧的地址关系。
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

// Pass:比较与分支折叠。每个块先做“相隔若干条”的折叠,再线性扫描做三段式、
// 两段式(比较+分支、零比较+分支)的相邻折叠。返回是否有改动。
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

// Pass(RA 前):全局地址复用。统计各全局被 LA 的次数,挑出复用最多的少数几个
// (上限 kMaxCachedGlobals),在入口块预取到共享寄存器,把其余同符号的 LA 改成
// 拷贝该共享寄存器。当全局种类多且单个复用极高时判定不划算而放弃(启发式)。
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

// Pass(RA 前):地址合并。逐块跟踪常量(LI/COPY)、基址版本号与“基址+常量偏移”
// 候选,把 load/store 的地址重定位到共同基址以缩短偏移或复用地址寄存器。指令定义、
// 调用(破坏调用者保存寄存器)都会相应失效候选与提升版本号。返回是否有改动。
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

// 收集每个虚拟寄存器的全部使用位置:(块下标, 指令下标) 对,按块内顺序排列。
// 供函数级拷贝传播判定“同块内位于拷贝定义之前的使用”。
std::map<uint32_t, std::vector<std::pair<std::size_t, std::size_t>>>
collectUsePositions(const MachineFunction &mf) {
  std::map<uint32_t, std::vector<std::pair<std::size_t, std::size_t>>>
      positions;
  for (std::size_t blockIndex = 0; blockIndex < mf.blocks.size();
       ++blockIndex) {
    const std::vector<MachineInstr> &instrs = mf.blocks[blockIndex].instrs;
    for (std::size_t instrIndex = 0; instrIndex < instrs.size();
         ++instrIndex) {
      for (const MachineOperand &use : instrs[instrIndex].uses) {
        if (isVirtualRegOperand(use)) {
          positions[use.regValue.id].emplace_back(blockIndex, instrIndex);
        }
      }
    }
  }
  return positions;
}

// 用标准不动点迭代计算支配集:dominators[i] 为支配块 i 的全部块(含自身)。
// 入口块为 0;不可达块只支配自己。结果仅由 CFG 前驱决定,确定性可复现。
// 支配集用位图表示(每块一个 bit),交运算为按字 AND,比 std::set 快约两个
// 数量级(大函数如 82_long_func 的 452 块场景下避免 O(B^3) 集合并集开销)。
using DomBitSet = std::vector<uint64_t>;

std::vector<DomBitSet>
computeDominators(const MachineCFGAnalysis::Result &cfg) {
  const std::size_t blockCount = cfg.preds.size();
  const std::size_t words = (blockCount + 63) / 64;
  std::vector<DomBitSet> dominators(
      blockCount, DomBitSet(words, 0));
  if (blockCount == 0) {
    return dominators;
  }
  dominators[0][0] = 1;  // 入口块只支配自己
  const uint64_t validMask =
      (blockCount % 64 == 0)
          ? ~uint64_t(0)
          : ((uint64_t(1) << (blockCount % 64)) - 1);
  for (std::size_t i = 1; i < blockCount; ++i) {
    // 其余块先假设支配所有块,再按 dom(n) = {n} ∪ ⋂ dom(pred) 迭代收缩。
    for (std::size_t w = 0; w < words; ++w) {
      dominators[i][w] = ~uint64_t(0);
    }
    dominators[i][words - 1] &= validMask;
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t i = 1; i < blockCount; ++i) {
      DomBitSet next(words, ~uint64_t(0));
      bool firstPred = true;
      for (int pred : cfg.preds[i]) {
        if (pred < 0 || pred >= static_cast<int>(blockCount)) {
          continue;
        }
        const DomBitSet &predDoms =
            dominators[static_cast<std::size_t>(pred)];
        if (firstPred) {
          next = predDoms;
          firstPred = false;
        } else {
          for (std::size_t w = 0; w < words; ++w) {
            next[w] &= predDoms[w];
          }
        }
      }
      if (firstPred) {
        std::fill(next.begin(), next.end(), uint64_t(0));
      }
      next[i / 64] |= uint64_t(1) << (i % 64);
      if (next != dominators[i]) {
        dominators[i] = std::move(next);
        changed = true;
      }
    }
  }
  return dominators;
}

// 查询块 dominator 是否支配块 dominated(位图 O(1) 判位)。
bool dominatesBlock(const std::vector<DomBitSet> &dominators,
                    std::size_t dominator, std::size_t dominated) {
  const DomBitSet &set = dominators[dominated];
  return (set[dominator / 64] >> (dominator % 64)) & 1;
}

// Pass(RA 前):函数级拷贝传播。
// 第一遍沿整个函数收集“源/目标均单定义”的干净拷贝(含链式解析),并用可达性
// 守卫把关:拷贝块必须支配目标的所有使用块(支配守卫),且同块内不存在位于拷贝
// 定义之前的使用(块内顺序守卫)。满足守卫的候选进入函数级替换表,其全部使用可
// 改写、拷贝本身可删除;不满足的维持旧块内行为(只传播块内使用、保留拷贝)。
// 第二遍逐块用“函数级表 + 块内增量”改写使用,并删除已全量改写的拷贝与恒等拷贝。
// 正确性依据:源/目标均单定义 ⇒ 源的值在整个函数内恒定不变,传播不跨越任何
// 重定义(虚拟源唯一定义;物理源仅允许 x0,永不重定义),故跨块改写安全。
bool runPreRACopyPropagation(MachineFunction &mf,
                             MachineFunctionAnalysisManager &am) {
  const std::size_t blockCount = mf.blocks.size();
  const auto useBlocks = collectUseBlocks(mf);
  const auto usePositions = collectUsePositions(mf);
  const auto defCounts = collectVirtualDefCounts(mf);
  const auto &cfg = am.getResult<MachineCFGAnalysis>(mf);
  const auto dominators = computeDominators(cfg);

  // 第一遍:构建函数级替换表 dst.id -> 解析后的源,并标记可删除的拷贝目标。
  std::map<uint32_t, MachineReg> globalReplacements;
  std::set<uint32_t> propagated;  // 已全量传播、拷贝可删除的目标寄存器
  for (std::size_t blockIndex = 0; blockIndex < blockCount; ++blockIndex) {
    const std::vector<MachineInstr> &instrs = mf.blocks[blockIndex].instrs;
    for (std::size_t instrIndex = 0; instrIndex < instrs.size();
         ++instrIndex) {
      const MachineInstr &instr = instrs[instrIndex];
      if (!isCopyPropagationCandidate(instr, defCounts)) {
        continue;
      }
      const MachineReg dst = instr.defs[0].regValue;
      const MachineReg src =
          resolveReplacement(instr.uses[0].regValue, globalReplacements);
      if (sameRegister(dst, src)) {
        // 恒等拷贝(dst == 解析后的源)直接可删,不进入替换表。
        propagated.insert(dst.id);
        continue;
      }

      // 支配守卫:拷贝块必须支配目标的所有使用块(可达性守卫)。
      bool guarded = true;
      auto uses = useBlocks.find(dst.id);
      if (uses != useBlocks.end()) {
        for (std::size_t useBlock : uses->second) {
          if (!dominatesBlock(dominators, blockIndex, useBlock)) {
            guarded = false;
            break;
          }
        }
      }
      // 块内顺序守卫:同块内位于拷贝定义之前的使用不允许改写(与旧块内语义一致)。
      if (guarded) {
        auto positions = usePositions.find(dst.id);
        if (positions != usePositions.end()) {
          for (const auto &[useBlock, useIndex] : positions->second) {
            if (useBlock == blockIndex && useIndex < instrIndex) {
              guarded = false;
              break;
            }
          }
        }
      }
      if (!guarded) {
        continue;
      }
      globalReplacements[dst.id] = src;
      propagated.insert(dst.id);
    }
  }

  // 第二遍:逐块改写使用并删除已全量改写的拷贝。
  bool changed = false;
  for (std::size_t blockIndex = 0; blockIndex < blockCount; ++blockIndex) {
    MachineBasicBlock &block = mf.blocks[blockIndex];
    std::map<uint32_t, MachineReg> replacements = globalReplacements;
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
        if (sameRegister(dst, src) || propagated.count(dst.id) != 0) {
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

// Pass(RA 前):块内公共子表达式消除。逐块维护表达式表与 load 表及其依赖索引:
//   - 调用或非 store 副作用指令整体清表;store 只清 load 表(内存变了但 ALU 表达式
//     的虚拟/x0 操作数不受影响);
//   - 每条指令先按替换表改写使用,再让其定义失效相关表项;
//   - 命中相同键且该定义使用全在本块内时,记录替换并删除本条(用已有结果代替);
//   - 表满(kMaxLocalCSEEntries)时不再新增。返回是否有改动。
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

// Pass(RA 后):最终窥孔清理。删除可移除指令(NOP/自拷贝/自加零),并对零元恒等式
// 做折叠;折叠结果若又成了可删除指令则一并丢弃。返回是否有改动。
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
