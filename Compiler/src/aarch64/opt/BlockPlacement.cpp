#include "aarch64/opt/MachineOptPasses.hpp"

#include "aarch64/Passes.hpp"
#include "aarch64/Target.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace aarch64 {
// Block Placement pass 的公共实现。
//
// 作用：在保持逻辑 CFG 完全不变的前提下，按分支频率与 fallthrough 收益重排
// 机器基本块的线性顺序，让高频边尽量成为相邻块之间的 fallthrough，从而减少
// 显式跳转（尤其是动态执行次数多的跳转），并在需要时反转条件分支方向。
//
// 流水线位置：属于机器无关后端流水线的后期布局阶段，依赖机器循环分析
// （MachineLoopAnalysis）与机器块频率分析（MachineBlockFrequencyAnalysis）。
//
// RISC-V 与 ARM64 两后端使用相同实现
// 两后端各保留一份相同实现。文件内不含 namespace 包裹，直接是命名空间内的正文。

namespace {

// 一个块结尾的控制流形态：返回、无条件跳转、条件分支。
enum class ControlKind { Return, Unconditional, Conditional };

// 布局前固化下来的一个块的控制流信息。因为重排会改变块相邻关系，条件分支的
// fallthrough（false 目标）在移动前必须先记录，否则事后无法还原语义。
struct SavedControl {
  ControlKind kind = ControlKind::Return;
  std::size_t originalIndex = 0;   // 该块在原始顺序中的下标
  MachineInstr terminator;         // 原始终结指令（RET 或条件分支）
  std::string trueTarget;          // 条件为真 / 无条件跳转的目标块标签
  std::string falseTarget;         // 条件为假（原 fallthrough）的目标块标签
};

// 带权重的 CFG 边，权重来自块频率分析，用于 Pettis-Hansen 链式布局。
struct WeightedEdge {
  int from = -1;
  int to = -1;
  uint64_t weight = 0;
};

// 一种候选布局的代价，按字典序比较；靠前的维度权重更高。
// P1#10:新增 brokenUnknownFallthroughWeight(方向未知 50/50 的条件分支上被
// 打断的 fallthrough 边权——这类分支按"不推翻原方向"规则保持原方向)与
// takenConditionalWeight(条件分支被实际执行侧的总边权,诊断用)。
struct LayoutCost {
  uint64_t dynamicJumpExecutions = 0;   // 显式跳转的动态执行次数（最重要）
  uint64_t staticJumpCount = 0;         // 静态跳转指令条数（代码体积）
  uint64_t brokenFallthroughWeight = 0; // 未能成为 fallthrough 的边权之和
  uint64_t brokenUnknownFallthroughWeight = 0; // 方向未知分支上的断边权
  uint64_t takenConditionalWeight = 0;  // 条件分支执行侧总权重
  uint64_t blockDisplacement = 0;       // 块相对原位置的总位移（稳定性 tie-break）

  bool operator<(const LayoutCost &other) const {
    if (dynamicJumpExecutions != other.dynamicJumpExecutions) {
      return dynamicJumpExecutions < other.dynamicJumpExecutions;
    }
    if (staticJumpCount != other.staticJumpCount) {
      return staticJumpCount < other.staticJumpCount;
    }
    if (brokenFallthroughWeight != other.brokenFallthroughWeight) {
      return brokenFallthroughWeight < other.brokenFallthroughWeight;
    }
    return blockDisplacement < other.blockDisplacement;
  }
};

using LabelEdge = std::pair<std::string, std::string>;
using LabelEdgeSet = std::set<LabelEdge>;

// 判断操作码是否为条件分支（用于识别与反转终结指令）。
bool isConditionalBranch(Opcode opcode) {
  switch (opcode) {
  case Opcode::BEQZ:
  case Opcode::BNEZ:
  case Opcode::BEQ:
  case Opcode::BNE:
  case Opcode::BLT:
  case Opcode::BGE:
    return true;
  default:
    return false;
  }
}

// 判断操作码是否为本 pass 能识别并可安全重建的终结指令。
bool isKnownTerminator(Opcode opcode) {
  return opcode == Opcode::RET || opcode == Opcode::J ||
         isConditionalBranch(opcode);
}

// 求条件分支的反转操作码（如 BEQ 反转为 BNE）。当块被移动、原 fallthrough
// 目标不再相邻时，需要反转分支方向来重新指向新的相邻块。返回是否成功。
bool invertedBranch(Opcode opcode, Opcode &inverted) {
  switch (opcode) {
  case Opcode::BEQZ:
    inverted = Opcode::BNEZ;
    return true;
  case Opcode::BNEZ:
    inverted = Opcode::BEQZ;
    return true;
  case Opcode::BEQ:
    inverted = Opcode::BNE;
    return true;
  case Opcode::BNE:
    inverted = Opcode::BEQ;
    return true;
  case Opcode::BLT:
    inverted = Opcode::BGE;
    return true;
  case Opcode::BGE:
    inverted = Opcode::BLT;
    return true;
  default:
    return false;
  }
}

// 是否启用变换后的 MIR 校验。debug 构建默认开启，release 下由环境变量控制。
bool verificationEnabled() {
#ifndef NDEBUG
  return true;
#else
  return std::getenv("BACKEND_VERIFY_MIR") != nullptr;
#endif
}

// 是否输出布局统计信息（由环境变量控制），用于调参与诊断。
bool statisticsEnabled() {
  return std::getenv("BACKEND_BLOCK_PLACEMENT_STATS") != nullptr;
}

// P1#10 A/B 开关:BACKEND_DISABLE_PASSES 含 block-placement-rotate(或 all)时,
// 关闭"真循环旋转 + trip count 注入"(供 backend_ab.py 的 disable:block-placement-rotate
// 配置做 A/B 对照);与 MachineAnalysis 中 blockPlacementRotateDisabled 保持同一语义。
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

// 布局后分支位移检查:按每条指令 4 字节的保守估计,计算所有条件分支到目标块
// 的距离。任一条件分支超出 RISC-V 条件分支范围(±4KB,留 64 字节余量)即返回
// false,由调用方回退到原始顺序。AArch64 的条件分支范围为 ±1MB,同一检查对
// 其更保守,不会误判合法布局。
bool branchesInRange(const MachineFunction &mf) {
  const std::size_t blockCount = mf.blocks.size();
  if (blockCount == 0) {
    return true;
  }
  std::vector<uint64_t> blockOffset(blockCount, 0);
  uint64_t offset = 0;
  for (std::size_t block = 0; block < blockCount; ++block) {
    blockOffset[block] = offset;
    offset += static_cast<uint64_t>(mf.blocks[block].instrs.size()) * 4;
  }
  constexpr int64_t kBranchRangeLimit = 4096 - 64;
  for (std::size_t block = 0; block < blockCount; ++block) {
    const MachineBasicBlock &bb = mf.blocks[block];
    for (std::size_t instr = 0; instr < bb.instrs.size(); ++instr) {
      const MachineInstr &mi = bb.instrs[instr];
      if (!isConditionalBranch(mi.opcode)) {
        continue;
      }
      std::string target;
      for (const MachineOperand &use : mi.uses) {
        if (use.kind == OperandKind::Block) {
          target = use.blockLabel;
          break;
        }
      }
      if (target.empty()) {
        throw BackendError("machine conditional branch has no block target");
      }
      const uint64_t branchOffset = blockOffset[block] + instr * 4;
      bool found = false;
      for (std::size_t targetBlock = 0; targetBlock < blockCount;
           ++targetBlock) {
        if (mf.blocks[targetBlock].label != target) {
          continue;
        }
        found = true;
        const int64_t delta =
            static_cast<int64_t>(blockOffset[targetBlock]) -
            static_cast<int64_t>(branchOffset);
        if (delta > kBranchRangeLimit || delta < -kBranchRangeLimit) {
          return false;
        }
        break;
      }
      if (!found) {
        throw BackendError("machine branch targets unknown block " + target);
      }
    }
  }
  return true;
}

// 以下 same* 系列用于逐字段比较指令/操作数/整个程序，供“变换前后是否真正改变
// 了程序”与校验逻辑使用。
bool sameReg(const MachineReg &lhs, const MachineReg &rhs) {
  return lhs.physical == rhs.physical && lhs.id == rhs.id && lhs.rc == rhs.rc &&
         lhs.phys == rhs.phys;
}

bool sameOperand(const MachineOperand &lhs, const MachineOperand &rhs) {
  if (lhs.kind != rhs.kind) {
    return false;
  }
  switch (lhs.kind) {
  case OperandKind::Reg:
    return sameReg(lhs.regValue, rhs.regValue);
  case OperandKind::Imm:
    return lhs.immValue == rhs.immValue;
  case OperandKind::FrameIndex:
    return lhs.frameIndexValue == rhs.frameIndexValue;
  case OperandKind::Global:
    return lhs.symbol == rhs.symbol;
  case OperandKind::Block:
    return lhs.blockLabel == rhs.blockLabel;
  case OperandKind::ValueRef:
    return lhs.valueRef == rhs.valueRef;
  }
  return false;
}

// 比较两条指令的操作数列表。ignoreBlockTargets 为真时跳过块目标操作数的比较，
// 用于“除跳转目标外其余是否相同”的判断（反转分支时目标会变但其余不变）。
bool sameOperandList(const std::vector<MachineOperand> &lhs,
                     const std::vector<MachineOperand> &rhs,
                     bool ignoreBlockTargets) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    if (ignoreBlockTargets && lhs[i].kind == OperandKind::Block &&
        rhs[i].kind == OperandKind::Block) {
      continue;
    }
    if (!sameOperand(lhs[i], rhs[i])) {
      return false;
    }
  }
  return true;
}

// 比较两条指令是否完全一致。ignoreOpcodeAndBlockTargets 用于校验反转分支：
// 此时操作码与块目标允许不同，只要求其余字段一致。
bool sameInstruction(const MachineInstr &lhs, const MachineInstr &rhs,
                     bool ignoreOpcodeAndBlockTargets = false) {
  return (ignoreOpcodeAndBlockTargets || lhs.opcode == rhs.opcode) &&
         sameOperandList(lhs.defs, rhs.defs, false) &&
         sameOperandList(lhs.uses, rhs.uses, ignoreOpcodeAndBlockTargets) &&
         lhs.implicitDefs == rhs.implicitDefs &&
         lhs.implicitUses == rhs.implicitUses &&
         lhs.sideEffects == rhs.sideEffects &&
         lhs.terminator == rhs.terminator && lhs.comment == rhs.comment;
}

// 逐块逐指令比较两个块序列是否完全相同，用于判断布局是否实际产生了变化。
bool sameProgram(const std::vector<MachineBasicBlock> &lhs,
                 const std::vector<MachineBasicBlock> &rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t block = 0; block < lhs.size(); ++block) {
    if (lhs[block].label != rhs[block].label ||
        lhs[block].instrs.size() != rhs[block].instrs.size()) {
      return false;
    }
    for (std::size_t instr = 0; instr < lhs[block].instrs.size(); ++instr) {
      if (!sameInstruction(lhs[block].instrs[instr],
                           rhs[block].instrs[instr])) {
        return false;
      }
    }
  }
  return true;
}

// 饱和加法，避免频率权重累加时溢出（溢出则钳到最大值）。
uint64_t saturatingAdd(uint64_t lhs, uint64_t rhs) {
  const uint64_t limit = std::numeric_limits<uint64_t>::max();
  return rhs > limit - lhs ? limit : lhs + rhs;
}

// 取分支指令唯一的块目标标签；有多个或没有块目标都视为非法。
std::string branchTarget(const MachineInstr &instr) {
  std::string target;
  for (const MachineOperand &use : instr.uses) {
    if (use.kind != OperandKind::Block) {
      continue;
    }
    if (!target.empty()) {
      throw BackendError("machine branch has multiple block targets");
    }
    target = use.blockLabel;
  }
  if (target.empty()) {
    throw BackendError("machine branch has no block target");
  }
  return target;
}

// 从频率分析结果中查出 from 到 to 这条边的权重；表不一致或边缺失均报错。
uint64_t edgeWeightForTarget(
    const MachineBlockFrequencyAnalysis::Result &frequencies, std::size_t from,
    std::size_t to) {
  if (from >= frequencies.successors.size() ||
      from >= frequencies.edgeWeight.size()) {
    throw BackendError("stale machine frequency edge lookup");
  }
  const auto &successors = frequencies.successors[from];
  const auto &weights = frequencies.edgeWeight[from];
  if (successors.size() != weights.size()) {
    throw BackendError("malformed machine frequency edge table");
  }
  for (std::size_t index = 0; index < successors.size(); ++index) {
    if (successors[index] == static_cast<int>(to)) {
      return weights[index];
    }
  }
  throw BackendError("missing machine frequency edge");
}

// 把分支指令的唯一块目标就地改写为 target。
void setBranchTarget(MachineInstr &instr, const std::string &target) {
  bool replaced = false;
  for (MachineOperand &use : instr.uses) {
    if (use.kind != OperandKind::Block) {
      continue;
    }
    if (replaced) {
      throw BackendError("machine branch has multiple block targets");
    }
    use.blockLabel = target;
    replaced = true;
  }
  if (!replaced) {
    throw BackendError("machine branch has no block target");
  }
}

// 在块末尾追加一条到 label 的无条件跳转（当目标不再是相邻块时用于补回控制流）。
void appendJump(MachineBasicBlock &block, const std::string &label) {
  MachineInstr jump;
  jump.opcode = Opcode::J;
  jump.uses.push_back(MachineOperand::block(label));
  jump.terminator = true;
  block.instrs.push_back(std::move(jump));
}

// 遍历函数所有块，把每个块结尾的控制流固化到 controls 中，并收集逻辑边集合
// edges（供事后校验 CFG 未变）。返回 false 表示遇到无法可靠重建的控制流形态
// （未知终结指令、不可反转的条件码等），此时调用方应放弃对该函数做布局。
// 关键点：条件分支的 false 目标此刻等于旧布局里的相邻块，必须在移动前记下。
bool saveControlFlow(const MachineFunction &mf,
                     std::map<std::string, SavedControl> &controls,
                     LabelEdgeSet &edges) {
  std::set<std::string> labels;
  for (const MachineBasicBlock &block : mf.blocks) {
    if (block.label.empty() || !labels.insert(block.label).second) {
      throw BackendError("duplicate or empty machine block label");
    }
  }

  auto validateTarget = [&](const std::string &target) {
    if (labels.count(target) == 0) {
      throw BackendError("machine branch targets unknown block " + target);
    }
  };

  for (std::size_t index = 0; index < mf.blocks.size(); ++index) {
    const MachineBasicBlock &block = mf.blocks[index];
    std::vector<const MachineInstr *> terminators;
    bool sawTerminator = false;
    for (const MachineInstr &instr : block.instrs) {
      const bool terminator =
          instr.terminator || isKnownTerminator(instr.opcode);
      if (!terminator) {
        if (sawTerminator) {
          return false;
        }
        continue;
      }
      sawTerminator = true;
      if (!isKnownTerminator(instr.opcode)) {
        // 未知条件码或未知 terminator 没有可靠的反转/重建规则。
        return false;
      }
      terminators.push_back(&instr);
    }

    SavedControl saved;
    saved.originalIndex = index;
    if (terminators.empty()) {
      if (index + 1 >= mf.blocks.size()) {
        // MIR 没有表达“无 successor 但也不 RET”的终结形式。
        return false;
      }
      saved.kind = ControlKind::Unconditional;
      saved.trueTarget = mf.blocks[index + 1].label;
    } else if (terminators.size() == 1) {
      const MachineInstr &term = *terminators.front();
      if (term.opcode == Opcode::RET) {
        for (const MachineOperand &use : term.uses) {
          if (use.kind == OperandKind::Block) {
            throw BackendError("RET must not have a machine successor");
          }
        }
        saved.kind = ControlKind::Return;
        saved.terminator = term;
      } else if (term.opcode == Opcode::J) {
        saved.kind = ControlKind::Unconditional;
        saved.trueTarget = branchTarget(term);
      } else if (isConditionalBranch(term.opcode)) {
        if (index + 1 >= mf.blocks.size()) {
          return false;
        }
        Opcode ignored = Opcode::NOP;
        if (!invertedBranch(term.opcode, ignored)) {
          return false;
        }
        saved.kind = ControlKind::Conditional;
        saved.terminator = term;
        saved.trueTarget = branchTarget(term);
        // 关键：在移动前固化旧布局产生的 false/fallthrough successor。
        saved.falseTarget = mf.blocks[index + 1].label;
      } else {
        return false;
      }
    } else if (terminators.size() == 2 &&
               isConditionalBranch(terminators[0]->opcode) &&
               terminators[1]->opcode == Opcode::J) {
      Opcode ignored = Opcode::NOP;
      if (!invertedBranch(terminators[0]->opcode, ignored)) {
        return false;
      }
      saved.kind = ControlKind::Conditional;
      saved.terminator = *terminators[0];
      saved.trueTarget = branchTarget(*terminators[0]);
      saved.falseTarget = branchTarget(*terminators[1]);
    } else {
      return false;
    }

    if (saved.kind != ControlKind::Return) {
      validateTarget(saved.trueTarget);
      edges.emplace(block.label, saved.trueTarget);
    }
    if (saved.kind == ControlKind::Conditional) {
      validateTarget(saved.falseTarget);
      edges.emplace(block.label, saved.falseTarget);
    }
    controls.emplace(block.label, std::move(saved));
  }
  return true;
}

// 用 Pettis-Hansen 贪心算法构造块的初始链式布局。
// 思路：把 CFG 的非回边按权重从大到小排序，依次尝试把权重大的边的两端块拼成
// 一条链（边的起点块必须是某链的尾、终点块必须是另一链的头），使高频边尽量
// 落在相邻块之间成为 fallthrough。回边被排除（回边天然指向已在前面的头块，
// 不适合作为顺序 fallthrough）。入口块所在链始终保持在最前，不被接到别的链后。
// 最后从入口链出发，按“与已放置块的连接权重、块热度、原始下标”择优拼接剩余链。
std::vector<int> buildPettisHansenOrder(
    std::size_t blockCount,
    const MachineLoopAnalysis::Result &loops,
    const MachineBlockFrequencyAnalysis::Result &frequencies) {
  if (frequencies.blockFrequency.size() != blockCount ||
      frequencies.successors.size() != blockCount ||
      frequencies.edgeWeight.size() != blockCount) {
    throw BackendError("stale block frequency analysis");
  }

  std::vector<WeightedEdge> edges;
  for (std::size_t from = 0; from < blockCount; ++from) {
    if (frequencies.successors[from].size() !=
        frequencies.edgeWeight[from].size()) {
      throw BackendError("malformed block frequency edge table");
    }
    for (std::size_t edge = 0; edge < frequencies.successors[from].size();
         ++edge) {
      const int to = frequencies.successors[from][edge];
      if (to < 0 || to >= static_cast<int>(blockCount)) {
        throw BackendError("invalid weighted CFG edge");
      }
      if (loops.backedges.count({static_cast<int>(from), to}) != 0) {
        continue;
      }
      edges.push_back(
          {static_cast<int>(from), to, frequencies.edgeWeight[from][edge]});
    }
  }
  std::sort(edges.begin(), edges.end(),
            [](const WeightedEdge &lhs, const WeightedEdge &rhs) {
              if (lhs.weight != rhs.weight) {
                return lhs.weight > rhs.weight;
              }
              if (lhs.from != rhs.from) {
                return lhs.from < rhs.from;
              }
              return lhs.to < rhs.to;
            });

  std::vector<std::vector<int>> chains(blockCount);
  std::vector<int> owner(blockCount);
  std::vector<bool> alive(blockCount, true);
  for (std::size_t i = 0; i < blockCount; ++i) {
    chains[i].push_back(static_cast<int>(i));
    owner[i] = static_cast<int>(i);
  }

  // 按权重从大到小合并链：只有 from 是某链尾、to 是另一链头时才能拼接，保证
  // 每条链是一段线性可 fallthrough 的块序列。
  for (const WeightedEdge &edge : edges) {
    const int fromChain = owner[static_cast<std::size_t>(edge.from)];
    const int toChain = owner[static_cast<std::size_t>(edge.to)];
    if (fromChain == toChain ||
        chains[static_cast<std::size_t>(fromChain)].back() != edge.from ||
        chains[static_cast<std::size_t>(toChain)].front() != edge.to) {
      continue;
    }
    // 入口 chain 不能被接到其他 chain 后面，否则入口块不再是第一个。
    if (toChain == owner[0]) {
      continue;
    }
    auto &destination = chains[static_cast<std::size_t>(fromChain)];
    auto &source = chains[static_cast<std::size_t>(toChain)];
    destination.insert(destination.end(), source.begin(), source.end());
    for (int block : source) {
      owner[static_cast<std::size_t>(block)] = fromChain;
    }
    alive[static_cast<std::size_t>(toChain)] = false;
  }

  std::vector<int> order;
  order.reserve(blockCount);
  std::vector<bool> placed(blockCount, false);
  auto appendChain = [&](int chain) {
    for (int block : chains[static_cast<std::size_t>(chain)]) {
      if (placed[static_cast<std::size_t>(block)]) {
        throw BackendError("machine block placed more than once");
      }
      placed[static_cast<std::size_t>(block)] = true;
      order.push_back(block);
    }
    alive[static_cast<std::size_t>(chain)] = false;
  };

  appendChain(owner[0]);
  while (order.size() < blockCount) {
    // 在尚未放置的链里择优：优先与已放置块连接权重大的链，其次热度高的，
    // 最后用最小原始下标做稳定 tie-break，保证确定性输出。
    int best = -1;
    uint64_t bestConnection = 0;
    uint64_t bestHeat = 0;
    int bestOriginal = std::numeric_limits<int>::max();
    for (std::size_t chain = 0; chain < blockCount; ++chain) {
      if (!alive[chain]) {
        continue;
      }
      uint64_t connection = 0;
      uint64_t heat = 0;
      int original = std::numeric_limits<int>::max();
      for (int block : chains[chain]) {
        heat = saturatingAdd(
            heat, frequencies.blockFrequency[static_cast<std::size_t>(block)]);
        original = std::min(original, block);
      }
      for (const WeightedEdge &edge : edges) {
        const bool fromHere = owner[static_cast<std::size_t>(edge.from)] ==
                              static_cast<int>(chain);
        const bool toHere =
            owner[static_cast<std::size_t>(edge.to)] == static_cast<int>(chain);
        if ((fromHere && placed[static_cast<std::size_t>(edge.to)]) ||
            (toHere && placed[static_cast<std::size_t>(edge.from)])) {
          connection = saturatingAdd(connection, edge.weight);
        }
      }
      if (best < 0 || connection > bestConnection ||
          (connection == bestConnection && heat > bestHeat) ||
          (connection == bestConnection && heat == bestHeat &&
           original < bestOriginal)) {
        best = static_cast<int>(chain);
        bestConnection = connection;
        bestHeat = heat;
        bestOriginal = original;
      }
    }
    if (best < 0) {
      throw BackendError("failed to place every machine block");
    }
    appendChain(best);
  }
  if (order.empty() || order.front() != 0) {
    throw BackendError("machine block placement moved the entry block");
  }
  return order;
}

// 两个前向声明，定义在文件后方：weightedFallthrough 统计给定顺序下成为
// fallthrough 的边权之和；totalEdgeWeight 统计所有边权之和。
uint64_t
weightedFallthrough(const std::vector<int> &order,
                    const MachineBlockFrequencyAnalysis::Result &frequencies);

uint64_t
totalEdgeWeight(const MachineBlockFrequencyAnalysis::Result &frequencies);

// 计算某个候选布局 order 的代价 LayoutCost。逐块按其原始控制流形态推算：
// 无条件跳转若目标不再相邻则计一次显式跳转（含动态执行次数）；条件分支若真假
// 目标都不相邻则必然要补一条跳转，动态代价取“落到显式跳转那一侧”的边权。
// useWeightedOrientation 为真时假定会按权重择优反转分支，取两侧较小权重。
LayoutCost computeLayoutCost(
    const MachineFunction &originalMf, const std::vector<int> &order,
    const std::map<std::string, SavedControl> &controls,
    const MachineBlockFrequencyAnalysis::Result &frequencies,
    bool useWeightedOrientation) {
  if (originalMf.blocks.size() != order.size()) {
    throw BackendError("stale machine block placement order");
  }

  LayoutCost cost;
  const uint64_t totalWeight = totalEdgeWeight(frequencies);
  uint64_t weighted = weightedFallthrough(order, frequencies);
  if (weighted > totalWeight) {
    weighted = totalWeight;
  }
  cost.brokenFallthroughWeight = totalWeight - weighted;

  for (std::size_t newPosition = 0; newPosition < order.size(); ++newPosition) {
    const int oldIndex = order[newPosition];
    if (oldIndex < 0 ||
        oldIndex >= static_cast<int>(originalMf.blocks.size())) {
      throw BackendError("invalid machine block placement order");
    }
    const MachineBasicBlock &block =
        originalMf.blocks[static_cast<std::size_t>(oldIndex)];
    const auto savedIt = controls.find(block.label);
    if (savedIt == controls.end()) {
      throw BackendError("lost machine block control-flow record");
    }
    const SavedControl &saved = savedIt->second;
    const std::string next =
        newPosition + 1 < order.size()
            ? originalMf.blocks[static_cast<std::size_t>(order[newPosition + 1])]
                  .label
            : std::string();
    cost.blockDisplacement = saturatingAdd(
        cost.blockDisplacement,
        static_cast<uint64_t>(std::llabs(static_cast<long long>(newPosition) -
                                         static_cast<long long>(saved.originalIndex))));

    switch (saved.kind) {
    case ControlKind::Return:
      break;
    case ControlKind::Unconditional:
      if (saved.trueTarget != next) {
        cost.dynamicJumpExecutions = saturatingAdd(
            cost.dynamicJumpExecutions,
            frequencies.blockFrequency[static_cast<std::size_t>(saved.originalIndex)]);
        ++cost.staticJumpCount;
      }
      break;
    case ControlKind::Conditional: {
      const std::size_t from = saved.originalIndex;
      const std::size_t trueTarget =
          controls.at(saved.trueTarget).originalIndex;
      const std::size_t falseTarget =
          controls.at(saved.falseTarget).originalIndex;
      const uint64_t trueWeight =
          edgeWeightForTarget(frequencies, from, trueTarget);
      const uint64_t falseWeight =
          edgeWeightForTarget(frequencies, from, falseTarget);
      // 条件分支执行侧的总权重(诊断:条件分支的动态执行量)。
      cost.takenConditionalWeight = saturatingAdd(
          cost.takenConditionalWeight, trueWeight);
      // 方向未知(两侧等权 0.5/0.5)的条件分支:按"不推翻原方向"规则保持原
      // 方向,统计其被打断的 fallthrough 边权(非相邻一侧),用于诊断未知方向
      // 分支在布局中的代价。
      if (trueWeight == falseWeight) {
        if (saved.trueTarget == next) {
          cost.brokenUnknownFallthroughWeight = saturatingAdd(
              cost.brokenUnknownFallthroughWeight, falseWeight);
        } else if (saved.falseTarget == next) {
          cost.brokenUnknownFallthroughWeight = saturatingAdd(
              cost.brokenUnknownFallthroughWeight, trueWeight);
        } else {
          cost.brokenUnknownFallthroughWeight = saturatingAdd(
              cost.brokenUnknownFallthroughWeight, trueWeight);
        }
      }
      if (saved.trueTarget == next || saved.falseTarget == next) {
        break;
      }
      ++cost.staticJumpCount;
      if (useWeightedOrientation) {
        cost.dynamicJumpExecutions = saturatingAdd(
            cost.dynamicJumpExecutions, std::min(trueWeight, falseWeight));
      } else {
        cost.dynamicJumpExecutions = saturatingAdd(
            cost.dynamicJumpExecutions, falseWeight);
      }
      break;
    }
    }
  }

  return cost;
}

// 在种子布局基础上，尝试“循环旋转”：把 latch 块移到 header 块之前，使回边
// 变成 fallthrough、循环体的条件分支落在末尾。这类旋转会改变分支方向与代码相邻
// 关系，只有当它能把一条更高频的显式跳转换成更便宜的形态（动态跳转次数严格
// 下降）时才接受，静态体积或位移的 tie-break 不足以作为旋转依据。
void considerBackedgeFallthroughOrders(
    const MachineFunction &mf, const std::vector<int> &seed,
    const LayoutCost &seedCost, const MachineLoopAnalysis::Result &loops,
    const MachineBlockFrequencyAnalysis::Result &frequencies,
    const std::map<std::string, SavedControl> &controls,
    bool useWeightedOrientation, std::vector<int> &bestOrder,
    LayoutCost &bestCost, std::size_t &rotations) {
  if (!useWeightedOrientation ||
      blockPlacementRotateDisabled() ||
      std::getenv("BACKEND_DISABLE_BACKEDGE_FALLTHROUGH_PLACEMENT") !=
          nullptr) {
    return;
  }

  // 迭代应用循环旋转:每轮在"当前最优顺序"上尝试所有回边的 latch 前置 header
  // 旋转,把回边变成 fallthrough、header 的 guard 分支落在循环末尾(检查-执行
  // 结构,每次迭代不再执行显式 J);只接受动态跳转严格下降的旋转,重复直到不再
  // 改进。旧实现每函数只采纳一次旋转;迭代使一个函数内的多个循环都能旋转,
  // 且每个旋转都在前序旋转结果上重新计价,保证总代价单调下降。
  std::vector<int> current = seed;
  LayoutCost currentCost = seedCost;
  bool improved = true;
  while (improved) {
    improved = false;
    int bestLatch = -1;
    std::vector<int> bestTrial;
    LayoutCost bestTrialCost = currentCost;
    for (const auto &[latch, header] : loops.backedges) {
      if (latch < 0 || header <= 0 || latch == header ||
          latch >= static_cast<int>(mf.blocks.size()) ||
          header >= static_cast<int>(mf.blocks.size())) {
        continue;
      }

      const std::string &latchLabel =
          mf.blocks[static_cast<std::size_t>(latch)].label;
      const std::string &headerLabel =
          mf.blocks[static_cast<std::size_t>(header)].label;
      const auto saved = controls.find(latchLabel);
      if (saved == controls.end() ||
          saved->second.kind != ControlKind::Unconditional ||
          saved->second.trueTarget != headerLabel) {
        continue;
      }

      std::vector<int> trial = current;
      const auto latchPosition = std::find(trial.begin(), trial.end(), latch);
      if (latchPosition == trial.end()) {
        throw BackendError("backedge latch missing from placement order");
      }
      trial.erase(latchPosition);
      const auto headerPosition = std::find(trial.begin(), trial.end(), header);
      if (headerPosition == trial.end()) {
        throw BackendError("backedge header missing from placement order");
      }
      trial.insert(headerPosition, latch);

      const LayoutCost trialCost = computeLayoutCost(
          mf, trial, controls, frequencies, useWeightedOrientation);
      // 旋转会改变分支方向与代码相邻关系。只在主目标上接受它:用一个更便宜的
      // 形态替换更高频的显式跳转(动态跳转严格下降)。静态体积或位移 tie-break
      // 单独不足以作为旋转依据。
      if (trialCost.dynamicJumpExecutions >=
          currentCost.dynamicJumpExecutions) {
        continue;
      }
      if (trialCost < bestTrialCost) {
        bestLatch = latch;
        bestTrial = std::move(trial);
        bestTrialCost = trialCost;
      }
    }
    if (bestLatch >= 0 && bestTrialCost < currentCost) {
      current = std::move(bestTrial);
      currentCost = bestTrialCost;
      ++rotations;
      improved = true;
    }
  }
  if (currentCost < bestCost) {
    bestOrder = std::move(current);
    bestCost = currentCost;
  }
}

// 统计给定顺序下，有多少边权落在“相邻块”之间（即成为 fallthrough）。
uint64_t
weightedFallthrough(const std::vector<int> &order,
                    const MachineBlockFrequencyAnalysis::Result &frequencies) {
  std::vector<int> position(order.size(), -1);
  for (std::size_t i = 0; i < order.size(); ++i) {
    position[static_cast<std::size_t>(order[i])] = static_cast<int>(i);
  }
  uint64_t weight = 0;
  for (std::size_t from = 0; from < frequencies.successors.size(); ++from) {
    for (std::size_t edge = 0; edge < frequencies.successors[from].size();
         ++edge) {
      const int to = frequencies.successors[from][edge];
      if (position[from] + 1 == position[static_cast<std::size_t>(to)]) {
        weight = saturatingAdd(weight, frequencies.edgeWeight[from][edge]);
      }
    }
  }
  return weight;
}

// 统计所有 CFG 边的权重总和（fallthrough 收益的分母/上界）。
uint64_t
totalEdgeWeight(const MachineBlockFrequencyAnalysis::Result &frequencies) {
  uint64_t result = 0;
  for (const auto &blockEdges : frequencies.edgeWeight) {
    for (uint64_t weight : blockEdges) {
      result = saturatingAdd(result, weight);
    }
  }
  return result;
}

// 删除块内所有终结指令，为按新相邻关系重建终结指令做准备。
void eraseTerminators(MachineBasicBlock &block) {
  block.instrs.erase(std::remove_if(block.instrs.begin(), block.instrs.end(),
                                    [](const MachineInstr &instr) {
                                      return instr.terminator ||
                                             isKnownTerminator(instr.opcode);
                                    }),
                     block.instrs.end());
}

// 按最终的块顺序重建每个块的终结指令，使控制流与新相邻关系一致：
// - RET 原样放回；
// - 无条件跳转：目标恰为相邻块则省略跳转（fallthrough），否则补一条 J；
// - 条件分支：若 false 目标相邻则保持原方向；若 true 目标相邻则反转分支指向
//   false 目标；两者都不相邻时，按权重决定是否反转，再补一条到另一目标的 J。
void rebuildTerminators(
    MachineFunction &mf, const std::map<std::string, SavedControl> &controls,
    const MachineBlockFrequencyAnalysis::Result &frequencies,
    bool useWeightedOrientation) {
  for (std::size_t index = 0; index < mf.blocks.size(); ++index) {
    MachineBasicBlock &block = mf.blocks[index];
    const auto savedIt = controls.find(block.label);
    if (savedIt == controls.end()) {
      throw BackendError("lost machine block control-flow record");
    }
    const SavedControl &saved = savedIt->second;
    const std::string next = index + 1 < mf.blocks.size()
                                 ? mf.blocks[index + 1].label
                                 : std::string();
    eraseTerminators(block);

    if (saved.kind == ControlKind::Return) {
      block.instrs.push_back(saved.terminator);
      continue;
    }
    if (saved.kind == ControlKind::Unconditional) {
      if (saved.trueTarget != next) {
        appendJump(block, saved.trueTarget);
      }
      continue;
    }

    MachineInstr conditional = saved.terminator;
    if (saved.falseTarget == next) {
      setBranchTarget(conditional, saved.trueTarget);
      block.instrs.push_back(std::move(conditional));
      continue;
    }
    if (saved.trueTarget == next) {
      Opcode inverted = Opcode::NOP;
      if (!invertedBranch(conditional.opcode, inverted)) {
        throw BackendError("cannot invert unknown machine condition opcode");
      }
      conditional.opcode = inverted;
      setBranchTarget(conditional, saved.falseTarget);
      block.instrs.push_back(std::move(conditional));
      continue;
    }

    if (useWeightedOrientation) {
      const std::size_t from = saved.originalIndex;
      const std::size_t trueTarget = controls.at(saved.trueTarget).originalIndex;
      const std::size_t falseTarget =
          controls.at(saved.falseTarget).originalIndex;
      const uint64_t trueWeight =
          edgeWeightForTarget(frequencies, from, trueTarget);
      const uint64_t falseWeight =
          edgeWeightForTarget(frequencies, from, falseTarget);
      if (falseWeight > trueWeight) {
        Opcode inverted = Opcode::NOP;
        if (!invertedBranch(conditional.opcode, inverted)) {
          throw BackendError("cannot invert unknown machine condition opcode");
        }
        conditional.opcode = inverted;
        setBranchTarget(conditional, saved.falseTarget);
        block.instrs.push_back(std::move(conditional));
        appendJump(block, saved.trueTarget);
        continue;
      }
    }

    setBranchTarget(conditional, saved.trueTarget);
    block.instrs.push_back(std::move(conditional));
    appendJump(block, saved.falseTarget);
  }
}

// 由块的 succs 收集“标签到标签”的逻辑边集合，用于比较布局前后 CFG 是否一致。
LabelEdgeSet cfgLabelEdges(const MachineFunction &mf) {
  LabelEdgeSet result;
  for (const MachineBasicBlock &block : mf.blocks) {
    for (int succ : block.succs) {
      if (succ < 0 || succ >= static_cast<int>(mf.blocks.size())) {
        throw BackendError("invalid CFG successor after block placement");
      }
      result.emplace(block.label,
                     mf.blocks[static_cast<std::size_t>(succ)].label);
    }
  }
  return result;
}

// 布局后的安全校验：块集合不增不减不重复、RET 不能凭空多出 successor、逻辑
// CFG 边与布局前完全一致，并重新固化控制流对比每个终结指令的语义（方向要么
// 保持、要么恰好是合法反转）。任何不一致都抛异常，防止布局引入错误。
void verifyPlacedFunction(const MachineFunction &mf,
                          const std::map<std::string, SavedControl> &controls,
                          const LabelEdgeSet &oldEdges) {
  if (mf.blocks.empty()) {
    return;
  }
  std::set<std::string> labels;
  for (const MachineBasicBlock &block : mf.blocks) {
    if (!labels.insert(block.label).second) {
      throw BackendError("machine block duplicated by block placement");
    }
    const auto saved = controls.find(block.label);
    if (saved == controls.end()) {
      throw BackendError("machine block added by block placement");
    }
    if (saved->second.kind == ControlKind::Return && !block.succs.empty()) {
      throw BackendError("RET acquired a successor after block placement");
    }
  }
  if (labels.size() != controls.size()) {
    throw BackendError("machine block lost by block placement");
  }
  if (cfgLabelEdges(mf) != oldEdges) {
    throw BackendError("logical CFG edges changed during block placement");
  }

  std::map<std::string, SavedControl> rebuiltControls;
  LabelEdgeSet rebuiltEdges;
  if (!saveControlFlow(mf, rebuiltControls, rebuiltEdges)) {
    throw BackendError("cannot recover control flow after block placement");
  }
  for (const auto &[label, oldControl] : controls) {
    const auto rebuilt = rebuiltControls.find(label);
    if (rebuilt == rebuiltControls.end() ||
        rebuilt->second.kind != oldControl.kind) {
      throw BackendError("terminator kind changed during block placement");
    }
    const SavedControl &newControl = rebuilt->second;
    if (oldControl.kind == ControlKind::Return) {
      if (!sameInstruction(oldControl.terminator, newControl.terminator)) {
        throw BackendError("RET operands changed during block placement");
      }
      continue;
    }
    if (oldControl.kind == ControlKind::Unconditional) {
      if (oldControl.trueTarget != newControl.trueTarget) {
        throw BackendError(
            "unconditional target changed during block placement");
      }
      continue;
    }

    if (!sameInstruction(oldControl.terminator, newControl.terminator, true)) {
      throw BackendError("conditional operands changed during block placement");
    }
    const bool sameDirection =
        oldControl.terminator.opcode == newControl.terminator.opcode &&
        oldControl.trueTarget == newControl.trueTarget &&
        oldControl.falseTarget == newControl.falseTarget;
    Opcode inverse = Opcode::NOP;
    const bool reversedDirection =
        invertedBranch(oldControl.terminator.opcode, inverse) &&
        inverse == newControl.terminator.opcode &&
        oldControl.trueTarget == newControl.falseTarget &&
        oldControl.falseTarget == newControl.trueTarget;
    if (!sameDirection && !reversedDirection) {
      throw BackendError(
          "conditional true/false semantics changed during block placement");
    }
  }
}

} // namespace

// 布局 pass 的入口：对单个机器函数执行块重排。
// 步骤：固化控制流 -> 若频率可靠则用 Pettis-Hansen 构造候选顺序并尝试循环旋转
// -> 与原始顺序比较代价，择优选定顺序 -> 物理重排块并重建终结指令与 CFG ->
// 校验 -> 若实际未变则返回 false。返回是否修改了函数。
bool runBlockPlacement(
    MachineFunction &mf, const MachineLoopAnalysis::Result &loops,
    const MachineBlockFrequencyAnalysis::Result &frequencies) {
  std::size_t placementRotations = 0;
  if (mf.blocks.empty()) {
    return false;
  }
  if (loops.depth.size() != mf.blocks.size()) {
    throw BackendError("stale machine loop analysis");
  }
  std::map<std::string, SavedControl> controls;
  LabelEdgeSet oldEdges;
  if (!saveControlFlow(mf, controls, oldEdges)) {
    return false;
  }

  const std::string entryLabel = mf.blocks.front().label;
  std::vector<int> originalOrder(mf.blocks.size());
  for (std::size_t index = 0; index < originalOrder.size(); ++index) {
    originalOrder[index] = static_cast<int>(index);
  }
  std::vector<int> candidateOrder = originalOrder;
  const bool useWeightedOrientation = frequencies.reliable;
  if (useWeightedOrientation) {
    candidateOrder = buildPettisHansenOrder(mf.blocks.size(), loops, frequencies);
  }

  const LayoutCost originalCost =
      computeLayoutCost(mf, originalOrder, controls, frequencies,
                        useWeightedOrientation);
  LayoutCost candidateCost = useWeightedOrientation
                                 ? computeLayoutCost(mf, candidateOrder,
                                                     controls, frequencies,
                                                     useWeightedOrientation)
                                 : originalCost;
  if (useWeightedOrientation) {
    const std::vector<int> pettisHansenOrder = candidateOrder;
    const LayoutCost pettisHansenCost = candidateCost;
    std::size_t rotations = 0;
    considerBackedgeFallthroughOrders(
        mf, originalOrder, originalCost, loops, frequencies, controls,
        useWeightedOrientation, candidateOrder, candidateCost, rotations);
    considerBackedgeFallthroughOrders(
        mf, pettisHansenOrder, pettisHansenCost, loops, frequencies, controls,
        useWeightedOrientation, candidateOrder, candidateCost, rotations);
    placementRotations = rotations;
  }

  const std::vector<int> *selectedOrder = &originalOrder;
  const LayoutCost *selectedCost = &originalCost;
  if (useWeightedOrientation && candidateCost < originalCost) {
    selectedOrder = &candidateOrder;
    selectedCost = &candidateCost;
  }

  if (statisticsEnabled()) {
    std::cerr << "# block-placement-stats blocks=" << mf.blocks.size()
              << " reliable=" << (frequencies.reliable ? 1 : 0)
              << " converged=" << (frequencies.converged ? 1 : 0)
              << " edge-weight=" << totalEdgeWeight(frequencies)
              << " weighted-fallthrough-before="
              << weightedFallthrough(originalOrder, frequencies)
              << " weighted-fallthrough-candidate="
              << weightedFallthrough(candidateOrder, frequencies)
              << " weighted-fallthrough-after="
              << weightedFallthrough(*selectedOrder, frequencies)
              << " dynamic-j-before=" << originalCost.dynamicJumpExecutions
              << " dynamic-j-candidate="
              << candidateCost.dynamicJumpExecutions
              << " dynamic-j-after=" << selectedCost->dynamicJumpExecutions
              << " static-j-before=" << originalCost.staticJumpCount
              << " static-j-candidate=" << candidateCost.staticJumpCount
              << " static-j-after=" << selectedCost->staticJumpCount
              << " broken-fallthrough-before="
              << originalCost.brokenFallthroughWeight
              << " broken-fallthrough-candidate="
              << candidateCost.brokenFallthroughWeight
              << " broken-fallthrough-after="
              << selectedCost->brokenFallthroughWeight
              << " expected-j-executions-before="
              << originalCost.dynamicJumpExecutions
              << " expected-j-executions-candidate="
              << candidateCost.dynamicJumpExecutions
              << " expected-j-executions-after="
              << selectedCost->dynamicJumpExecutions
              << " broken-unknown-fallthrough-before="
              << originalCost.brokenUnknownFallthroughWeight
              << " broken-unknown-fallthrough-candidate="
              << candidateCost.brokenUnknownFallthroughWeight
              << " broken-unknown-fallthrough-after="
              << selectedCost->brokenUnknownFallthroughWeight
              << " taken-conditional-before="
              << originalCost.takenConditionalWeight
              << " taken-conditional-candidate="
              << candidateCost.takenConditionalWeight
              << " taken-conditional-after="
              << selectedCost->takenConditionalWeight
              << " rotations=" << placementRotations
              << '\n';
  }

  // 按给定顺序构造布局后的函数体(重建终结指令与 CFG),供选中顺序与
  // 位移回退共用。
  auto buildFromOrder = [&](const std::vector<int> &order) {
    MachineFunction built;
    built.blocks = mf.blocks;
    std::vector<MachineBasicBlock> placed;
    placed.reserve(built.blocks.size());
    std::vector<bool> moved(built.blocks.size(), false);
    for (int oldIndex : order) {
      if (oldIndex < 0 ||
          oldIndex >= static_cast<int>(built.blocks.size()) ||
          moved[static_cast<std::size_t>(oldIndex)]) {
        throw BackendError("invalid machine block placement order");
      }
      moved[static_cast<std::size_t>(oldIndex)] = true;
      placed.push_back(
          std::move(built.blocks[static_cast<std::size_t>(oldIndex)]));
    }
    built.blocks = std::move(placed);

    // 从这里起不得再使用旧 block index、旧块引用或分析中的下标缓存。
    if (built.blocks.size() != controls.size() ||
        built.blocks.front().label != entryLabel) {
      throw BackendError("invalid result from machine block placement");
    }
    rebuildTerminators(built, controls, frequencies, useWeightedOrientation);
    rebuildCFG(built);
    return built;
  };

  MachineFunction candidate = buildFromOrder(*selectedOrder);
  if (verificationEnabled()) {
    verifyPlacedFunction(candidate, controls, oldEdges);
  }
  // P1#10:布局后分支位移检查——旋转/重排可能拉长条件分支到目标块的距离;
  // 任一条件分支超出 ±4KB(保守按每条指令 4 字节估计)时回退到原始顺序,
  // 不做 branch island(当前函数即放弃重排,语义与汇编均安全)。
  if (!branchesInRange(candidate) && selectedOrder != &originalOrder) {
    candidate = buildFromOrder(originalOrder);
    if (verificationEnabled()) {
      verifyPlacedFunction(candidate, controls, oldEdges);
    }
  }
  if (sameProgram(mf.blocks, candidate.blocks)) {
    return false;
  }
  mf.blocks = std::move(candidate.blocks);
  return true;
}

} // namespace aarch64
