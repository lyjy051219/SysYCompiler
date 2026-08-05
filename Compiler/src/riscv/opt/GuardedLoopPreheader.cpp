#include "riscv/opt/MachineOptPasses.hpp"

#include "riscv/Passes.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace riscv {
// Guarded Loop Preheader pass 的公共实现。
//
// 作用：针对可能零次执行（zero-trip）的循环，避免在“循环体一次都不进”时白白
// 执行预头里那些较昂贵但可推测的计算。做法是把预头拆成两部分：计算循环 guard
// 条件所需的指令切片留在原预头，其余昂贵计算移入一个新插入的 compute 块；然后
// 在原预头放置一个 guard 分支，当 guard 判定循环不会进入时直接跳到 exit，跳过 compute
// 块。这样零次迭代的路径不再付出昂贵计算的代价，正常进入循环则照常执行。
//
// 流水线位置：寄存器分配前（Pre-RA）的机器函数级 pass，依赖机器 CFG、活跃性、
// 循环分析。要求预头里被搬移的指令都是无副作用、可推测执行的纯虚拟寄存器运算。
//
// RISC-V 与 ARM64 两后端使用相同实现
// 两后端各保留一份相同实现。

namespace {

// 只有当可搬移计算的总开销达到此阈值时才值得插入 guard，避免为省一点点计算反而
// 增加一个分支与一个块的额外成本。
constexpr unsigned kMinGuardedCost = 8;

// 一个可施加 guard 变换的候选：预头/header 块下标、可省下的计算开销、预头指令中
// 哪些属于 guard 切片（保留在预头）的位图，以及构造好的 guard 分支指令。
struct Candidate {
  std::size_t preheader = 0;
  std::size_t header = 0;
  unsigned cost = 0;
  std::vector<bool> guardSlice;
  MachineInstr guard;
};

// 是否是条件分支操作码。
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

// 求条件分支的反转操作码（guard 需要把 header 的“进入循环”条件反转为“跳过循环”）。
std::optional<Opcode> invertedBranch(Opcode opcode) {
  switch (opcode) {
  case Opcode::BEQZ:
    return Opcode::BNEZ;
  case Opcode::BNEZ:
    return Opcode::BEQZ;
  case Opcode::BEQ:
    return Opcode::BNE;
  case Opcode::BNE:
    return Opcode::BEQ;
  case Opcode::BLT:
    return Opcode::BGE;
  case Opcode::BGE:
    return Opcode::BLT;
  default:
    return std::nullopt;
  }
}

// 操作数是否是虚拟寄存器（非物理寄存器）。
bool isVirtualRegister(const MachineOperand &operand) {
  return operand.kind == OperandKind::Reg && !operand.regValue.physical;
}

// 取指令唯一的块目标标签到 target；有多个或没有块目标返回 false。
bool hasSingleBlockTarget(const MachineInstr &instr, std::string &target) {
  target.clear();
  for (const MachineOperand &use : instr.uses) {
    if (use.kind != OperandKind::Block) {
      continue;
    }
    if (!target.empty()) {
      return false;
    }
    target = use.blockLabel;
  }
  return !target.empty();
}

// 把指令唯一的块目标就地改写为 target；有多个或没有块目标返回 false。
bool replaceSingleBlockTarget(MachineInstr &instr, const std::string &target) {
  MachineOperand *blockUse = nullptr;
  for (MachineOperand &use : instr.uses) {
    if (use.kind != OperandKind::Block) {
      continue;
    }
    if (blockUse != nullptr) {
      return false;
    }
    blockUse = &use;
  }
  if (blockUse == nullptr) {
    return false;
  }
  blockUse->blockLabel = target;
  return true;
}

// 判断一条条件分支是否形态规范：是条件分支且为终结指令、无副作用、无 def、无
// 隐式操作数，且寄存器操作数个数（BEQZ/BNEZ 为 1，其余为 2）与恰一个块目标匹配。
// 只有规范分支才能安全地被复制、反转并改目标以充当 guard。
bool wellFormedConditionalBranch(const MachineInstr &instr) {
  if (!isConditionalBranch(instr.opcode) || !instr.terminator ||
      instr.sideEffects || !instr.defs.empty() || !instr.implicitDefs.empty() ||
      !instr.implicitUses.empty()) {
    return false;
  }

  const std::size_t expectedRegisters =
      instr.opcode == Opcode::BEQZ || instr.opcode == Opcode::BNEZ ? 1 : 2;
  std::size_t registers = 0;
  std::size_t blocks = 0;
  for (const MachineOperand &use : instr.uses) {
    if (use.kind == OperandKind::Reg) {
      ++registers;
    } else if (use.kind == OperandKind::Block) {
      ++blocks;
    } else {
      return false;
    }
  }
  return registers == expectedRegisters && blocks == 1;
}

// 判断一条指令是否是形态规范的无条件跳转（J，唯一块目标写入 target）。
bool wellFormedJump(const MachineInstr &instr, std::string &target) {
  return instr.opcode == Opcode::J && instr.terminator && !instr.sideEffects &&
         instr.defs.empty() && instr.implicitDefs.empty() &&
         instr.implicitUses.empty() && instr.uses.size() == 1 &&
         hasSingleBlockTarget(instr, target);
}

// 判断预头里一条指令是否可被安全地推测执行并搬到 guard 之后（compute 块）。要求
// 无副作用、非终结、所有 def 都是虚拟寄存器、use 只含寄存器或立即数，且操作码
// 属于白名单里的纯算术/逻辑/移位/比较/浮点等无陷阱指令。只有这样，把它挪到
// guard 分支之后（可能不执行）才不会改变程序语义。
bool isSpeculatablePreheaderInstruction(const MachineInstr &instr) {
  if (instr.sideEffects || instr.terminator || instr.defs.empty() ||
      !instr.implicitDefs.empty() || !instr.implicitUses.empty()) {
    return false;
  }
  for (const MachineOperand &def : instr.defs) {
    if (!isVirtualRegister(def)) {
      return false;
    }
  }
  for (const MachineOperand &use : instr.uses) {
    if (use.kind != OperandKind::Reg && use.kind != OperandKind::Imm) {
      return false;
    }
  }

  switch (instr.opcode) {
  case Opcode::COPY:
  case Opcode::F_COPY:
  case Opcode::FMV_W_X:
  case Opcode::FMV_X_W:
  case Opcode::LI:
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
    return true;
  default:
    return false;
  }
}

// 各操作码的近似执行开销权重，用于估算可搬移计算能省下多少，决定是否值得 guard。
unsigned instructionCost(const MachineInstr &instr) {
  switch (instr.opcode) {
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

// 建立“块标签 -> 块下标”的映射，便于按分支目标标签定位块。
std::map<std::string, std::size_t> blockIndices(const MachineFunction &mf) {
  std::map<std::string, std::size_t> result;
  for (std::size_t index = 0; index < mf.blocks.size(); ++index) {
    result.emplace(mf.blocks[index].label, index);
  }
  return result;
}

// 统计每个虚拟寄存器在整个函数中被定义的次数（phi 消除后可能非 SSA、有多次
// 定义），用于识别哪些寄存器是唯一定义、可安全搬移。
std::map<uint32_t, std::size_t>
virtualDefinitionCounts(const MachineFunction &mf) {
  std::map<uint32_t, std::size_t> result;
  for (const MachineBasicBlock &block : mf.blocks) {
    for (const MachineInstr &instr : block.instrs) {
      for (const MachineOperand &def : instr.defs) {
        if (isVirtualRegister(def)) {
          ++result[def.regValue.id];
        }
      }
    }
  }
  return result;
}

// 计算预头 [0, computeEnd) 中必须保留在 guard 之前（guard 切片）的指令位图。 guard 切片
// 是计算 guard 条件所必需的那部分。做法：先把任何“多次定义”的寄存器所在指令强制
// 保留（phi 消除留下的非 SSA 拷贝与 use-before-def 无法可靠搬移），再从 guard 条件
// 的 use 出发，不依赖指令顺序地做定义-使用闭包，凡定义了被需要值的指令都并入切片。
std::vector<bool> retainedDependencySlice(
    const MachineBasicBlock &preheader, std::size_t computeEnd,
    const MachineInstr &condition,
    const std::map<uint32_t, std::size_t> &definitionCounts) {
  std::set<uint32_t> needed;
  for (const MachineOperand &use : condition.uses) {
    if (isVirtualRegister(use)) {
      needed.insert(use.regValue.id);
    }
  }

  std::vector<bool> slice(computeEnd, false);
  // Phi elimination can leave non-SSA copies and use-before-def order in P.
  // Retain every multi-defined seed, then close over all P-local definitions
  // of retained uses without relying on instruction order.
  for (std::size_t index = 0; index < computeEnd; ++index) {
    const MachineInstr &instr = preheader.instrs[index];
    const bool hasNonUniqueDefinition = std::any_of(
        instr.defs.begin(), instr.defs.end(), [&](const MachineOperand &def) {
          if (!isVirtualRegister(def)) {
            return false;
          }
          auto count = definitionCounts.find(def.regValue.id);
          return count == definitionCounts.end() || count->second != 1;
        });
    slice[index] = hasNonUniqueDefinition;
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t index = 0; index < computeEnd; ++index) {
      if (!slice[index]) {
        continue;
      }
      for (const MachineOperand &use : preheader.instrs[index].uses) {
        if (isVirtualRegister(use) && needed.insert(use.regValue.id).second) {
          changed = true;
        }
      }
    }

    for (std::size_t index = 0; index < computeEnd; ++index) {
      if (slice[index]) {
        continue;
      }
      const MachineInstr &instr = preheader.instrs[index];
      const bool definesNeeded = std::any_of(
          instr.defs.begin(), instr.defs.end(), [&](const MachineOperand &def) {
            return isVirtualRegister(def) && needed.count(def.regValue.id) != 0;
          });
      if (definesNeeded) {
        slice[index] = true;
        changed = true;
      }
    }
  }
  return slice;
}

// 校验 guard 切片与 compute 切片可以按“guard 切片整体前移、compute 切片整体后移”
// 的方式重排而不破坏依赖：若某条 compute 指令使用了某个 guard 切片寄存器，而该寄存器
// 在 guard 切片中的最后一次定义位置排在这条 compute 指令之后，则重排会产生
// use-before-def，返回 false 拒绝。
bool partitionCanReorder(const MachineBasicBlock &preheader,
                         const std::vector<bool> &guardSlice) {
  std::map<uint32_t, std::size_t> lastGuardDefinition;
  for (std::size_t index = 0; index < guardSlice.size(); ++index) {
    if (!guardSlice[index]) {
      continue;
    }
    for (const MachineOperand &def : preheader.instrs[index].defs) {
      if (isVirtualRegister(def)) {
        lastGuardDefinition[def.regValue.id] = index;
      }
    }
  }

  for (std::size_t index = 0; index < guardSlice.size(); ++index) {
    if (guardSlice[index]) {
      continue;
    }
    for (const MachineOperand &use : preheader.instrs[index].uses) {
      if (!isVirtualRegister(use)) {
        continue;
      }
      auto definition = lastGuardDefinition.find(use.regValue.id);
      if (definition != lastGuardDefinition.end() &&
          definition->second > index) {
        return false;
      }
    }
  }
  return true;
}

// 由 header 与 latch 反向沿前驱收集自然循环的成员块（不越过 header）。
std::set<int> naturalLoopMembers(int header, int latch,
                                 const MachineCFGAnalysis::Result &cfg) {
  std::set<int> members{header, latch};
  std::vector<int> work;
  if (latch != header) {
    work.push_back(latch);
  }
  while (!work.empty()) {
    const int block = work.back();
    work.pop_back();
    for (int pred : cfg.preds[static_cast<std::size_t>(block)]) {
      if (pred != header && members.insert(pred).second) {
        work.push_back(pred);
      }
    }
  }
  return members;
}

// 块是否以一条到 target 的无条件跳转结尾，且此前没有其他终结指令。
bool blockEndsWithJumpTo(const MachineBasicBlock &block,
                         const std::string &target) {
  if (block.instrs.empty()) {
    return false;
  }
  std::string actual;
  if (!wellFormedJump(block.instrs.back(), actual) || actual != target) {
    return false;
  }
  return std::none_of(
      block.instrs.begin(), block.instrs.end() - 1,
      [](const MachineInstr &instr) { return instr.terminator; });
}

// exit 块是否含有 PHI。含 PHI 说明它有多个前驱、边特定的取值，插入 guard 会新增
// 一条从预头直达 exit 的边，破坏其 PHI 语义，因此这类候选要排除。
bool exitHasPhi(const MachineBasicBlock &block) {
  return std::any_of(
      block.instrs.begin(), block.instrs.end(),
      [](const MachineInstr &instr) { return instr.opcode == Opcode::PHI; });
}

// 针对一个 header 与其唯一 latch，检验并构造 guard 候选。要求严格的规范形态：
// 循环恰好由 header 与 latch 两块组成；header 只有“条件分支 + 无条件跳转”两条
// 指令，一个目标进入循环体、另一个是循环外的 exit；body(=latch) 单出口回到
// header；header 只有唯一的循环外前驱且它正是紧邻其前的预头，预头单出口进 header，
// exit 无 PHI。随后要求预头（去掉末尾跳转后）每条指令都可推测执行，切分出 guard
// 切片与 compute 切片并验证可重排；估算 compute 开销需达到阈值；compute 定义的
// 值必须唯一定义、且不在 exit 活跃、不被 guard 条件或循环外任何块使用（否则搬到可能
// 不执行的 compute 块会出错）。最后由 header 分支复制出 guard：若原条件是“进入循环
// 才走条件目标”，则反转，使 guard 在“循环不进入”时跳到 exit。
std::optional<Candidate>
buildCandidate(MachineFunction &mf, std::size_t headerIndex, int latchIndex,
               const MachineCFGAnalysis::Result &cfg,
               const MachineLivenessAnalysis::Result &liveness,
               const MachineLoopAnalysis::Result &loops,
               const std::map<std::string, std::size_t> &indices,
               const std::map<uint32_t, std::size_t> &definitionCounts) {
  const std::set<int> members =
      naturalLoopMembers(static_cast<int>(headerIndex), latchIndex, cfg);
  if (members.size() != 2 || members.count(latchIndex) == 0) {
    return std::nullopt;
  }

  const MachineBasicBlock &header = mf.blocks[headerIndex];
  if (header.instrs.size() != 2 ||
      !wellFormedConditionalBranch(header.instrs[0])) {
    return std::nullopt;
  }

  std::string conditionalLabel;
  std::string jumpLabel;
  if (!hasSingleBlockTarget(header.instrs[0], conditionalLabel) ||
      !wellFormedJump(header.instrs[1], jumpLabel)) {
    return std::nullopt;
  }
  auto conditionalFound = indices.find(conditionalLabel);
  auto jumpFound = indices.find(jumpLabel);
  if (conditionalFound == indices.end() || jumpFound == indices.end()) {
    return std::nullopt;
  }
  const std::size_t conditionalTarget = conditionalFound->second;
  const std::size_t jumpTarget = jumpFound->second;

  bool conditionalEnters = false;
  std::size_t bodyIndex = 0;
  std::size_t exitIndex = 0;
  // 判定 header 两个出边哪个进循环体（=latch）、哪个是循环外 exit。conditionalEnters
  // 记录“进入循环”的是条件分支目标还是 fallthrough 跳转目标，决定 guard 是否需反转。
  if (conditionalTarget == static_cast<std::size_t>(latchIndex) &&
      members.count(static_cast<int>(jumpTarget)) == 0) {
    conditionalEnters = true;
    bodyIndex = conditionalTarget;
    exitIndex = jumpTarget;
  } else if (jumpTarget == static_cast<std::size_t>(latchIndex) &&
             members.count(static_cast<int>(conditionalTarget)) == 0) {
    bodyIndex = jumpTarget;
    exitIndex = conditionalTarget;
  } else {
    return std::nullopt;
  }

  if (cfg.succs[bodyIndex].size() != 1 ||
      cfg.succs[bodyIndex].front() != static_cast<int>(headerIndex) ||
      !blockEndsWithJumpTo(mf.blocks[bodyIndex], header.label)) {
    return std::nullopt;
  }

  std::vector<int> outsidePreds;
  for (int pred : cfg.preds[headerIndex]) {
    if (members.count(pred) == 0) {
      outsidePreds.push_back(pred);
    }
  }
  if (outsidePreds.size() != 1 || outsidePreds.front() < 0) {
    return std::nullopt;
  }
  const std::size_t preheaderIndex =
      static_cast<std::size_t>(outsidePreds.front());
  if (preheaderIndex + 1 != headerIndex ||
      preheaderIndex >= loops.depth.size() ||
      headerIndex >= loops.depth.size() || loops.depth[preheaderIndex] <= 0 ||
      loops.depth[headerIndex] <= loops.depth[preheaderIndex] ||
      cfg.succs[preheaderIndex].size() != 1 ||
      cfg.succs[preheaderIndex].front() != static_cast<int>(headerIndex) ||
      exitIndex >= mf.blocks.size() || exitHasPhi(mf.blocks[exitIndex])) {
    return std::nullopt;
  }

  const MachineBasicBlock &preheader = mf.blocks[preheaderIndex];
  std::size_t computeEnd = preheader.instrs.size();
  if (computeEnd != 0 && preheader.instrs.back().opcode == Opcode::J) {
    std::string target;
    if (!wellFormedJump(preheader.instrs.back(), target) ||
        target != header.label) {
      return std::nullopt;
    }
    --computeEnd;
  }
  if (computeEnd == 0) {
    return std::nullopt;
  }

  for (std::size_t index = 0; index < computeEnd; ++index) {
    if (!isSpeculatablePreheaderInstruction(preheader.instrs[index])) {
      return std::nullopt;
    }
  }

  const std::vector<bool> guardSlice = retainedDependencySlice(
      preheader, computeEnd, header.instrs[0], definitionCounts);
  if (!partitionCanReorder(preheader, guardSlice)) {
    return std::nullopt;
  }

  std::set<uint32_t> computeDefs;
  unsigned cost = 0;
  for (std::size_t index = 0; index < computeEnd; ++index) {
    if (guardSlice[index]) {
      continue;
    }
    const MachineInstr &instr = preheader.instrs[index];
    cost += instructionCost(instr);
    for (const MachineOperand &def : instr.defs) {
      computeDefs.insert(def.regValue.id);
    }
  }
  if (cost < kMinGuardedCost) {
    return std::nullopt;
  }

  for (uint32_t def : computeDefs) {
    auto count = definitionCounts.find(def);
    if (count == definitionCounts.end() || count->second != 1 ||
        liveness.liveIn[exitIndex].count(def) != 0) {
      return std::nullopt;
    }
  }
  for (const MachineOperand &use : header.instrs[0].uses) {
    if (isVirtualRegister(use) && computeDefs.count(use.regValue.id) != 0) {
      return std::nullopt;
    }
  }

  for (std::size_t blockIndex = 0; blockIndex < mf.blocks.size();
       ++blockIndex) {
    if (blockIndex == preheaderIndex ||
        members.count(static_cast<int>(blockIndex)) != 0) {
      continue;
    }
    for (const MachineInstr &instr : mf.blocks[blockIndex].instrs) {
      for (const MachineOperand &use : instr.uses) {
        if (isVirtualRegister(use) && computeDefs.count(use.regValue.id) != 0) {
          return std::nullopt;
        }
      }
    }
  }

  MachineInstr guard = header.instrs[0];
  if (conditionalEnters) {
    auto inverse = invertedBranch(guard.opcode);
    if (!inverse) {
      return std::nullopt;
    }
    guard.opcode = *inverse;
  }
  if (!replaceSingleBlockTarget(guard, mf.blocks[exitIndex].label)) {
    return std::nullopt;
  }
  guard.comment = "skip zero-trip loop preheader";

  Candidate candidate;
  candidate.preheader = preheaderIndex;
  candidate.header = headerIndex;
  candidate.cost = cost;
  candidate.guardSlice = guardSlice;
  candidate.guard = std::move(guard);
  return candidate;
}

// 为新插入的 compute 块生成一个在函数内唯一的标签（预头标签加后缀，冲突则递增）。
std::string uniqueComputeLabel(const MachineFunction &mf,
                               const std::string &preheaderLabel) {
  const std::string base = preheaderLabel + ".guarded_compute";
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

// 落地一个候选：把预头指令按切片位图拆分——guard 切片留在预头、其余移入新建的
// compute 块——预头末尾放置 guard 分支，compute 块插到 header 之前（guard 不跳过时
// 顺序 fallthrough 进入），最后重建 CFG。
void applyCandidate(MachineFunction &mf, Candidate candidate) {
  MachineBasicBlock &preheader = mf.blocks[candidate.preheader];
  MachineBasicBlock compute;
  compute.label = uniqueComputeLabel(mf, preheader.label);
  compute.irBlock = preheader.irBlock;
  std::vector<MachineInstr> original = std::move(preheader.instrs);
  preheader.instrs.clear();
  for (std::size_t index = 0; index < candidate.guardSlice.size(); ++index) {
    if (candidate.guardSlice[index]) {
      preheader.instrs.push_back(std::move(original[index]));
    } else {
      compute.instrs.push_back(std::move(original[index]));
    }
  }
  preheader.instrs.push_back(std::move(candidate.guard));
  using BlockDifference = std::vector<MachineBasicBlock>::difference_type;
  mf.blocks.insert(mf.blocks.begin() +
                       static_cast<BlockDifference>(candidate.header),
                   std::move(compute));
  rebuildCFG(mf);
}

} // namespace

// pass 入口：扫描所有 header（只处理恰有唯一 latch 的回边），构造 guard 候选，取
// 开销最大的一个应用。每次调用只变换一个候选，因为 CFG 改动会使上面取用的缓存
// 分析失效；若要求不动点，交由调用方带着新分析重跑。返回是否修改了函数。
bool runPreRAGuardedLoopPreheader(MachineFunction &mf,
                                  MachineFunctionAnalysisManager &am) {
  if (mf.blocks.size() < 5) {
    return false;
  }

  const auto &cfg = am.getResult<MachineCFGAnalysis>(mf);
  const auto &liveness = am.getResult<MachineLivenessAnalysis>(mf);
  const auto &loops = am.getResult<MachineLoopAnalysis>(mf);
  const auto indices = blockIndices(mf);
  const auto definitionCounts = virtualDefinitionCounts(mf);

  std::optional<Candidate> best;
  for (std::size_t header = 0; header < mf.blocks.size(); ++header) {
    std::vector<int> latches;
    for (const auto &[latch, target] : loops.backedges) {
      if (target == static_cast<int>(header)) {
        latches.push_back(latch);
      }
    }
    if (latches.size() != 1) {
      continue;
    }

    auto candidate = buildCandidate(mf, header, latches.front(), cfg, liveness,
                                    loops, indices, definitionCounts);
    if (!candidate) {
      continue;
    }
    if (!best || candidate->cost > best->cost) {
      best = std::move(candidate);
    }
  }

  if (!best) {
    return false;
  }
  // CFG mutation invalidates the cached analyses used above. Transform only
  // the highest-cost candidate per invocation and let the caller rerun with
  // fresh analyses if it wants a fixed point.
  applyCandidate(mf, std::move(*best));
  return true;
}

} // namespace riscv
