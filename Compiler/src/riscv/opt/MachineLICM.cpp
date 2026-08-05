#include "riscv/opt/MachineOptPasses.hpp"

#include "riscv/Passes.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace riscv {
// Machine LICM（机器层循环不变量外提）pass 的公共实现。
//
// 作用：把循环内"每轮迭代计算值相同"且可安全推测执行的机器指令移动到循环
// 入口之前的 preheader，使这些计算每轮循环只执行一次。中端 IR LICM 已处理
// 值级与内存证明级的外提；本 pass 处理机器形态残余：LI/LA 逐 use 物化、机器层
// 地址链（ADDI/ADD/COPY）、机器化简后新出现的不变值，以及少量可证安全的
// 循环不变 load（T1a/T2 分层证明）。
//
// 候选白名单：目标无关纯计算（LI/LA/COPY/FMV/算术/逻辑/移位/比较/浮点/FCVT/
// GEP）+ load（LBU/LW/LD/FLW，走内存安全分层）；默认禁 DIV/REM 族（零 trip
// 除零 trap 风险，BACKEND_MACHINE_LICM_TRAP_OPS 且除数可证非零才放行）；
// store/call/terminator/隐式寄存器指令一律不外提。
//
// A/B 开关：BACKEND_DISABLE_PASSES=pre-ra-machine-licm（或 all）。
// 细粒度环境变量（默认值见各开关）：
//   BACKEND_MACHINE_LICM_STATS=1           打印命中统计到 stderr
//   BACKEND_MACHINE_LICM_NO_LOADS=1        关闭全部 load 外提
//   BACKEND_MACHINE_LICM_SPECULATE_LOADS=1 开启 T1b 可推测 load（默认关）
//   BACKEND_MACHINE_LICM_TRAP_OPS=1        开启除法规（需除数非零证明）
//   BACKEND_MACHINE_LICM_ROUNDS=N          整 pass 重复轮数（默认 1）
//   BACKEND_MACHINE_LICM_MAX_HOISTS=N      每函数外提上限（默认 64）
//   BACKEND_MACHINE_LICM_MIN_PROFIT=N      收益阈值（默认 1024，频率定点刻度）
//   BACKEND_MACHINE_LICM_PRESSURE=N        循环级压力守卫（默认 20）
//   BACKEND_MACHINE_LICM_MAX_LOOP_BLOCKS=N 循环体块数上限（默认 64）
//
// RISC-V 与 ARM64 两后端使用相同实现；两后端各保留一份相同实现。

namespace {

// ==== 环境变量开关 ====

bool envFlag(const char *name) { return std::getenv(name) != nullptr; }

int envInt(const char *name, int defaultValue) {
  const char *raw = std::getenv(name);
  if (raw == nullptr) {
    return defaultValue;
  }
  return std::atoi(raw);
}

struct Options {
  bool loadsEnabled = true;
  bool specLoads = false;
  bool trapOps = false;
  int rounds = 1;
  int maxHoists = 64;
  uint64_t minProfit = 1024;
  int pressure = 20;
  int maxLoopBlocks = 64;
  bool stats = false;
  bool debug = false;
};

Options readOptions() {
  Options opts;
  opts.loadsEnabled = !envFlag("BACKEND_MACHINE_LICM_NO_LOADS");
  opts.specLoads = envFlag("BACKEND_MACHINE_LICM_SPECULATE_LOADS");
  opts.trapOps = envFlag("BACKEND_MACHINE_LICM_TRAP_OPS");
  opts.rounds = envInt("BACKEND_MACHINE_LICM_ROUNDS", 1);
  opts.maxHoists = envInt("BACKEND_MACHINE_LICM_MAX_HOISTS", 64);
  opts.minProfit =
      static_cast<uint64_t>(envInt("BACKEND_MACHINE_LICM_MIN_PROFIT", 1024));
  opts.pressure = envInt("BACKEND_MACHINE_LICM_PRESSURE", 20);
  opts.maxLoopBlocks = envInt("BACKEND_MACHINE_LICM_MAX_LOOP_BLOCKS", 64);
  opts.stats = envFlag("BACKEND_MACHINE_LICM_STATS");
  opts.debug = envFlag("BACKEND_MACHINE_LICM_DEBUG");
  return opts;
}

// ==== 操作数与操作码分类 ====

bool isVirtualRegister(const MachineOperand &operand) {
  return operand.kind == OperandKind::Reg && !operand.regValue.physical;
}

bool isTerminatorOpcode(Opcode opcode) {
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

bool isLoadOpcode(Opcode opcode) {
  return opcode == Opcode::LBU || opcode == Opcode::LW ||
         opcode == Opcode::LD || opcode == Opcode::FLW;
}

bool isStoreOpcode(Opcode opcode) {
  return opcode == Opcode::SB || opcode == Opcode::SW ||
         opcode == Opcode::SD || opcode == Opcode::FSW;
}

// load 的字节宽度，用于在界证明。
uint64_t loadWidthBytes(Opcode opcode) {
  switch (opcode) {
  case Opcode::LBU:
    return 1;
  case Opcode::LD:
    return 8;
  default:
    return 4; // LW/FLW
  }
}

// 除法规（可能因除零 trap），默认禁外提。
bool isTrapOpcode(Opcode opcode) {
  switch (opcode) {
  case Opcode::DIV:
  case Opcode::DIVW:
  case Opcode::DIVUW:
  case Opcode::REM:
  case Opcode::REMW:
  case Opcode::REMUW:
    return true;
  default:
    return false;
  }
}

// 目标无关纯计算白名单（与 isSpeculatablePreheaderInstruction 同构，含 GEP）。
bool isPureWhiteListedOpcode(Opcode opcode) {
  switch (opcode) {
  case Opcode::COPY:
  case Opcode::F_COPY:
  case Opcode::FMV_W_X:
  case Opcode::FMV_X_W:
  case Opcode::LI:
  case Opcode::LA:
  case Opcode::ADD:
  case Opcode::ADDW:
  case Opcode::ADDI:
  case Opcode::ADDIW:
  case Opcode::SUB:
  case Opcode::SUBW:
  case Opcode::MUL:
  case Opcode::MULW:
  case Opcode::AND:
  case Opcode::OR:
  case Opcode::XOR:
  case Opcode::ANDI:
  case Opcode::ORI:
  case Opcode::XORI:
  case Opcode::SLL:
  case Opcode::SLLW:
  case Opcode::SRAW:
  case Opcode::SRLW:
  case Opcode::SLLI:
  case Opcode::SLLIW:
  case Opcode::SRAI:
  case Opcode::SRAIW:
  case Opcode::SRLIW:
  case Opcode::SLT:
  case Opcode::SLTU:
  case Opcode::SEQZ:
  case Opcode::SNEZ:
  case Opcode::FEQ_S:
  case Opcode::FLT_S:
  case Opcode::FLE_S:
  case Opcode::FADD_S:
  case Opcode::FSUB_S:
  case Opcode::FMUL_S:
  case Opcode::FDIV_S:
  case Opcode::FCVT_W_S:
  case Opcode::FCVT_S_W:
  case Opcode::GEP:
    return true;
  default:
    return false;
  }
}

// 各操作码的近似执行开销权重（load 单独按 3 计）。
uint32_t instructionCost(Opcode opcode) {
  switch (opcode) {
  case Opcode::FDIV_S:
    return 16;
  case Opcode::MUL:
  case Opcode::MULW:
  case Opcode::FMUL_S:
    return 3;
  case Opcode::LI:
    return 2;
  default:
    return 1;
  }
}

// 统计用的操作码名字（白名单 + load + 除法规）。
const char *opcodeName(Opcode opcode) {
  switch (opcode) {
  case Opcode::COPY: return "COPY";
  case Opcode::F_COPY: return "F_COPY";
  case Opcode::FMV_W_X: return "FMV_W_X";
  case Opcode::FMV_X_W: return "FMV_X_W";
  case Opcode::LI: return "LI";
  case Opcode::LA: return "LA";
  case Opcode::ADD: return "ADD";
  case Opcode::ADDW: return "ADDW";
  case Opcode::ADDI: return "ADDI";
  case Opcode::ADDIW: return "ADDIW";
  case Opcode::SUB: return "SUB";
  case Opcode::SUBW: return "SUBW";
  case Opcode::MUL: return "MUL";
  case Opcode::MULW: return "MULW";
  case Opcode::AND: return "AND";
  case Opcode::OR: return "OR";
  case Opcode::XOR: return "XOR";
  case Opcode::ANDI: return "ANDI";
  case Opcode::ORI: return "ORI";
  case Opcode::XORI: return "XORI";
  case Opcode::SLL: return "SLL";
  case Opcode::SLLW: return "SLLW";
  case Opcode::SRAW: return "SRAW";
  case Opcode::SRLW: return "SRLW";
  case Opcode::SLLI: return "SLLI";
  case Opcode::SLLIW: return "SLLIW";
  case Opcode::SRAI: return "SRAI";
  case Opcode::SRAIW: return "SRAIW";
  case Opcode::SRLIW: return "SRLIW";
  case Opcode::SLT: return "SLT";
  case Opcode::SLTU: return "SLTU";
  case Opcode::SEQZ: return "SEQZ";
  case Opcode::SNEZ: return "SNEZ";
  case Opcode::FEQ_S: return "FEQ_S";
  case Opcode::FLT_S: return "FLT_S";
  case Opcode::FLE_S: return "FLE_S";
  case Opcode::FADD_S: return "FADD_S";
  case Opcode::FSUB_S: return "FSUB_S";
  case Opcode::FMUL_S: return "FMUL_S";
  case Opcode::FDIV_S: return "FDIV_S";
  case Opcode::FCVT_W_S: return "FCVT_W_S";
  case Opcode::FCVT_S_W: return "FCVT_S_W";
  case Opcode::GEP: return "GEP";
  case Opcode::LBU: return "LBU";
  case Opcode::LW: return "LW";
  case Opcode::LD: return "LD";
  case Opcode::FLW: return "FLW";
  case Opcode::DIV: return "DIV";
  case Opcode::DIVW: return "DIVW";
  case Opcode::DIVUW: return "DIVUW";
  case Opcode::REM: return "REM";
  case Opcode::REMW: return "REMW";
  case Opcode::REMUW: return "REMUW";
  default: return "?";
  }
}

// 饱和整数运算，避免收益计算溢出回绕。
uint64_t saturatingMul(uint64_t lhs, uint64_t rhs) {
  const uint64_t limit = std::numeric_limits<uint64_t>::max();
  if (lhs == 0 || rhs == 0) {
    return 0;
  }
  if (lhs > limit / rhs) {
    return limit;
  }
  return lhs * rhs;
}

uint64_t saturatingAdd(uint64_t lhs, uint64_t rhs) {
  const uint64_t limit = std::numeric_limits<uint64_t>::max();
  return rhs > limit - lhs ? limit : lhs + rhs;
}

// ==== 函数级静态图（单次分析阶段内不变） ====

using DefCounts = std::map<uint32_t, std::size_t>;
using DefLocations = std::map<uint32_t, std::pair<int, std::size_t>>;
using UseLocations =
    std::map<uint32_t, std::vector<std::pair<int, std::size_t>>>;

struct FunctionSnapshot {
  DefCounts defCounts;
  DefLocations defLocations;
  UseLocations uses;
};

FunctionSnapshot buildSnapshot(const MachineFunction &mf) {
  FunctionSnapshot snap;
  for (std::size_t block = 0; block < mf.blocks.size(); ++block) {
    const MachineBasicBlock &b = mf.blocks[block];
    for (std::size_t index = 0; index < b.instrs.size(); ++index) {
      const MachineInstr &instr = b.instrs[index];
      for (const MachineOperand &def : instr.defs) {
        if (isVirtualRegister(def)) {
          ++snap.defCounts[def.regValue.id];
          snap.defLocations[def.regValue.id] =
              std::make_pair(static_cast<int>(block), index);
        }
      }
      for (const MachineOperand &use : instr.uses) {
        if (isVirtualRegister(use)) {
          snap.uses[use.regValue.id].emplace_back(
              static_cast<int>(block), index);
        }
      }
    }
  }
  return snap;
}

// 迭代不动点支配集（由 CFG 分析结果计算，模式复用既有 pass）。
using DominatorSets = std::vector<std::set<int>>;

DominatorSets computeDominators(const MachineCFGAnalysis::Result &cfg) {
  const std::size_t count = cfg.succs.size();
  DominatorSets dominators(count);
  if (count == 0) {
    return dominators;
  }
  std::vector<bool> reachable(count, false);
  std::vector<int> work{0};
  reachable[0] = true;
  while (!work.empty()) {
    const int block = work.back();
    work.pop_back();
    for (int succ : cfg.succs[static_cast<std::size_t>(block)]) {
      if (succ >= 0 && succ < static_cast<int>(count) &&
          !reachable[static_cast<std::size_t>(succ)]) {
        reachable[static_cast<std::size_t>(succ)] = true;
        work.push_back(succ);
      }
    }
  }
  std::set<int> allReachable;
  for (std::size_t block = 0; block < count; ++block) {
    if (reachable[block]) {
      allReachable.insert(static_cast<int>(block));
    }
  }
  dominators[0].insert(0);
  for (std::size_t block = 1; block < count; ++block) {
    if (reachable[block]) {
      dominators[block] = allReachable;
    }
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t block = 1; block < count; ++block) {
      if (!reachable[block]) {
        continue;
      }
      std::set<int> next;
      bool first = true;
      for (int pred : cfg.preds[block]) {
        if (pred < 0 || pred >= static_cast<int>(count) ||
            !reachable[static_cast<std::size_t>(pred)]) {
          continue;
        }
        if (first) {
          next = dominators[static_cast<std::size_t>(pred)];
          first = false;
        } else {
          std::set<int> intersection;
          std::set_intersection(
              next.begin(), next.end(),
              dominators[static_cast<std::size_t>(pred)].begin(),
              dominators[static_cast<std::size_t>(pred)].end(),
              std::inserter(intersection, intersection.begin()));
          next = std::move(intersection);
        }
      }
      next.insert(static_cast<int>(block));
      if (next != dominators[block]) {
        dominators[block] = std::move(next);
        changed = true;
      }
    }
  }
  return dominators;
}

// ==== load 地址根解析与在界证明 ====

enum class RootKind { Unknown, Global, FrameIndex };

struct AddressRoot {
  RootKind kind = RootKind::Unknown;
  std::string symbol;
  int frameIndex = -1;
  int64_t offset = 0;
};

// 由基址操作数出发，沿 COPY/LA/ADDI 链（深度 ≤ 8，遇未知即 Unknown）解析
// 地址根：Global 符号或 FrameIndex 栈槽 + 累计立即偏移。SysY 无指针运算，
// 地址链形态有限，其余形态一律 Unknown（保守）。
AddressRoot resolveAddressRoot(const MachineFunction &mf,
                               const FunctionSnapshot &snap,
                               const MachineOperand &base, int depth) {
  if (base.kind == OperandKind::FrameIndex) {
    AddressRoot root;
    root.kind = RootKind::FrameIndex;
    root.frameIndex = base.frameIndexValue;
    return root;
  }
  if (!isVirtualRegister(base) || depth > 8) {
    return AddressRoot{};
  }
  auto count = snap.defCounts.find(base.regValue.id);
  if (count == snap.defCounts.end() || count->second != 1) {
    return AddressRoot{};
  }
  auto defIt = snap.defLocations.find(base.regValue.id);
  if (defIt == snap.defLocations.end()) {
    return AddressRoot{};
  }
  const MachineInstr &def =
      mf.blocks[static_cast<std::size_t>(defIt->second.first)]
          .instrs[defIt->second.second];
  switch (def.opcode) {
  case Opcode::COPY: {
    if (def.uses.size() != 1 || !isVirtualRegister(def.uses[0])) {
      return AddressRoot{};
    }
    return resolveAddressRoot(mf, snap, def.uses[0], depth + 1);
  }
  case Opcode::LA: {
    if (def.uses.size() != 1 || def.uses[0].kind != OperandKind::Global) {
      return AddressRoot{};
    }
    AddressRoot root;
    root.kind = RootKind::Global;
    root.symbol = def.uses[0].symbol;
    return root;
  }
  case Opcode::ADDI:
  case Opcode::ADDIW: {
    if (def.uses.size() != 2 || !isVirtualRegister(def.uses[0]) ||
        def.uses[1].kind != OperandKind::Imm) {
      return AddressRoot{};
    }
    AddressRoot root = resolveAddressRoot(mf, snap, def.uses[0], depth + 1);
    if (root.kind != RootKind::Unknown) {
      root.offset += def.uses[1].immValue;
    }
    return root;
  }
  default:
    return AddressRoot{};
  }
}

// 地址根是否可证在界：Global 查模块快照尺寸、FrameIndex 查栈对象尺寸；
// 要求 0 ≤ offset 且 offset + 读宽度 ≤ 对象尺寸。
bool rootInBounds(const MachineFunction &mf, const AddressRoot &root,
                  uint64_t width) {
  if (root.kind == RootKind::Unknown || root.offset < 0) {
    return false;
  }
  const uint64_t offset = static_cast<uint64_t>(root.offset);
  if (root.kind == RootKind::Global) {
    auto global = mf.globals.find(root.symbol);
    if (global == mf.globals.end()) {
      return false;
    }
    return global->second.size > 0 &&
           offset <= static_cast<uint64_t>(global->second.size) &&
           width <= static_cast<uint64_t>(global->second.size) - offset;
  }
  for (const StackObject &object : mf.stackObjects) {
    if (object.index == root.frameIndex) {
      return object.size > 0 && offset <= static_cast<uint64_t>(object.size) &&
             width <= static_cast<uint64_t>(object.size) - offset;
    }
  }
  return false;
}

// 除法规的除数是否可证非零：立即数非零，或唯一 def 为 LI 非零。
bool divisorProvablyNonZero(const MachineFunction &mf,
                            const FunctionSnapshot &snap,
                            const MachineInstr &instr) {
  if (instr.uses.size() < 2) {
    return false;
  }
  const MachineOperand &divisor = instr.uses[1];
  if (divisor.kind == OperandKind::Imm) {
    return divisor.immValue != 0;
  }
  if (!isVirtualRegister(divisor)) {
    return false;
  }
  auto count = snap.defCounts.find(divisor.regValue.id);
  if (count == snap.defCounts.end() || count->second != 1) {
    return false;
  }
  auto defIt = snap.defLocations.find(divisor.regValue.id);
  if (defIt == snap.defLocations.end()) {
    return false;
  }
  const MachineInstr &def =
      mf.blocks[static_cast<std::size_t>(defIt->second.first)]
          .instrs[defIt->second.second];
  return def.opcode == Opcode::LI && def.uses.size() == 1 &&
         def.uses[0].kind == OperandKind::Imm && def.uses[0].immValue != 0;
}

// ==== 循环级数据结构 ====

// 一个候选：满足结构检查的循环内指令（白名单纯计算 或 load）。
struct Candidate {
  int block = -1;
  std::size_t instrIdx = 0;
  Opcode opcode = Opcode::NOP;
  uint32_t defReg = 0;
  uint64_t profit = 0;
  bool marked = false;   // 通过不变性 + 收益门槛（最终外提集）
  bool removed = false;  // Phase 2 实际从原块移除
  int loadTier = 0;      // 0=非 load；1=T1a；2=T1b；3=T2
  std::set<int> deps;    // 依赖的已标记候选（候选收集序下标）
};

// 一个循环的全部分析数据（Phase 1 只读使用，不做任何改写）。
struct LoopData {
  int header = -1;
  std::string headerLabel;
  int preheader = -1;          // 现有 preheader 块下标；-1 表示需边分裂创建
  bool needEdgeSplit = false;
  std::vector<int> outsidePreds;
  std::set<int> members;
  std::map<uint32_t, int> defsInLoop;         // 循环内定义次数
  std::vector<const MachineInstr *> stores;   // 循环内 store（Phase 1 有效）
  bool hasCall = false;
  bool hasMemcpy = false;
  std::vector<Candidate> candidates;
  std::vector<int> topoOrder;  // 外提插入序（候选收集序下标）
  int orderIndex = -1;         // 处理序（Phase 2 用）
};

// 一个待创建的 preheader（边分裂）。
struct Creation {
  int insertPos = -1;
  std::vector<int> outsidePreds;
  std::string headerLabel;
  std::string preheaderLabel;
};

// ==== 收益模型 ====

// 块热度：频率模型不可靠时回退的深度热度（RegAlloc 同款 10^min(depth,3)）。
uint64_t blockHeat(const MachineLoopAnalysis::Result &loops, int block) {
  const int depth =
      block >= 0 && block < static_cast<int>(loops.depth.size())
          ? loops.depth[static_cast<std::size_t>(block)]
          : 0;
  uint64_t heat = 1;
  for (int i = 0; i < std::min(depth, 3); ++i) {
    heat *= 10;
  }
  return heat;
}

uint64_t blockFrequencyValue(const MachineBlockFrequencyAnalysis::Result &freq,
                             const MachineLoopAnalysis::Result &loops,
                             int block) {
  if (freq.reliable) {
    if (block >= 0 && block < static_cast<int>(freq.blockFrequency.size())) {
      return freq.blockFrequency[static_cast<std::size_t>(block)];
    }
    return 0;
  }
  return blockHeat(loops, block);
}

// 计算候选收益：Profit = Σ_use cost × (freq(useBlock) − freq(preheader))；
// def 存在循环外 use 时整体打 5 折（跨循环活跃区间代价）。
// 边分裂循环（preheader 尚不存在）以 header 频率作保守代理。
uint64_t computeProfit(uint32_t defReg, const FunctionSnapshot &snap,
                       const LoopData &loop,
                       const MachineBlockFrequencyAnalysis::Result &freq,
                       const MachineLoopAnalysis::Result &loops,
                       uint32_t cost, uint32_t &inLoopUses) {
  const uint64_t preFreq = blockFrequencyValue(
      freq, loops, loop.preheader >= 0 ? loop.preheader : loop.header);
  uint64_t profit = 0;
  inLoopUses = 0;
  bool outsideUse = false;
  auto usesIt = snap.uses.find(defReg);
  if (usesIt != snap.uses.end()) {
    for (const auto &[block, index] : usesIt->second) {
      (void)index;
      if (loop.members.count(block) != 0) {
        const uint64_t useFreq = blockFrequencyValue(freq, loops, block);
        const uint64_t delta = useFreq > preFreq ? useFreq - preFreq : 0;
        profit = saturatingAdd(profit, saturatingMul(cost, delta));
        ++inLoopUses;
      } else {
        outsideUse = true;
      }
    }
  }
  if (outsideUse) {
    profit /= 2;
  }
  return profit;
}

// ==== preheader 判定与边分裂 ====

// 把块内指向 oldLabel 的分支目标改写为 newLabel。
void rewriteBlockTargets(MachineBasicBlock &block, const std::string &oldLabel,
                         const std::string &newLabel) {
  for (MachineInstr &instr : block.instrs) {
    for (MachineOperand &use : instr.uses) {
      if (use.kind == OperandKind::Block && use.blockLabel == oldLabel) {
        use.blockLabel = newLabel;
      }
    }
  }
}

// 为新 preheader 生成函数内唯一标签（仿 uniqueComputeLabel 模式）。
std::string uniquePreheaderLabel(const MachineFunction &mf,
                                 const std::string &headerLabel) {
  const std::string base = headerLabel + ".licm.preheader";
  std::string candidate = base;
  unsigned suffix = 0;
  auto used = [&](const std::string &label) {
    return std::any_of(
        mf.blocks.begin(), mf.blocks.end(),
        [&](const MachineBasicBlock &block) { return block.label == label; });
  };
  while (used(candidate)) {
    candidate = base + "." + std::to_string(++suffix);
  }
  return candidate;
}

// 判定 header 的 preheader：唯一的循环外前驱即 preheader；多个循环外前驱需
// 边分裂创建（新块插在 header 原位置，所有循环外前驱的显式目标改指新块，
// 落穿边由相邻性自动落入新块）；无循环外前驱（header 即 entry）跳过该循环。
bool determinePreheader(LoopData &loop, const MachineCFGAnalysis::Result &cfg) {
  loop.outsidePreds.clear();
  for (int pred : cfg.preds[static_cast<std::size_t>(loop.header)]) {
    if (loop.members.count(pred) == 0) {
      loop.outsidePreds.push_back(pred);
    }
  }
  if (loop.outsidePreds.empty()) {
    return false; // header == entry（或不可达）：跳过
  }
  if (loop.outsidePreds.size() == 1) {
    loop.preheader = loop.outsidePreds.front();
    return true;
  }
  loop.needEdgeSplit = true;
  loop.preheader = -1;
  return true;
}

// ==== 候选收集 ====

// def 的块是否支配所有 use 块（同块 use 还需在 def 之后）。这是当前程序
// 形态下的合法性防御检查：外提后 preheader 支配 header 支配循环内所有块，
// 支配关系自动保持。
bool defDominatesUses(const FunctionSnapshot &snap,
                      const DominatorSets &dominators, int defBlock,
                      std::size_t defIdx, uint32_t defReg) {
  auto usesIt = snap.uses.find(defReg);
  if (usesIt == snap.uses.end()) {
    return true;
  }
  for (const auto &[block, index] : usesIt->second) {
    if (block == defBlock) {
      if (index <= defIdx) {
        return false; // 同块 use 在 def 之前（非 SSA 边角，拒绝）
      }
    } else if (dominators[static_cast<std::size_t>(block)].count(defBlock) ==
               0) {
      return false;
    }
  }
  return true;
}

// load 的内存安全分层：
//   T1a：循环无 store 无 call，地址可证在界（零 trip 安全）；
//   T2 ：循环有 store，地址可证在界 + 与循环内每个 store 根不相交；
//   T1b：循环无 store 无 call，地址不可证在界，但 load 块支配全部 latch
//        （循环内无条件执行），接受零 trip 残余风险——默认关。
// 循环内有 call/MEMCPY/MEMSET 一律阻断 load 外提。
int computeLoadTier(const MachineFunction &mf, const FunctionSnapshot &snap,
                    const LoopData &loop, const DominatorSets &dominators,
                    const MachineInstr &load, int loadBlock,
                    const Options &opts, const std::vector<int> &latches) {
  if (loop.hasCall || loop.hasMemcpy) {
    return 0; // 循环内有 call/MEMCPY/MEMSET：一律阻断 load
  }
  const AddressRoot root = resolveAddressRoot(mf, snap, load.uses[0], 0);
  const bool inBounds = rootInBounds(mf, root, loadWidthBytes(load.opcode));
  if (loop.stores.empty()) {
    if (inBounds) {
      return 1; // T1a
    }
    if (opts.specLoads && !latches.empty()) {
      const bool dominatesAllLatches = std::all_of(
          latches.begin(), latches.end(), [&](int latch) {
            return dominators[static_cast<std::size_t>(latch)]
                .count(loadBlock) != 0;
          });
      if (dominatesAllLatches) {
        return 2; // T1b
      }
    }
    return 0;
  }
  // T2：地址可证在界 + 根不相交（零 trip 安全同样要求在界）。
  if (root.kind == RootKind::Unknown || !inBounds) {
    return 0;
  }
  for (const MachineInstr *store : loop.stores) {
    if (store->uses.size() < 2) {
      return 0;
    }
    const AddressRoot storeRoot =
        resolveAddressRoot(mf, snap, store->uses[1], 0);
    if (storeRoot.kind == RootKind::Unknown) {
      return 0; // 参数派生或解析失败：无法证明不相交
    }
    bool sameRoot = false;
    if (root.kind == RootKind::Global && storeRoot.kind == RootKind::Global &&
        root.symbol == storeRoot.symbol) {
      sameRoot = true;
    } else if (root.kind == RootKind::FrameIndex &&
               storeRoot.kind == RootKind::FrameIndex &&
               root.frameIndex == storeRoot.frameIndex) {
      sameRoot = true;
    }
    if (sameRoot) {
      // load 根为 const 全局时同根 store 不存在（const 不可写），放行。
      if (root.kind == RootKind::Global) {
        auto global = mf.globals.find(root.symbol);
        if (global != mf.globals.end() && global->second.constant) {
          continue;
        }
      }
      return 0;
    }
  }
  return 3; // T2
}

// 收集循环内全部候选（结构 + 白名单/trap 守卫 + 单赋值 + 支配 + load 分层）。
void collectCandidates(const MachineFunction &mf, const FunctionSnapshot &snap,
                       const DominatorSets &dominators, LoopData &loop,
                       const Options &opts, const std::vector<int> &latches,
                       int &loadBlocked) {
  for (int block : loop.members) {
    const MachineBasicBlock &b = mf.blocks[static_cast<std::size_t>(block)];
    for (std::size_t index = 0; index < b.instrs.size(); ++index) {
      const MachineInstr &instr = b.instrs[index];
      if (instr.sideEffects || instr.terminator ||
          !instr.implicitDefs.empty() || !instr.implicitUses.empty()) {
        continue;
      }
      if (instr.defs.size() != 1 || !isVirtualRegister(instr.defs[0])) {
        continue;
      }
      const Opcode opcode = instr.opcode;
      const bool isLoad = isLoadOpcode(opcode);
      bool pure = isPureWhiteListedOpcode(opcode);
      if (isTrapOpcode(opcode)) {
        // 除法规默认禁；开启时还需除数可证非零。
        pure = opts.trapOps && divisorProvablyNonZero(mf, snap, instr);
      }
      if (!pure && !isLoad) {
        continue;
      }
      if (isLoad && !opts.loadsEnabled) {
        continue;
      }
      // uses 仅允许 Reg(vreg)/Imm/Global/FrameIndex；禁物理寄存器。
      bool usesOk = true;
      for (const MachineOperand &use : instr.uses) {
        if (use.kind == OperandKind::Reg && use.regValue.physical) {
          usesOk = false;
          break;
        }
        if (use.kind != OperandKind::Reg && use.kind != OperandKind::Imm &&
            use.kind != OperandKind::Global &&
            use.kind != OperandKind::FrameIndex) {
          usesOk = false;
          break;
        }
      }
      if (!usesOk) {
        continue;
      }
      if (isLoad) {
        // load 形态：基址(Reg/FrameIndex) + 立即偏移。
        if (instr.uses.size() != 2 ||
            instr.uses[1].kind != OperandKind::Imm ||
            (instr.uses[0].kind != OperandKind::FrameIndex &&
             !isVirtualRegister(instr.uses[0]))) {
          continue;
        }
      }
      const uint32_t defReg = instr.defs[0].regValue.id;
      auto count = snap.defCounts.find(defReg);
      if (count == snap.defCounts.end() || count->second != 1) {
        continue; // 单赋值：PHI 消除后的非 SSA 拷贝不在候选内
      }
      if (!defDominatesUses(snap, dominators, block, index, defReg)) {
        continue;
      }
      int tier = 0;
      if (isLoad) {
        tier = computeLoadTier(mf, snap, loop, dominators, instr, block, opts,
                               latches);
        if (opts.debug) {
          const AddressRoot dbgRoot =
              resolveAddressRoot(mf, snap, instr.uses[0], 0);
          std::cerr << "# licm-debug     load op=" << opcodeName(instr.opcode)
                    << " block=" << block << " idx=" << index
                    << " tier=" << tier
                    << " root=" << (dbgRoot.kind == RootKind::Global
                                        ? dbgRoot.symbol
                                        : (dbgRoot.kind == RootKind::FrameIndex
                                               ? std::to_string(dbgRoot.frameIndex)
                                               : "unknown"))
                    << " off=" << dbgRoot.offset
                    << " inBounds=" << rootInBounds(
                           mf, dbgRoot, loadWidthBytes(instr.opcode))
                    << " stores=" << loop.stores.size()
                    << " call=" << loop.hasCall << "\n";
        }
        if (tier == 0) {
          ++loadBlocked;
          continue;
        }
      }
      Candidate candidate;
      candidate.block = block;
      candidate.instrIdx = index;
      candidate.opcode = opcode;
      candidate.defReg = defReg;
      candidate.loadTier = tier;
      loop.candidates.push_back(std::move(candidate));
    }
  }
}

// ==== 不变性 fixpoint + 收益门槛 ====

// 收益门槛：profit ≥ minProfit 且（cost ≥ 2 或 循环内 use 数 ≥ 2 或
// 循环内唯一 use 属于被外提链）。链式依赖（如地址链 SLLI→ADD→ADD）逐链节
// 只有 1 个循环内 use，单节按廉价规则会被拒；若整链一起外提，则除链尾外每节
// 的 use 也随之离开循环，无跨循环活跃区间代价——这是"不单独外提"的链式例外。
bool passesProfitGate(uint64_t profit, uint32_t cost, uint32_t inLoopUses,
                      bool chainUse, const Options &opts) {
  if (profit < opts.minProfit) {
    return false;
  }
  if (cost >= 2 || inLoopUses >= 2) {
    return true;
  }
  return inLoopUses == 1 && chainUse;
}

// 标记候选：以"循环内无 def 的 vreg ∪ 已标记候选的 def"为不变集合做动态
// 闭包（依赖链单轮闭合）；每个候选需过收益门槛才标记，门槛不过的 def 不进
// 集合（否则后续候选会依赖一个实际未外提的值，造成 use-before-def）。
// eligible 非空时只允许其中候选被标记（预算裁剪后的重跑）。
void markHoistable(const MachineFunction &mf, const FunctionSnapshot &snap,
                   LoopData &loop,
                   const MachineBlockFrequencyAnalysis::Result &freq,
                   const MachineLoopAnalysis::Result &loops,
                   const Options &opts, const std::set<int> *eligible) {
  std::set<uint32_t> hoistedDefs;
  // chainEligible 保存全部已标记候选的 def（每个 vreg 单赋值，可反查指令）。
  // 链式例外：廉价单用候选的唯一循环内 use 落在已标记候选上时放行——
  // 链节随整链外提，use 离开循环，无跨循环活跃区间代价。
  std::set<uint32_t> chainEligible;
  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t index = 0; index < loop.candidates.size(); ++index) {
      Candidate &c = loop.candidates[index];
      if (c.marked) {
        continue;
      }
      if (eligible != nullptr &&
          eligible->count(static_cast<int>(index)) == 0) {
        continue;
      }
      const MachineInstr &instr =
          mf.blocks[static_cast<std::size_t>(c.block)].instrs[c.instrIdx];
      // 不变性：全部 vreg use 在循环内无 def，或由已标记候选定义。
      bool invariant = true;
      for (const MachineOperand &use : instr.uses) {
        if (!isVirtualRegister(use)) {
          continue;
        }
        const uint32_t reg = use.regValue.id;
        auto defIt = loop.defsInLoop.find(reg);
        if (defIt != loop.defsInLoop.end() && hoistedDefs.count(reg) == 0) {
          if (opts.debug) {
            std::cerr << "# licm-debug     reject op=" << opcodeName(c.opcode)
                      << " block=" << c.block << " idx=" << c.instrIdx
                      << " useReg=" << reg
                      << " defsInLoop=" << defIt->second << "\n";
          }
          invariant = false;
          break;
        }
      }
      if (!invariant) {
        continue;
      }
      const uint32_t cost =
          isLoadOpcode(c.opcode) ? 3 : instructionCost(c.opcode);
      uint32_t inLoopUses = 0;
      const uint64_t profit = computeProfit(c.defReg, snap, loop, freq, loops,
                                            cost, inLoopUses);
      // 链式例外：唯一循环内 use 所在指令本身是已标记候选（其 def 在
      // chainEligible 中，每个 vreg 单赋值保证该 def 唯一对应那条指令）。
      bool chainUse = false;
      if (inLoopUses == 1) {
        auto usesIt = snap.uses.find(c.defReg);
        if (usesIt != snap.uses.end()) {
          for (const auto &[block, index] : usesIt->second) {
            if (loop.members.count(block) == 0) {
              continue;
            }
            const MachineInstr &useInstr =
                mf.blocks[static_cast<std::size_t>(block)].instrs[index];
            for (const MachineOperand &def : useInstr.defs) {
              if (isVirtualRegister(def) &&
                  chainEligible.count(def.regValue.id) != 0) {
                chainUse = true;
              }
            }
          }
        }
      }
      if (!passesProfitGate(profit, cost, inLoopUses, chainUse, opts)) {
        continue; // 收益不足（统计见 countProfitSkips）
      }
      c.marked = true;
      c.profit = profit;
      hoistedDefs.insert(c.defReg);
      chainEligible.insert(c.defReg);
      changed = true;
    }
  }
}

// 收益跳过统计：对"通过不变性但收益不足"的候选计数（统计口径，不改标记；
// 链式例外无法在此精确复算，按非链口径保守计数）。
void countProfitSkips(const MachineFunction &mf, const FunctionSnapshot &snap,
                      const LoopData &loop,
                      const MachineBlockFrequencyAnalysis::Result &freq,
                      const MachineLoopAnalysis::Result &loops,
                      const Options &opts, int &profitSkips) {
  std::set<uint32_t> hoistedDefs;
  for (const Candidate &c : loop.candidates) {
    if (c.marked) {
      hoistedDefs.insert(c.defReg);
    }
  }
  for (const Candidate &c : loop.candidates) {
    if (c.marked) {
      continue;
    }
    const MachineInstr &instr =
        mf.blocks[static_cast<std::size_t>(c.block)].instrs[c.instrIdx];
    bool invariant = true;
    for (const MachineOperand &use : instr.uses) {
      if (!isVirtualRegister(use)) {
        continue;
      }
      auto defIt = loop.defsInLoop.find(use.regValue.id);
      if (defIt != loop.defsInLoop.end() &&
          hoistedDefs.count(use.regValue.id) == 0) {
        invariant = false;
        break;
      }
    }
    if (!invariant) {
      continue;
    }
    const uint32_t cost =
        isLoadOpcode(c.opcode) ? 3 : instructionCost(c.opcode);
    uint32_t inLoopUses = 0;
    const uint64_t profit = computeProfit(c.defReg, snap, loop, freq, loops,
                                          cost, inLoopUses);
    if (!passesProfitGate(profit, cost, inLoopUses, false, opts)) {
      ++profitSkips;
    }
  }
}

// 构建候选依赖图并拓扑排序（稳定序：候选收集序）。单赋值保证候选 def 互异，
// 依赖图必为 DAG；防御性 break 兜底（未排入的候选视为不外提）。
void buildDepsAndTopoOrder(const MachineFunction &mf, LoopData &loop) {
  std::map<uint32_t, int> defByCandidate;
  for (std::size_t index = 0; index < loop.candidates.size(); ++index) {
    const Candidate &c = loop.candidates[index];
    if (c.marked) {
      defByCandidate[c.defReg] = static_cast<int>(index);
    }
  }
  for (std::size_t index = 0; index < loop.candidates.size(); ++index) {
    Candidate &c = loop.candidates[index];
    if (!c.marked) {
      continue;
    }
    c.deps.clear();
    const MachineInstr &instr =
        mf.blocks[static_cast<std::size_t>(c.block)].instrs[c.instrIdx];
    for (const MachineOperand &use : instr.uses) {
      if (!isVirtualRegister(use)) {
        continue;
      }
      auto dep = defByCandidate.find(use.regValue.id);
      if (dep != defByCandidate.end() &&
          dep->second != static_cast<int>(index)) {
        c.deps.insert(dep->second);
      }
    }
  }
  loop.topoOrder.clear();
  std::set<int> emitted;
  const std::size_t markedCount =
      static_cast<std::size_t>(std::count_if(
          loop.candidates.begin(), loop.candidates.end(),
          [](const Candidate &c) { return c.marked; }));
  while (emitted.size() < markedCount) {
    bool progress = false;
    for (std::size_t index = 0; index < loop.candidates.size(); ++index) {
      const Candidate &c = loop.candidates[index];
      if (!c.marked || emitted.count(static_cast<int>(index)) != 0) {
        continue;
      }
      const bool depsReady = std::all_of(
          c.deps.begin(), c.deps.end(),
          [&](int dep) { return emitted.count(dep) != 0; });
      if (!depsReady) {
        continue;
      }
      emitted.insert(static_cast<int>(index));
      loop.topoOrder.push_back(static_cast<int>(index));
      progress = true;
    }
    if (!progress) {
      break; // 防御：理论不可达（单赋值保证依赖图为 DAG）
    }
  }
  // 未排入拓扑序的候选不能安全外提（依赖未闭合），取消标记。
  if (emitted.size() != markedCount) {
    for (Candidate &c : loop.candidates) {
      if (c.marked &&
          emitted.count(static_cast<int>(&c - loop.candidates.data())) == 0) {
        c.marked = false;
      }
    }
  }
}

// ==== 统计 ====

struct Stats {
  int loops = 0;
  int candidates = 0;
  int hoisted = 0;
  int preheaderCreated = 0;
  int pressureSkips = 0;
  int profitSkips = 0;
  int loadBlocked = 0;
  std::map<Opcode, int> byOpcode;
  std::map<int, int> byTier;
};

void printStats(const MachineFunction &mf, const Stats &stats) {
  std::cerr << "# machine-licm-stats func=" << mf.name
            << " loops=" << stats.loops
            << " candidates=" << stats.candidates
            << " hoisted=" << stats.hoisted
            << " preheader-created=" << stats.preheaderCreated
            << " pressure-skipped=" << stats.pressureSkips
            << " profit-skipped=" << stats.profitSkips
            << " load-blocked=" << stats.loadBlocked << "\n";
  for (const auto &[op, count] : stats.byOpcode) {
    std::cerr << "# machine-licm-hoists op=" << opcodeName(op)
              << " n=" << count << "\n";
  }
  for (const auto &[tier, count] : stats.byTier) {
    std::cerr << "# machine-licm-loads tier=t" << tier
              << " n=" << count << "\n";
  }
}

// ==== Phase 2：落地 ====

// 单个 Phase 2 中的指令移除动作。
struct Removal {
  int block = -1;
  std::size_t instrIdx = 0;
  int loopIndex = -1;
  Opcode opcode = Opcode::NOP;
  uint32_t defReg = 0;
};

// 落地一轮的全部外提：先创建缺失的 preheader（边分裂），再从原块按
// (块降序, 指令降序, 循环处理序升序) 移除指令（避免同块位置偏移干扰校验），
// 最后按各循环的拓扑序插入到对应 preheader 首条终结指令之前。
void applyHoists(MachineFunction &mf, std::vector<LoopData> &loopDatas,
                 std::vector<Creation> &creations, Stats &stats) {
  // 1) 边分裂创建 preheader：按插入位置降序处理，保证后续位置不被偏移。
  if (!creations.empty()) {
    std::sort(creations.begin(), creations.end(),
              [](const Creation &a, const Creation &b) {
                return a.insertPos > b.insertPos;
              });
    for (std::size_t k = 0; k < creations.size(); ++k) {
      Creation &creation = creations[k];
      // 改写循环外前驱的显式目标（其当前下标 = 原下标 + 已插入且
      // insertPos ≤ 原下标的创建数）。
      for (int pred : creation.outsidePreds) {
        int shift = 0;
        for (std::size_t j = 0; j < k; ++j) {
          if (creations[j].insertPos <= pred) {
            ++shift;
          }
        }
        MachineBasicBlock &predBlock =
            mf.blocks[static_cast<std::size_t>(pred + shift)];
        rewriteBlockTargets(predBlock, creation.headerLabel,
                            creation.preheaderLabel);
      }
      MachineBasicBlock preheader;
      preheader.label = creation.preheaderLabel;
      MachineInstr jump;
      jump.opcode = Opcode::J;
      jump.uses.push_back(MachineOperand::block(creation.headerLabel));
      jump.terminator = true;
      preheader.instrs.push_back(std::move(jump));
      using BlockDifference = std::vector<MachineBasicBlock>::difference_type;
      mf.blocks.insert(
          mf.blocks.begin() + static_cast<BlockDifference>(creation.insertPos),
          std::move(preheader));
      ++stats.preheaderCreated;
    }
    rebuildCFG(mf);
  }

  // 块下标偏移：原始下标 b 在全部创建完成后的当前下标。
  auto shiftFor = [&](int b) {
    int shift = 0;
    for (const Creation &creation : creations) {
      if (creation.insertPos <= b) {
        ++shift;
      }
    }
    return shift;
  };
  auto shiftLess = [&](int p) {
    int shift = 0;
    for (const Creation &creation : creations) {
      if (creation.insertPos < p) {
        ++shift;
      }
    }
    return shift;
  };

  // 2) 收集全部移除动作并排序（块/指令降序，同位置外层循环先移除）。
  std::vector<Removal> removals;
  for (const LoopData &loop : loopDatas) {
    for (const Candidate &c : loop.candidates) {
      if (c.marked) {
        Removal removal;
        removal.block = c.block;
        removal.instrIdx = c.instrIdx;
        removal.loopIndex = loop.orderIndex;
        removal.opcode = c.opcode;
        removal.defReg = c.defReg;
        removals.push_back(std::move(removal));
      }
    }
  }
  std::sort(removals.begin(), removals.end(),
            [](const Removal &a, const Removal &b) {
              if (a.block != b.block) {
                return a.block > b.block;
              }
              if (a.instrIdx != b.instrIdx) {
                return a.instrIdx > b.instrIdx;
              }
              return a.loopIndex < b.loopIndex;
            });

  // 3) 移除：按 defReg 收集到各循环的桶；位置内容不匹配（已被外层循环
  // 外提）则跳过。
  std::vector<std::map<uint32_t, MachineInstr>> moved(loopDatas.size());
  for (const Removal &removal : removals) {
    const int block = removal.block + shiftFor(removal.block);
    if (block < 0 || block >= static_cast<int>(mf.blocks.size())) {
      continue;
    }
    MachineBasicBlock &b = mf.blocks[static_cast<std::size_t>(block)];
    if (removal.instrIdx >= b.instrs.size()) {
      continue;
    }
    MachineInstr &instr = b.instrs[removal.instrIdx];
    if (instr.opcode != removal.opcode || instr.defs.empty() ||
        !isVirtualRegister(instr.defs[0]) ||
        instr.defs[0].regValue.id != removal.defReg) {
      continue; // 该位置已被其他循环的外提改写/移动
    }
    moved[static_cast<std::size_t>(removal.loopIndex)][removal.defReg] =
        std::move(instr);
    b.instrs.erase(b.instrs.begin() +
                   static_cast<std::ptrdiff_t>(removal.instrIdx));
  }

  // 4) 插入：按循环处理序，拓扑序追加到 preheader 首条终结指令之前。
  for (std::size_t loopIndex = 0; loopIndex < loopDatas.size(); ++loopIndex) {
    LoopData &loop = loopDatas[loopIndex];
    if (moved[loopIndex].empty()) {
      continue;
    }
    const int preheaderIdx =
        loop.needEdgeSplit
            ? loop.header + shiftLess(loop.header)
            : loop.preheader + shiftFor(loop.preheader);
    if (preheaderIdx < 0 ||
        preheaderIdx >= static_cast<int>(mf.blocks.size())) {
      continue;
    }
    MachineBasicBlock &preheader =
        mf.blocks[static_cast<std::size_t>(preheaderIdx)];
    std::vector<MachineInstr> hoisted;
    for (int candidateIndex : loop.topoOrder) {
      Candidate &c =
          loop.candidates[static_cast<std::size_t>(candidateIndex)];
      if (!c.marked) {
        continue;
      }
      auto found = moved[loopIndex].find(c.defReg);
      if (found == moved[loopIndex].end()) {
        continue; // 未被实际移除（跨循环重复标记，外层已处理）
      }
      hoisted.push_back(std::move(found->second));
      moved[loopIndex].erase(found);
      c.removed = true;
      ++stats.byOpcode[c.opcode];
      if (c.loadTier != 0) {
        ++stats.byTier[c.loadTier];
      }
    }
    if (hoisted.empty()) {
      continue;
    }
    // 插入点：首条终结指令之前（无终结指令则块尾）。
    std::size_t pos = preheader.instrs.size();
    for (std::size_t i = 0; i < preheader.instrs.size(); ++i) {
      if (preheader.instrs[i].terminator ||
          isTerminatorOpcode(preheader.instrs[i].opcode)) {
        pos = i;
        break;
      }
    }
    preheader.instrs.insert(
        preheader.instrs.begin() + static_cast<std::ptrdiff_t>(pos),
        std::make_move_iterator(hoisted.begin()),
        std::make_move_iterator(hoisted.end()));
    stats.hoisted += static_cast<int>(hoisted.size());
  }
}

// ==== 单轮处理 ====

bool runRound(MachineFunction &mf, MachineFunctionAnalysisManager &am,
              const Options &opts, Stats &stats) {
  const auto &cfg = am.getResult<MachineCFGAnalysis>(mf);
  const auto &liveness = am.getResult<MachineLivenessAnalysis>(mf);
  const auto &loops = am.getResult<MachineLoopAnalysis>(mf);
  const auto &freq = am.getResult<MachineBlockFrequencyAnalysis>(mf);
  if (loops.membersByHeader.empty()) {
    return false;
  }
  const FunctionSnapshot snap = buildSnapshot(mf);
  const DominatorSets dominators = computeDominators(cfg);

  // 外层→内层：header 的 depth 升序，同深度按块下标升序。
  std::vector<int> headers;
  for (const auto &[header, members] : loops.membersByHeader) {
    (void)members;
    headers.push_back(header);
  }
  std::sort(headers.begin(), headers.end(), [&](int a, int b) {
    const int depthA = loops.depth[static_cast<std::size_t>(a)];
    const int depthB = loops.depth[static_cast<std::size_t>(b)];
    if (depthA != depthB) {
      return depthA < depthB;
    }
    return a < b;
  });
  if (opts.debug) {
    std::cerr << "# licm-debug func=" << mf.name
              << " reliable=" << freq.reliable
              << " blocks=" << mf.blocks.size()
              << " loops=" << headers.size() << "\n";
    for (const auto &[latch, target] : loops.backedges) {
      std::cerr << "# licm-debug   backedge latch=" << latch
                << " header=" << target << "\n";
    }
    for (std::size_t b = 0; b < mf.blocks.size(); ++b) {
      if (mf.blocks[b].label == ".Lmain.bb26" ||
          mf.blocks[b].label == ".Lmain.bb27" ||
          mf.blocks[b].label == ".Lmain.bb24") {
        std::string preds;
        std::string succs;
        for (int p : cfg.preds[b]) {
          preds += std::to_string(p) + ",";
        }
        for (int s : cfg.succs[b]) {
          succs += std::to_string(s) + ",";
        }
        std::cerr << "# licm-debug   cfg block=" << b
                  << " label=" << mf.blocks[b].label << " preds={" << preds
                  << "} succs={" << succs << "} depth="
                  << loops.depth[b] << "\n";
      }
    }
  }

  std::vector<LoopData> loopDatas;
  std::vector<Creation> creations;
  int hoistBudget = opts.maxHoists;
  for (int header : headers) {
    LoopData loop;
    loop.header = header;
    loop.headerLabel = mf.blocks[static_cast<std::size_t>(header)].label;
    loop.members = loops.membersByHeader.at(header);
    if (!determinePreheader(loop, cfg)) {
      continue; // header == entry：跳过
    }
    // 压力守卫：循环体块数上限 + liveThrough 上限。
    if (static_cast<int>(loop.members.size()) > opts.maxLoopBlocks) {
      ++stats.pressureSkips;
      continue;
    }
    std::set<uint32_t> liveThrough =
        liveness.liveIn[static_cast<std::size_t>(header)];
    for (int member : loop.members) {
      const auto &liveOut =
          liveness.liveOut[static_cast<std::size_t>(member)];
      liveThrough.insert(liveOut.begin(), liveOut.end());
    }
    if (static_cast<int>(liveThrough.size()) >= opts.pressure) {
      ++stats.pressureSkips;
      continue;
    }
    // 循环内定义/store/call/memcpy 扫描。
    for (int member : loop.members) {
      for (const MachineInstr &instr :
           mf.blocks[static_cast<std::size_t>(member)].instrs) {
        for (const MachineOperand &def : instr.defs) {
          if (isVirtualRegister(def)) {
            ++loop.defsInLoop[def.regValue.id];
          }
        }
        if (isStoreOpcode(instr.opcode)) {
          loop.stores.push_back(&instr);
        }
        if (instr.opcode == Opcode::CALL) {
          loop.hasCall = true;
        }
        if (instr.opcode == Opcode::MEMCPY_INLINE ||
            instr.opcode == Opcode::MEMSET_INLINE) {
          loop.hasMemcpy = true;
        }
      }
    }
    ++stats.loops;
    // latch 列表（T1b 用）。
    std::vector<int> latches;
    for (const auto &[latch, target] : loops.backedges) {
      if (target == header) {
        latches.push_back(latch);
      }
    }
    collectCandidates(mf, snap, dominators, loop, opts, latches,
                      stats.loadBlocked);
    stats.candidates += static_cast<int>(loop.candidates.size());
    if (opts.debug) {
      std::cerr << "# licm-debug   header=" << header
                << " depth=" << loops.depth[static_cast<std::size_t>(header)]
                << " members=" << loop.members.size()
                << " preheader=" << loop.preheader
                << " split=" << loop.needEdgeSplit
                << " candidates=" << loop.candidates.size()
                << " stores=" << loop.stores.size()
                << " call=" << loop.hasCall
                << " liveThrough=" << liveThrough.size() << "\n";
    }
    if (loop.candidates.empty()) {
      continue;
    }
    markHoistable(mf, snap, loop, freq, loops, opts, nullptr);
    countProfitSkips(mf, snap, loop, freq, loops, opts, stats.profitSkips);
    if (opts.debug) {
      int marked = 0;
      for (const Candidate &c : loop.candidates) {
        if (c.marked) {
          ++marked;
        }
      }
      std::cerr << "# licm-debug   header=" << header << " marked=" << marked
                << " profitSkips=" << stats.profitSkips << "\n";
      for (const Candidate &c : loop.candidates) {
        if (!c.marked) {
          continue;
        }
        std::cerr << "# licm-debug     mark op=" << opcodeName(c.opcode)
                  << " block=" << c.block << " idx=" << c.instrIdx
                  << " profit=" << c.profit << " tier=" << c.loadTier << "\n";
      }
    }
    // 每函数上限：超出时按 (profit 降序, 块, 指令) 保留 top，重跑闭包。
    int marked = 0;
    for (const Candidate &c : loop.candidates) {
      if (c.marked) {
        ++marked;
      }
    }
    if (marked > hoistBudget) {
      std::vector<int> ranked;
      for (std::size_t index = 0; index < loop.candidates.size(); ++index) {
        if (loop.candidates[index].marked) {
          ranked.push_back(static_cast<int>(index));
        }
      }
      std::sort(ranked.begin(), ranked.end(), [&](int a, int b) {
        const Candidate &ca = loop.candidates[static_cast<std::size_t>(a)];
        const Candidate &cb = loop.candidates[static_cast<std::size_t>(b)];
        if (ca.profit != cb.profit) {
          return ca.profit > cb.profit;
        }
        if (ca.block != cb.block) {
          return ca.block < cb.block;
        }
        return ca.instrIdx < cb.instrIdx;
      });
      std::set<int> eligible;
      for (int i = 0; i < hoistBudget; ++i) {
        eligible.insert(ranked[static_cast<std::size_t>(i)]);
      }
      for (Candidate &c : loop.candidates) {
        c.marked = false;
      }
      markHoistable(mf, snap, loop, freq, loops, opts, &eligible);
    }
    marked = 0;
    for (const Candidate &c : loop.candidates) {
      if (c.marked) {
        ++marked;
      }
    }
    if (marked == 0) {
      continue;
    }
    buildDepsAndTopoOrder(mf, loop);
    marked = 0;
    for (const Candidate &c : loop.candidates) {
      if (c.marked) {
        ++marked;
      }
    }
    if (marked == 0) {
      continue;
    }
    hoistBudget -= marked;
    loop.orderIndex = static_cast<int>(loopDatas.size());
    if (loop.needEdgeSplit) {
      Creation creation;
      creation.insertPos = loop.header;
      creation.outsidePreds = loop.outsidePreds;
      creation.headerLabel = loop.headerLabel;
      creation.preheaderLabel = uniquePreheaderLabel(mf, loop.headerLabel);
      creations.push_back(std::move(creation));
    }
    loopDatas.push_back(std::move(loop));
  }
  if (loopDatas.empty()) {
    return false;
  }
  applyHoists(mf, loopDatas, creations, stats);
  return true;
}

} // namespace

// pass 入口：按 BACKEND_MACHINE_LICM_ROUNDS 重复执行单轮处理（每轮后
// 使分析失效重算），返回本函数是否有任何外提发生。
bool runPreRAMachineLICM(MachineFunction &mf,
                         MachineFunctionAnalysisManager &am) {
  const Options opts = readOptions();
  Stats stats;
  bool anyChange = false;
  for (int round = 0; round < opts.rounds; ++round) {
    const bool changed = runRound(mf, am, opts, stats);
    if (changed) {
      am.invalidate(mf, MachinePreservationStatus::none());
      anyChange = true;
    } else {
      break;
    }
  }
  if (opts.stats) {
    printStats(mf, stats);
  }
  return anyChange;
}

} // namespace riscv
