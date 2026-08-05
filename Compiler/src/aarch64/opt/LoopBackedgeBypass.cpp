#include "aarch64/opt/MachineOptPasses.hpp"

#include <algorithm>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace aarch64 {
// Loop Backedge Bypass pass 的公共实现。
//
// 作用：消除“每次迭代都先无条件跳回 header、再由 header 的分支判断是否继续”的
// 冗余控制流。对于 header 仅包含一个条件分支（或一个可重放的整数 load 加一个对
// 其结果的零比较分支）的循环，本 pass 在 latch（循环体尾）末尾直接插入“继续循环”
// 的条件分支跳回循环体、否则跳到 exit，把回边分支下沉到 latch，绕过对 header 的
// 再次跳转。若 header 的判定依赖某个 load，则把该 load 一并重放到 latch 末尾。
//
// 流水线位置：寄存器分配之后（Post-RA），因此只处理物理寄存器、需要保持已定的
// 寄存器约定。变换只追加/改写 latch 末尾指令，header 保留，保证语义等价。
//
// RISC-V 与 ARM64 两后端使用相同实现
// 两后端各保留一份相同实现。

namespace {

// 是否是条件分支操作码。
bool isConditionalBranch(Opcode OpcodeValue) {
  return OpcodeValue == Opcode::BEQZ || OpcodeValue == Opcode::BNEZ ||
         OpcodeValue == Opcode::BEQ || OpcodeValue == Opcode::BNE ||
         OpcodeValue == Opcode::BLT || OpcodeValue == Opcode::BGE;
}

// 求条件分支的反转操作码。当 header 条件的“真”目标是循环外 exit 时，下沉到
// latch 的“继续循环”分支需要反转，使真目标指向循环体。
std::optional<Opcode> invertedBranch(Opcode OpcodeValue) {
  switch (OpcodeValue) {
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

// 取分支指令的块目标标签（从后往前找第一个块操作数）。
std::optional<std::string> branchTarget(const MachineInstr &Branch) {
  for (auto It = Branch.uses.rbegin(); It != Branch.uses.rend(); ++It) {
    if (It->kind == OperandKind::Block)
      return It->blockLabel;
  }
  return std::nullopt;
}

// 块是否以一条到 Target 的无条件跳转（J）结尾。
bool endsWithJumpTo(const MachineBasicBlock &Block,
                    const std::string &Target) {
  if (Block.instrs.empty())
    return false;
  const MachineInstr &Terminator = Block.instrs.back();
  const auto TargetLabel = branchTarget(Terminator);
  return Terminator.opcode == Opcode::J && TargetLabel &&
         *TargetLabel == Target;
}

// 构造一条到 Target 的无条件跳转指令。
MachineInstr makeJump(const std::string &Target) {
  MachineInstr Jump;
  Jump.opcode = Opcode::J;
  Jump.uses.push_back(MachineOperand::block(Target));
  Jump.terminator = true;
  return Jump;
}

// 一次待落地的改写：在 bodyIndex 块末尾（去掉原跳转后）依次追加可选的重放 load、
// “继续循环”条件分支、以及到 exit 的跳转。
struct Rewrite {
  std::size_t bodyIndex = 0;
  std::optional<MachineInstr> replayLoad;
  MachineInstr continueBranch;
  std::string exitLabel;
};

// 逐字段比较两个物理/虚拟寄存器是否相同。
bool sameRegister(const MachineReg &Left, const MachineReg &Right) {
  return Left.physical == Right.physical && Left.id == Right.id &&
         Left.rc == Right.rc && Left.phys == Right.phys;
}

// 操作数是否是物理通用寄存器（GPR32/GPR64）。
bool isPhysicalGPR(const MachineOperand &Operand) {
  if (Operand.kind != OperandKind::Reg || !Operand.regValue.physical)
    return false;
  return Operand.regValue.rc == RegClass::GPR32 ||
         Operand.regValue.rc == RegClass::GPR64;
}

// 判断一条 load 是否可安全重放到 latch 末尾。要求是形态规范的整数 load（LBU/LW/
// LD，一个物理 GPR 目标、一个物理 GPR 基址、一个立即数偏移，无副作用、无隐式
// 操作数），且目标不是 x0。可重放意味着在 body 末尾再执行一次这条纯 load 不改变
// 语义（内存在循环体内未被本 load 结果左右）。
bool isReplayableIntegerLoad(const MachineInstr &Load) {
  if (Load.opcode != Opcode::LBU && Load.opcode != Opcode::LW &&
      Load.opcode != Opcode::LD)
    return false;
  if (Load.sideEffects || Load.terminator || Load.defs.size() != 1 ||
      Load.uses.size() != 2 || !Load.implicitDefs.empty() ||
      !Load.implicitUses.empty() || !isPhysicalGPR(Load.defs.front()) ||
      !isPhysicalGPR(Load.uses.front()) ||
      Load.uses[1].kind != OperandKind::Imm)
    return false;
  return Load.defs.front().regValue.phys != PhysReg::X0;
}

// 判断分支是否是对 Load 结果做零比较的规范 BEQZ/BNEZ（分支测试的寄存器正是
// Load 的目标寄存器）。这是“load + 零分支”式 header 的识别条件。
bool zeroBranchTestsDefinition(const MachineInstr &Branch,
                               const MachineInstr &Load) {
  if (Branch.opcode != Opcode::BEQZ && Branch.opcode != Opcode::BNEZ)
    return false;
  if (!Branch.terminator || Branch.sideEffects || !Branch.defs.empty() ||
      Branch.uses.size() != 2 || !Branch.implicitDefs.empty() ||
      !Branch.implicitUses.empty() || !isPhysicalGPR(Branch.uses.front()) ||
      Branch.uses[1].kind != OperandKind::Block)
    return false;
  return sameRegister(Branch.uses.front().regValue,
                      Load.defs.front().regValue);
}

using DominatorSets = std::vector<std::set<int>>;

// 用经典的迭代不动点算法计算每个块的支配块集合。先从入口做可达性标记，不可达块
// 支配集留空；可达块初始化为全体可达块，反复用“各前驱支配集的交集加自身”更新，
// 直到收敛。用于确认 latch 确实被 header 支配（是真正的自然循环回边）。
DominatorSets computeDominators(const MachineFunction &MF) {
  const std::size_t Count = MF.blocks.size();
  DominatorSets Dominators(Count);
  if (Count == 0)
    return Dominators;

  std::vector<bool> Reachable(Count, false);
  std::vector<int> Work{0};
  Reachable[0] = true;
  while (!Work.empty()) {
    const int Block = Work.back();
    Work.pop_back();
    for (int Successor : MF.blocks[static_cast<std::size_t>(Block)].succs) {
      if (Successor >= 0 && Successor < static_cast<int>(Count) &&
          !Reachable[static_cast<std::size_t>(Successor)]) {
        Reachable[static_cast<std::size_t>(Successor)] = true;
        Work.push_back(Successor);
      }
    }
  }

  std::set<int> AllReachable;
  for (std::size_t Block = 0; Block < Count; ++Block)
    if (Reachable[Block])
      AllReachable.insert(static_cast<int>(Block));
  Dominators[0].insert(0);
  for (std::size_t Block = 1; Block < Count; ++Block)
    if (Reachable[Block])
      Dominators[Block] = AllReachable;

  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (std::size_t Block = 1; Block < Count; ++Block) {
      if (!Reachable[Block])
        continue;
      std::set<int> Next;
      bool First = true;
      for (int Predecessor : MF.blocks[Block].preds) {
        if (Predecessor < 0 || Predecessor >= static_cast<int>(Count) ||
            !Reachable[static_cast<std::size_t>(Predecessor)])
          continue;
        if (First) {
          Next = Dominators[static_cast<std::size_t>(Predecessor)];
          First = false;
        } else {
          std::set<int> Intersection;
          std::set_intersection(
              Next.begin(), Next.end(),
              Dominators[static_cast<std::size_t>(Predecessor)].begin(),
              Dominators[static_cast<std::size_t>(Predecessor)].end(),
              std::inserter(Intersection, Intersection.begin()));
          Next = std::move(Intersection);
        }
      }
      Next.insert(static_cast<int>(Block));
      if (Next != Dominators[Block]) {
        Dominators[Block] = std::move(Next);
        Changed = true;
      }
    }
  }
  return Dominators;
}

// 由 Header 与 Latch 反向沿前驱收集自然循环成员块（不越过 Header），用于判断
// header 的哪个分支目标在循环体内、哪个在循环外。
std::set<int> naturalLoopMembers(const MachineFunction &MF, int Header,
                                 int Latch) {
  std::set<int> Members{Header, Latch};
  std::vector<int> Work;
  if (Header != Latch)
    Work.push_back(Latch);
  while (!Work.empty()) {
    const int Block = Work.back();
    Work.pop_back();
    for (int Predecessor :
         MF.blocks[static_cast<std::size_t>(Block)].preds) {
      if (Predecessor != Header && Members.insert(Predecessor).second)
        Work.push_back(Predecessor);
    }
  }
  return Members;
}

} // namespace

// pass 入口：扫描每个块看它是否是可旁路的 header（单条条件分支，或“可重放 load
// + 零比较分支”）。确定 header 的真/假目标后，对每个满足条件的 latch（末尾无条件
// 跳回 header、单后继、且被 header 支配）判断循环体与 exit，构造“继续循环”分支
// （真目标指向循环体，必要时反转），连同可选重放 load 记入 Rewrites。最后统一
// 落地：把每个 latch 末尾的跳转替换为 [可选 load] + 继续分支 + 到 exit 的跳转。
// 返回是否有改写发生。
bool runPostRALoopBackedgeBypass(MachineFunction &MF) {
  std::vector<Rewrite> Rewrites;
  const DominatorSets Dominators = computeDominators(MF);
  for (std::size_t HeaderIndex = 0; HeaderIndex + 1 < MF.blocks.size();
       ++HeaderIndex) {
    const MachineBasicBlock &Header = MF.blocks[HeaderIndex];
    const MachineInstr *ReplayLoad = nullptr;
    const MachineInstr *Condition = nullptr;
    if (Header.instrs.size() == 1 &&
        isConditionalBranch(Header.instrs.front().opcode)) {
      Condition = &Header.instrs.front();
    } else if (Header.instrs.size() == 2 &&
               isReplayableIntegerLoad(Header.instrs.front()) &&
               zeroBranchTestsDefinition(Header.instrs.back(),
                                         Header.instrs.front())) {
      ReplayLoad = &Header.instrs.front();
      Condition = &Header.instrs.back();
    } else {
      continue;
    }

    const auto TrueTarget = branchTarget(*Condition);
    if (!TrueTarget)
      continue;
    const std::string FalseTarget = MF.blocks[HeaderIndex + 1].label;

    std::size_t TrueIndex = MF.blocks.size();
    for (std::size_t I = 0; I < MF.blocks.size(); ++I) {
      if (MF.blocks[I].label == *TrueTarget) {
        TrueIndex = I;
        break;
      }
    }
    if (TrueIndex == MF.blocks.size())
      continue;

    const std::size_t FalseIndex = HeaderIndex + 1;
    for (std::size_t LatchIndex = 0; LatchIndex < MF.blocks.size();
         ++LatchIndex) {
      if (LatchIndex == HeaderIndex ||
          !endsWithJumpTo(MF.blocks[LatchIndex], Header.label) ||
          MF.blocks[LatchIndex].succs.size() != 1 ||
          MF.blocks[LatchIndex].succs.front() !=
              static_cast<int>(HeaderIndex) ||
          LatchIndex >= Dominators.size() ||
          Dominators[LatchIndex].count(static_cast<int>(HeaderIndex)) == 0) {
        continue;
      }

      const std::set<int> Members = naturalLoopMembers(
          MF, static_cast<int>(HeaderIndex), static_cast<int>(LatchIndex));
      const bool TrueIsBody = Members.count(static_cast<int>(TrueIndex)) != 0;
      const bool FalseIsBody =
          Members.count(static_cast<int>(FalseIndex)) != 0;
      // 恰好一个分支目标在循环体内、另一个在体外才可旁路；两者同为体内或同为
      // 体外则无法把“继续/退出”映射到一次条件分支上。
      if (TrueIsBody == FalseIsBody)
        continue;

      const std::size_t BodyIndex = TrueIsBody ? TrueIndex : FalseIndex;
      const std::string ExitLabel = TrueIsBody ? FalseTarget : *TrueTarget;
      MachineInstr Continue = *Condition;
      if (!TrueIsBody) {
        const auto Inverse = invertedBranch(Continue.opcode);
        if (!Inverse)
          continue;
        Continue.opcode = *Inverse;
      }
      bool ReplacedTarget = false;
      for (MachineOperand &Use : Continue.uses) {
        if (Use.kind == OperandKind::Block) {
          Use.blockLabel = MF.blocks[BodyIndex].label;
          ReplacedTarget = true;
        }
      }
      if (!ReplacedTarget)
        continue;
      Rewrites.push_back(Rewrite{
          LatchIndex,
          ReplayLoad ? std::optional<MachineInstr>(*ReplayLoad) : std::nullopt,
          std::move(Continue), ExitLabel});
    }
  }

  // 统一落地所有改写：去掉 latch 原末尾的无条件跳转，改成 [可选重放 load] +
  // 继续循环条件分支 + 到 exit 的跳转。
  for (Rewrite &Item : Rewrites) {
    MachineBasicBlock &Body = MF.blocks[Item.bodyIndex];
    Body.instrs.pop_back();
    if (Item.replayLoad)
      Body.instrs.push_back(std::move(*Item.replayLoad));
    Body.instrs.push_back(std::move(Item.continueBranch));
    Body.instrs.push_back(makeJump(Item.exitLabel));
  }
  return !Rewrites.empty();
}

} // namespace aarch64
