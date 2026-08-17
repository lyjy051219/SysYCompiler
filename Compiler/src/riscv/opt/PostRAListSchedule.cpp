#include "riscv/opt/MachineOptPasses.hpp"

#include "riscv/Registers.hpp"
#include "riscv/Target.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <type_traits>
#include <utility>
#include <vector>

namespace riscv {
// Post-RA List Scheduling pass 的 RISC-V 实现。
//
// 作用：在寄存器分配之后，对每个基本块内部按依赖与指令延迟重排指令，尽量让长
// 延迟指令（乘除、访存、浮点、调用）的结果在被使用前有时间就绪，从而隐藏流水线
// 延迟、缩短块的完成周期。调度以一个简单的按周期推进的表调度启发式进行：把块按
// barrier（终结指令、调用、内联 memcpy/memset、有副作用的非 store）切成若干段，
// 段内构建依赖图（寄存器数据依赖、写后写/读后写的顺序依赖、访存顺序依赖），按
// “关键路径优先级”择就绪指令发射。只有当重排后模拟出的完成周期严格更短时才采用。
//
// 流水线位置：寄存器分配之后（Post-RA），因此按物理寄存器建立依赖，调用会污染
// caller-saved 寄存器。只重排块内指令顺序，不改变指令本身，语义保持等价。
//
// 双发射实验开关：BACKEND_SCHED_ISSUE_WIDTH=2 启用 2-wide 端口模型（默认关）。
// 端口抽象：INT x2 / MEM x1 / FP x1；barrier 独占一个 issue 周期。当前 A/B 显示
// 局部用例改善与回退并存，整体未达默认启用阈值，因此只保留为实验开关。
namespace {

// 依赖跟踪用的寄存器键：物理寄存器按 phys 区分，虚拟寄存器按 id 区分。
struct RegKey {
  bool physical = false;
  uint32_t id = 0;
  PhysReg phys = PhysReg::NoReg;
};

bool operator<(const RegKey &lhs, const RegKey &rhs) {
  if (lhs.physical != rhs.physical) {
    return lhs.physical < rhs.physical;
  }
  if (lhs.physical) {
    return static_cast<uint16_t>(lhs.phys) < static_cast<uint16_t>(rhs.phys);
  }
  return lhs.id < rhs.id;
}

bool operator==(const RegKey &lhs, const RegKey &rhs) {
  return lhs.physical == rhs.physical && lhs.id == rhs.id &&
         lhs.phys == rhs.phys;
}

// 由机器寄存器构造 RegKey。
RegKey makeRegKey(const MachineReg &reg) {
  if (reg.physical) {
    return RegKey{true, 0, reg.phys};
  }
  return RegKey{false, reg.id, PhysReg::NoReg};
}

RegKey makeRegKey(PhysReg reg) { return RegKey{true, 0, reg}; }

// 是否是零寄存器 x0。x0 恒为零、写入无效，因此在依赖跟踪中忽略，避免制造伪依赖。
bool isZeroRegister(const RegKey &reg) {
  return reg.physical && reg.phys == PhysReg::X0;
}

// 调度发射宽度：默认保持既有单发射模型；设置 BACKEND_SCHED_ISSUE_WIDTH=2 时
// 启用双发射模型（BOOM v3 官方口径为双发射）。宽度只取 1 或 2，其他值按 1。
uint32_t scheduleIssueWidth() {
  static const uint32_t width = [] {
    const char *raw = std::getenv("BACKEND_SCHED_ISSUE_WIDTH");
    if (raw == nullptr) {
      return 1u;
    }
    const int value = std::atoi(raw);
    return value >= 2 ? 2u : 1u;
  }();
  return width;
}

bool dualIssueEnabled() { return scheduleIssueWidth() == 2; }

// 以下三个谓词区分 load / store / 终结指令的操作码。
bool isLoadOpcode(Opcode opcode) {
  return opcode == Opcode::LBU || opcode == Opcode::LW ||
         opcode == Opcode::LD || opcode == Opcode::FLW;
}

bool isStoreOpcode(Opcode opcode) {
  return opcode == Opcode::SB || opcode == Opcode::SW ||
         opcode == Opcode::SD || opcode == Opcode::FSW;
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

// 是否是调度屏障：终结指令、调用、内联 memcpy/memset，或有副作用的非 store 指令。
// 屏障把块切分为独立调度段，其两侧指令不可互相跨越重排。
bool isBarrierInstruction(const MachineInstr &instr) {
  if (instr.terminator || isTerminatorOpcode(instr.opcode)) {
    return true;
  }
  if (instr.opcode == Opcode::CALL || instr.opcode == Opcode::MEMCPY_INLINE ||
      instr.opcode == Opcode::MEMSET_INLINE) {
    return true;
  }
  return instr.sideEffects && !isStoreOpcode(instr.opcode);
}

// 是否参与内存顺序：load/store、调用、内联 memcpy/memset、有副作用的非 store。
// 这类指令之间保持原相对顺序（通过内存顺序依赖边），以免破坏访存语义。
bool isMemoryOrderInstruction(const MachineInstr &instr) {
  return isLoadOpcode(instr.opcode) || isStoreOpcode(instr.opcode) ||
         instr.opcode == Opcode::CALL || instr.opcode == Opcode::MEMCPY_INLINE ||
         instr.opcode == Opcode::MEMSET_INLINE ||
         (instr.sideEffects && !isStoreOpcode(instr.opcode));
}

// 各操作码的近似执行延迟（周期数），是调度关键路径与就绪时间估算的基础。
uint32_t opcodeLatency(Opcode opcode) {
  switch (opcode) {
  case Opcode::NOP:
    return 0;
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
  case Opcode::BEQZ:
  case Opcode::BNEZ:
  case Opcode::BEQ:
  case Opcode::BNE:
  case Opcode::BLT:
  case Opcode::BGE:
    return 1;
  case Opcode::MUL:
  case Opcode::MULW:
    return 3;
  case Opcode::FADD_S:
  case Opcode::FSUB_S:
  case Opcode::FMUL_S:
    return 3;
  case Opcode::FMADD_S:
  case Opcode::FMSUB_S:
  case Opcode::FNMADD_S:
  case Opcode::FNMSUB_S:
    return 4;
  case Opcode::DIV:
  case Opcode::DIVW:
  case Opcode::DIVUW:
  case Opcode::REM:
  case Opcode::REMW:
  case Opcode::REMUW:
  case Opcode::FDIV_S:
    return 12;
  case Opcode::FCVT_W_S:
  case Opcode::FCVT_S_W:
  case Opcode::FEQ_S:
  case Opcode::FLT_S:
  case Opcode::FLE_S:
    return 3;
  case Opcode::LBU:
  case Opcode::LW:
  case Opcode::LD:
  case Opcode::FLW:
    return 3;
  case Opcode::SB:
  case Opcode::SW:
  case Opcode::SD:
  case Opcode::FSW:
    return 1;
  case Opcode::CALL:
    return 20;
  case Opcode::MEMCPY_INLINE:
    return 8;
  case Opcode::MEMSET_INLINE:
    return 8;
  case Opcode::RET:
  case Opcode::J:
    return 1;
  case Opcode::PHI:
  case Opcode::SELECT:
  case Opcode::PARALLEL_COPY:
    return 1;
  case Opcode::GEP:
    return 1;
  }
  return 1;
}

// 发射端口分类：双发射模型按 BOOM 的 INT/MEM/FP 端口约束配对。
// barrier 指令（调用、内联 memcpy/memset 等）独占一个 issue 周期，不与任何指令配对。
enum class IssuePortClass : uint8_t { INT, MEM, FP, BARRIER };

IssuePortClass issuePortClass(Opcode opcode) {
  if (isLoadOpcode(opcode) || isStoreOpcode(opcode)) {
    return IssuePortClass::MEM;
  }
  switch (opcode) {
  case Opcode::FADD_S:
  case Opcode::FSUB_S:
  case Opcode::FMUL_S:
  case Opcode::FMADD_S:
  case Opcode::FMSUB_S:
  case Opcode::FNMADD_S:
  case Opcode::FNMSUB_S:
  case Opcode::FDIV_S:
  case Opcode::FCVT_W_S:
  case Opcode::FCVT_S_W:
  case Opcode::FEQ_S:
  case Opcode::FLT_S:
  case Opcode::FLE_S:
  case Opcode::F_COPY:
  case Opcode::FMV_W_X:
  case Opcode::FMV_X_W:
    return IssuePortClass::FP;
  case Opcode::CALL:
  case Opcode::MEMCPY_INLINE:
  case Opcode::MEMSET_INLINE:
    return IssuePortClass::BARRIER;
  default:
    return IssuePortClass::INT;
  }
}

// 两条指令能否在同一 issue 周期发射（只判断端口资源，数据依赖由调用方把关）。
bool portsConflict(IssuePortClass lhs, IssuePortClass rhs) {
  if (lhs == IssuePortClass::BARRIER || rhs == IssuePortClass::BARRIER) {
    return true;
  }
  if (lhs == IssuePortClass::MEM && rhs == IssuePortClass::MEM) {
    return true;
  }
  if (lhs == IssuePortClass::FP && rhs == IssuePortClass::FP) {
    return true;
  }
  return false; // INT+INT / INT+MEM / INT+FP / MEM+FP 可配对
}

// 从操作数容器收集去重后的寄存器键（忽略无效寄存器与 x0）。模板同时支持
// MachineOperand 列表与 PhysReg 列表（隐式操作数）两种形态。
template <typename ContainerT>
std::vector<RegKey> collectRegKeys(const ContainerT &operands) {
  std::vector<RegKey> result;
  result.reserve(operands.size());
  for (const auto &operand : operands) {
    if constexpr (std::is_same_v<std::decay_t<decltype(operand)>, MachineOperand>) {
      if (operand.kind != OperandKind::Reg || !operand.regValue.isValid()) {
        continue;
      }
      RegKey key = makeRegKey(operand.regValue);
      if (!isZeroRegister(key)) {
        result.push_back(key);
      }
    } else {
      if (!isValidPhysReg(operand) || operand == PhysReg::X0) {
        continue;
      }
      result.push_back(makeRegKey(operand));
    }
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

// 一条指令预先算好的调度属性：延迟、是否访存、是否屏障，以及去重合并了显式与
// 隐式操作数后的 use/def 寄存器集合。
struct InstructionInfo {
  uint32_t latency = 0;
  bool memoryOp = false;
  bool barrier = false;
  IssuePortClass port = IssuePortClass::INT;
  std::vector<RegKey> uses;
  std::vector<RegKey> defs;
};

// 为块内每条指令构建 InstructionInfo（合并显式与隐式操作数并去重）。
std::vector<InstructionInfo>
buildInstructionInfo(const std::vector<MachineInstr> &instrs) {
  std::vector<InstructionInfo> info;
  info.reserve(instrs.size());
  for (const MachineInstr &instr : instrs) {
    InstructionInfo item;
    item.latency = opcodeLatency(instr.opcode);
    item.barrier = isBarrierInstruction(instr);
    item.memoryOp = isMemoryOrderInstruction(instr);
    item.port = issuePortClass(instr.opcode);
    item.uses = collectRegKeys(instr.uses);
    item.defs = collectRegKeys(instr.defs);
    const auto implicitUses = collectRegKeys(instr.implicitUses);
    const auto implicitDefs = collectRegKeys(instr.implicitDefs);
    item.uses.insert(item.uses.end(), implicitUses.begin(), implicitUses.end());
    item.defs.insert(item.defs.end(), implicitDefs.begin(), implicitDefs.end());
    std::sort(item.uses.begin(), item.uses.end());
    item.uses.erase(std::unique(item.uses.begin(), item.uses.end()),
                    item.uses.end());
    std::sort(item.defs.begin(), item.defs.end());
    item.defs.erase(std::unique(item.defs.begin(), item.defs.end()),
                    item.defs.end());
    info.push_back(std::move(item));
  }
  return info;
}

// 调度模拟的状态：每个寄存器的就绪周期、内存就绪周期、当前发射周期、以及整段
// 的最终完成周期（用于比较不同顺序的优劣）。
struct ScheduleState {
  std::map<RegKey, uint64_t> regReady;
  uint64_t memoryReady = 0;
  uint64_t cycle = 0;
  uint64_t finish = 0;
  // 双发射模型的端口资源（单发射路径不使用）。INT 有两个等价端口，
  // MEM/FP 各一个；lastIssueCycle 用于阻止 barrier 与其他指令同周期发射。
  bool dualIssue = false;
  std::array<uint64_t, 2> intPortReady = {{0, 0}};
  uint64_t memPortReady = 0;
  uint64_t fpPortReady = 0;
  uint64_t lastIssueCycle = std::numeric_limits<uint64_t>::max();
};

// 在端口类别不冲突的基础上，进一步校验 INT+INT 是否有两个空闲 INT 槽。
bool canPairAt(const InstructionInfo &lhs, const InstructionInfo &rhs,
               const ScheduleState &state, uint64_t cycle) {
  if (portsConflict(lhs.port, rhs.port)) {
    return false;
  }
  if (lhs.port == IssuePortClass::INT && rhs.port == IssuePortClass::INT) {
    const int freeSlots =
        (state.intPortReady[0] <= cycle ? 1 : 0) +
        (state.intPortReady[1] <= cycle ? 1 : 0);
    return freeSlots >= 2;
  }
  return true;
}

// 取寄存器 reg 的就绪周期（未记录则为 0）。
uint64_t readyTimeForReg(const ScheduleState &state, const RegKey &reg) {
  auto it = state.regReady.find(reg);
  if (it == state.regReady.end()) {
    return 0;
  }
  return it->second;
}

// 设置寄存器就绪周期（x0 忽略）。
void setRegReady(ScheduleState &state, const RegKey &reg, uint64_t readyTime) {
  if (!isZeroRegister(reg)) {
    state.regReady[reg] = readyTime;
  }
}

// 调用会破坏所有 caller-saved 寄存器：把它们的就绪周期推到调用完成周期，模拟其
// 值在调用后才可用（防止把依赖这些寄存器的指令错误地提前到调用之前之后重排）。
void clobberCallerSavedRegs(ScheduleState &state, uint64_t readyTime) {
  for (PhysReg reg : callerSavedRegs(RegClass::GPR64)) {
    setRegReady(state, makeRegKey(reg), readyTime);
  }
  for (PhysReg reg : callerSavedRegs(RegClass::FPR32)) {
    setRegReady(state, makeRegKey(reg), readyTime);
  }
}

// 模拟发射一条指令：发射周期取当前周期与所有 use 就绪周期（访存还要等内存就绪）
// 的最大值；完成周期为发射周期加延迟。更新当前周期、整段完成周期、各 def 与内存
// 的就绪周期；若是调用则污染 caller-saved 寄存器。返回该指令的完成周期。
uint64_t issueInstruction(const MachineInstr &instr, const InstructionInfo &info,
                          ScheduleState &state) {
  uint64_t issue = state.cycle;
  for (const RegKey &use : info.uses) {
    issue = std::max(issue, readyTimeForReg(state, use));
  }
  if (info.memoryOp) {
    issue = std::max(issue, state.memoryReady);
  }

  const uint64_t completion = issue + info.latency;
  state.cycle = issue + 1;
  state.finish = std::max(state.finish, completion);

  for (const RegKey &def : info.defs) {
    setRegReady(state, def, completion);
  }
  if (info.memoryOp) {
    state.memoryReady = completion;
  }
  if (instr.opcode == Opcode::CALL) {
    clobberCallerSavedRegs(state, completion);
  }
  return completion;
}

// 双发射模型下，指令在给定基准周期上的最早可发射周期。基准周期之外的延迟
// 来自寄存器依赖、内存顺序以及端口占用；barrier 还会检查同周期是否已有指令。
uint64_t earliestIssueCycle(const InstructionInfo &info,
                            const ScheduleState &state, uint64_t cycle) {
  uint64_t issue = cycle;
  for (const RegKey &use : info.uses) {
    issue = std::max(issue, readyTimeForReg(state, use));
  }
  if (info.memoryOp) {
    issue = std::max(issue, state.memoryReady);
  }
  switch (info.port) {
  case IssuePortClass::INT:
    issue = std::max(issue, std::min(state.intPortReady[0], state.intPortReady[1]));
    break;
  case IssuePortClass::MEM:
    issue = std::max(issue, state.memPortReady);
    break;
  case IssuePortClass::FP:
    issue = std::max(issue, state.fpPortReady);
    break;
  case IssuePortClass::BARRIER:
    issue = std::max(issue, state.memPortReady);
    issue = std::max(issue, state.fpPortReady);
    issue = std::max(issue, state.intPortReady[0]);
    issue = std::max(issue, state.intPortReady[1]);
    if (state.lastIssueCycle == cycle) {
      issue = std::max(issue, cycle + 1);
    }
    break;
  }
  return issue;
}

bool canIssueAt(const InstructionInfo &info, const ScheduleState &state,
                uint64_t cycle) {
  return earliestIssueCycle(info, state, cycle) == cycle;
}

// 在指定周期发射一条指令并更新完成周期、寄存器/内存/端口状态。不推进
// state.cycle；由调用方在完成同周期发射（0/1/2 条）后统一设置。
uint64_t issueInstructionAt(const MachineInstr &instr,
                            const InstructionInfo &info, ScheduleState &state,
                            uint64_t issue) {
  const uint64_t completion = issue + info.latency;
  state.finish = std::max(state.finish, completion);

  for (const RegKey &def : info.defs) {
    setRegReady(state, def, completion);
  }
  if (info.memoryOp) {
    state.memoryReady = completion;
  }
  // 发射槽位按“占用 1 个 issue cycle”释放，而不是按执行延迟释放：
  // 乘/除/访存/浮点虽然结果延迟较长，但它们并不把发射端口占住多个周期。
  // 旧实现把 intPortReady/memPortReady/fpPortReady 设成 completion，会把
  // 3 周期的 load、12 周期的除法后面的无关指令错误地堵在发射端口上，
  // 严重低估 BOOM 的发射宽度。除法和 barrier 属于不可流水化的特例。
  switch (info.port) {
  case IssuePortClass::INT: {
    int chosenSlot = -1;
    for (int i = 0; i < 2; ++i) {
      if (state.intPortReady[static_cast<std::size_t>(i)] <= issue) {
        chosenSlot = i;
        break;
      }
    }
    if (chosenSlot < 0) {
      chosenSlot = state.intPortReady[0] <= state.intPortReady[1] ? 0 : 1;
    }
    state.intPortReady[static_cast<std::size_t>(chosenSlot)] = issue + 1;
    break;
  }
  case IssuePortClass::MEM:
    state.memPortReady = issue + 1;
    break;
  case IssuePortClass::FP:
    // 浮点加减乘在 BOOM FPU 上按流水线工作，端口每周期可再次发射；
    // fdiv 变延迟且不可完全流水化，保守按完成周期释放端口。
    state.fpPortReady = instr.opcode == Opcode::FDIV_S ? completion : issue + 1;
    break;
  case IssuePortClass::BARRIER:
    state.intPortReady[0] = completion;
    state.intPortReady[1] = completion;
    state.memPortReady = completion;
    state.fpPortReady = completion;
    break;
  }
  if (instr.opcode == Opcode::CALL) {
    clobberCallerSavedRegs(state, completion);
  }
  state.lastIssueCycle = issue;
  return completion;
}

// 段内依赖图的一个结点。completionSuccs 是数据依赖后继（需等前驱完成周期），
// orderSuccs 是顺序依赖后继（写后写、读后写、访存顺序，只需前驱已发射）；
// indegree 供拓扑就绪判定；releaseTime 是外部（跨段状态）施加的最早可发射周期。
struct SegmentNode {
  size_t originalIndex = 0;
  uint64_t releaseTime = 0;
  uint32_t latency = 0;
  int indegree = 0;
  std::vector<size_t> completionSuccs;
  std::vector<size_t> orderSuccs;
  std::vector<RegKey> uses;
  std::vector<RegKey> defs;
  bool memoryOp = false;
};

// 添加一条数据依赖边（真依赖，后继需等前驱完成周期），去重并递增后继入度。
void addCompletionEdge(std::vector<SegmentNode> &nodes, size_t from, size_t to) {
  auto &succs = nodes[from].completionSuccs;
  if (std::find(succs.begin(), succs.end(), to) == succs.end()) {
    succs.push_back(to);
    ++nodes[to].indegree;
  }
}

// 添加一条顺序依赖边（反依赖/输出依赖/访存顺序，后继只需前驱已发射即可），去重
// 并递增后继入度。
void addOrderEdge(std::vector<SegmentNode> &nodes, size_t from, size_t to) {
  auto &succs = nodes[from].orderSuccs;
  if (std::find(succs.begin(), succs.end(), to) == succs.end()) {
    succs.push_back(to);
    ++nodes[to].indegree;
  }
}

// 就绪结点间的择优比较：优先级（关键路径长度）高者优先，相等时按原始下标靠前者
// 优先，保证调度确定性且尽量不打乱原顺序。
bool moreImportantNode(size_t lhs, size_t rhs,
                       const std::vector<SegmentNode> &nodes,
                       const std::vector<uint64_t> &priorities) {
  if (priorities[lhs] != priorities[rhs]) {
    return priorities[lhs] > priorities[rhs];
  }
  return nodes[lhs].originalIndex < nodes[rhs].originalIndex;
}

// 双发射模型的择优比较：先看端口/依赖约束下谁能在更早周期发射，再看关键路径
// 优先级，最后按原始下标。避免“优先级最高的结点占住一个未就绪端口”白白浪费周期。
bool betterDualCandidate(size_t lhs, size_t rhs,
                         const std::vector<SegmentNode> &nodes,
                         const std::vector<uint64_t> &priorities,
                         const std::vector<InstructionInfo> &infos,
                         const ScheduleState &state, uint64_t cycle) {
  const uint64_t lhsIssue =
      earliestIssueCycle(infos[nodes[lhs].originalIndex], state, cycle);
  const uint64_t rhsIssue =
      earliestIssueCycle(infos[nodes[rhs].originalIndex], state, cycle);
  if (lhsIssue != rhsIssue) {
    return lhsIssue < rhsIssue;
  }
  return moreImportantNode(lhs, rhs, nodes, priorities);
}

// 结点的调度优先级 = 自身延迟 + 所有后继优先级的最大值，即从该结点到段尾的关键
// 路径长度。逆拓扑（按下标从后往前）一次算出，供列表调度择优。
uint64_t maxPrioritySuccessor(const std::vector<SegmentNode> &nodes,
                             const std::vector<uint64_t> &priorities,
                             size_t index) {
  uint64_t best = 0;
  for (size_t succ : nodes[index].completionSuccs) {
    best = std::max(best, priorities[succ]);
  }
  for (size_t succ : nodes[index].orderSuccs) {
    best = std::max(best, priorities[succ]);
  }
  return nodes[index].latency + best;
}

// 对一个非屏障指令段做表调度并返回新的指令下标顺序。
// 第一步建立依赖图：真依赖（use 依赖段内最近一次 def）连数据边；写后写、读后写
// 连顺序边；访存指令串成一条内存顺序链；段外已有的 def 通过 releaseTime 施加最早
// 发射约束。第二步逆拓扑算每个结点的关键路径优先级。第三步按周期推进做列表调度：
// 在入度为 0 且已到 releaseTime 的就绪结点里，用 moreImportantNode 择优发射，无就绪
// 结点则把当前周期快进到下一个 releaseTime；发射后更新后继的入度与 releaseTime
// （数据后继等完成周期、顺序后继等发射周期）。检测到环则抛异常。
std::vector<size_t>
scheduleSegment(const std::vector<MachineInstr> &instrs,
                const std::vector<InstructionInfo> &infos,
                const std::vector<size_t> &segmentIndices,
                ScheduleState &state) {
  std::vector<size_t> empty;
  if (segmentIndices.empty()) {
    return empty;
  }

  std::vector<SegmentNode> nodes(segmentIndices.size());
  std::map<RegKey, size_t> lastDef;
  std::map<RegKey, std::vector<size_t>> openUses;
  size_t lastMemoryNode = std::numeric_limits<size_t>::max();

  for (size_t localIndex = 0; localIndex < segmentIndices.size(); ++localIndex) {
    const size_t originalIndex = segmentIndices[localIndex];
    const InstructionInfo &info = infos[originalIndex];
    SegmentNode &node = nodes[localIndex];
    node.originalIndex = originalIndex;
    node.releaseTime = 0;
    node.latency = info.latency;
    node.uses = info.uses;
    node.defs = info.defs;
    node.memoryOp = info.memoryOp;

    if (node.memoryOp) {
      if (lastMemoryNode != std::numeric_limits<size_t>::max()) {
        addOrderEdge(nodes, lastMemoryNode, localIndex);
      } else {
        node.releaseTime = std::max(node.releaseTime, state.cycle);
      }
      lastMemoryNode = localIndex;
    }

    for (const RegKey &use : node.uses) {
      auto found = lastDef.find(use);
      if (found != lastDef.end()) {
        addCompletionEdge(nodes, found->second, localIndex);
      } else {
        node.releaseTime =
            std::max(node.releaseTime, readyTimeForReg(state, use));
      }
    }

    for (const RegKey &def : node.defs) {
      auto defFound = lastDef.find(def);
      if (defFound != lastDef.end()) {
        addOrderEdge(nodes, defFound->second, localIndex);  // 写后写：保持顺序
      }
      auto useFound = openUses.find(def);
      if (useFound != openUses.end()) {
        for (size_t useIndex : useFound->second) {
          addOrderEdge(nodes, useIndex, localIndex);  // 读后写：读必须先于覆写
        }
        useFound->second.clear();
      }
    }

    for (const RegKey &def : node.defs) {
      lastDef[def] = localIndex;
    }
    for (const RegKey &use : node.uses) {
      openUses[use].push_back(localIndex);
    }
  }

  std::vector<uint64_t> priorities(nodes.size(), 0);
  for (size_t reverse = 0; reverse < nodes.size(); ++reverse) {
    const size_t index = nodes.size() - reverse - 1;
    priorities[index] = maxPrioritySuccessor(nodes, priorities, index);
  }

  std::vector<bool> scheduled(nodes.size(), false);
  std::vector<size_t> order;
  order.reserve(nodes.size());
  size_t scheduledCount = 0;
  uint64_t issueCycle = state.cycle;

  while (scheduledCount < nodes.size()) {
    size_t chosen = std::numeric_limits<size_t>::max();
    uint64_t nextRelease = std::numeric_limits<uint64_t>::max();

    for (size_t index = 0; index < nodes.size(); ++index) {
      if (scheduled[index] || nodes[index].indegree != 0) {
        continue;
      }
      if (nodes[index].releaseTime <= issueCycle) {
        if (chosen == std::numeric_limits<size_t>::max()) {
          chosen = index;
        } else if (state.dualIssue) {
          if (betterDualCandidate(index, chosen, nodes, priorities, infos,
                                  state, issueCycle)) {
            chosen = index;
          }
        } else if (moreImportantNode(index, chosen, nodes, priorities)) {
          chosen = index;
        }
      } else {
        nextRelease = std::min(nextRelease, nodes[index].releaseTime);
      }
    }

    if (chosen == std::numeric_limits<size_t>::max()) {
      if (nextRelease == std::numeric_limits<uint64_t>::max()) {
        throw BackendError("post-RA list scheduler encountered a cycle");
      }
      issueCycle = nextRelease;
      state.cycle = issueCycle;
      continue;
    }

    const size_t firstOriginal = nodes[chosen].originalIndex;
    const InstructionInfo &firstInfo = infos[firstOriginal];

    if (!state.dualIssue) {
      state.cycle = issueCycle;
      const uint64_t completion =
          issueInstruction(instrs[firstOriginal], firstInfo, state);
      issueCycle = state.cycle;
      scheduled[chosen] = true;
      ++scheduledCount;
      order.push_back(firstOriginal);

      for (size_t succ : nodes[chosen].completionSuccs) {
        nodes[succ].indegree--;
        nodes[succ].releaseTime =
            std::max(nodes[succ].releaseTime, completion);
      }
      for (size_t succ : nodes[chosen].orderSuccs) {
        nodes[succ].indegree--;
        nodes[succ].releaseTime =
            std::max(nodes[succ].releaseTime, state.cycle);
      }
      continue;
    }

    // 双发射：先定第一条指令的真实发射周期（端口/依赖可能晚于 releaseTime），
    // 再尝试在同一周期挑选第二条端口可配对的就绪指令。
    issueCycle =
        earliestIssueCycle(firstInfo, state, std::max(issueCycle, state.cycle));
    state.cycle = issueCycle;

    size_t second = std::numeric_limits<size_t>::max();
    for (size_t index = 0; index < nodes.size(); ++index) {
      if (index == chosen || scheduled[index] || nodes[index].indegree != 0 ||
          nodes[index].releaseTime > issueCycle) {
        continue;
      }
      const InstructionInfo &candidateInfo =
          infos[nodes[index].originalIndex];
      if (!canPairAt(firstInfo, candidateInfo, state, issueCycle)) {
        continue;
      }
      if (earliestIssueCycle(candidateInfo, state, issueCycle) > issueCycle) {
        continue;
      }
      if (second == std::numeric_limits<size_t>::max() ||
          moreImportantNode(index, second, nodes, priorities)) {
        second = index;
      }
    }

    const uint64_t firstCompletion =
        issueInstructionAt(instrs[firstOriginal], firstInfo, state, issueCycle);
    scheduled[chosen] = true;
    ++scheduledCount;
    order.push_back(firstOriginal);

    uint64_t secondCompletion = 0;
    bool hasSecond = false;
    if (second != std::numeric_limits<size_t>::max()) {
      const size_t secondOriginal = nodes[second].originalIndex;
      secondCompletion = issueInstructionAt(
          instrs[secondOriginal], infos[secondOriginal], state, issueCycle);
      scheduled[second] = true;
      ++scheduledCount;
      order.push_back(secondOriginal);
      hasSecond = true;
    }

    state.cycle = issueCycle + 1;
    issueCycle = state.cycle;

    for (size_t succ : nodes[chosen].completionSuccs) {
      nodes[succ].indegree--;
      nodes[succ].releaseTime =
          std::max(nodes[succ].releaseTime, firstCompletion);
    }
    for (size_t succ : nodes[chosen].orderSuccs) {
      nodes[succ].indegree--;
      nodes[succ].releaseTime = std::max(nodes[succ].releaseTime, state.cycle);
    }
    if (hasSecond) {
      for (size_t succ : nodes[second].completionSuccs) {
        nodes[succ].indegree--;
        nodes[succ].releaseTime =
            std::max(nodes[succ].releaseTime, secondCompletion);
      }
      for (size_t succ : nodes[second].orderSuccs) {
        nodes[succ].indegree--;
        nodes[succ].releaseTime =
            std::max(nodes[succ].releaseTime, state.cycle);
      }
    }
  }

  return order;
}

// 按给定顺序整段模拟一遍，返回完成周期。用于评估原始顺序与候选顺序的优劣。
// 双发射开启时按固定顺序贪心配对：先找当前位置最早可发射周期，再看下一条能否
// 同周期配对上；单发射路径保持原实现不变。
uint64_t simulateOrder(const std::vector<MachineInstr> &instrs,
                       const std::vector<InstructionInfo> &infos,
                       const std::vector<size_t> &order) {
  ScheduleState state;
  state.dualIssue = dualIssueEnabled();

  if (!state.dualIssue) {
    for (size_t index : order) {
      issueInstruction(instrs[index], infos[index], state);
    }
    return state.finish;
  }

  size_t cursor = 0;
  uint64_t cycle = 0;
  while (cursor < order.size()) {
    const size_t firstIndex = order[cursor];
    const InstructionInfo &firstInfo = infos[firstIndex];
    cycle = earliestIssueCycle(firstInfo, state, cycle);
    state.cycle = cycle;
    issueInstructionAt(instrs[firstIndex], firstInfo, state, cycle);
    ++cursor;

    if (cursor < order.size()) {
      const size_t secondIndex = order[cursor];
      const InstructionInfo &secondInfo = infos[secondIndex];
      if (canPairAt(firstInfo, secondInfo, state, cycle) &&
          earliestIssueCycle(secondInfo, state, cycle) <= cycle) {
        issueInstructionAt(instrs[secondIndex], secondInfo, state, cycle);
        ++cursor;
      }
    }
    cycle = cycle + 1;
    state.cycle = cycle;
  }
  return state.finish;
}

// 生成恒等顺序 0,1,2,...（原始指令顺序）。
std::vector<size_t> identityOrder(const std::vector<MachineInstr> &instrs) {
  std::vector<size_t> order(instrs.size());
  for (size_t i = 0; i < instrs.size(); ++i) {
    order[i] = i;
  }
  return order;
}

// 两个顺序是否相同。
bool sameOrder(const std::vector<size_t> &lhs, const std::vector<size_t> &rhs) {
  return lhs == rhs;
}

// 构造整块的候选调度顺序：顺序扫描，遇非屏障指令累积成段，遇屏障时先调度已累积
// 的段再放入屏障本身；一旦发射了终结指令，其后的指令原样附加不再调度。返回全块
// 的候选下标顺序，并在 state 中留下候选顺序的完成周期。
std::vector<size_t> buildCandidateOrder(const std::vector<MachineInstr> &instrs,
                                        const std::vector<InstructionInfo> &infos,
                                        ScheduleState &state) {
  std::vector<size_t> order;
  order.reserve(instrs.size());
  std::vector<size_t> pendingSegment;
  bool afterTerminator = false;

  // 屏障与终结指令之后的内容不参与配对，按固定位置单独发射；单发射路径仍走原函数。
  auto issueFixed = [&](size_t index) {
    if (!state.dualIssue) {
      issueInstruction(instrs[index], infos[index], state);
      return;
    }
    const uint64_t issue =
        earliestIssueCycle(infos[index], state, state.cycle);
    issueInstructionAt(instrs[index], infos[index], state, issue);
    state.cycle = issue + 1;
  };

  for (size_t index = 0; index < instrs.size(); ++index) {
    if (afterTerminator) {
      order.push_back(index);
      issueFixed(index);
      continue;
    }

    const InstructionInfo &info = infos[index];
    if (info.barrier) {
      if (!pendingSegment.empty()) {
        std::vector<size_t> segmentOrder =
            scheduleSegment(instrs, infos, pendingSegment, state);
        order.insert(order.end(), segmentOrder.begin(), segmentOrder.end());
        pendingSegment.clear();
      }
      order.push_back(index);
      issueFixed(index);
      if (isTerminatorOpcode(instrs[index].opcode) || instrs[index].terminator) {
        afterTerminator = true;
      }
      continue;
    }

    pendingSegment.push_back(index);
  }

  if (!pendingSegment.empty()) {
    std::vector<size_t> segmentOrder =
        scheduleSegment(instrs, infos, pendingSegment, state);
    order.insert(order.end(), segmentOrder.begin(), segmentOrder.end());
  }

  return order;
}

// 快速预判该块是否值得调度。单发射模型只关注能否隐藏长延迟；双发射模型下，
// 任何长度不小于 2 的非屏障段都有同周期配对的潜在收益。
bool blockHasUsefulSchedulingOpportunity(
    const std::vector<InstructionInfo> &infos) {
  const bool dual = dualIssueEnabled();
  std::vector<size_t> segment;
  auto segmentUseful = [&]() {
    if (segment.size() < 2) {
      return false;
    }
    if (dual) {
      return true;
    }
    for (size_t segmentIndex : segment) {
      if (infos[segmentIndex].memoryOp || infos[segmentIndex].latency > 1) {
        return true;
      }
    }
    return false;
  };
  for (size_t index = 0; index < infos.size(); ++index) {
    if (infos[index].barrier) {
      if (segmentUseful()) {
        return true;
      }
      segment.clear();
      continue;
    }
    segment.push_back(index);
  }
  return segmentUseful();
}

} // namespace

// pass 入口：对每个非零频率、指令数不少于 2 的块，若有调度价值则生成候选顺序，
// 与原始顺序模拟比较完成周期，仅当候选严格更短时才真正按候选顺序重排块内指令。
// 返回是否修改了函数。频率数据与块数不匹配视为错误。
bool runPostRAListSchedule(MachineFunction &mf,
                           const MachineBlockFrequencyAnalysis::Result &frequencies) {
  if (frequencies.blockFrequency.size() != mf.blocks.size()) {
    throw BackendError("post-RA list scheduler received mismatched frequency data");
  }

  bool changed = false;
  for (size_t blockIndex = 0; blockIndex < mf.blocks.size(); ++blockIndex) {
    if (frequencies.blockFrequency[blockIndex] == 0) {
      continue;
    }

    MachineBasicBlock &block = mf.blocks[blockIndex];
    if (block.instrs.size() < 2) {
      continue;
    }

    const std::vector<InstructionInfo> infos = buildInstructionInfo(block.instrs);
    if (!blockHasUsefulSchedulingOpportunity(infos)) {
      continue;
    }

    const std::vector<size_t> originalOrder = identityOrder(block.instrs);
    const uint64_t originalFinish = simulateOrder(block.instrs, infos, originalOrder);

    ScheduleState candidateState;
    candidateState.dualIssue = dualIssueEnabled();
    const std::vector<size_t> candidateOrder =
        buildCandidateOrder(block.instrs, infos, candidateState);
    if (sameOrder(candidateOrder, originalOrder)) {
      continue;
    }

    const uint64_t candidateFinish = candidateState.finish;
    if (candidateFinish >= originalFinish) {
      continue;
    }

    std::vector<MachineInstr> reordered;
    reordered.reserve(block.instrs.size());
    for (size_t index : candidateOrder) {
      reordered.push_back(block.instrs[index]);
    }
    block.instrs = std::move(reordered);
    changed = true;
  }

  return changed;
}

} // namespace riscv
