#include "riscv/RegAlloc.hpp"

#include "riscv/Registers.hpp"
#include "riscv/Target.hpp"
#include "riscv/opt/MachineAnalysis.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
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
// 本文件实现基于图着色的寄存器分配(Iterated Register Coalescing 风格)。
// 流程:分析程序(收集虚拟寄存器类、热度、可重物化定义、循环回边)->为整型与
// 浮点两个寄存器体各自构建干涉图(依赖活跃分析)->运行简化/合并/冻结/溢出选择
// 与着色->对溢出的虚拟寄存器改写为访存或重物化,循环直到无溢出。位于后端流水线
// 中消 PHI/降 SELECT 之后。RISC-V 与 ARM64 两后端使用相同实现,
// 两后端各保留一份相同实现。

namespace {

// 图节点用 32 位编号:虚拟寄存器直接用其 id;物理寄存器加上一个高位基址区分。
using Node = uint32_t;

// 物理寄存器节点的编号基址(高位置位),用来把物理寄存器与虚拟寄存器 id 分开。
constexpr Node kPhysicalNodeBase = 0x80000000U;
// 分配迭代的应急轮数上限,超过则判定无法收敛并报错。
constexpr int kEmergencyRoundLimit = 128;

// 判断操作数是否为虚拟寄存器。
bool isVirtualReg(const MachineOperand &operand) {
  return operand.kind == OperandKind::Reg && !operand.regValue.physical;
}

// 判断操作数是否为物理寄存器。
bool isPhysicalReg(const MachineOperand &operand) {
  return operand.kind == OperandKind::Reg && operand.regValue.physical;
}

// 寄存器类是否属于浮点体。
bool isFloatClass(RegClass rc) { return rc == RegClass::FPR32; }

// 两个寄存器类是否属于同一寄存器体(同为浮点或同为整型)。
bool sameBank(RegClass lhs, RegClass rhs) {
  return isFloatClass(lhs) == isFloatClass(rhs);
}

// 寄存器类与物理寄存器是否属于同一体。
bool sameBank(RegClass rc, PhysReg reg) {
  return isFloatClass(rc) == getRegisterInfo(reg).isFloat;
}

// 返回某寄存器类可用于着色的物理寄存器集合(即可分配颜色)。
std::vector<PhysReg> allocationColors(RegClass rc) {
  return allocatableRegs(rc);
}

// 把物理寄存器映射为图节点编号(加高位基址)。
Node physicalNode(PhysReg reg) {
  return kPhysicalNodeBase + static_cast<Node>(reg);
}

// 判断节点是否为物理寄存器节点。
bool isPhysicalNode(Node node) { return node >= kPhysicalNodeBase; }

// 从物理寄存器节点编号还原出物理寄存器。
PhysReg nodePhysicalReg(Node node) {
  return static_cast<PhysReg>(node - kPhysicalNodeBase);
}

// 记录虚拟寄存器 id 到其寄存器类的映射;若同一 id 出现过不同类则报错(类应一致)。
void rememberClass(std::map<uint32_t, RegClass> &classes, MachineReg reg) {
  auto [it, inserted] = classes.emplace(reg.id, reg.rc);
  if (!inserted && it->second != reg.rc) {
    throw BackendError("virtual register used with inconsistent classes");
  }
}

// 遍历全函数,收集所有虚拟寄存器 id 及其寄存器类。
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

// 收集指令使用端的虚拟寄存器 id。
std::vector<uint32_t> virtualUses(const MachineInstr &instr) {
  std::vector<uint32_t> result;
  for (const MachineOperand &operand : instr.uses) {
    if (isVirtualReg(operand)) {
      result.push_back(operand.regValue.id);
    }
  }
  return result;
}

// 收集指令定义端的虚拟寄存器 id。
std::vector<uint32_t> virtualDefs(const MachineInstr &instr) {
  std::vector<uint32_t> result;
  for (const MachineOperand &operand : instr.defs) {
    if (isVirtualReg(operand)) {
      result.push_back(operand.regValue.id);
    }
  }
  return result;
}

// 取活跃变量分析结果:有外部分析管理器则复用其缓存;否则临时建一个本地管理器,
// 注册 CFG 与活跃分析后现算(分配可脱离流水线独立调用)。
MachineLivenessAnalysis::Result
getLiveness(MachineFunction &mf, MachineFunctionAnalysisManager *am) {
  if (am != nullptr) {
    return am->getResult<MachineLivenessAnalysis>(mf);
  }

  MachineFunctionAnalysisManager localAM;
  localAM.registerPass([] { return MachineCFGAnalysis(); });
  localAM.registerPass([] { return MachineLivenessAnalysis(); });
  return localAM.getResult<MachineLivenessAnalysis>(mf);
}

// 溢出改写后使分析缓存失效(指令已变,活跃/循环等结果需重算)。
void invalidateAnalysesAfterRewrite(MachineFunction &mf,
                                    MachineFunctionAnalysisManager *am) {
  if (am != nullptr) {
    am->invalidate(mf, MachinePreservationStatus::none());
  }
}

// 取循环分析结果,来源策略与 getLiveness 相同。用于计算块热度与识别回边。
MachineLoopAnalysis::Result
getLoopInfo(MachineFunction &mf, MachineFunctionAnalysisManager *am) {
  if (am != nullptr) {
    return am->getResult<MachineLoopAnalysis>(mf);
  }

  MachineFunctionAnalysisManager localAM;
  localAM.registerPass([] { return MachineCFGAnalysis(); });
  localAM.registerPass([] { return MachineLoopAnalysis(); });
  return localAM.getResult<MachineLoopAnalysis>(mf);
}

// 分配过程需要的程序级信息汇总。
struct ProgramInfo {
  std::map<uint32_t, RegClass> classes;          // 虚拟寄存器 id 到寄存器类
  std::map<uint32_t, uint64_t> heat;             // 各虚拟寄存器的使用热度(按循环深度加权)
  std::map<uint32_t, MachineInstr> rematerializable; // 可重物化的虚拟寄存器及其定义指令
  std::set<uint32_t> generatedBySpill;           // 由溢出改写新产生的临时寄存器
  std::set<std::size_t> loopLatchBlocks;         // 循环 latch 块下标集合
  bool aggressiveLARematerialization = false;    // 是否激进重物化 LA(环境变量开关)
  bool coalesceGPRVirtualCopies = true;          // 是否允许合并 GPR 虚拟寄存器间的拷贝
  bool coalesceFPRVirtualCopies = true;          // 是否允许合并 FPR 虚拟寄存器间的拷贝
};

// 分析程序,填充 ProgramInfo。
// 关键步骤:收集虚拟寄存器类;读环境变量决定重物化/合并开关;记录循环 latch;
// 逐块按循环深度计算热度(每深一层乘 10,最多三层),使用计 2 倍权重、定义计 1 倍;
// 把恰好只定义一次的 LI/LA 记为可重物化候选(重物化比访存溢出更划算)。
ProgramInfo analyzeProgram(const MachineFunction &mf,
                           const std::set<uint32_t> &generatedBySpill,
                           const MachineLoopAnalysis::Result &loopInfo) {
  ProgramInfo info;
  info.classes = collectVRegs(mf);
  info.generatedBySpill = generatedBySpill;
  info.aggressiveLARematerialization =
      std::getenv("BACKEND_AGGRESSIVE_LA_REMAT") != nullptr;
  info.coalesceGPRVirtualCopies =
      std::getenv("BACKEND_DISABLE_GPR_VREG_COALESCING") == nullptr;
  info.coalesceFPRVirtualCopies =
      std::getenv("BACKEND_DISABLE_FPR_VREG_COALESCING") == nullptr;
  for (const auto &[latch, header] : loopInfo.backedges) {
    (void)header;
    if (latch >= 0) {
      info.loopLatchBlocks.insert(static_cast<std::size_t>(latch));
    }
  }
  std::map<uint32_t, int> defCount;
  std::map<uint32_t, MachineInstr> candidates;
  for (std::size_t blockIndex = 0; blockIndex < mf.blocks.size();
       ++blockIndex) {
    uint64_t blockHeat = 1;
    for (int i = 0; i < std::min(loopInfo.depth[blockIndex], 3); ++i) {
      blockHeat *= 10;
    }
    for (const MachineInstr &instr : mf.blocks[blockIndex].instrs) {
      for (uint32_t use : virtualUses(instr)) {
        info.heat[use] += 2 * blockHeat;
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

  for (const auto &[id, candidate] : candidates) {
    if (defCount[id] == 1) {
      info.rematerializable.emplace(id, candidate);
    }
  }
  return info;
}

// 合并阶段中一条拷贝相关"传送边"的状态:待处理、活跃、已合并、受约束、已冻结。
enum class MoveState { WorkList, Active, Coalesced, Constrained, Frozen };

// 一条传送(源自 COPY):dst<-src 两端节点、当前状态、是否允许作虚拟寄存器合并。
struct Move {
  Node dst = 0;
  Node src = 0;
  MoveState state = MoveState::WorkList;
  bool virtualCoalescingEligible = false;
};

// 单个寄存器体的干涉图。precolored 为物理寄存器(预着色)节点,initial 为待分配
// 虚拟寄存器节点;adjacency 是干涉邻接表,degree 是有效度数,moveList 记录每个
// 节点关联的传送下标,moves 是全部传送。colorCount 为可用颜色数。
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

  // 查询两节点间是否已存在干涉边。
  bool hasEdge(Node lhs, Node rhs) const {
    auto found = adjacency.find(lhs);
    return found != adjacency.end() && found->second.count(rhs) != 0;
  }

  // 加一条无向干涉边(自环或重边忽略)。updateDegree 为真时对非预着色端加度数;
  // 预着色节点度数视为无穷,不参与度数计数。
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

// 生成节点的可读名称(物理寄存器名或虚拟寄存器编号),用于错误信息。
std::string allocationNodeName(Node node) {
  if (isPhysicalNode(node)) {
    return std::string("physical register ") + regName(nodePhysicalReg(node));
  }
  return "virtual register " + std::to_string(node);
}

// 一致性校验:图中出现的每个节点都必须登记了度数(即落在可分配颜色集合内),
// 否则说明有寄存器越出可着色集合,报错。
void validateAllocationGraph(const AllocationGraph &graph) {
  auto requireDegree = [&](Node node) {
    if (graph.degree.count(node) == 0) {
      throw BackendError("register allocation graph contains " +
                         allocationNodeName(node) +
                         " outside the allocatable color set");
    }
  };

  for (const auto &[node, neighbors] : graph.adjacency) {
    requireDegree(node);
    for (Node neighbor : neighbors) {
      requireDegree(neighbor);
    }
  }
  for (const Move &move : graph.moves) {
    requireDegree(move.dst);
    requireDegree(move.src);
  }
}

// 判断节点是否属于给定寄存器体(物理看寄存器信息,虚拟看其类)。
bool nodeMatchesBank(Node node, bool floatBank,
                     const std::map<uint32_t, RegClass> &classes) {
  if (isPhysicalNode(node)) {
    return getRegisterInfo(nodePhysicalReg(node)).isFloat == floatBank;
  }
  auto found = classes.find(node);
  return found != classes.end() && isFloatClass(found->second) == floatBank;
}

// 判断节点是否为 fa0-fa7(浮点参数/返回值寄存器)的预着色节点。
// RISC-V 与 AArch64 的 PhysReg 枚举中 FA0-FA7 连续排列。
bool isFprArgReturnNode(Node node) {
  if (!isPhysicalNode(node)) {
    return false;
  }
  const PhysReg reg = nodePhysicalReg(node);
  return reg >= PhysReg::FA0 && reg <= PhysReg::FA7;
}

// 从一组操作数中挑出属于本寄存器体的虚拟寄存器节点。
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

// 从一组操作数中挑出属于本寄存器体、且可分配的物理寄存器节点。
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

// 把指令的隐式定义并入物理定义集合;若为 CALL,还要把本体所有调用者保存寄存器
// 视为被破坏(加入定义集),使跨调用活跃的值与它们干涉。最后排序去重。
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

// 判断指令是否为本寄存器体的拷贝指令(浮点看 F_COPY,整型看 COPY)。
bool isCopyForBank(const MachineInstr &instr, bool floatBank) {
  return floatBank ? instr.opcode == Opcode::F_COPY
                   : instr.opcode == Opcode::COPY;
}

// 为指定寄存器体构建干涉图。
// 关键步骤:先把本体所有可分配物理寄存器登记为预着色节点、所有本体虚拟寄存器
// 登记为待分配节点;再逐块自块尾向块首用 liveOut 回溯活跃集:每条指令的定义与
// 当前活跃值之间连干涉边(拷贝指令的源不与目标连边,以便后续合并);CALL 处把
// 调用者保存寄存器并入定义,使跨调用活跃值与之干涉;识别可合并的拷贝并登记为
// 传送;带合并屏障的拷贝改为显式连干涉边以阻止合并。最后设置度数并校验。
AllocationGraph buildAllocationGraph(const MachineFunction &mf,
                                     const ProgramInfo &info,
                                     const MachineLivenessAnalysis::Result &live,
                                     bool floatBank) {
  AllocationGraph graph;
  graph.floatBank = floatBank;
  const RegClass bankClass = floatBank ? RegClass::FPR32 : RegClass::GPR64;
  for (PhysReg reg : allocatableRegs(bankClass)) {
    Node node = physicalNode(reg);
    graph.precolored.insert(node);
    graph.adjacency[node];
  }
  graph.colorCount = static_cast<int>(allocationColors(bankClass).size());

  for (const auto &[id, rc] : info.classes) {
    if (isFloatClass(rc) == floatBank) {
      graph.initial.insert(id);
      graph.adjacency[id];
    }
  }

  for (std::size_t blockIndex = 0; blockIndex < mf.blocks.size();
       ++blockIndex) {
    // 活跃集初值为块出口活跃(只保留本体节点),随后自块尾向块首回溯。
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

      const bool registerCopy =
          isCopyForBank(*it, floatBank) && it->defs.size() == 1 &&
          it->uses.size() == 1 && it->defs[0].kind == OperandKind::Reg &&
          it->uses[0].kind == OperandKind::Reg &&
          sameBank(it->defs[0].regValue.rc, it->uses[0].regValue.rc) &&
          (!it->defs[0].regValue.physical ||
           getRegisterInfo(it->defs[0].regValue.phys).allocatable) &&
          (!it->uses[0].regValue.physical ||
           getRegisterInfo(it->uses[0].regValue.phys).allocatable);
      const bool copy = registerCopy && !it->copyCoalescingBarrier;
      if (copy) {
        // 可合并拷贝:源不与目标连干涉边(否则永远无法合并),登记为传送。
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
          // phi 拷贝一般不参与虚拟寄存器合并,除非位于循环 latch(合并可省下热拷贝)。
          const bool virtualCoalescingEligible =
              !it->phiCopy || info.loopLatchBlocks.count(blockIndex) != 0;
          graph.moves.push_back(Move{dst, src, MoveState::WorkList,
                                     virtualCoalescingEligible});
          graph.moveList[dst].insert(moveIndex);
          graph.moveList[src].insert(moveIndex);
        }
      }

      if (registerCopy && it->copyCoalescingBarrier) {
        // A barrier is stronger than merely freezing a move: frozen moves
        // still carry color affinity.  An explicit interference edge makes
        // the serialized cycle survive both coalescing and color selection.
        std::vector<Node> barrierDefs = virtualDefs;
        barrierDefs.insert(barrierDefs.end(), physicalDefs.begin(),
                           physicalDefs.end());
        std::vector<Node> barrierUses = virtualUses;
        barrierUses.insert(barrierUses.end(), physicalUses.begin(),
                           physicalUses.end());
        for (Node def : barrierDefs) {
          for (Node use : barrierUses) {
            graph.addEdge(def, use, false);
          }
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
      // 定义与当前活跃值互相干涉(不能占同一寄存器)。
      for (Node def : allDefs) {
        for (Node value : currentLive) {
          graph.addEdge(def, value, false);
        }
      }
      // 标准活跃回溯:定义点使其被定义值离开活跃集,再把使用值加入活跃集。
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
  // 预着色节点度数置为很大的值,使其在简化阶段永不被当作低度数节点摘除。
  for (Node node : graph.precolored) {
    graph.degree[node] = std::numeric_limits<int>::max() / 4;
  }
  validateAllocationGraph(graph);
  return graph;
}

// 图着色的输出:每个虚拟寄存器分到的物理寄存器(colors)、必须溢出的节点
// (spills)、可作为下一轮候选的潜在溢出(potentialSpills,按代价排序)、
// 合并掉的传送数,以及溢出节点之间的冲突关系(供分配栈槽时复用)。
struct ColoringResult {
  std::map<uint32_t, PhysReg> colors;
  std::vector<uint32_t> spills;
  std::vector<uint32_t> potentialSpills;
  uint64_t coalescedMoves = 0;
  std::map<uint32_t, std::set<uint32_t>> spillConflicts;
};

// 迭代寄存器合并(IRC)分配器:对单个寄存器体的干涉图执行简化、合并、冻结、
// 溢出选择与颜色分配。构造时把预着色节点直接着以自身物理寄存器。
class IRCAllocator {
public:
  IRCAllocator(AllocationGraph graph, const ProgramInfo &info)
      : graph_(std::move(graph)), info_(info) {
    for (Node node : graph_.precolored) {
      colors_[node] = nodePhysicalReg(node);
    }
  }

  // 运行分配主循环并返回着色结果。
  // 优先级:有可简化节点就简化;否则尝试合并;否则冻结;否则选一个潜在溢出。
  // 循环直到四个工作表都空,再做颜色分配。最后整理出各类结果并按代价排序潜在溢出。
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
    // 潜在溢出:着色成功、非溢出临时、且不可重物化的普通虚拟寄存器,作为下一轮
    // 主动溢出候选。按 热度/度数 的代价升序排序(代价低者优先溢出)。
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
      // 记录每个虚拟寄存器与其他虚拟寄存器的冲突,供溢出栈槽共享判定使用。
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
  std::set<Node> simplifyWorkList_;   // 低度数、非传送相关、可直接简化的节点
  std::set<Node> freezeWorkList_;     // 低度数但传送相关、可冻结的节点
  std::set<Node> spillWorkList_;      // 高度数、可能需要溢出的节点
  std::set<Node> spilledNodes_;       // 着色失败、实际溢出的节点
  std::set<Node> coalescedNodes_;     // 已被合并进别的节点
  std::set<Node> coloredNodes_;       // 已着色节点
  std::set<Node> stackNodes_;         // 已压入选择栈(简化摘除)的节点
  std::vector<Node> selectStack_;     // 简化摘除顺序,着色时逆序弹出
  std::map<Node, Node> alias_;        // 合并后的别名映射(指向代表节点)
  std::map<Node, PhysReg> colors_;    // 节点已分配的物理寄存器
  std::set<PhysReg> newlyUsedCalleeSaved_; // 本次新用到的被调用者保存寄存器
  uint64_t coalescedMoveCount_ = 0;

  // 是否还存在待处理的传送。
  bool hasWorkListMove() const {
    return std::any_of(
        graph_.moves.begin(), graph_.moves.end(),
        [](const Move &move) { return move.state == MoveState::WorkList; });
  }

  // 返回第一条待处理传送的下标;不存在则报错(调用前应确认存在)。
  std::size_t firstWorkListMove() const {
    for (std::size_t i = 0; i < graph_.moves.size(); ++i) {
      if (graph_.moves[i].state == MoveState::WorkList) {
        return i;
      }
    }
    throw BackendError("missing register coalescing worklist move");
  }

  // 返回节点当前有效邻居(排除已压栈简化和已合并的节点)。
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

  // 返回节点关联的、仍处于待处理或活跃状态的传送下标集合。
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

  // 节点是否传送相关(尚有未决传送)。
  bool moveRelated(Node node) const { return !nodeMoves(node).empty(); }

  // 求节点的合并代表(路径压缩):已合并节点顺着 alias 链找到最终代表。
  Node getAlias(Node node) {
    auto found = alias_.find(node);
    if (coalescedNodes_.count(node) == 0 || found == alias_.end()) {
      return node;
    }
    found->second = getAlias(found->second);
    return found->second;
  }

  // 初始化工作表:按度数和是否传送相关把每个待分配节点分到溢出/冻结/简化表。
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

  // 把给定节点关联的活跃传送重新激活为待处理(度数下降后可能重新可合并)。
  void enableMoves(const std::set<Node> &nodes) {
    for (Node node : nodes) {
      for (std::size_t moveIndex : nodeMoves(node)) {
        if (graph_.moves[moveIndex].state == MoveState::Active) {
          graph_.moves[moveIndex].state = MoveState::WorkList;
        }
      }
    }
  }

  // 邻居因某节点被摘除而度数减一;当度数恰好从 colorCount 降到其下时,该节点
  // 由"可能溢出"变为"可着色",重新激活其相关传送并移出溢出表进入冻结/简化表。
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

  // 简化:摘除一个低度数节点,压入选择栈,并对其邻居度数减一。
  void simplify() {
    Node node = *simplifyWorkList_.begin();
    simplifyWorkList_.erase(node);
    selectStack_.push_back(node);
    stackNodes_.insert(node);
    for (Node neighbor : adjacent(node)) {
      decrementDegree(neighbor);
    }
  }

  // George 合并判据的单邻居检查:邻居本身低度数、或已预着色、或已与目标干涉,
  // 则该邻居不会因合并而变得不可着色。
  bool ok(Node neighbor, Node precolored) const {
    return graph_.degree.at(neighbor) < graph_.colorCount ||
           graph_.precolored.count(neighbor) != 0 ||
           graph_.hasEdge(neighbor, precolored);
  }

  // Briggs 保守合并判据:合并后邻居集中高度数节点数少于颜色数,则合并安全。
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

  // 节点若变为非预着色、非传送相关且低度数,则从冻结表移入简化表。
  void addWorkList(Node node) {
    if (graph_.precolored.count(node) == 0 && !moveRelated(node) &&
        graph_.degree[node] < graph_.colorCount) {
      freezeWorkList_.erase(node);
      simplifyWorkList_.insert(node);
    }
  }

  // 把 removed 合并进 keep:更新别名、并接传送表与干涉边,并对受影响邻居减度;
  // 若 keep 因此变为高度数则从冻结表移入溢出表。
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

  // 处理一条待合并传送:取两端别名(优先让预着色端作保留节点);
  // 两端相同则直接算合并成功;两端已干涉或都想删预着色端则标记为受约束;
  // 浮点体与整型体各有特殊处理;通过 George/Briggs 判据决定能否合并。
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

    // 浮点体解冻(P0#3):允许 FPR 虚拟-虚拟及普通虚拟-物理拷贝走 George/Briggs
    // 保守合并。唯一保留冻结的是「函数含调用时、合并代表为 fa0-fa7 预着色节点」
    // 的传送:这类拷贝还承担调用点/返回点浮点参数与返回值落位的顺序隔离,
    // 在含调用函数中与旧行为保持一致;叶函数及 ft/fs 预着色、虚拟-虚拟 FPR
    // 拷贝一律参与合并。另有 BACKEND_DISABLE_FPR_VREG_COALESCING 应急 A/B 开关。
    if (graph_.floatBank) {
      if (!info_.coalesceFPRVirtualCopies) {
        // 应急开关:完整复现改造前的「FPR 体全部冻结」行为,用于严格 A/B 对比。
        move.state = MoveState::Frozen;
        addWorkList(keep);
        addWorkList(removed);
        return;
      }
      if (graph_.containsCall && isFprArgReturnNode(keep)) {
        move.state = MoveState::Frozen;
        addWorkList(keep);
        addWorkList(removed);
        return;
      }
      if (graph_.precolored.count(keep) == 0 &&
          !move.virtualCoalescingEligible) {
        move.state = MoveState::Frozen;
        addWorkList(keep);
        addWorkList(removed);
        return;
      }
    }

    // Cyclic PHI bundles carry an explicit barrier and never enter the move
    // worklist.  Ordinary GPR COPYs and acyclic PHI bundles can therefore use
    // the existing conservative test below.  Keep an emergency A/B switch so
    // the old affinity-only behavior remains directly reproducible.
    if (graph_.precolored.count(keep) == 0 &&
        (!info_.coalesceGPRVirtualCopies ||
         !move.virtualCoalescingEligible)) {
      move.state = MoveState::Frozen;
      addWorkList(keep);
      addWorkList(removed);
      return;
    }

    bool canCombine = false;
    if (graph_.precolored.count(keep) != 0) {
      // 与预着色节点合并:用 George 判据逐个检查被删端的邻居。
      canCombine = true;
      for (Node neighbor : adjacent(removed)) {
        if (!ok(neighbor, keep)) {
          canCombine = false;
          break;
        }
      }
    } else {
      // 两个虚拟寄存器合并:用 Briggs 保守判据检查合并后的邻居集。
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

  // 冻结节点的所有传送:放弃这些合并机会(标记 Frozen);若对端因此不再传送相关
  // 且低度数,则把对端从冻结表移入简化表。
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

  // 冻结:取一个冻结表节点移入简化表,并冻结其全部传送(放弃其合并意愿)。
  void freeze() {
    Node node = *freezeWorkList_.begin();
    freezeWorkList_.erase(node);
    simplifyWorkList_.insert(node);
    freezeMoves(node);
  }

  // 计算溢出优先级键(元组比较,越小越优先溢出)。综合:是否可重物化、是否溢出
  // 生成的临时、以及 热度/度数 代价。可重物化(尤其单指令小 LI)代价更低而更优先
  // 溢出;溢出生成的临时留到最后以保证改写收敛。激进模式下走另一套简化规则。
  std::tuple<int, uint64_t, uint32_t> spillKey(Node node) const {
    const bool rematerializable = info_.rematerializable.count(node) != 0;
    const bool generated = info_.generatedBySpill.count(node) != 0;
    const uint64_t heat = info_.heat.count(node) != 0 ? info_.heat.at(node) : 1;
    const uint64_t degree =
        static_cast<uint64_t>(std::max(1, graph_.degree.at(node)));
    uint64_t score = (heat * 1024U) / degree;

    if (info_.aggressiveLARematerialization) {
      score = generated ? std::numeric_limits<uint64_t>::max() : score;
      return std::make_tuple(rematerializable ? 0 : 1, score, node);
    }

    // Rematerialization is not uniformly cheap.  A signed-12-bit LI costs one
    // instruction and deserves a small spill preference, whereas a large LI
    // or LA expands to multiple instructions.  Treating all of them as
    // categorically cheaper than every normal value made hot loop-invariant
    // constants and global bases the first spill candidates, regardless of
    // weighted use count.  Let the multi-instruction forms compete by the same
    // heat/degree cost as an ordinary spill and keep generated spill temps as
    // the last resort so rewriting still converges.
    const MachineInstr *rematDefinition =
        rematerializable ? &info_.rematerializable.at(node) : nullptr;
    const bool cheapSingleInstructionLI =
        rematDefinition && rematDefinition->opcode == Opcode::LI &&
        rematDefinition->uses.size() == 1 &&
        rematDefinition->uses.front().kind == OperandKind::Imm &&
        rematDefinition->uses.front().immValue >= -2048 &&
        rematDefinition->uses.front().immValue <= 2047;
    if (cheapSingleInstructionLI) {
      score = std::max<uint64_t>(1, score / 2);
    }
    return std::make_tuple(generated ? 1 : 0, score, node);
  }

  // 溢出选择:从溢出表中挑代价最小(spillKey 最小)的节点当作可能溢出,乐观地
  // 移入简化表(先假设能着色,着色阶段若真无颜色再实际溢出),并冻结其传送。
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

  // 求节点当前可用的颜色:从其寄存器类全部可分配寄存器出发,去掉已被邻居
  // (取合并代表后)占用的颜色。
  std::vector<PhysReg> availableColors(Node node) {
    const RegClass rc = info_.classes.at(node);
    std::vector<PhysReg> available = allocationColors(rc);
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

  // 在可用颜色中择优:优先选与传送对端同色的颜色(利于消除拷贝的亲和性);
  // 其次选调用者保存寄存器(避免额外保存开销);再次复用已用过的被调用者保存
  // 寄存器;都没有则取第一个可用颜色。
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

    PhysReg color = PhysReg::NoReg;
    color = choose([&](PhysReg reg) { return affinity.count(reg) != 0; });
    if (color != PhysReg::NoReg) {
      return color;
    }
    color = choose([](PhysReg reg) { return getRegisterInfo(reg).callerSaved; });
    if (color != PhysReg::NoReg) {
      return color;
    }
    color = choose(
        [&](PhysReg reg) { return newlyUsedCalleeSaved_.count(reg) != 0; });
    return color != PhysReg::NoReg ? color : available.front();
  }

  // 着色:按选择栈逆序弹出节点依次分配颜色;无可用颜色则实际溢出。分配到被调用者
  // 保存寄存器时记录之。最后把已合并节点着以其代表的颜色,代表未着色则该节点溢出。
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

// 溢出槽字节大小:GPR64 为 8,其余(GPR32/FPR32)为 4。
int64_t spillSize(RegClass rc) { return rc == RegClass::GPR64 ? 8 : 4; }

// 按寄存器类选择溢出重载(load)操作码。
Opcode spillLoadOpcode(RegClass rc) {
  if (rc == RegClass::FPR32) {
    return Opcode::FLW;
  }
  return rc == RegClass::GPR64 ? Opcode::LD : Opcode::LW;
}

// 按寄存器类选择溢出保存(store)操作码。
Opcode spillStoreOpcode(RegClass rc) {
  if (rc == RegClass::FPR32) {
    return Opcode::FSW;
  }
  return rc == RegClass::GPR64 ? Opcode::SD : Opcode::SW;
}

// 构造一条从溢出槽把值重载到 dst 的加载指令(帧索引 + 偏移 0)。
MachineInstr makeSpillLoad(MachineReg dst, int frameIndex) {
  MachineInstr load;
  load.opcode = spillLoadOpcode(dst.rc);
  load.defs.push_back(MachineOperand::reg(dst));
  load.uses.push_back(MachineOperand::frameIndex(frameIndex));
  load.uses.push_back(MachineOperand::imm(0));
  return load;
}

// 构造一条把 src 保存到溢出槽的存储指令(带副作用,不可被 DCE 删除)。
MachineInstr makeSpillStore(MachineReg src, int frameIndex) {
  MachineInstr store;
  store.opcode = spillStoreOpcode(src.rc);
  store.uses.push_back(MachineOperand::reg(src));
  store.uses.push_back(MachineOperand::frameIndex(frameIndex));
  store.uses.push_back(MachineOperand::imm(0));
  store.sideEffects = true;
  return store;
}

// 溢出改写的统计结果:新生成的临时寄存器集合、加载/存储条数、重物化次数、槽数。
struct SpillRewriteResult {
  std::set<uint32_t> generated;
  uint64_t loads = 0;
  uint64_t stores = 0;
  uint64_t rematerializations = 0;
  uint64_t slots = 0;
};

// 为普通(非重物化)溢出的虚拟寄存器分配栈槽,允许互不冲突者共享同一槽以省空间。
// 关键步骤:按冲突度数降序处理(冲突多者先分配,便于后来者复用);对每个待溢出
// 寄存器,找一个大小/寄存器体一致且与其现有占用者都不冲突的已有槽复用,找不到则
// 新建栈对象。返回虚拟寄存器 id 到栈槽帧索引的映射。
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

// 对溢出的虚拟寄存器改写指令:可重物化者在每个使用点重新发射其定义指令(不占栈),
// 普通溢出者则为使用插入重载、为定义插入保存,并把该处使用/定义换成新临时寄存器。
// 关键步骤:先区分重物化与普通溢出并为后者分配栈槽;逐指令处理——若某定义是被
// 重物化的寄存器则删除该定义指令;对每个溢出使用在指令前插入重载或重物化并替换;
// 对每个溢出定义把结果改写到新临时并在指令后插入保存。返回改写统计。
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
      // 若本指令定义的是被重物化的寄存器,则原定义指令整条删除(改到使用点重发)。
      bool removeDefinition = false;
      for (const MachineOperand &def : instr.defs) {
        if (isVirtualReg(def) && rematerialized.count(def.regValue.id) != 0) {
          removeDefinition = true;
        }
      }
      if (removeDefinition) {
        continue;
      }

      // 处理溢出使用:同一 id 在本指令内只重载/重物化一次,复用同一新临时寄存器。
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
      // 处理溢出定义:结果写入新临时寄存器,并在指令后追加一条保存到栈槽。
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

// 把单个操作数中的虚拟寄存器替换为其分到的物理寄存器;未着色则报错。
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

// 用着色结果改写整个函数:把所有虚拟寄存器换成物理寄存器,并顺带删除源与目标
// 同一物理寄存器的自拷贝(合并后变得冗余的 COPY/F_COPY)。
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

// 统计并记录函数实际用到的被调用者保存寄存器(来自着色结果与显式物理操作数),
// 供frame lowering为其预留保存空间。
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

// 生成一次分配状态的签名(下一个虚拟寄存器号、栈对象数、各块指令数、本轮溢出集)。
// 用于检测多轮改写是否陷入原地打转(签名重复即无进展)。
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

// 寄存器分配主驱动(内部实现)。反复迭代:分析程序与活跃/循环信息,为整型与浮点
// 两体各构图着色;若有溢出则加入选定的重物化候选与一批潜在溢出,改写指令后重来;
// 无溢出则记录被调用者保存寄存器、用颜色改写全函数并返回。带无进展检测与轮数上限
// 兜底。stats 可选,记录轮数/合并数/溢出统计;am 可选,复用外部分析缓存。
void allocateRegistersImpl(MachineFunction &mf, RegAllocStats *stats,
                           MachineFunctionAnalysisManager *am) {
  std::set<uint32_t> generatedBySpill;
  std::string previousSignature;
  int repeatedSignature = 0;

  for (int round = 0; round < kEmergencyRoundLimit; ++round) {
    if (stats != nullptr) {
      ++stats->rounds;
    }
    const MachineLoopAnalysis::Result loopInfo = getLoopInfo(mf, am);
    ProgramInfo info = analyzeProgram(mf, generatedBySpill, loopInfo);
    if (info.classes.empty()) {
      return;
    }
    const MachineLivenessAnalysis::Result live = getLiveness(mf, am);

    ColoringResult gpr =
        IRCAllocator(buildAllocationGraph(mf, info, live, false), info).run();
    ColoringResult fpr =
        IRCAllocator(buildAllocationGraph(mf, info, live, true), info).run();

    // 合并两体的着色与溢出结果。
    std::map<uint32_t, PhysReg> colors = gpr.colors;
    colors.insert(fpr.colors.begin(), fpr.colors.end());
    std::vector<uint32_t> spills = gpr.spills;
    spills.insert(spills.end(), fpr.spills.begin(), fpr.spills.end());
    if (!spills.empty()) {
      // 高压函数若逐轮只挑出一个常量，会退化为与常量数成正比的重分配轮次。
      // 只批量处理冷 LI。循环热 LI/LA 都必须交给着色器的热度模型；否则
      // loop-invariant immediate hoist 和全局基址复用会在这里被反向拆回每次 use
      // 一份。单个直线块中的一处定义+一处使用 heat=3，仍覆盖原本为编译时间
      // 收敛而设置的常见批处理场景。
      for (const auto &[id, definition] : info.rematerializable) {
        const uint64_t heat =
            info.heat.count(id) != 0 ? info.heat.at(id) : 1;
        if (info.aggressiveLARematerialization ||
            (definition.opcode == Opcode::LI && heat <= 3)) {
          spills.push_back(id);
        }
      }
      if (info.classes.size() > 64 || round >= 1) {
        std::vector<uint32_t> batch;
        const std::size_t batchLimit = info.classes.size() > 64 ? 8 : 64;
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
    // 本轮无任何溢出:分配成功,记录被调用者保存寄存器并把颜色落实到指令后返回。
    if (spills.empty()) {
      recordUsedCalleeSaved(mf, colors);
      rewriteFunction(mf, colors);
      return;
    }

    // 合并两体的溢出冲突关系,供栈槽共享判定。
    std::map<uint32_t, std::set<uint32_t>> conflicts = gpr.spillConflicts;
    for (const auto &[id, neighbors] : fpr.spillConflicts) {
      conflicts[id].insert(neighbors.begin(), neighbors.end());
    }
    // 无进展检测:连续多轮溢出签名相同说明改写没有收敛,报错避免死循环。
    const std::string signature = allocationSignature(mf, spills);
    repeatedSignature =
        signature == previousSignature ? repeatedSignature + 1 : 0;
    if (repeatedSignature >= 2) {
      throw BackendError("register allocation made no progress in " + mf.name);
    }
    previousSignature = signature;

    // 按选定溢出集改写指令(插入访存或重物化),使分析缓存失效后进入下一轮。
    SpillRewriteResult rewrite = rewriteSpills(mf, info, spills, conflicts);
    invalidateAnalysesAfterRewrite(mf, am);
    generatedBySpill.insert(rewrite.generated.begin(), rewrite.generated.end());
    if (stats != nullptr) {
      stats->spilledVRegs += spills.size();
      stats->spillLoads += rewrite.loads;
      stats->spillStores += rewrite.stores;
      stats->rematerializations += rewrite.rematerializations;
      stats->spillSlots += rewrite.slots;
    }
  }
  throw BackendError("register allocation emergency round limit exceeded in " +
                     mf.name);
}

} // anonymous namespace

// 寄存器分配对外入口:不带统计、不带外部分析管理器。
void allocateRegisters(MachineFunction &mf) {
  allocateRegistersImpl(mf, nullptr, nullptr);
}

// 寄存器分配对外入口:复用外部分析管理器的缓存。
void allocateRegisters(MachineFunction &mf, MachineFunctionAnalysisManager &am) {
  allocateRegistersImpl(mf, nullptr, &am);
}

// 寄存器分配对外入口:输出分配统计信息。
void allocateRegisters(MachineFunction &mf, RegAllocStats *stats) {
  allocateRegistersImpl(mf, stats, nullptr);
}

} // namespace riscv
