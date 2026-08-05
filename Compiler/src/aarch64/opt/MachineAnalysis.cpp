#include "aarch64/opt/MachineAnalysis.hpp"

#include "aarch64/Target.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aarch64 {
// Machine Analysis 的实现：统计信息、控制流图(CFG)、活跃变量、自然循环
// 与循环深度、以及块执行频率估计。这些分析被寄存器分配、机器 DCE、CFG 清理
// 等 pass 通过分析管理器按需取用并缓存。此为 RISC-V 与 ARM64 两后端
// 相同的公共实现,两后端各保留一份相同实现。

namespace {

// 与 Passes.cpp 中保持一致的终结指令判断
bool isTerminatorOpcode(Opcode opcode) {
  switch (opcode) {
  case Opcode::RET:
  case Opcode::TAILCALL:
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

// 判断该终结指令是否不允许顺序落入下一块:无条件跳转 J 和返回 RET。
bool isNoFallthroughOpcode(Opcode opcode) {
  return opcode == Opcode::RET || opcode == Opcode::J;
}

// 判断块结尾能否顺序落入下一块。自块尾向前找到首条终结指令:
// 若它是 J/RET 则不落入,否则(如条件分支)可落入;整块无终结指令也视为可落入。
bool blockCanFallThrough(const MachineBasicBlock &block) {
  for (auto it = block.instrs.rbegin(); it != block.instrs.rend(); ++it) {
    if (it->terminator || isTerminatorOpcode(it->opcode)) {
      return !isNoFallthroughOpcode(it->opcode);
    }
  }
  return true;
}

// 向 CFG 结果中加入一条 from->to 边,同时维护后继表和前驱表,去重避免重边。
void addEdge(MachineCFGAnalysis::Result &result, int from, int to) {
  auto &succList = result.succs[static_cast<std::size_t>(from)];
  if (std::find(succList.begin(), succList.end(), to) == succList.end()) {
    succList.push_back(to);
  }
  auto &predList = result.preds[static_cast<std::size_t>(to)];
  if (std::find(predList.begin(), predList.end(), from) == predList.end()) {
    predList.push_back(from);
  }
}

// 判断操作数是否为虚拟寄存器。
bool isVirtualReg(const MachineOperand &operand) {
  return operand.kind == OperandKind::Reg && !operand.regValue.physical;
}

// 收集指令所有使用端的虚拟寄存器 id。
std::vector<uint32_t> virtualUses(const MachineInstr &instr) {
  std::vector<uint32_t> result;
  for (const MachineOperand &operand : instr.uses) {
    if (isVirtualReg(operand)) {
      result.push_back(operand.regValue.id);
    }
  }
  return result;
}

// 收集指令所有定义端的虚拟寄存器 id。
std::vector<uint32_t> virtualDefs(const MachineInstr &instr) {
  std::vector<uint32_t> result;
  for (const MachineOperand &operand : instr.defs) {
    if (isVirtualReg(operand)) {
      result.push_back(operand.regValue.id);
    }
  }
  return result;
}

} // namespace

// 各分析 pass 的唯一标识键(用于分析管理器区分与缓存不同分析结果)。
MachinePassID MachineInstrStatsAnalysis::Key;
MachinePassID MachineCFGAnalysis::Key;
MachinePassID MachineLivenessAnalysis::Key;
MachinePassID MachineLoopAnalysis::Key;
MachinePassID MachineBlockFrequencyAnalysis::Key;

// 指令统计分析:统计块数、指令总数,以及拷贝类、调用、PHI 指令的条数。
// 供报告与调试使用,不参与正确性判定。
MachineInstrStatsAnalysis::Result
MachineInstrStatsAnalysis::run(MachineFunction &mf,
                               MachineFunctionAnalysisManager &am) const {
  (void)am;
  Result result;
  result.blockCount = mf.blocks.size();
  for (const MachineBasicBlock &block : mf.blocks) {
    result.instrCount += block.instrs.size();
    for (const MachineInstr &instr : block.instrs) {
      switch (instr.opcode) {
      case Opcode::COPY:
      case Opcode::F_COPY:
      case Opcode::PARALLEL_COPY:
        ++result.copyCount;
        break;
      case Opcode::CALL:
        ++result.callCount;
        break;
      case Opcode::PHI:
        ++result.phiCount;
        break;
      default:
        break;
      }
    }
  }
  return result;
}

// CFG 分析:为每个块建立前驱与后继表。
// 关键步骤:先建标签到块下标的哈希映射;再逐块扫描终结指令的块标签操作数
// 加显式跳转边;若块可顺序落入,则再加一条到下一块的落入边。
MachineCFGAnalysis::Result
MachineCFGAnalysis::run(MachineFunction &mf,
                        MachineFunctionAnalysisManager &am) const {
  (void)am;
  Result result;
  const std::size_t blockCount = mf.blocks.size();
  result.preds.resize(blockCount);
  result.succs.resize(blockCount);

  // 标签到块下标的映射，避免逐块线性查找
  std::unordered_map<std::string, int> labelToIndex;
  for (std::size_t i = 0; i < blockCount; ++i) {
    labelToIndex[mf.blocks[i].label] = static_cast<int>(i);
  }

  for (std::size_t i = 0; i < blockCount; ++i) {
    for (const MachineInstr &instr : mf.blocks[i].instrs) {
      if (!instr.terminator && !isTerminatorOpcode(instr.opcode)) {
        continue;
      }
      for (const MachineOperand &use : instr.uses) {
        if (use.kind != OperandKind::Block) {
          continue;
        }
        auto it = labelToIndex.find(use.blockLabel);
        if (it == labelToIndex.end()) {
          throw BackendError("machine branch targets unknown block " +
                             use.blockLabel);
        }
        const int from = static_cast<int>(i);
        const int to = it->second;
        addEdge(result, from, to);
      }
    }
    if (blockCanFallThrough(mf.blocks[i]) && i + 1 < blockCount) {
      addEdge(result, static_cast<int>(i), static_cast<int>(i + 1));
    }
  }
  return result;
}

// 活跃变量分析:计算每个块的 liveIn/liveOut(以虚拟寄存器 id 为单位)。
// 先求各块的局部 use(在本块被定义前先被使用)与 def 集合;再用标准的数据流
// 迭代到不动点:liveOut = 各后继 liveIn 之并,liveIn = use 并 (liveOut 减 def)。
// 按块下标逆序迭代以加快收敛。
MachineLivenessAnalysis::Result
MachineLivenessAnalysis::run(MachineFunction &mf,
                             MachineFunctionAnalysisManager &am) const {
  Result live;
  const auto &cfg = am.getResult<MachineCFGAnalysis>(mf);
  const std::size_t blockCount = mf.blocks.size();
  live.uses.resize(blockCount);
  live.defs.resize(blockCount);
  live.liveIn.resize(blockCount);
  live.liveOut.resize(blockCount);

  for (std::size_t blockIndex = 0; blockIndex < blockCount; ++blockIndex) {
    for (const MachineInstr &instr : mf.blocks[blockIndex].instrs) {
      for (uint32_t use : virtualUses(instr)) {
        // 在本块内此使用之前尚未被定义,才计入块的 use 集(向上暴露的使用)。
        if (live.defs[blockIndex].count(use) == 0) {
          live.uses[blockIndex].insert(use);
        }
      }
      for (uint32_t def : virtualDefs(instr)) {
        live.defs[blockIndex].insert(def);
      }
    }
  }

  bool changed = true;
  while (changed) {
    changed = false;
    // 逆序遍历块(从末块到首块),这样后继的更新能更快回传,减少迭代轮数。
    for (std::size_t reverse = 0; reverse < blockCount; ++reverse) {
      const std::size_t blockIndex = blockCount - reverse - 1;
      RegSet newOut;
      // liveOut = 所有后继 liveIn 的并集。
      for (int succ : cfg.succs[blockIndex]) {
        if (succ < 0 || succ >= static_cast<int>(blockCount)) {
          throw BackendError("invalid CFG successor during liveness analysis");
        }
        const RegSet &succIn = live.liveIn[static_cast<std::size_t>(succ)];
        newOut.insert(succIn.begin(), succIn.end());
      }

      // liveIn = use 并 (liveOut 中未被本块定义覆盖的部分)。
      RegSet newIn = live.uses[blockIndex];
      for (uint32_t value : newOut) {
        if (live.defs[blockIndex].count(value) == 0) {
          newIn.insert(value);
        }
      }
      if (newIn != live.liveIn[blockIndex] ||
          newOut != live.liveOut[blockIndex]) {
        live.liveIn[blockIndex] = std::move(newIn);
        live.liveOut[blockIndex] = std::move(newOut);
        changed = true;
      }
    }
  }
  return live;
}

// 循环分析:计算每个块的循环嵌套深度,并记录所有回边。
// 关键步骤:先从入口做可达性标记;再用迭代法求 dominators;
// 若一条边 from->succ 的目标 succ 支配 from,则该边为回边、succ 为循环头;
// 由回边反向遍历前驱收集自然循环成员,给成员块深度加一。
MachineLoopAnalysis::Result
MachineLoopAnalysis::run(MachineFunction &mf,
                         MachineFunctionAnalysisManager &am) const {
  Result result;
  const auto &cfg = am.getResult<MachineCFGAnalysis>(mf);
  const std::size_t count = mf.blocks.size();
  result.depth.assign(count, 0);
  if (count == 0) {
    return result;
  }

  std::vector<bool> reachable(count, false);
  std::vector<int> reachWork{0};
  reachable[0] = true;
  // 从入口块出发做深度优先可达性标记,只对可达块计算支配关系。
  while (!reachWork.empty()) {
    const int block = reachWork.back();
    reachWork.pop_back();
    for (int succ : cfg.succs[static_cast<std::size_t>(block)]) {
      if (succ < 0 || succ >= static_cast<int>(count)) {
        throw BackendError("invalid CFG successor during loop reachability");
      }
      if (!reachable[static_cast<std::size_t>(succ)]) {
        reachable[static_cast<std::size_t>(succ)] = true;
        reachWork.push_back(succ);
      }
    }
  }

  std::vector<std::set<int>> dominators(count);
  std::set<int> all;
  for (std::size_t i = 0; i < count; ++i) {
    if (reachable[i]) {
      all.insert(static_cast<int>(i));
    }
  }
  // 支配集初值:入口只被自己支配;其余可达块先假设被所有可达块支配,再迭代收缩。
  dominators[0].insert(0);
  for (std::size_t i = 1; i < count; ++i) {
    if (reachable[i]) {
      dominators[i] = all;
    }
  }

  bool changed = true;
  // 迭代求支配集不动点:dom(n) = {n} 并 (所有可达前驱的 dom 之交)。
  while (changed) {
    changed = false;
    for (std::size_t i = 1; i < count; ++i) {
      if (!reachable[i]) {
        continue;
      }
      std::set<int> next;
      bool firstPred = true;
      for (int pred : cfg.preds[i]) {
        if (pred < 0 || pred >= static_cast<int>(count)) {
          throw BackendError("invalid CFG predecessor during loop analysis");
        }
        if (!reachable[static_cast<std::size_t>(pred)]) {
          continue;
        }
        if (firstPred) {
          next = dominators[static_cast<std::size_t>(pred)];
          firstPred = false;
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
      if (firstPred) {
        next.clear();
      }
      next.insert(static_cast<int>(i));
      if (next != dominators[i]) {
        dominators[i] = std::move(next);
        changed = true;
      }
    }
  }

  // 同一 header 的多个 latch 属于同一个自然循环。先保留所有回边明细，
  // 再按 header 合并成员，避免把 latch 数误当成嵌套深度。
  std::map<int, std::set<int>> latchesByHeader;
  // 识别回边:若后继 succ 支配了当前块 from,则 from->succ 是回边,succ 为循环头。
  for (std::size_t from = 0; from < count; ++from) {
    if (!reachable[from]) {
      continue;
    }
    for (int succ : cfg.succs[from]) {
      if (succ < 0 || succ >= static_cast<int>(count)) {
        throw BackendError("invalid CFG successor during loop analysis");
      }
      if (dominators[from].count(succ) == 0) {
        continue;
      }

      result.backedges.emplace(static_cast<int>(from), succ);
      latchesByHeader[succ].insert(static_cast<int>(from));
    }
  }

  for (const auto &[header, latches] : latchesByHeader) {
    std::set<int> members{header};
    std::vector<int> work;
    for (int latch : latches) {
      members.insert(latch);
      // 自环的自然循环只有 header；绝不能从 header 反向走到 preheader。
      if (latch != header) {
        work.push_back(latch);
      }
    }
    // 从各 latch 反向沿前驱扩展,凡被 header 支配的块都属于该自然循环。
    while (!work.empty()) {
      const int block = work.back();
      work.pop_back();
      for (int pred : cfg.preds[static_cast<std::size_t>(block)]) {
        if (pred < 0 || pred >= static_cast<int>(count)) {
          throw BackendError("invalid natural-loop predecessor");
        }
        if (pred != header && reachable[static_cast<std::size_t>(pred)] &&
            dominators[static_cast<std::size_t>(pred)].count(header) != 0 &&
            members.insert(pred).second) {
          work.push_back(pred);
        }
      }
    }
    result.membersByHeader[header] = members;
    for (int block : members) {
      // 循环成员块深度加一;嵌套循环的块会被多个自然循环各加一次,得到嵌套深度。
      ++result.depth[static_cast<std::size_t>(block)];
    }
  }
  return result;
}

namespace {

// 饱和加法:溢出时返回 uint64 最大值,避免频率累加回绕。
uint64_t saturatingAdd(uint64_t lhs, uint64_t rhs) {
  const uint64_t limit = std::numeric_limits<uint64_t>::max();
  return rhs > limit - lhs ? limit : lhs + rhs;
}

// 带溢出标志的饱和加法:一旦发生饱和就把 saturated 置真,供上层标记结果不可靠。
uint64_t saturatingAddChecked(uint64_t lhs, uint64_t rhs, bool &saturated) {
  const uint64_t limit = std::numeric_limits<uint64_t>::max();
  if (rhs > limit - lhs) {
    saturated = true;
    return limit;
  }
  return lhs + rhs;
}

// 计算 value * probability / scale(概率以定点数表示)。拆成整数部分与余数
// 分别乘,避免中间溢出;整数部分若会溢出则直接饱和到最大值。
uint64_t scaledProduct(uint64_t value, uint32_t probability) {
  constexpr uint64_t scale = MachineBlockFrequencyAnalysis::ProbabilityScale;
  const uint64_t whole = value / scale;
  const uint64_t remainder = value % scale;
  const uint64_t limit = std::numeric_limits<uint64_t>::max();
  if (whole != 0 && probability > limit / whole) {
    return limit;
  }
  return saturatingAdd(whole * probability, remainder * probability / scale);
}

// 给一条边打权重偏好(用于分配后继间的执行概率):回边最高(8),进入更深
// 循环次之(4),回到更浅层最低(1),同层默认(2)。偏好越大概率越高。
uint32_t edgePreference(const MachineLoopAnalysis::Result &loops, int from,
                        int to) {
  if (loops.backedges.count({from, to}) != 0) {
    return 8;
  }
  const int fromDepth = loops.depth[static_cast<std::size_t>(from)];
  const int toDepth = loops.depth[static_cast<std::size_t>(to)];
  if (toDepth > fromDepth) {
    return 4;
  }
  if (toDepth < fromDepth) {
    return 1;
  }
  return 2;
}

// 判断 CFG 中是否存在"封闭的有环强连通分量":内部有环、且没有任何出边离开
// 该分量(即不可退出的死循环)。用 Tarjan 求强连通分量。存在这类分量时
// 频率估计不再可靠(无法收敛到有限期望访问次数)。
bool hasClosedCyclicSCC(const std::vector<std::vector<int>> &successors) {
  const std::size_t count = successors.size();
  if (count == 0) {
    return false;
  }
  std::vector<bool> reachable(count, false);
  std::vector<int> reachWork{0};
  reachable[0] = true;
  while (!reachWork.empty()) {
    const int node = reachWork.back();
    reachWork.pop_back();
    for (int succ : successors[static_cast<std::size_t>(node)]) {
      if (succ < 0 || succ >= static_cast<int>(count)) {
        throw BackendError("invalid CFG successor during SCC reachability");
      }
      if (!reachable[static_cast<std::size_t>(succ)]) {
        reachable[static_cast<std::size_t>(succ)] = true;
        reachWork.push_back(succ);
      }
    }
  }
  std::vector<int> index(count, -1);
  std::vector<int> lowlink(count, -1);
  std::vector<int> stack;
  std::vector<bool> onStack(count, false);
  std::vector<int> componentMark(count, 0);
  int componentEpoch = 0;
  int nextIndex = 0;
  bool found = false;

  std::function<void(int)> visit = [&](int node) {
    // Tarjan 递归:为节点赋 index/lowlink 并入栈;遍历后继更新 lowlink;
    // 当 lowlink == index 时弹出栈得到一个强连通分量。
    const std::size_t nodeIndex = static_cast<std::size_t>(node);
    index[nodeIndex] = lowlink[nodeIndex] = nextIndex++;
    stack.push_back(node);
    onStack[nodeIndex] = true;
    for (int succ : successors[nodeIndex]) {
      if (succ < 0 || succ >= static_cast<int>(count)) {
        throw BackendError("invalid CFG successor during SCC analysis");
      }
      const std::size_t succIndex = static_cast<std::size_t>(succ);
      if (!reachable[succIndex]) {
        continue;
      }
      if (index[succIndex] < 0) {
        visit(succ);
        lowlink[nodeIndex] = std::min(lowlink[nodeIndex], lowlink[succIndex]);
      } else if (onStack[succIndex]) {
        lowlink[nodeIndex] = std::min(lowlink[nodeIndex], index[succIndex]);
      }
    }
    if (lowlink[nodeIndex] != index[nodeIndex]) {
      return;
    }

    std::vector<int> component;
    while (true) {
      const int member = stack.back();
      stack.pop_back();
      onStack[static_cast<std::size_t>(member)] = false;
      component.push_back(member);
      if (member == node) {
        break;
      }
    }
    ++componentEpoch;
    for (int member : component) {
      componentMark[static_cast<std::size_t>(member)] = componentEpoch;
    }
    bool cyclic = component.size() > 1;
    bool closed = true;
    // 分量内含自环或多节点即为有环;若有后继指向分量外则不封闭。
    for (int member : component) {
      for (int succ : successors[static_cast<std::size_t>(member)]) {
        if (succ == member) {
          cyclic = true;
        }
        if (componentMark[static_cast<std::size_t>(succ)] != componentEpoch) {
          closed = false;
        }
      }
    }
    found = found || (cyclic && closed);
  };

  for (std::size_t node = 0; node < count; ++node) {
    if (reachable[node] && index[node] < 0) {
      visit(static_cast<int>(node));
    }
  }
  return found;
}

// ==== P1#10:机器层循环 trip count 估计 ====

// P1#10 A/B 开关:BACKEND_DISABLE_PASSES 含 block-placement-rotate(或 all)时,
// 关闭"真循环旋转 + trip count 注入",退回旧的固定偏好频率模型与旧布局决策。
bool blockPlacementRotateDisabled() {
  const char *raw = std::getenv("BACKEND_DISABLE_PASSES");
  if (raw == nullptr) {
    return false;
  }
  std::string text(raw);
  for (char &ch : text) {
    if (ch == ',' || ch == ';') {
      ch = ' ';
    }
  }
  std::istringstream input(text);
  std::string name;
  while (input >> name) {
    std::string normalized;
    normalized.reserve(name.size());
    for (char ch : name) {
      normalized.push_back(
          ch == '_'
              ? '-'
              : static_cast<char>(
                    std::tolower(static_cast<unsigned char>(ch))));
    }
    if (normalized == "all" || normalized == "block-placement-rotate") {
      return true;
    }
  }
  return false;
}

// 操作数是否引用物理寄存器(分配后所有寄存器均已物理化)。
bool isPhysRegOperand(const MachineOperand &operand) {
  return operand.kind == OperandKind::Reg && operand.regValue.physical;
}

// 两个寄存器引用是否指向同一物理寄存器。
bool samePhysicalReg(const MachineReg &lhs, const MachineReg &rhs) {
  return lhs.physical && rhs.physical && lhs.phys == rhs.phys;
}

// 单个自然循环的 trip count 估计:估计值 + 循环体成员 + header 的继续/退出后继。
// continueSucc/exitSucc 仅在 header 恰好有两个后继且一内一外时有效。
struct LoopTripEstimate {
  uint64_t tripCount = 0;
  std::set<int> body;
  int continueSucc = -1;
  int exitSucc = -1;
};

// 从 guard 指令提取比较操作数对。guard 可以是直接比较分支(BLT/BGE/BEQ/BNE),
// 也可以是 BEQZ/BNEZ + 同块内前驱 SLT/SLTU 的组合。无法识别时返回 false。
bool guardCompareRegs(const MachineBasicBlock &headerBlock,
                      const MachineInstr &guard, MachineReg &op1,
                      MachineReg &op2) {
  switch (guard.opcode) {
  case Opcode::BEQ:
  case Opcode::BNE:
  case Opcode::BLT:
  case Opcode::BGE: {
    MachineReg first;
    MachineReg second;
    int regCount = 0;
    for (const MachineOperand &use : guard.uses) {
      if (use.kind == OperandKind::Block || !isPhysRegOperand(use)) {
        continue;
      }
      if (regCount == 0) {
        first = use.regValue;
      } else if (regCount == 1) {
        second = use.regValue;
      } else {
        return false;
      }
      ++regCount;
    }
    if (regCount != 2) {
      return false;
    }
    op1 = first;
    op2 = second;
    return true;
  }
  case Opcode::BEQZ:
  case Opcode::BNEZ: {
    if (guard.uses.empty() || !isPhysRegOperand(guard.uses[0])) {
      return false;
    }
    const MachineReg cond = guard.uses[0].regValue;
    // 回溯查找定义条件寄存器的 SLT/SLTU(即循环条件比较)。
    for (auto it = headerBlock.instrs.rbegin(); it != headerBlock.instrs.rend();
         ++it) {
      if (it->opcode != Opcode::SLT && it->opcode != Opcode::SLTU) {
        continue;
      }
      if (it->defs.size() != 1 || !isPhysRegOperand(it->defs[0]) ||
          !samePhysicalReg(it->defs[0].regValue, cond)) {
        continue;
      }
      if (it->uses.size() < 2 || !isPhysRegOperand(it->uses[0]) ||
          !isPhysRegOperand(it->uses[1])) {
        return false;
      }
      op1 = it->uses[0].regValue;
      op2 = it->uses[1].regValue;
      return true;
    }
    return false;
  }
  default:
    return false;
  }
}

// 判断指令是否为 reg 的"常量步长自增"定义(ADDI/ADDIW/ADD/ADDW),返回步长;
// 不匹配时返回 0。
int64_t inductionStepOf(const MachineInstr &instr, const MachineReg &reg) {
  switch (instr.opcode) {
  case Opcode::ADDI:
  case Opcode::ADDIW:
  case Opcode::ADD:
  case Opcode::ADDW:
    break;
  default:
    return 0;
  }
  if (instr.defs.size() != 1 || !isPhysRegOperand(instr.defs[0]) ||
      !samePhysicalReg(instr.defs[0].regValue, reg)) {
    return 0;
  }
  if (instr.uses.size() < 2 || !isPhysRegOperand(instr.uses[0]) ||
      !samePhysicalReg(instr.uses[0].regValue, reg)) {
    return 0;
  }
  if (instr.uses[1].kind != OperandKind::Imm) {
    return 0;
  }
  return instr.uses[1].immValue;
}

// 在 header 块内、guard 之前查找 limit 寄存器的常量定义(LI imm)。
bool headerConstantLimit(const MachineBasicBlock &headerBlock,
                         const MachineInstr &guard, const MachineReg &limitReg,
                         int64_t &limit) {
  for (const MachineInstr &instr : headerBlock.instrs) {
    if (&instr == &guard) {
      break;
    }
    if (instr.opcode != Opcode::LI || instr.defs.size() != 1 ||
        !isPhysRegOperand(instr.defs[0]) ||
        !samePhysicalReg(instr.defs[0].regValue, limitReg)) {
      continue;
    }
    if (instr.uses.size() != 1 || instr.uses[0].kind != OperandKind::Imm) {
      return false;
    }
    limit = instr.uses[0].immValue;
    return true;
  }
  return false;
}

// limit 寄存器是否在循环体内被定义(是则不是循环不变量,无法估计)。
bool limitDefinedInLoop(const std::set<int> &body, const MachineFunction &mf,
                        const MachineReg &limitReg) {
  for (int block : body) {
    for (const MachineInstr &instr :
         mf.blocks[static_cast<std::size_t>(block)].instrs) {
      for (const MachineOperand &def : instr.defs) {
        if (isPhysRegOperand(def) && samePhysicalReg(def.regValue, limitReg)) {
          return true;
        }
      }
    }
  }
  return false;
}

// 机器层循环 trip count 估计:对每个"归纳寄存器 + 常量/不变量界限"的计数循环,
// 估计迭代次数;其余循环不估计。估计值用于把 header guard 的继续/退出概率从
// 固定偏好(2/3)提升为 T/(T+1),使块布局的旋转/排序决策与真实循环次数一致。
// 与 RegAlloc::computeNaturalLoops 使用同一 MachineLoopAnalysis::Result 口径
// (按 header 合并 latch、从 latch 反向扩展循环体)。
std::map<int, LoopTripEstimate>
estimateMachineTripCounts(const MachineFunction &mf,
                          const MachineCFGAnalysis::Result &cfg,
                          const MachineLoopAnalysis::Result &loops) {
  std::map<int, LoopTripEstimate> result;
  const std::size_t count = mf.blocks.size();
  if (count == 0) {
    return result;
  }

  // 按 header 合并 latch(与 RegAlloc::computeNaturalLoops 相同口径)。
  std::map<int, std::set<int>> latchesByHeader;
  for (const auto &[latch, header] : loops.backedges) {
    latchesByHeader[header].insert(latch);
  }
  auto loopBody = [&](int header, const std::set<int> &latches) {
    std::set<int> body;
    std::vector<int> work(latches.begin(), latches.end());
    while (!work.empty()) {
      const int cur = work.back();
      work.pop_back();
      if (cur == header || !body.insert(cur).second) {
        continue;
      }
      for (int pred : cfg.preds[static_cast<std::size_t>(cur)]) {
        if (pred < 0 || pred >= static_cast<int>(count)) {
          throw BackendError("invalid natural-loop predecessor");
        }
        work.push_back(pred);
      }
    }
    body.insert(header);
    return body;
  };

  for (const auto &[header, latches] : latchesByHeader) {
    if (header < 0 || header >= static_cast<int>(count)) {
      continue;
    }
    // 多 latch 循环(如 continue 制造的第二条回边)暂不估计,保持保守。
    if (latches.size() != 1) {
      continue;
    }
    const MachineBasicBlock &headerBlock =
        mf.blocks[static_cast<std::size_t>(header)];
    // 找 header 内第一条条件分支作为循环 guard。
    const MachineInstr *guard = nullptr;
    for (const MachineInstr &instr : headerBlock.instrs) {
      switch (instr.opcode) {
      case Opcode::BEQZ:
      case Opcode::BNEZ:
      case Opcode::BEQ:
      case Opcode::BNE:
      case Opcode::BLT:
      case Opcode::BGE:
        guard = &instr;
        break;
      default:
        break;
      }
      if (guard != nullptr) {
        break;
      }
    }
    if (guard == nullptr) {
      continue;
    }

    MachineReg op1;
    MachineReg op2;
    if (!guardCompareRegs(headerBlock, *guard, op1, op2)) {
      continue;
    }
    const std::set<int> body = loopBody(header, latches);

    // 归纳寄存器:比较操作数中在循环体内有常量自增定义者(只允许恰好一个)。
    int64_t step1 = 0;
    int64_t step2 = 0;
    for (int block : body) {
      for (const MachineInstr &instr :
           mf.blocks[static_cast<std::size_t>(block)].instrs) {
        const int64_t s1 = inductionStepOf(instr, op1);
        if (s1 != 0) {
          step1 = s1;
        }
        const int64_t s2 = inductionStepOf(instr, op2);
        if (s2 != 0) {
          step2 = s2;
        }
      }
    }
    MachineReg iv;
    MachineReg limitReg;
    int64_t step = 0;
    bool ivIsOp1 = false;
    if (step1 != 0 && step2 == 0) {
      iv = op1;
      limitReg = op2;
      step = step1;
      ivIsOp1 = true;
    } else if (step2 != 0 && step1 == 0) {
      iv = op2;
      limitReg = op1;
      step = step2;
      ivIsOp1 = false;
    } else {
      continue;
    }

    // guard 的目标是否在循环体内(决定分支方向语义)。
    std::string guardTarget;
    for (const MachineOperand &use : guard->uses) {
      if (use.kind == OperandKind::Block) {
        guardTarget = use.blockLabel;
        break;
      }
    }
    if (guardTarget.empty()) {
      continue;
    }
    bool targetInBody = false;
    for (int block : body) {
      if (mf.blocks[static_cast<std::size_t>(block)].label == guardTarget) {
        targetInBody = true;
        break;
      }
    }

    // 推导"继续执行循环"的条件方向:小于(递增)/不等于。
    enum class TripDirection { None, Less, Neq };
    TripDirection direction = TripDirection::None;
    switch (guard->opcode) {
    case Opcode::BLT:
      // BLT op1,op2,T:目标在体内则 op1<op2 时继续,否则不估计。
      direction = targetInBody ? TripDirection::Less : TripDirection::None;
      break;
    case Opcode::BGE:
      // BGE op1,op2,T:目标在体外则 op1<op2 时继续,否则不估计。
      direction = targetInBody ? TripDirection::None : TripDirection::Less;
      break;
    case Opcode::BNE:
      direction = targetInBody ? TripDirection::Neq : TripDirection::None;
      break;
    case Opcode::BEQ:
      direction = targetInBody ? TripDirection::None : TripDirection::Neq;
      break;
    case Opcode::BNEZ:
      // SLT/SLTU op1,op2 为真时继续:op1 < op2。
      direction = TripDirection::Less;
      break;
    case Opcode::BEQZ:
      // SLT op1,op2 为假时继续:op1 >= op2(少见,不估计)。
      direction = TripDirection::None;
      break;
    default:
      break;
    }
    if (direction == TripDirection::None) {
      continue;
    }

    // 计算 trip count:先取 limit 常量,再按方向与步长估计。
    uint64_t trip = 0;
    bool haveConst = false;
    int64_t limitConst = 0;
    for (const MachineOperand &use : guard->uses) {
      if (use.kind == OperandKind::Imm) {
        limitConst = use.immValue;
        haveConst = true;
        break;
      }
    }
    if (!haveConst) {
      haveConst = headerConstantLimit(headerBlock, *guard, limitReg, limitConst);
    }
    if (direction == TripDirection::Less) {
      if (haveConst) {
        if (ivIsOp1) {
          // 递增:iv 从 0(或更小)增至 limit,迭代数约 limit/step。
          if (step > 0 && limitConst > 0) {
            trip = static_cast<uint64_t>(limitConst) /
                   static_cast<uint64_t>(step);
          }
        } else {
          // 递减:iv 从 0 减至 limit(limit<0),迭代数约 -limit/|step|。
          if (step < 0 && limitConst < 0) {
            trip = static_cast<uint64_t>(-limitConst) /
                   static_cast<uint64_t>(-step);
          }
        }
      } else if (!limitDefinedInLoop(body, mf, limitReg)) {
        // 界限是循环不变量(如数组长度参数),默认估计 8 次迭代。
        trip = 8;
      }
    } else if (direction == TripDirection::Neq) {
      if (haveConst) {
        const int64_t absLimit = limitConst < 0 ? -limitConst : limitConst;
        const int64_t absStep = step < 0 ? -step : step;
        if (absStep > 0 && absLimit > 0) {
          trip = static_cast<uint64_t>(absLimit) /
                 static_cast<uint64_t>(absStep);
        }
      } else if (!limitDefinedInLoop(body, mf, limitReg)) {
        trip = 8;
      }
    }
    // 迭代过少(或无法估计)时不动频率,保持旧行为;防止数值过大。
    if (trip < 4 || trip > (1ULL << 20)) {
      continue;
    }

    // 识别 header 的继续/退出后继(一内一外),供频率覆盖使用。
    LoopTripEstimate estimate;
    estimate.tripCount = trip;
    estimate.body = body;
    const std::vector<int> &succs =
        cfg.succs[static_cast<std::size_t>(header)];
    for (int succ : succs) {
      if (succ < 0 || succ >= static_cast<int>(count)) {
        throw BackendError("invalid CFG successor during trip count estimate");
      }
      if (body.count(succ) != 0) {
        estimate.continueSucc = succ;
      } else {
        estimate.exitSucc = succ;
      }
    }
    if (estimate.continueSucc >= 0 && estimate.exitSucc >= 0 &&
        succs.size() == 2) {
      result[header] = std::move(estimate);
    }
  }
  return result;
}

} // namespace

// 块频率分析:估计每个块相对入口的执行频率与每条边的权重。
// 关键步骤:先由循环偏好给每个块的各条出边分配定点概率(和为 ProbabilityScale);
// 再以入口频率为基准,做有界轮数的期望访问次数传播直到稳定;最后由块频率乘
// 边概率得到边权重。存在不可退出环或发生数值饱和时把 reliable 标记为假。
MachineBlockFrequencyAnalysis::Result
MachineBlockFrequencyAnalysis::run(MachineFunction &mf,
                                   MachineFunctionAnalysisManager &am) const {
  Result result;
  const auto &cfg = am.getResult<MachineCFGAnalysis>(mf);
  const auto &loops = am.getResult<MachineLoopAnalysis>(mf);
  const std::size_t count = mf.blocks.size();
  result.blockFrequency.assign(count, 0);
  result.successors = cfg.succs;
  result.edgeProbability.resize(count);
  result.edgeWeight.resize(count);
  if (count == 0) {
    result.converged = true;
    result.reliable = true;
    return result;
  }
  // 可靠性前提:入口无前驱,且不存在不可退出的有环强连通分量。
  result.reliable = cfg.preds[0].empty() && !hasClosedCyclicSCC(cfg.succs);

  for (std::size_t from = 0; from < count; ++from) {
    const auto &succs = cfg.succs[from];
    auto &probabilities = result.edgeProbability[from];
    probabilities.assign(succs.size(), 0);
    if (succs.empty()) {
      continue;
    }
    // 唯一后继必然被执行,概率取满刻度。
    if (succs.size() == 1) {
      probabilities[0] = ProbabilityScale;
      continue;
    }

    // 多后继:按各边偏好占总偏好的比例分配概率。
    std::vector<uint32_t> preferences;
    preferences.reserve(succs.size());
    uint32_t totalPreference = 0;
    for (int succ : succs) {
      if (succ < 0 || succ >= static_cast<int>(count)) {
        throw BackendError("invalid CFG successor during frequency analysis");
      }
      const uint32_t preference =
          edgePreference(loops, static_cast<int>(from), succ);
      preferences.push_back(preference);
      totalPreference += preference;
    }
    uint32_t assigned = 0;
    for (std::size_t i = 0; i < succs.size(); ++i) {
      probabilities[i] = ProbabilityScale * preferences[i] / totalPreference;
      assigned += probabilities[i];
    }
    // 整除余数按原 successor 顺序发放；该顺序由原块下标确定。
    for (std::size_t i = 0; assigned < ProbabilityScale; ++assigned) {
      ++probabilities[i % probabilities.size()];
    }
  }

  // P1#10:trip count 注入——对可识别的计数循环,用估计迭代数 T 覆盖 header
  // guard 的继续/退出概率(从固定偏好 2/3 提升为 T/(T+1)),使布局的旋转与
  // 排序决策与真实循环次数一致。开关:BACKEND_DISABLE_PASSES=block-placement-rotate。
  if (!blockPlacementRotateDisabled()) {
    const std::map<int, LoopTripEstimate> tripCounts =
        estimateMachineTripCounts(mf, cfg, loops);
    for (const auto &[header, estimate] : tripCounts) {
      auto &probabilities =
          result.edgeProbability[static_cast<std::size_t>(header)];
      if (probabilities.size() != 2) {
        continue;
      }
      std::size_t continueIndex = probabilities.size();
      std::size_t exitIndex = probabilities.size();
      const std::vector<int> &succs =
          cfg.succs[static_cast<std::size_t>(header)];
      for (std::size_t i = 0; i < succs.size(); ++i) {
        if (succs[i] == estimate.continueSucc) {
          continueIndex = i;
        }
        if (succs[i] == estimate.exitSucc) {
          exitIndex = i;
        }
      }
      if (continueIndex >= probabilities.size() ||
          exitIndex >= probabilities.size()) {
        continue;
      }
      const uint64_t total = estimate.tripCount + 1;
      const uint32_t continueProb = static_cast<uint32_t>(
          ProbabilityScale * estimate.tripCount / total);
      probabilities[continueIndex] = continueProb;
      probabilities[exitIndex] = ProbabilityScale - continueProb;
    }
    if (std::getenv("BACKEND_BLOCK_PLACEMENT_STATS") != nullptr) {
      for (const auto &[header, estimate] : tripCounts) {
        std::cerr << "# machine-trip-count-stats header=" << header
                  << " trip=" << estimate.tripCount
                  << " continue=" << estimate.continueSucc
                  << " exit=" << estimate.exitSucc << '\n';
      }
    }
  }

  // entry 的基准频率为 1（用 1024 表示）。这是确定性的 expected-visit
  // 有界估计。P1#10 把可估计循环的继续概率提升为 T/(T+1) 后,收敛轮数与 trip
  // count 同阶(如 T=250 的循环约需数百轮),旧 96 轮上限会让嵌套循环内层块
  // 的频率停在 0,导致旋转决策失效;上限提高到 4096 后常见计数循环可完全
  // 收敛,超大 trip count 的循环频率按轮数线性有界,相对量级仍正确。
  // converged 仍只记录是否达到精确定点;普通可退出循环即使尚未精确稳定,
  // 估计仍可用于布局。
  std::vector<uint64_t> frequencies(count, 0);
  frequencies[0] = ProbabilityScale;
  bool propagationSaturated = false;
  // 迭代传播:每轮把各块频率按出边概率贡献给后继;入口频率固定为基准值。
  // 到达 4096 轮上限或前后一致即停止;精确稳定时置 converged。
  for (int iteration = 0; iteration < 4096; ++iteration) {
    std::vector<uint64_t> next(count, 0);
    next[0] = ProbabilityScale;
    for (std::size_t from = 0; from < count; ++from) {
      for (std::size_t edge = 0; edge < cfg.succs[from].size(); ++edge) {
        const int to = cfg.succs[from][edge];
        if (to == 0) {
          continue;
        }
        const uint64_t contribution = scaledProduct(
            frequencies[from], result.edgeProbability[from][edge]);
        next[static_cast<std::size_t>(to)] =
            saturatingAddChecked(next[static_cast<std::size_t>(to)],
                                 contribution, propagationSaturated);
      }
    }
    if (next == frequencies) {
      frequencies = std::move(next);
      result.converged = true;
      break;
    }
    frequencies = std::move(next);
  }
  result.blockFrequency = std::move(frequencies);
  if (propagationSaturated) {
    result.reliable = false;
  }

  // 由块频率与边概率得到边权重,供块布局等后续使用。
  for (std::size_t from = 0; from < count; ++from) {
    auto &weights = result.edgeWeight[from];
    weights.reserve(cfg.succs[from].size());
    for (uint32_t probability : result.edgeProbability[from]) {
      weights.push_back(
          scaledProduct(result.blockFrequency[from], probability));
    }
  }
  return result;
}

} // namespace aarch64
