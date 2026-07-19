#include "aarch64/RegAlloc.hpp"

#include "aarch64/Registers.hpp"
#include "aarch64/Target.hpp"
#include "aarch64/opt/MachineAnalysis.hpp"

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

namespace aarch64 {
namespace {

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

std::vector<PhysReg> allocationColors(RegClass rc) {
  return allocatableRegs(rc);
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

void invalidateAnalysesAfterRewrite(MachineFunction &mf,
                                    MachineFunctionAnalysisManager *am) {
  if (am != nullptr) {
    am->invalidate(mf, MachinePreservationStatus::none());
  }
}

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

struct ProgramInfo {
  std::map<uint32_t, RegClass> classes;
  std::map<uint32_t, uint64_t> heat;
  std::map<uint32_t, MachineInstr> rematerializable;
  std::set<uint32_t> generatedBySpill;
  std::set<std::size_t> loopLatchBlocks;
  bool aggressiveLARematerialization = false;
  bool coalesceGPRVirtualCopies = true;
};

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

enum class MoveState { WorkList, Active, Coalesced, Constrained, Frozen };

struct Move {
  Node dst = 0;
  Node src = 0;
  MoveState state = MoveState::WorkList;
  bool virtualCoalescingEligible = false;
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

std::string allocationNodeName(Node node) {
  if (isPhysicalNode(node)) {
    return std::string("physical register ") + regName(nodePhysicalReg(node));
  }
  return "virtual register " + std::to_string(node);
}

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
  validateAllocationGraph(graph);
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
  IRCAllocator(AllocationGraph graph, const ProgramInfo &info)
      : graph_(std::move(graph)), info_(info) {
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

  void simplify() {
    Node node = *simplifyWorkList_.begin();
    simplifyWorkList_.erase(node);
    selectStack_.push_back(node);
    stackNodes_.insert(node);
    for (Node neighbor : adjacent(node)) {
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

    // 浮点参数和返回值 COPY 还承担栈上传参与 fa0-fa7 之间的顺序隔离。
    // 在当前 MIR 中将其与预着色节点合并会丢失该顺序信息。
    if (graph_.floatBank) {
      move.state = MoveState::Frozen;
      addWorkList(keep);
      addWorkList(removed);
      return;
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
    if (spills.empty()) {
      recordUsedCalleeSaved(mf, colors);
      rewriteFunction(mf, colors);
      return;
    }

    std::map<uint32_t, std::set<uint32_t>> conflicts = gpr.spillConflicts;
    for (const auto &[id, neighbors] : fpr.spillConflicts) {
      conflicts[id].insert(neighbors.begin(), neighbors.end());
    }
    const std::string signature = allocationSignature(mf, spills);
    repeatedSignature =
        signature == previousSignature ? repeatedSignature + 1 : 0;
    if (repeatedSignature >= 2) {
      throw BackendError("register allocation made no progress in " + mf.name);
    }
    previousSignature = signature;

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

void allocateRegisters(MachineFunction &mf) {
  allocateRegistersImpl(mf, nullptr, nullptr);
}

void allocateRegisters(MachineFunction &mf, MachineFunctionAnalysisManager &am) {
  allocateRegistersImpl(mf, nullptr, &am);
}

void allocateRegisters(MachineFunction &mf, RegAllocStats *stats) {
  allocateRegistersImpl(mf, stats, nullptr);
}

} // namespace aarch64
