#include "../../../include/riscv/RegAlloc.hpp"

#include "../../../include/riscv/Registers.hpp"
#include "../../../include/riscv/Target.hpp"

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace riscv {
namespace {

using VRegSet = std::unordered_set<uint32_t>;

constexpr PhysReg kLeafGprColors[] = {
    PhysReg::T0, PhysReg::T1, PhysReg::T2, PhysReg::T3,
    PhysReg::T4, PhysReg::T5, PhysReg::A0, PhysReg::A1,
    PhysReg::A2, PhysReg::A3, PhysReg::A4, PhysReg::A5,
    PhysReg::A6, PhysReg::A7};

constexpr PhysReg kNonLeafGprColors[] = {
    PhysReg::S1, PhysReg::S2,  PhysReg::S3,  PhysReg::S4,
    PhysReg::S5, PhysReg::S6,  PhysReg::S7,  PhysReg::S8,
    PhysReg::S9, PhysReg::S10, PhysReg::S11};

constexpr PhysReg kNonLeafCallerGprColors[] = {
    PhysReg::T0, PhysReg::T1, PhysReg::T2, PhysReg::T3,
    PhysReg::T4, PhysReg::T5};

constexpr PhysReg kLeafFprColors[] = {
    PhysReg::FT0, PhysReg::FT1,  PhysReg::FT2,  PhysReg::FT3,
    PhysReg::FT4, PhysReg::FT5,  PhysReg::FT6,  PhysReg::FT7,
    PhysReg::FT8, PhysReg::FT9,  PhysReg::FT10, PhysReg::FT11,
    PhysReg::FA0, PhysReg::FA1,  PhysReg::FA2,  PhysReg::FA3,
    PhysReg::FA4, PhysReg::FA5,  PhysReg::FA6,  PhysReg::FA7};

constexpr PhysReg kNonLeafFprColors[] = {
    PhysReg::FS0, PhysReg::FS1,  PhysReg::FS2,  PhysReg::FS3,
    PhysReg::FS4, PhysReg::FS5,  PhysReg::FS6,  PhysReg::FS7,
    PhysReg::FS8, PhysReg::FS9,  PhysReg::FS10, PhysReg::FS11};

constexpr PhysReg kNonLeafCallerFprColors[] = {
    PhysReg::FT0, PhysReg::FT1,  PhysReg::FT2,  PhysReg::FT3,
    PhysReg::FT4, PhysReg::FT5,  PhysReg::FT6,  PhysReg::FT7,
    PhysReg::FT8, PhysReg::FT9,  PhysReg::FT10, PhysReg::FT11};

constexpr int kMaxSpillIterations = 32;

bool isVirtualReg(const MachineOperand &operand) {
  return operand.kind == OperandKind::Reg && !operand.regValue.physical;
}

bool containsCall(const MachineFunction &mf) {
  for (const MachineBasicBlock &block : mf.blocks) {
    for (const MachineInstr &instr : block.instrs) {
      if (instr.opcode == Opcode::CALL) {
        return true;
      }
    }
  }
  return false;
}

void rememberClass(std::unordered_map<uint32_t, RegClass> &classes,
                   MachineReg reg) {
  auto [it, inserted] = classes.emplace(reg.id, reg.rc);
  if (!inserted && it->second != reg.rc) {
    throw BackendError("virtual register used with inconsistent classes");
  }
}

void collectVRegs(const MachineFunction &mf,
                  std::unordered_map<uint32_t, RegClass> &classes) {
  for (const MachineBasicBlock &block : mf.blocks) {
    for (const MachineInstr &instr : block.instrs) {
      for (const MachineOperand &def : instr.defs) {
        if (isVirtualReg(def)) {
          rememberClass(classes, def.regValue);
        }
      }
      for (const MachineOperand &use : instr.uses) {
        if (isVirtualReg(use)) {
          rememberClass(classes, use.regValue);
        }
      }
    }
  }
}

void addSet(VRegSet &dst, const VRegSet &src) {
  for (uint32_t value : src) {
    dst.insert(value);
  }
}

std::vector<uint32_t> instrUses(const MachineInstr &instr) {
  std::vector<uint32_t> result;
  for (const MachineOperand &use : instr.uses) {
    if (isVirtualReg(use)) {
      result.push_back(use.regValue.id);
    }
  }
  return result;
}

std::vector<uint32_t> instrDefs(const MachineInstr &instr) {
  std::vector<uint32_t> result;
  for (const MachineOperand &def : instr.defs) {
    if (isVirtualReg(def)) {
      result.push_back(def.regValue.id);
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

  for (std::size_t i = 0; i < blockCount; ++i) {
    for (const MachineInstr &instr : mf.blocks[i].instrs) {
      for (uint32_t use : instrUses(instr)) {
        if (live.defs[i].find(use) == live.defs[i].end()) {
          live.uses[i].insert(use);
        }
      }
      for (uint32_t def : instrDefs(instr)) {
        live.defs[i].insert(def);
      }
    }
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t reverseIndex = 0; reverseIndex < blockCount;
         ++reverseIndex) {
      const std::size_t i = blockCount - reverseIndex - 1;

      VRegSet newOut;
      for (int succ : mf.blocks[i].succs) {
        if (succ < 0 || succ >= static_cast<int>(blockCount)) {
          throw BackendError("invalid CFG successor during register allocation");
        }
        addSet(newOut, live.liveIn[static_cast<std::size_t>(succ)]);
      }

      VRegSet newIn = live.uses[i];
      for (uint32_t value : newOut) {
        if (live.defs[i].find(value) == live.defs[i].end()) {
          newIn.insert(value);
        }
      }

      if (newIn != live.liveIn[i] || newOut != live.liveOut[i]) {
        live.liveIn[i] = std::move(newIn);
        live.liveOut[i] = std::move(newOut);
        changed = true;
      }
    }
  }

  return live;
}

using Interference = std::unordered_map<uint32_t, VRegSet>;
using ForbiddenColors = std::unordered_map<uint32_t, std::set<PhysReg>>;

void addInterference(Interference &graph,
                     const std::unordered_map<uint32_t, RegClass> &classes,
                     uint32_t lhs, uint32_t rhs) {
  const bool lhsFpr = classes.at(lhs) == RegClass::FPR32;
  const bool rhsFpr = classes.at(rhs) == RegClass::FPR32;
  if (lhs == rhs || lhsFpr != rhsFpr) {
    return;
  }
  graph[lhs].insert(rhs);
  graph[rhs].insert(lhs);
}

struct AllocationGraph {
  Interference interference;
  ForbiddenColors forbiddenColors;
  VRegSet liveAcrossCall;
};

struct ColoringResult {
  std::unordered_map<uint32_t, PhysReg> allocation;
  std::vector<uint32_t> spills;
};

void forbidColors(ForbiddenColors &forbidden,
                  const std::vector<uint32_t> &vregs,
                  const std::vector<PhysReg> &regs) {
  for (uint32_t vreg : vregs) {
    for (PhysReg reg : regs) {
      forbidden[vreg].insert(reg);
    }
  }
}

AllocationGraph buildAllocationGraph(
    const MachineFunction &mf,
    const std::unordered_map<uint32_t, RegClass> &classes,
    const Liveness &live) {
  AllocationGraph result;
  Interference graph;
  for (const auto &[id, rc] : classes) {
    (void)rc;
    graph.emplace(id, VRegSet{});
  }

  for (std::size_t blockIndex = 0; blockIndex < mf.blocks.size();
       ++blockIndex) {
    VRegSet currentLive = live.liveOut[blockIndex];
    const MachineBasicBlock &block = mf.blocks[blockIndex];
    for (auto it = block.instrs.rbegin(); it != block.instrs.rend(); ++it) {
      if (it->opcode == Opcode::CALL) {
        addSet(result.liveAcrossCall, currentLive);
      }
      const std::vector<uint32_t> defs = instrDefs(*it);
      const std::vector<uint32_t> uses = instrUses(*it);

      if (!it->implicitDefs.empty()) {
        std::vector<uint32_t> protectedVRegs = uses;
        protectedVRegs.insert(protectedVRegs.end(), defs.begin(), defs.end());
        protectedVRegs.insert(protectedVRegs.end(), currentLive.begin(),
                              currentLive.end());
        std::sort(protectedVRegs.begin(), protectedVRegs.end());
        protectedVRegs.erase(
            std::unique(protectedVRegs.begin(), protectedVRegs.end()),
            protectedVRegs.end());
        forbidColors(result.forbiddenColors, protectedVRegs, it->implicitDefs);
      }

      for (uint32_t def : defs) {
        for (uint32_t liveReg : currentLive) {
          addInterference(graph, classes, def, liveReg);
        }
      }
      for (uint32_t def : defs) {
        currentLive.erase(def);
      }
      for (uint32_t use : uses) {
        currentLive.insert(use);
      }
    }
  }

  result.interference = std::move(graph);
  return result;
}

std::vector<PhysReg> colorsFor(RegClass rc, bool hasCall,
                               bool liveAcrossCall) {
  if (rc == RegClass::FPR32) {
    if (!hasCall) {
      return std::vector<PhysReg>(std::begin(kLeafFprColors),
                                  std::end(kLeafFprColors));
    }
    std::vector<PhysReg> colors(std::begin(kNonLeafFprColors),
                                std::end(kNonLeafFprColors));
    if (!liveAcrossCall) {
      colors.insert(colors.end(), std::begin(kNonLeafCallerFprColors),
                    std::end(kNonLeafCallerFprColors));
    }
    return colors;
  }

  if (!hasCall) {
    return std::vector<PhysReg>(std::begin(kLeafGprColors),
                                std::end(kLeafGprColors));
  }
  std::vector<PhysReg> colors(std::begin(kNonLeafGprColors),
                              std::end(kNonLeafGprColors));
  if (!liveAcrossCall) {
    colors.insert(colors.end(), std::begin(kNonLeafCallerGprColors),
                  std::end(kNonLeafCallerGprColors));
  }
  return colors;
}

std::vector<PhysReg> colorsForNode(RegClass rc, bool hasCall,
                                   bool liveAcrossCall, uint32_t node,
                                   const ForbiddenColors &forbiddenColors) {
  std::vector<PhysReg> colors = colorsFor(rc, hasCall, liveAcrossCall);
  auto forbidden = forbiddenColors.find(node);
  if (forbidden == forbiddenColors.end()) {
    return colors;
  }
  colors.erase(std::remove_if(colors.begin(), colors.end(), [&](PhysReg reg) {
                 return forbidden->second.find(reg) != forbidden->second.end();
               }),
               colors.end());
  return colors;
}

int remainingDegree(const Interference &graph,
                    const std::unordered_set<uint32_t> &remaining,
                    uint32_t node) {
  int degree = 0;
  auto found = graph.find(node);
  if (found == graph.end()) {
    return degree;
  }
  for (uint32_t neighbor : found->second) {
    if (remaining.find(neighbor) != remaining.end()) {
      ++degree;
    }
  }
  return degree;
}

ColoringResult colorClass(
    RegClass rc, bool hasCall, const std::vector<uint32_t> &classNodes,
    const Interference &graph, const VRegSet &liveAcrossCall,
    const ForbiddenColors &forbiddenColors, const std::string &functionName) {
  ColoringResult result;
  if (classNodes.empty()) {
    return result;
  }

  std::unordered_set<uint32_t> remaining(classNodes.begin(), classNodes.end());
  std::unordered_map<uint32_t, int> degree;
  std::unordered_map<uint32_t, int> colorLimit;
  std::vector<uint32_t> simplifiable;
  simplifiable.reserve(classNodes.size());
  for (uint32_t node : classNodes) {
    const std::vector<PhysReg> colors = colorsForNode(
        rc, hasCall, liveAcrossCall.find(node) != liveAcrossCall.end(), node,
        forbiddenColors);
    colorLimit[node] = static_cast<int>(colors.size());
    degree[node] = remainingDegree(graph, remaining, node);
    if (degree[node] < colorLimit[node]) {
      simplifiable.push_back(node);
    }
  }

  std::vector<uint32_t> stack;
  while (!remaining.empty()) {
    uint32_t selected = 0;

    while (!simplifiable.empty()) {
      uint32_t candidate = simplifiable.back();
      simplifiable.pop_back();
      if (remaining.find(candidate) == remaining.end()) {
        continue;
      }
      if (degree[candidate] < colorLimit[candidate]) {
        selected = candidate;
        break;
      }
    }

    if (selected == 0) {
      selected = *std::max_element(
          remaining.begin(), remaining.end(), [&](uint32_t lhs, uint32_t rhs) {
            return degree[lhs] < degree[rhs];
          });
    }

    stack.push_back(selected);
    remaining.erase(selected);

    auto neighbors = graph.find(selected);
    if (neighbors == graph.end()) {
      continue;
    }
    for (uint32_t neighbor : neighbors->second) {
      if (remaining.find(neighbor) == remaining.end()) {
        continue;
      }
      int &neighborDegree = degree[neighbor];
      if (neighborDegree > 0) {
        --neighborDegree;
      }
      if (neighborDegree < colorLimit[neighbor]) {
        simplifiable.push_back(neighbor);
      }
    }
  }

  while (!stack.empty()) {
    uint32_t node = stack.back();
    stack.pop_back();

    const std::vector<PhysReg> colors = colorsForNode(
        rc, hasCall, liveAcrossCall.find(node) != liveAcrossCall.end(), node,
        forbiddenColors);
    if (colors.empty()) {
      result.spills.push_back(node);
      continue;
    }

    std::set<PhysReg> usedColors;
    auto neighbors = graph.find(node);
    if (neighbors != graph.end()) {
      for (uint32_t neighbor : neighbors->second) {
        auto colored = result.allocation.find(neighbor);
        if (colored != result.allocation.end()) {
          usedColors.insert(colored->second);
        }
      }
    }

    auto color = std::find_if(colors.begin(), colors.end(), [&](PhysReg reg) {
      return usedColors.find(reg) == usedColors.end();
    });
    if (color == colors.end()) {
      (void)functionName;
      result.spills.push_back(node);
      continue;
    }
    result.allocation.emplace(node, *color);
  }

  return result;
}

ColoringResult colorGraph(
    const std::unordered_map<uint32_t, RegClass> &classes,
    const Interference &graph, const VRegSet &liveAcrossCall,
    const ForbiddenColors &forbiddenColors, bool hasCall,
    const std::string &functionName) {
  std::vector<uint32_t> gprs;
  std::vector<uint32_t> fprs;
  for (const auto &[id, rc] : classes) {
    if (rc == RegClass::FPR32) {
      fprs.push_back(id);
    } else {
      gprs.push_back(id);
    }
  }
  std::sort(gprs.begin(), gprs.end());
  std::sort(fprs.begin(), fprs.end());

  ColoringResult result =
      colorClass(RegClass::GPR64, hasCall, gprs, graph, liveAcrossCall,
                 forbiddenColors, functionName);
  ColoringResult fprResult =
      colorClass(RegClass::FPR32, hasCall, fprs, graph, liveAcrossCall,
                 forbiddenColors, functionName);
  result.allocation.insert(fprResult.allocation.begin(),
                           fprResult.allocation.end());
  result.spills.insert(result.spills.end(), fprResult.spills.begin(),
                       fprResult.spills.end());
  return result;
}

void rewriteOperand(
    MachineOperand &operand, const std::unordered_map<uint32_t, PhysReg> &colors) {
  if (!isVirtualReg(operand)) {
    return;
  }
  const MachineReg oldReg = operand.regValue;
  auto color = colors.find(oldReg.id);
  if (color == colors.end()) {
    throw BackendError("uncolored virtual register after graph coloring");
  }
  operand.regValue = MachineReg::preg(color->second, oldReg.rc);
}

void rewriteFunction(MachineFunction &mf,
                     const std::unordered_map<uint32_t, PhysReg> &colors) {
  for (MachineBasicBlock &block : mf.blocks) {
    for (MachineInstr &instr : block.instrs) {
      for (MachineOperand &def : instr.defs) {
        rewriteOperand(def, colors);
      }
      for (MachineOperand &use : instr.uses) {
        rewriteOperand(use, colors);
      }
    }
  }
}

void recordUsedCalleeSaved(
    MachineFunction &mf, const std::unordered_map<uint32_t, PhysReg> &colors) {
  std::set<PhysReg> used;
  for (const auto &[id, reg] : colors) {
    (void)id;
    if (getRegisterInfo(reg).calleeSaved) {
      used.insert(reg);
    }
  }

  for (const MachineBasicBlock &block : mf.blocks) {
    for (const MachineInstr &instr : block.instrs) {
      for (const MachineOperand &def : instr.defs) {
        if (def.kind == OperandKind::Reg && def.regValue.physical &&
            getRegisterInfo(def.regValue.phys).calleeSaved) {
          used.insert(def.regValue.phys);
        }
      }
      for (const MachineOperand &use : instr.uses) {
        if (use.kind == OperandKind::Reg && use.regValue.physical &&
            getRegisterInfo(use.regValue.phys).calleeSaved) {
          used.insert(use.regValue.phys);
        }
      }
    }
  }

  mf.usedCalleeSaved.assign(used.begin(), used.end());
}

int64_t spillSize(RegClass rc) {
  return rc == RegClass::GPR64 ? 8 : 4;
}

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

void spillVirtualRegisters(
    MachineFunction &mf, const std::unordered_map<uint32_t, RegClass> &classes,
    const std::vector<uint32_t> &spills) {
  std::unordered_map<uint32_t, int> spillSlots;
  for (uint32_t id : spills) {
    if (spillSlots.find(id) != spillSlots.end()) {
      continue;
    }
    RegClass rc = classes.at(id);
    spillSlots[id] = mf.createStackObject(spillSize(rc), spillSize(rc),
                                          "spill." + std::to_string(id));
  }

  for (MachineBasicBlock &block : mf.blocks) {
    std::vector<MachineInstr> rewritten;
    for (MachineInstr instr : block.instrs) {
      for (MachineOperand &use : instr.uses) {
        if (!isVirtualReg(use)) {
          continue;
        }
        auto slot = spillSlots.find(use.regValue.id);
        if (slot == spillSlots.end()) {
          continue;
        }
        MachineReg tmp = mf.createVReg(use.regValue.rc);
        rewritten.push_back(makeSpillLoad(tmp, slot->second));
        use.regValue = tmp;
      }

      std::vector<MachineInstr> stores;
      for (MachineOperand &def : instr.defs) {
        if (!isVirtualReg(def)) {
          continue;
        }
        auto slot = spillSlots.find(def.regValue.id);
        if (slot == spillSlots.end()) {
          continue;
        }
        MachineReg tmp = mf.createVReg(def.regValue.rc);
        def.regValue = tmp;
        stores.push_back(makeSpillStore(tmp, slot->second));
      }

      rewritten.push_back(std::move(instr));
      rewritten.insert(rewritten.end(), stores.begin(), stores.end());
    }
    block.instrs = std::move(rewritten);
  }
}

void allocateRegistersImpl(MachineFunction &mf, int depth) {
  if (depth > kMaxSpillIterations) {
    throw BackendError("register allocation spill iteration limit exceeded in " +
                       mf.name);
  }

  std::unordered_map<uint32_t, RegClass> classes;
  collectVRegs(mf, classes);
  if (classes.empty()) {
    return;
  }

  const Liveness live = computeLiveness(mf);
  const AllocationGraph graph = buildAllocationGraph(mf, classes, live);
  const ColoringResult colors =
      colorGraph(classes, graph.interference, graph.liveAcrossCall,
                 graph.forbiddenColors, containsCall(mf), mf.name);
  if (!colors.spills.empty()) {
    spillVirtualRegisters(mf, classes, colors.spills);
    allocateRegistersImpl(mf, depth + 1);
    return;
  }

  recordUsedCalleeSaved(mf, colors.allocation);
  rewriteFunction(mf, colors.allocation);
}

} // namespace

void allocateRegisters(MachineFunction &mf) {
  allocateRegistersImpl(mf, 0);
}

} // namespace riscv
