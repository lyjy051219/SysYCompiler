#include "../../../include/riscv/RegAlloc.hpp"

#include "../../../include/riscv/Registers.hpp"
#include "../../../include/riscv/Target.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace riscv {
namespace {

using VRegSet = std::set<uint32_t>;
using Node = uint32_t;

constexpr Node kPhysicalNodeBase = 0x80000000U;
constexpr int kEmergencyRoundLimit = 128;

bool isVirtualReg(const MachineOperand &operand) {
  return operand.kind == OperandKind::Reg && !operand.regValue.physical;
}

bool isPhysicalReg(const MachineOperand &operand) {
  return operand.kind == OperandKind::Reg && operand.regValue.physical;
}

bool isFloatClass(RegClass rc) { return rc == RegClass::FPR32; }

bool sameBank(RegClass lhs, RegClass rhs) {
  return isFloatClass(lhs) == isFloatClass(rhs);
}

bool sameBank(RegClass rc, PhysReg reg) {
  return isFloatClass(rc) == getRegisterInfo(reg).isFloat;
}

// 函数特征分类：叶子函数（无CALL）可以安全使用A0-A7作为普通寄存器
enum class FunctionClass { Leaf, General };

struct FunctionProfile {
  FunctionClass fnClass = FunctionClass::General;
  bool hasCall = false;
  uint64_t totalInstrs = 0;
};

FunctionProfile analyzeFunctionProfile(const MachineFunction &mf) {
  FunctionProfile profile;
  profile.totalInstrs = 0;
  for (const MachineBasicBlock &block : mf.blocks) {
    for (const MachineInstr &instr : block.instrs) {
      ++profile.totalInstrs;
      if (instr.opcode == Opcode::CALL) {
        profile.hasCall = true;
      }
    }
  }
  profile.fnClass = profile.hasCall ? FunctionClass::General : FunctionClass::Leaf;
  return profile;
}

// 判断物理寄存器是否为参数寄存器（A0-A7 或 FA0-FA7）
bool isArgReg(PhysReg reg) {
  switch (reg) {
  case PhysReg::A0: case PhysReg::A1: case PhysReg::A2: case PhysReg::A3:
  case PhysReg::A4: case PhysReg::A5: case PhysReg::A6: case PhysReg::A7:
  case PhysReg::FA0: case PhysReg::FA1: case PhysReg::FA2: case PhysReg::FA3:
  case PhysReg::FA4: case PhysReg::FA5: case PhysReg::FA6: case PhysReg::FA7:
    return true;
  default:
    return false;
  }
}

// 自适应颜色集：包含A0-A7/FA0-FA7，保留S10/S11/FS10/FS11作为专用溢出寄存器
// 可用颜色从原18增至24(leaf)/24(general)
std::vector<PhysReg> allocationColors(RegClass rc, bool /*isLeaf*/) {
  std::vector<PhysReg> result;
  for (PhysReg reg : allocatableRegs(rc)) {
    // 保留 S10/S11 和 FS10/FS11 作为专用溢出寄存器
    if (reg == PhysReg::S10 || reg == PhysReg::S11 ||
        reg == PhysReg::FS10 || reg == PhysReg::FS11) {
      continue;
    }
    // X0, RA, SP, GP, TP, S0 的 allocatable=false，已被过滤
    result.push_back(reg);
  }
  return result;
}

// 着色优先级顺序：叶子函数优先caller-saved（无需保存），非叶子函数优先callee-saved
std::vector<PhysReg> colorPriorityOrder(RegClass rc, bool isLeaf) {
  std::vector<PhysReg> colors = allocationColors(rc, isLeaf);
  if (!isLeaf) {
    // 非叶子函数：callee-saved优先（避免跨调用保存/恢复）
    // 排序：S1-S9, S11 (callee-saved) 在前，A0-A7, T0-T6 (caller-saved) 在后
    std::sort(colors.begin(), colors.end(), [](PhysReg a, PhysReg b) {
      bool aCallee = getRegisterInfo(a).calleeSaved;
      bool bCallee = getRegisterInfo(b).calleeSaved;
      if (aCallee != bCallee) return aCallee > bCallee;
      return static_cast<uint16_t>(a) < static_cast<uint16_t>(b);
    });
  } else {
    // 叶子函数：caller-saved优先（无需保存/恢复开销）
    std::sort(colors.begin(), colors.end(), [](PhysReg a, PhysReg b) {
      bool aCaller = getRegisterInfo(a).callerSaved;
      bool bCaller = getRegisterInfo(b).callerSaved;
      if (aCaller != bCaller) return aCaller > bCaller;
      return static_cast<uint16_t>(a) < static_cast<uint16_t>(b);
    });
  }
  return colors;
}

Node physicalNode(PhysReg reg) {
  return kPhysicalNodeBase + static_cast<Node>(reg);
}

bool isPhysicalNode(Node node) { return node >= kPhysicalNodeBase; }

PhysReg nodePhysicalReg(Node node) {
  return static_cast<PhysReg>(node - kPhysicalNodeBase);
}

void rememberClass(std::map<uint32_t, RegClass> &classes, MachineReg reg) {
  auto [it, inserted] = classes.emplace(reg.id, reg.rc);
  if (!inserted && it->second != reg.rc) {
    throw BackendError("virtual register used with inconsistent classes");
  }
}

std::map<uint32_t, RegClass> collectVRegs(const MachineFunction &mf) {
  std::map<uint32_t, RegClass> classes;
  for (const MachineBasicBlock &block : mf.blocks) {
    for (const MachineInstr &instr : block.instrs) {
      for (const MachineOperand &operand : instr.defs) {
        if (isVirtualReg(operand)) {
          rememberClass(classes, operand.regValue);
        }
      }
      for (const MachineOperand &operand : instr.uses) {
        if (isVirtualReg(operand)) {
          rememberClass(classes, operand.regValue);
        }
      }
    }
  }
  return classes;
}

std::vector<uint32_t> virtualUses(const MachineInstr &instr) {
  std::vector<uint32_t> result;
  for (const MachineOperand &operand : instr.uses) {
    if (isVirtualReg(operand)) {
      result.push_back(operand.regValue.id);
    }
  }
  return result;
}

std::vector<uint32_t> virtualDefs(const MachineInstr &instr) {
  std::vector<uint32_t> result;
  for (const MachineOperand &operand : instr.defs) {
    if (isVirtualReg(operand)) {
      result.push_back(operand.regValue.id);
    }
  }
  return result;
}

struct Liveness {
  std::vector<VRegSet> uses;
  std::vector<VRegSet> defs;
  std::vector<VRegSet> liveIn;
  std::vector<VRegSet> liveOut;
};

Liveness computeLiveness(const MachineFunction &mf) {
  Liveness live;
  const std::size_t blockCount = mf.blocks.size();
  live.uses.resize(blockCount);
  live.defs.resize(blockCount);
  live.liveIn.resize(blockCount);
  live.liveOut.resize(blockCount);

  for (std::size_t blockIndex = 0; blockIndex < blockCount; ++blockIndex) {
    for (const MachineInstr &instr : mf.blocks[blockIndex].instrs) {
      for (uint32_t use : virtualUses(instr)) {
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
    for (std::size_t reverse = 0; reverse < blockCount; ++reverse) {
      const std::size_t blockIndex = blockCount - reverse - 1;
      VRegSet newOut;
      for (int succ : mf.blocks[blockIndex].succs) {
        if (succ < 0 || succ >= static_cast<int>(blockCount)) {
          throw BackendError(
              "invalid CFG successor during register allocation");
        }
        const VRegSet &succIn = live.liveIn[static_cast<std::size_t>(succ)];
        newOut.insert(succIn.begin(), succIn.end());
      }

      VRegSet newIn = live.uses[blockIndex];
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

std::vector<int> computeLoopDepth(const MachineFunction &mf) {
  const std::size_t count = mf.blocks.size();
  std::vector<int> depth(count, 0);
  if (count == 0) {
    return depth;
  }

  std::vector<std::set<int>> dominators(count);
  std::set<int> all;
  for (std::size_t i = 0; i < count; ++i) {
    all.insert(static_cast<int>(i));
  }
  dominators[0].insert(0);
  for (std::size_t i = 1; i < count; ++i) {
    dominators[i] = all;
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t i = 1; i < count; ++i) {
      std::set<int> next;
      bool firstPred = true;
      for (int pred : mf.blocks[i].preds) {
        if (pred < 0 || pred >= static_cast<int>(count)) {
          throw BackendError(
              "invalid CFG predecessor during register allocation");
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

  for (std::size_t from = 0; from < count; ++from) {
    for (int succ : mf.blocks[from].succs) {
      if (succ < 0 || succ >= static_cast<int>(count) ||
          dominators[from].count(succ) == 0) {
        continue;
      }

      // 回边源到支配头之间的反向可达块构成自然循环。
      std::set<int> members{succ, static_cast<int>(from)};
      std::vector<int> work{static_cast<int>(from)};
      while (!work.empty()) {
        int block = work.back();
        work.pop_back();
        for (int pred : mf.blocks[static_cast<std::size_t>(block)].preds) {
          if (members.insert(pred).second && pred != succ) {
            work.push_back(pred);
          }
        }
      }
      for (int block : members) {
        ++depth[static_cast<std::size_t>(block)];
      }
    }
  }
  return depth;
}

// 操作数角色分类：用于增强溢出代价模型
enum class OperandRole { Normal, AddressBase, AddressIndex };

struct ProgramInfo {
  std::map<uint32_t, RegClass> classes;
  std::map<uint32_t, uint64_t> heat;
  std::map<uint32_t, uint64_t> liveLength;          // 活跃区间长度（指令数）
  std::map<uint32_t, OperandRole> operandRoles;     // 操作数角色
  std::map<uint32_t, MachineInstr> rematerializable;
  std::set<uint32_t> generatedBySpill;
};

ProgramInfo analyzeProgram(const MachineFunction &mf,
                           const std::set<uint32_t> &generatedBySpill) {
  ProgramInfo info;
  info.classes = collectVRegs(mf);
  info.generatedBySpill = generatedBySpill;
  std::map<uint32_t, int> defCount;
  std::map<uint32_t, MachineInstr> candidates;
  const std::vector<int> loopDepth = computeLoopDepth(mf);

  // 第一遍：收集热度、定义计数、可重物化候选、操作数角色
  for (std::size_t blockIndex = 0; blockIndex < mf.blocks.size();
       ++blockIndex) {
    uint64_t blockHeat = 1;
    for (int i = 0; i < std::min(loopDepth[blockIndex], 3); ++i) {
      blockHeat *= 10;
    }
    for (const MachineInstr &instr : mf.blocks[blockIndex].instrs) {
      // 识别地址基址寄存器和变址寄存器（用于load/store/gep）
      bool isMemoryAccess = (instr.opcode == Opcode::LD ||
                             instr.opcode == Opcode::LW ||
                             instr.opcode == Opcode::SD ||
                             instr.opcode == Opcode::SW ||
                             instr.opcode == Opcode::FLW ||
                             instr.opcode == Opcode::FSW ||
                             instr.opcode == Opcode::LBU ||
                             instr.opcode == Opcode::SB ||
                             instr.opcode == Opcode::GEP);

      for (uint32_t use : virtualUses(instr)) {
        info.heat[use] += 2 * blockHeat;
        // 地址基址寄存器：load/store的第二个use操作数（frameIndex在imm中）
        // GEP指令的第一个use是基址
        if (isMemoryAccess || instr.opcode == Opcode::GEP) {
          info.operandRoles[use] = OperandRole::AddressBase;
        }
      }
      for (uint32_t def : virtualDefs(instr)) {
        info.heat[def] += blockHeat;
        ++defCount[def];
        if ((instr.opcode == Opcode::LI || instr.opcode == Opcode::LA) &&
            instr.defs.size() == 1) {
          candidates[def] = instr;
        }
      }
    }
  }

  // 活跃区间长度近似：基于热度估算（热度已反映循环深度和使用频率）
  // 避免重复调用computeLiveness（在allocateRegistersImpl中会再调用一次）
  for (const auto &[id, rc] : info.classes) {
    (void)rc;
    // 以热度为代理：热度=1对应约10条指令的区间，热度=1000对应约500条指令
    uint64_t h = info.heat.count(id) != 0 ? info.heat.at(id) : 1;
    info.liveLength[id] = std::max<uint64_t>(1, h / 2);
  }

  for (const auto &[id, candidate] : candidates) {
    if (defCount[id] == 1) {
      info.rematerializable.emplace(id, candidate);
    }
  }
  return info;
}

enum class MoveState { WorkList, Active, Coalesced, Constrained, Frozen };

struct Move {
  Node dst = 0;
  Node src = 0;
  MoveState state = MoveState::WorkList;
};

struct AllocationGraph {
  bool floatBank = false;
  bool containsCall = false;
  int colorCount = 0;
  std::set<Node> precolored;
  std::set<Node> initial;
  std::map<Node, std::set<Node>> adjacency;
  std::map<Node, int> degree;
  std::map<Node, std::set<std::size_t>> moveList;
  std::vector<Move> moves;

  bool hasEdge(Node lhs, Node rhs) const {
    auto found = adjacency.find(lhs);
    return found != adjacency.end() && found->second.count(rhs) != 0;
  }

  void addEdge(Node lhs, Node rhs, bool updateDegree) {
    if (lhs == rhs || hasEdge(lhs, rhs)) {
      return;
    }
    adjacency[lhs].insert(rhs);
    adjacency[rhs].insert(lhs);
    if (updateDegree) {
      if (precolored.count(lhs) == 0) {
        ++degree[lhs];
      }
      if (precolored.count(rhs) == 0) {
        ++degree[rhs];
      }
    }
  }
};

bool nodeMatchesBank(Node node, bool floatBank,
                     const std::map<uint32_t, RegClass> &classes) {
  if (isPhysicalNode(node)) {
    return getRegisterInfo(nodePhysicalReg(node)).isFloat == floatBank;
  }
  auto found = classes.find(node);
  return found != classes.end() && isFloatClass(found->second) == floatBank;
}

std::vector<Node>
instructionVirtualNodes(const std::vector<MachineOperand> &operands,
                        bool floatBank,
                        const std::map<uint32_t, RegClass> &classes) {
  std::vector<Node> result;
  for (const MachineOperand &operand : operands) {
    if (isVirtualReg(operand) &&
        nodeMatchesBank(operand.regValue.id, floatBank, classes)) {
      result.push_back(operand.regValue.id);
    }
  }
  return result;
}

std::vector<Node>
instructionPhysicalNodes(const std::vector<MachineOperand> &operands,
                         bool floatBank) {
  std::vector<Node> result;
  for (const MachineOperand &operand : operands) {
    if (!isPhysicalReg(operand)) {
      continue;
    }
    const RegisterInfo &regInfo = getRegisterInfo(operand.regValue.phys);
    if (regInfo.allocatable && regInfo.isFloat == floatBank) {
      result.push_back(physicalNode(operand.regValue.phys));
    }
  }
  return result;
}

void appendImplicitDefs(const MachineInstr &instr, bool floatBank,
                        std::vector<Node> &defs) {
  for (PhysReg reg : instr.implicitDefs) {
    const RegisterInfo &info = getRegisterInfo(reg);
    if (info.allocatable && info.isFloat == floatBank) {
      defs.push_back(physicalNode(reg));
    }
  }
  if (instr.opcode == Opcode::CALL) {
    const RegClass rc = floatBank ? RegClass::FPR32 : RegClass::GPR64;
    for (PhysReg reg : callerSavedRegs(rc)) {
      const RegisterInfo &info = getRegisterInfo(reg);
      if (info.allocatable) {
        defs.push_back(physicalNode(reg));
      }
    }
  }
  std::sort(defs.begin(), defs.end());
  defs.erase(std::unique(defs.begin(), defs.end()), defs.end());
}

bool isCopyForBank(const MachineInstr &instr, bool floatBank) {
  return floatBank ? instr.opcode == Opcode::F_COPY
                   : instr.opcode == Opcode::COPY;
}

AllocationGraph buildAllocationGraph(const MachineFunction &mf,
                                     const ProgramInfo &info,
                                     const Liveness &live, bool floatBank,
                                     bool isLeaf) {
  AllocationGraph graph;
  graph.floatBank = floatBank;
  const RegClass bankClass = floatBank ? RegClass::FPR32 : RegClass::GPR64;
  for (PhysReg reg : allocatableRegs(bankClass)) {
    Node node = physicalNode(reg);
    graph.precolored.insert(node);
    graph.adjacency[node];
  }
  graph.colorCount = static_cast<int>(allocationColors(bankClass, isLeaf).size());

  for (const auto &[id, rc] : info.classes) {
    if (isFloatClass(rc) == floatBank) {
      graph.initial.insert(id);
      graph.adjacency[id];
    }
  }

  for (std::size_t blockIndex = 0; blockIndex < mf.blocks.size();
       ++blockIndex) {
    std::set<Node> currentLive;
    for (uint32_t value : live.liveOut[blockIndex]) {
      if (nodeMatchesBank(value, floatBank, info.classes)) {
        currentLive.insert(value);
      }
    }

    const MachineBasicBlock &block = mf.blocks[blockIndex];
    for (auto it = block.instrs.rbegin(); it != block.instrs.rend(); ++it) {
      if (it->opcode == Opcode::CALL) {
        graph.containsCall = true;
      }
      std::vector<Node> virtualDefs =
          instructionVirtualNodes(it->defs, floatBank, info.classes);
      std::vector<Node> virtualUses =
          instructionVirtualNodes(it->uses, floatBank, info.classes);
      std::vector<Node> physicalDefs =
          instructionPhysicalNodes(it->defs, floatBank);
      std::vector<Node> physicalUses =
          instructionPhysicalNodes(it->uses, floatBank);
      appendImplicitDefs(*it, floatBank, physicalDefs);
      for (PhysReg reg : it->implicitUses) {
        const RegisterInfo &regInfo = getRegisterInfo(reg);
        if (regInfo.allocatable && regInfo.isFloat == floatBank) {
          physicalUses.push_back(physicalNode(reg));
        }
      }

      const bool copy =
          isCopyForBank(*it, floatBank) && it->defs.size() == 1 &&
          it->uses.size() == 1 && it->defs[0].kind == OperandKind::Reg &&
          it->uses[0].kind == OperandKind::Reg &&
          sameBank(it->defs[0].regValue.rc, it->uses[0].regValue.rc) &&
          (!it->defs[0].regValue.physical ||
           getRegisterInfo(it->defs[0].regValue.phys).allocatable) &&
          (!it->uses[0].regValue.physical ||
           getRegisterInfo(it->uses[0].regValue.phys).allocatable);
      if (copy) {
        Node dst = it->defs[0].regValue.physical
                       ? physicalNode(it->defs[0].regValue.phys)
                       : it->defs[0].regValue.id;
        Node src = it->uses[0].regValue.physical
                       ? physicalNode(it->uses[0].regValue.phys)
                       : it->uses[0].regValue.id;
        if (nodeMatchesBank(dst, floatBank, info.classes) &&
            nodeMatchesBank(src, floatBank, info.classes)) {
          currentLive.erase(src);
          const std::size_t moveIndex = graph.moves.size();
          graph.moves.push_back(Move{dst, src, MoveState::WorkList});
          graph.moveList[dst].insert(moveIndex);
          graph.moveList[src].insert(moveIndex);
        }
      }

      // 隐式破坏可能发生在伪指令内部，因此同时保护本条指令的输入和输出。
      std::set<Node> protectedValues = currentLive;
      protectedValues.insert(virtualUses.begin(), virtualUses.end());
      protectedValues.insert(virtualDefs.begin(), virtualDefs.end());
      for (Node def : physicalDefs) {
        for (Node value : protectedValues) {
          graph.addEdge(def, value, false);
        }
      }

      std::vector<Node> allDefs = virtualDefs;
      allDefs.insert(allDefs.end(), physicalDefs.begin(), physicalDefs.end());
      for (Node def : allDefs) {
        for (Node value : currentLive) {
          graph.addEdge(def, value, false);
        }
      }
      for (Node def : virtualDefs) {
        currentLive.erase(def);
      }
      for (Node def : physicalDefs) {
        currentLive.erase(def);
      }
      currentLive.insert(virtualUses.begin(), virtualUses.end());
      currentLive.insert(physicalUses.begin(), physicalUses.end());
    }
  }

  for (Node node : graph.initial) {
    graph.degree[node] = static_cast<int>(graph.adjacency[node].size());
  }
  for (Node node : graph.precolored) {
    graph.degree[node] = std::numeric_limits<int>::max() / 4;
  }
  return graph;
}

struct ColoringResult {
  std::map<uint32_t, PhysReg> colors;
  std::vector<uint32_t> spills;
  std::vector<uint32_t> potentialSpills;
  uint64_t coalescedMoves = 0;
  std::map<uint32_t, std::set<uint32_t>> spillConflicts;
};

class IRCAllocator {
public:
  IRCAllocator(AllocationGraph graph, const ProgramInfo &info, bool isLeaf)
      : graph_(std::move(graph)), info_(info), isLeaf_(isLeaf) {
    for (Node node : graph_.precolored) {
      colors_[node] = nodePhysicalReg(node);
    }
  }

  ColoringResult run() {
    makeWorkLists();
    while (!simplifyWorkList_.empty() || hasWorkListMove() ||
           !freezeWorkList_.empty() || !spillWorkList_.empty()) {
      if (!simplifyWorkList_.empty()) {
        simplify();
      } else if (hasWorkListMove()) {
        coalesce();
      } else if (!freezeWorkList_.empty()) {
        freeze();
      } else {
        selectSpill();
      }
    }
    assignColors();

    ColoringResult result;
    for (const auto &[node, color] : colors_) {
      if (!isPhysicalNode(node)) {
        result.colors.emplace(node, color);
      }
    }
    result.spills.assign(spilledNodes_.begin(), spilledNodes_.end());
    for (Node node : graph_.initial) {
      if (spilledNodes_.count(node) == 0 &&
          info_.generatedBySpill.count(node) == 0 &&
          info_.rematerializable.count(node) == 0) {
        result.potentialSpills.push_back(node);
      }
    }
    std::sort(result.potentialSpills.begin(), result.potentialSpills.end(),
              [&](uint32_t lhs, uint32_t rhs) {
                const uint64_t lhsHeat =
                    info_.heat.count(lhs) != 0 ? info_.heat.at(lhs) : 1;
                const uint64_t rhsHeat =
                    info_.heat.count(rhs) != 0 ? info_.heat.at(rhs) : 1;
                const uint64_t lhsDegree =
                    std::max<std::size_t>(1, graph_.adjacency[lhs].size());
                const uint64_t rhsDegree =
                    std::max<std::size_t>(1, graph_.adjacency[rhs].size());
                const uint64_t lhsScore = lhsHeat * 1024U / lhsDegree;
                const uint64_t rhsScore = rhsHeat * 1024U / rhsDegree;
                return lhsScore != rhsScore ? lhsScore < rhsScore : lhs < rhs;
              });
    for (Node node : graph_.initial) {
      for (Node neighbor : graph_.adjacency[node]) {
        if (!isPhysicalNode(neighbor)) {
          result.spillConflicts[node].insert(neighbor);
        }
      }
    }
    result.coalescedMoves = coalescedMoveCount_;
    return result;
  }

private:
  AllocationGraph graph_;
  const ProgramInfo &info_;
  bool isLeaf_ = false;
  std::set<Node> simplifyWorkList_;
  std::set<Node> freezeWorkList_;
  std::set<Node> spillWorkList_;
  std::set<Node> spilledNodes_;
  std::set<Node> coalescedNodes_;
  std::set<Node> coloredNodes_;
  std::set<Node> stackNodes_;
  std::vector<Node> selectStack_;
  std::map<Node, Node> alias_;
  std::map<Node, PhysReg> colors_;
  std::set<PhysReg> newlyUsedCalleeSaved_;
  uint64_t coalescedMoveCount_ = 0;

  bool hasWorkListMove() const {
    return std::any_of(
        graph_.moves.begin(), graph_.moves.end(),
        [](const Move &move) { return move.state == MoveState::WorkList; });
  }

  std::size_t firstWorkListMove() const {
    for (std::size_t i = 0; i < graph_.moves.size(); ++i) {
      if (graph_.moves[i].state == MoveState::WorkList) {
        return i;
      }
    }
    throw BackendError("missing register coalescing worklist move");
  }

  std::set<Node> adjacent(Node node) const {
    std::set<Node> result;
    auto found = graph_.adjacency.find(node);
    if (found == graph_.adjacency.end()) {
      return result;
    }
    for (Node neighbor : found->second) {
      if (stackNodes_.count(neighbor) == 0 &&
          coalescedNodes_.count(neighbor) == 0) {
        result.insert(neighbor);
      }
    }
    return result;
  }

  std::set<std::size_t> nodeMoves(Node node) const {
    std::set<std::size_t> result;
    auto found = graph_.moveList.find(node);
    if (found == graph_.moveList.end()) {
      return result;
    }
    for (std::size_t index : found->second) {
      MoveState state = graph_.moves[index].state;
      if (state == MoveState::WorkList || state == MoveState::Active) {
        result.insert(index);
      }
    }
    return result;
  }

  bool moveRelated(Node node) const { return !nodeMoves(node).empty(); }

  Node getAlias(Node node) {
    auto found = alias_.find(node);
    if (coalescedNodes_.count(node) == 0 || found == alias_.end()) {
      return node;
    }
    found->second = getAlias(found->second);
    return found->second;
  }

  void makeWorkLists() {
    for (Node node : graph_.initial) {
      if (graph_.degree[node] >= graph_.colorCount) {
        spillWorkList_.insert(node);
      } else if (moveRelated(node)) {
        freezeWorkList_.insert(node);
      } else {
        simplifyWorkList_.insert(node);
      }
    }
  }

  void enableMoves(const std::set<Node> &nodes) {
    for (Node node : nodes) {
      for (std::size_t moveIndex : nodeMoves(node)) {
        if (graph_.moves[moveIndex].state == MoveState::Active) {
          graph_.moves[moveIndex].state = MoveState::WorkList;
        }
      }
    }
  }

  void decrementDegree(Node node) {
    if (graph_.precolored.count(node) != 0) {
      return;
    }
    const int oldDegree = graph_.degree[node];
    --graph_.degree[node];
    if (oldDegree == graph_.colorCount) {
      std::set<Node> affected = adjacent(node);
      affected.insert(node);
      enableMoves(affected);
      spillWorkList_.erase(node);
      if (moveRelated(node)) {
        freezeWorkList_.insert(node);
      } else {
        simplifyWorkList_.insert(node);
      }
    }
  }

  // 改进的简化启发式：选择度最高的节点（保留低度节点以便后续容易着色）
  // 同度时选择溢出代价更低的（保留高代价节点以获得更好颜色）
  void simplify() {
    Node best = *simplifyWorkList_.begin();
    for (Node node : simplifyWorkList_) {
      if (graph_.degree[node] > graph_.degree[best]) {
        best = node;
      } else if (graph_.degree[node] == graph_.degree[best]) {
        // 同度时溢出代价更低的优先简化
        auto bestKey = spillKey(best);
        auto nodeKey = spillKey(node);
        if (nodeKey < bestKey) {
          best = node;
        }
      }
    }
    simplifyWorkList_.erase(best);
    selectStack_.push_back(best);
    stackNodes_.insert(best);
    for (Node neighbor : adjacent(best)) {
      decrementDegree(neighbor);
    }
  }

  bool ok(Node neighbor, Node precolored) const {
    return graph_.degree.at(neighbor) < graph_.colorCount ||
           graph_.precolored.count(neighbor) != 0 ||
           graph_.hasEdge(neighbor, precolored);
  }

  bool conservative(const std::set<Node> &nodes) const {
    int highDegree = 0;
    for (Node node : nodes) {
      auto found = graph_.degree.find(node);
      if (found != graph_.degree.end() && found->second >= graph_.colorCount) {
        ++highDegree;
      }
    }
    return highDegree < graph_.colorCount;
  }

  void addWorkList(Node node) {
    if (graph_.precolored.count(node) == 0 && !moveRelated(node) &&
        graph_.degree[node] < graph_.colorCount) {
      freezeWorkList_.erase(node);
      simplifyWorkList_.insert(node);
    }
  }

  void combine(Node keep, Node removed) {
    freezeWorkList_.erase(removed);
    spillWorkList_.erase(removed);
    simplifyWorkList_.erase(removed);
    coalescedNodes_.insert(removed);
    alias_[removed] = keep;
    graph_.moveList[keep].insert(graph_.moveList[removed].begin(),
                                 graph_.moveList[removed].end());
    enableMoves({removed});

    for (Node neighbor : adjacent(removed)) {
      graph_.addEdge(neighbor, keep, true);
      decrementDegree(neighbor);
    }
    if (graph_.degree[keep] >= graph_.colorCount &&
        freezeWorkList_.count(keep) != 0) {
      freezeWorkList_.erase(keep);
      spillWorkList_.insert(keep);
    }
  }

  void coalesce() {
    const std::size_t moveIndex = firstWorkListMove();
    Move &move = graph_.moves[moveIndex];
    Node x = getAlias(move.dst);
    Node y = getAlias(move.src);
    Node keep = x;
    Node removed = y;
    if (graph_.precolored.count(y) != 0) {
      keep = y;
      removed = x;
    }

    if (keep == removed) {
      move.state = MoveState::Coalesced;
      ++coalescedMoveCount_;
      addWorkList(keep);
      return;
    }
    if (graph_.precolored.count(removed) != 0 ||
        graph_.hasEdge(keep, removed)) {
      move.state = MoveState::Constrained;
      addWorkList(keep);
      addWorkList(removed);
      return;
    }

    // [改进] 浮点COPY合并：仅在涉及参数寄存器且函数有CALL时保守处理。
    // 其他情况（如局部浮点变量的COPY）可以安全合并。
    if (graph_.floatBank) {
      bool involvesFArg = false;
      if (graph_.precolored.count(keep) != 0) {
        involvesFArg = isArgReg(nodePhysicalReg(keep));
      }
      if (graph_.precolored.count(removed) != 0) {
        involvesFArg = involvesFArg || isArgReg(nodePhysicalReg(removed));
      }
      if (involvesFArg && graph_.containsCall) {
        // 涉及参数寄存器且函数有调用：保守处理，仅冻结
        move.state = MoveState::Frozen;
        addWorkList(keep);
        addWorkList(removed);
        return;
      }
      // 局部浮点变量：允许合并
    }

    // [改进] 虚拟-虚拟合并：使用Briggs保守检查，而非一律冻结。
    // 仅当两端都是纯虚拟寄存器（非预着色）时才尝试保守性检查。
    if (graph_.precolored.count(keep) == 0 &&
        graph_.precolored.count(removed) == 0) {
      std::set<Node> neighbors = adjacent(keep);
      const std::set<Node> removedNeighbors = adjacent(removed);
      neighbors.insert(removedNeighbors.begin(), removedNeighbors.end());
      if (conservative(neighbors)) {
        // 保守检查通过：允许虚拟-虚拟合并
        move.state = MoveState::Coalesced;
        ++coalescedMoveCount_;
        combine(keep, removed);
        addWorkList(keep);
        return;
      }
      // 保守检查失败：暂不合并但保留亲和性（Active而非Frozen）
      move.state = MoveState::Active;
      return;
    }

    bool canCombine = false;
    if (graph_.precolored.count(keep) != 0) {
      canCombine = true;
      for (Node neighbor : adjacent(removed)) {
        if (!ok(neighbor, keep)) {
          canCombine = false;
          break;
        }
      }
    } else {
      std::set<Node> neighbors = adjacent(keep);
      const std::set<Node> removedNeighbors = adjacent(removed);
      neighbors.insert(removedNeighbors.begin(), removedNeighbors.end());
      canCombine = conservative(neighbors);
    }

    if (canCombine) {
      move.state = MoveState::Coalesced;
      ++coalescedMoveCount_;
      combine(keep, removed);
      addWorkList(keep);
    } else {
      move.state = MoveState::Active;
    }
  }

  void freezeMoves(Node node) {
    for (std::size_t moveIndex : nodeMoves(node)) {
      Move &move = graph_.moves[moveIndex];
      Node dst = getAlias(move.dst);
      Node src = getAlias(move.src);
      Node other = src == getAlias(node) ? dst : src;
      move.state = MoveState::Frozen;
      if (freezeWorkList_.count(other) != 0 && !moveRelated(other) &&
          graph_.degree[other] < graph_.colorCount) {
        freezeWorkList_.erase(other);
        simplifyWorkList_.insert(other);
      }
    }
  }

  void freeze() {
    Node node = *freezeWorkList_.begin();
    freezeWorkList_.erase(node);
    simplifyWorkList_.insert(node);
    freezeMoves(node);
  }

  // 增强的溢出代价计算：综合循环热度、活跃区间长度、操作数角色
  std::tuple<int, uint64_t, uint32_t> spillKey(Node node) const {
    const bool rematerializable = info_.rematerializable.count(node) != 0;
    const bool generated = info_.generatedBySpill.count(node) != 0;
    const uint64_t heat = info_.heat.count(node) != 0 ? info_.heat.at(node) : 1;
    const uint64_t degree =
        static_cast<uint64_t>(std::max(1, graph_.degree.at(node)));

    // 基础分数：热度/度数（值越小越不容易被溢出）
    uint64_t score = generated ? std::numeric_limits<uint64_t>::max()
                               : (heat * 1024U) / degree;

    // 活跃区间长度因子：长生命周期更难溢出（+10%/100指令当量）
    if (info_.liveLength.count(node) != 0) {
      const uint64_t length = info_.liveLength.at(node);
      score = score * (1000 + length) / 1000;
    }

    // 地址基址寄存器惩罚：频繁load/store中需要恢复，代价高50%
    auto roleIt = info_.operandRoles.find(node);
    if (roleIt != info_.operandRoles.end() &&
        roleIt->second == OperandRole::AddressBase) {
      score = score * 150 / 100;
    }

    return std::make_tuple(rematerializable ? 0 : 1, score, node);
  }

  void selectSpill() {
    Node selected = *spillWorkList_.begin();
    for (Node node : spillWorkList_) {
      if (spillKey(node) < spillKey(selected)) {
        selected = node;
      }
    }
    spillWorkList_.erase(selected);
    simplifyWorkList_.insert(selected);
    freezeMoves(selected);
  }

  std::vector<PhysReg> availableColors(Node node) {
    const RegClass rc = info_.classes.at(node);
    std::vector<PhysReg> available = allocationColors(rc, isLeaf_);
    for (Node neighbor : graph_.adjacency[node]) {
      Node representative = getAlias(neighbor);
      auto color = colors_.find(representative);
      if (color == colors_.end()) {
        continue;
      }
      available.erase(
          std::remove(available.begin(), available.end(), color->second),
          available.end());
    }
    return available;
  }

  PhysReg chooseColor(Node node, const std::vector<PhysReg> &available) {
    std::set<PhysReg> affinity;
    for (std::size_t moveIndex : graph_.moveList[node]) {
      const Move &move = graph_.moves[moveIndex];
      Node other = getAlias(move.dst == node ? move.src : move.dst);
      auto color = colors_.find(other);
      if (color != colors_.end()) {
        affinity.insert(color->second);
      }
    }

    auto choose = [&](auto predicate) -> PhysReg {
      for (PhysReg reg : available) {
        if (predicate(reg)) {
          return reg;
        }
      }
      return PhysReg::NoReg;
    };

    // [改进] 始终尝试亲和性着色
    PhysReg color = choose([&](PhysReg reg) { return affinity.count(reg) != 0; });
    if (color != PhysReg::NoReg) {
      return color;
    }

    // 浮点含调用时优先callee-saved，否则优先caller-saved
    if (graph_.floatBank && graph_.containsCall) {
      color =
          choose([](PhysReg reg) { return getRegisterInfo(reg).calleeSaved; });
    } else {
      color =
          choose([](PhysReg reg) { return getRegisterInfo(reg).callerSaved; });
    }
    if (color != PhysReg::NoReg) {
      return color;
    }
    // 已使用的callee-saved：复用已有的保存槽
    color = choose(
        [&](PhysReg reg) { return newlyUsedCalleeSaved_.count(reg) != 0; });
    if (color != PhysReg::NoReg) {
      return color;
    }
    // 优先caller-saved
    color = choose(
        [&](PhysReg reg) { return getRegisterInfo(reg).callerSaved; });
    return color != PhysReg::NoReg ? color : available.front();
  }

  void assignColors() {
    while (!selectStack_.empty()) {
      Node node = selectStack_.back();
      selectStack_.pop_back();
      const std::vector<PhysReg> available = availableColors(node);
      if (available.empty()) {
        spilledNodes_.insert(node);
        continue;
      }
      PhysReg color = chooseColor(node, available);
      colors_[node] = color;
      coloredNodes_.insert(node);
      if (getRegisterInfo(color).calleeSaved) {
        newlyUsedCalleeSaved_.insert(color);
      }
    }

    for (Node node : coalescedNodes_) {
      Node representative = getAlias(node);
      auto color = colors_.find(representative);
      if (color == colors_.end()) {
        spilledNodes_.insert(node);
      } else {
        colors_[node] = color->second;
      }
    }
  }
};

int64_t spillSize(RegClass rc) { return rc == RegClass::GPR64 ? 8 : 4; }

Opcode spillLoadOpcode(RegClass rc) {
  if (rc == RegClass::FPR32) {
    return Opcode::FLW;
  }
  return rc == RegClass::GPR64 ? Opcode::LD : Opcode::LW;
}

Opcode spillStoreOpcode(RegClass rc) {
  if (rc == RegClass::FPR32) {
    return Opcode::FSW;
  }
  return rc == RegClass::GPR64 ? Opcode::SD : Opcode::SW;
}

MachineInstr makeSpillLoad(MachineReg dst, int frameIndex) {
  MachineInstr load;
  load.opcode = spillLoadOpcode(dst.rc);
  load.defs.push_back(MachineOperand::reg(dst));
  load.uses.push_back(MachineOperand::frameIndex(frameIndex));
  load.uses.push_back(MachineOperand::imm(0));
  return load;
}

MachineInstr makeSpillStore(MachineReg src, int frameIndex) {
  MachineInstr store;
  store.opcode = spillStoreOpcode(src.rc);
  store.uses.push_back(MachineOperand::reg(src));
  store.uses.push_back(MachineOperand::frameIndex(frameIndex));
  store.uses.push_back(MachineOperand::imm(0));
  store.sideEffects = true;
  return store;
}

struct SpillRewriteResult {
  std::set<uint32_t> generated;
  uint64_t loads = 0;
  uint64_t stores = 0;
  uint64_t rematerializations = 0;
  uint64_t slots = 0;
};

std::map<uint32_t, int>
allocateSpillSlots(MachineFunction &mf, const ProgramInfo &info,
                   const std::vector<uint32_t> &ordinarySpills,
                   const std::map<uint32_t, std::set<uint32_t>> &conflicts) {
  struct Slot {
    int frameIndex = -1;
    int64_t size = 0;
    bool floatBank = false;
    std::set<uint32_t> owners;
  };

  std::vector<uint32_t> ordered = ordinarySpills;
  std::sort(ordered.begin(), ordered.end(), [&](uint32_t lhs, uint32_t rhs) {
    const std::size_t lhsDegree =
        conflicts.count(lhs) != 0 ? conflicts.at(lhs).size() : 0;
    const std::size_t rhsDegree =
        conflicts.count(rhs) != 0 ? conflicts.at(rhs).size() : 0;
    return lhsDegree != rhsDegree ? lhsDegree > rhsDegree : lhs < rhs;
  });

  std::vector<Slot> slots;
  std::map<uint32_t, int> allocation;
  for (uint32_t id : ordered) {
    const int64_t size = spillSize(info.classes.at(id));
    const bool floatBank = isFloatClass(info.classes.at(id));
    int chosen = -1;
    for (std::size_t slotIndex = 0; slotIndex < slots.size(); ++slotIndex) {
      if (slots[slotIndex].size != size ||
          slots[slotIndex].floatBank != floatBank) {
        continue;
      }
      bool collides = false;
      for (uint32_t owner : slots[slotIndex].owners) {
        auto found = conflicts.find(id);
        if (found != conflicts.end() && found->second.count(owner) != 0) {
          collides = true;
          break;
        }
      }
      if (!collides) {
        chosen = static_cast<int>(slotIndex);
        break;
      }
    }
    if (chosen < 0) {
      const int frameIndex =
          mf.createStackObject(size, size, "spill." + std::to_string(id));
      slots.push_back(Slot{frameIndex, size, floatBank, {}});
      chosen = static_cast<int>(slots.size() - 1);
    }
    slots[static_cast<std::size_t>(chosen)].owners.insert(id);
    allocation[id] = slots[static_cast<std::size_t>(chosen)].frameIndex;
  }
  return allocation;
}

SpillRewriteResult
rewriteSpills(MachineFunction &mf, const ProgramInfo &info,
              const std::vector<uint32_t> &spills,
              const std::map<uint32_t, std::set<uint32_t>> &conflicts) {
  SpillRewriteResult result;
  std::set<uint32_t> spillSet(spills.begin(), spills.end());
  std::set<uint32_t> rematerialized;
  std::vector<uint32_t> ordinary;
  for (uint32_t id : spills) {
    if (info.rematerializable.count(id) != 0) {
      rematerialized.insert(id);
    } else {
      ordinary.push_back(id);
    }
  }
  const std::map<uint32_t, int> spillSlots =
      allocateSpillSlots(mf, info, ordinary, conflicts);
  std::set<int> uniqueSlots;
  for (const auto &[id, slot] : spillSlots) {
    (void)id;
    uniqueSlots.insert(slot);
  }
  result.slots = uniqueSlots.size();

  for (MachineBasicBlock &block : mf.blocks) {
    std::vector<MachineInstr> rewritten;
    for (MachineInstr instr : block.instrs) {
      bool removeDefinition = false;
      for (const MachineOperand &def : instr.defs) {
        if (isVirtualReg(def) && rematerialized.count(def.regValue.id) != 0) {
          removeDefinition = true;
        }
      }
      if (removeDefinition) {
        continue;
      }

      std::map<uint32_t, MachineReg> replacements;
      for (MachineOperand &use : instr.uses) {
        if (!isVirtualReg(use) || spillSet.count(use.regValue.id) == 0) {
          continue;
        }
        const uint32_t id = use.regValue.id;
        auto replacement = replacements.find(id);
        if (replacement == replacements.end()) {
          MachineReg temp = mf.createVReg(use.regValue.rc);
          replacements[id] = temp;
          result.generated.insert(temp.id);
          if (rematerialized.count(id) != 0) {
            MachineInstr clone = info.rematerializable.at(id);
            clone.defs[0] = MachineOperand::reg(temp);
            rewritten.push_back(std::move(clone));
            ++result.rematerializations;
          } else {
            rewritten.push_back(makeSpillLoad(temp, spillSlots.at(id)));
            ++result.loads;
          }
          replacement = replacements.find(id);
        }
        use.regValue = replacement->second;
      }

      std::vector<MachineInstr> stores;
      for (MachineOperand &def : instr.defs) {
        if (!isVirtualReg(def) || spillSlots.count(def.regValue.id) == 0) {
          continue;
        }
        const uint32_t id = def.regValue.id;
        MachineReg temp;
        auto replacement = replacements.find(id);
        if (replacement != replacements.end()) {
          temp = replacement->second;
        } else {
          temp = mf.createVReg(def.regValue.rc);
          replacements[id] = temp;
          result.generated.insert(temp.id);
        }
        def.regValue = temp;
        stores.push_back(makeSpillStore(temp, spillSlots.at(id)));
        ++result.stores;
      }

      rewritten.push_back(std::move(instr));
      rewritten.insert(rewritten.end(), stores.begin(), stores.end());
    }
    block.instrs = std::move(rewritten);
  }
  return result;
}

void rewriteOperand(MachineOperand &operand,
                    const std::map<uint32_t, PhysReg> &colors) {
  if (!isVirtualReg(operand)) {
    return;
  }
  auto color = colors.find(operand.regValue.id);
  if (color == colors.end()) {
    throw BackendError("uncolored virtual register after graph coloring");
  }
  operand.regValue = MachineReg::preg(color->second, operand.regValue.rc);
}

void rewriteFunction(MachineFunction &mf,
                     const std::map<uint32_t, PhysReg> &colors) {
  for (MachineBasicBlock &block : mf.blocks) {
    std::vector<MachineInstr> rewritten;
    for (MachineInstr instr : block.instrs) {
      for (MachineOperand &def : instr.defs) {
        rewriteOperand(def, colors);
      }
      for (MachineOperand &use : instr.uses) {
        rewriteOperand(use, colors);
      }
      const bool redundantCopy =
          (instr.opcode == Opcode::COPY || instr.opcode == Opcode::F_COPY) &&
          instr.defs.size() == 1 && instr.uses.size() == 1 &&
          instr.defs[0].kind == OperandKind::Reg &&
          instr.uses[0].kind == OperandKind::Reg &&
          instr.defs[0].regValue.physical && instr.uses[0].regValue.physical &&
          instr.defs[0].regValue.phys == instr.uses[0].regValue.phys;
      if (!redundantCopy) {
        rewritten.push_back(std::move(instr));
      }
    }
    block.instrs = std::move(rewritten);
  }
}

void recordUsedCalleeSaved(MachineFunction &mf,
                           const std::map<uint32_t, PhysReg> &colors) {
  std::set<PhysReg> used;
  for (const auto &[id, reg] : colors) {
    (void)id;
    if (getRegisterInfo(reg).calleeSaved) {
      used.insert(reg);
    }
  }
  for (const MachineBasicBlock &block : mf.blocks) {
    for (const MachineInstr &instr : block.instrs) {
      for (const MachineOperand &operand : instr.defs) {
        if (isPhysicalReg(operand) &&
            getRegisterInfo(operand.regValue.phys).calleeSaved) {
          used.insert(operand.regValue.phys);
        }
      }
      for (const MachineOperand &operand : instr.uses) {
        if (isPhysicalReg(operand) &&
            getRegisterInfo(operand.regValue.phys).calleeSaved) {
          used.insert(operand.regValue.phys);
        }
      }
    }
  }
  mf.usedCalleeSaved.assign(used.begin(), used.end());
}

std::string allocationSignature(const MachineFunction &mf,
                                const std::vector<uint32_t> &spills) {
  std::string signature = std::to_string(mf.nextVReg) + ":" +
                          std::to_string(mf.stackObjects.size());
  for (const MachineBasicBlock &block : mf.blocks) {
    signature += ":" + std::to_string(block.instrs.size());
  }
  signature += "|";
  for (uint32_t id : spills) {
    signature += std::to_string(id) + ",";
  }
  return signature;
}

void allocateRegistersImpl(MachineFunction &mf, RegAllocStats *stats) {
  // [新增] 函数特征分析：决定寄存器分配策略
  const FunctionProfile profile = analyzeFunctionProfile(mf);
  const bool isLeaf = (profile.fnClass == FunctionClass::Leaf);

  std::set<uint32_t> generatedBySpill;
  std::string previousSignature;
  int repeatedSignature = 0;

  for (int round = 0; round < kEmergencyRoundLimit; ++round) {
    if (stats != nullptr) {
      ++stats->rounds;
    }
    ProgramInfo info = analyzeProgram(mf, generatedBySpill);
    if (info.classes.empty()) {
      return;
    }
    const Liveness live = computeLiveness(mf);

    ColoringResult gpr =
        IRCAllocator(buildAllocationGraph(mf, info, live, false, isLeaf), info, isLeaf).run();
    ColoringResult fpr =
        IRCAllocator(buildAllocationGraph(mf, info, live, true, isLeaf), info, isLeaf).run();

    std::map<uint32_t, PhysReg> colors = gpr.colors;
    colors.insert(fpr.colors.begin(), fpr.colors.end());
    std::vector<uint32_t> spills = gpr.spills;
    spills.insert(spills.end(), fpr.spills.begin(), fpr.spills.end());

    // [改进] 重物化候选的批量溢出：降低批量大小（更多颜色可用）
    if (!spills.empty()) {
      for (const auto &[id, definition] : info.rematerializable) {
        (void)definition;
        spills.push_back(id);
      }
      // [改进] 减少批量溢出的激进程度（原：大函数8个，小函数64个）
      // 因为现在有更多颜色可用（GPR 24 vs 18），溢出压力降低
      if (info.classes.size() > 64 || round >= 1) {
        std::vector<uint32_t> batch;
        const std::size_t batchLimit = info.classes.size() > 64 ? 4 : 32;
        const std::size_t gprCount =
            std::min<std::size_t>(batchLimit, gpr.potentialSpills.size());
        batch.insert(batch.end(), gpr.potentialSpills.begin(),
                     gpr.potentialSpills.begin() + gprCount);
        const std::size_t remaining = batchLimit - batch.size();
        const std::size_t fprCount =
            std::min<std::size_t>(remaining, fpr.potentialSpills.size());
        batch.insert(batch.end(), fpr.potentialSpills.begin(),
                     fpr.potentialSpills.begin() + fprCount);
        spills.insert(spills.end(), batch.begin(), batch.end());
      }
    }
    std::sort(spills.begin(), spills.end());
    spills.erase(std::unique(spills.begin(), spills.end()), spills.end());

    if (stats != nullptr) {
      stats->coalescedMoves += gpr.coalescedMoves + fpr.coalescedMoves;
    }
    if (spills.empty()) {
      // [改进] 叶子函数无溢出：记录所有callee-saved使用
      recordUsedCalleeSaved(mf, colors);
      rewriteFunction(mf, colors);
      return;
    }

    // [新增] 分级溢出处理：少量溢出使用专用寄存器（mv替代ld/sd）
    // 仅在叶子函数或首轮尝试（避免与callee-saved管理冲突）
    std::map<uint32_t, std::set<uint32_t>> conflicts = gpr.spillConflicts;
    for (const auto &[id, neighbors] : fpr.spillConflicts) {
      conflicts[id].insert(neighbors.begin(), neighbors.end());
    }

    // 尝试专用寄存器溢出：将≤2个溢出映射到S10/S11或FS10/FS11
    const std::size_t gprSpillCount = gpr.spills.size();
    const std::size_t fprSpillCount = fpr.spills.size();
    bool usedDedicated = false;

    if ((gprSpillCount <= 2 || fprSpillCount <= 2) && round == 0 &&
        isLeaf) {
      // 叶子函数中溢出少量变量：使用专用溢出寄存器
      std::map<uint32_t, PhysReg> dedicatedMap;
      int gprDedicatedIdx = 0;
      int fprDedicatedIdx = 0;
      const PhysReg gprDedicated[] = {PhysReg::S10, PhysReg::S11};
      const PhysReg fprDedicated[] = {PhysReg::FS10, PhysReg::FS11};

      for (uint32_t id : gpr.spills) {
        if (gprDedicatedIdx < 2 && info.rematerializable.count(id) == 0) {
          dedicatedMap[id] = gprDedicated[gprDedicatedIdx++];
        }
      }
      for (uint32_t id : fpr.spills) {
        if (fprDedicatedIdx < 2 && info.rematerializable.count(id) == 0) {
          dedicatedMap[id] = fprDedicated[fprDedicatedIdx++];
        }
      }

      if (!dedicatedMap.empty()) {
        // 将专用寄存器映射的溢出变量直接着色到专用寄存器
        for (const auto &[id, reg] : dedicatedMap) {
          colors[id] = reg;
        }
        // 从溢出列表中移除已处理的变量
        spills.erase(std::remove_if(spills.begin(), spills.end(),
                                     [&](uint32_t id) {
                                       return dedicatedMap.count(id) != 0;
                                     }),
                     spills.end());
        usedDedicated = true;

        if (spills.empty()) {
          recordUsedCalleeSaved(mf, colors);
          rewriteFunction(mf, colors);
          return;
        }
      }
    }

    const std::string signature = allocationSignature(mf, spills);
    repeatedSignature =
        signature == previousSignature ? repeatedSignature + 1 : 0;
    if (repeatedSignature >= 2) {
      throw BackendError("register allocation made no progress in " + mf.name);
    }
    previousSignature = signature;

    // [改进] 专用寄存器溢出后仍有剩余溢出 → 传统栈槽溢出
    SpillRewriteResult rewrite = rewriteSpills(mf, info, spills, conflicts);
    generatedBySpill.insert(rewrite.generated.begin(), rewrite.generated.end());
    if (stats != nullptr) {
      stats->spilledVRegs += spills.size();
      stats->spillLoads += rewrite.loads;
      stats->spillStores += rewrite.stores;
      stats->rematerializations += rewrite.rematerializations;
      stats->spillSlots += rewrite.slots;
    }
    // 如果使用了专用寄存器溢出，不增加统计中的溢出计数（它们是mv，不是ld/sd）
    if (usedDedicated && stats != nullptr) {
      // 补偿：专用寄存器直接映射不算溢出
    }
  }
  throw BackendError("register allocation emergency round limit exceeded in " +
                     mf.name);
}

} // anonymous namespace

void allocateRegisters(MachineFunction &mf) {
  allocateRegistersImpl(mf, nullptr);
}

void allocateRegisters(MachineFunction &mf, RegAllocStats *stats) {
  allocateRegistersImpl(mf, stats);
}

} // namespace riscv
