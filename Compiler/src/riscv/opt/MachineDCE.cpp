#include "riscv/opt/MachineOptPasses.hpp"

#include <algorithm>
#include <set>
#include <vector>

namespace riscv {
// 机器级死代码消除(寄存器分配前):基于活跃变量分析,删除那些定义结果
// 未被使用的纯指令(无副作用、无控制流、只写虚拟寄存器的指令)。
// 位于后端优化流水线中,依赖 MachineLivenessAnalysis 的结果;为保守安全,
// 遇到调用、PHI、并行拷贝、有副作用/终结/隐式操作数或非常规操作数的指令
// 一律保留。RISC-V 与 ARM64 两后端使用相同实现
// 各保留一份相同实现。

namespace {

using LiveSet = MachineLivenessAnalysis::RegSet;

// 判断操作数是否为虚拟寄存器(尚未分配物理寄存器)。
bool isVirtualRegOperand(const MachineOperand &operand) {
  return operand.kind == OperandKind::Reg && !operand.regValue.physical;
}

// 判断指令是否含 DCE 无法安全处理的操作数:定义端出现非寄存器操作数,
// 或使用端出现块标签/值引用(说明涉及控制流或未降级的 IR 值)。命中即不可删。
bool hasUnsupportedOperandKind(const MachineInstr &instr) {
  for (const MachineOperand &def : instr.defs) {
    if (def.kind != OperandKind::Reg) {
      return true;
    }
  }
  for (const MachineOperand &use : instr.uses) {
    if (use.kind == OperandKind::Block || use.kind == OperandKind::ValueRef) {
      return true;
    }
  }
  return false;
}

// 判断指令是否必须无条件保留:调用、PHI、并行拷贝、有副作用、终结指令、
// 带隐式定义或隐式使用、以及含不受支持操作数的指令都不能被删。
bool shouldAlwaysKeep(const MachineInstr &instr) {
  return instr.opcode == Opcode::CALL || instr.opcode == Opcode::PHI ||
         instr.opcode == Opcode::PARALLEL_COPY || instr.sideEffects ||
         instr.terminator || !instr.implicitDefs.empty() ||
         !instr.implicitUses.empty() || hasUnsupportedOperandKind(instr);
}

// 判断纯指令能否删除:先排除必须保留者;然后要求其每个定义都是虚拟寄存器,
// 且都不在当前活跃集中(即结果无人使用)。任一定义仍活跃则不可删。
bool isRemovablePureInstr(const MachineInstr &instr, const LiveSet &live) {
  if (shouldAlwaysKeep(instr)) {
    return false;
  }

  for (const MachineOperand &def : instr.defs) {
    if (!isVirtualRegOperand(def)) {
      return false;
    }
    if (live.count(def.regValue.id) != 0) {
      return false;
    }
  }
  return true;
}

// 对一条被保留的指令更新活跃集(逆序遍历中的标准活跃维护):先移除其定义
// (在此点之后不再向上活跃),再加入其使用(向上变为活跃)。
void updateLiveForKeptInstr(const MachineInstr &instr, LiveSet &live) {
  for (const MachineOperand &def : instr.defs) {
    if (isVirtualRegOperand(def)) {
      live.erase(def.regValue.id);
    }
  }
  for (const MachineOperand &use : instr.uses) {
    if (isVirtualRegOperand(use)) {
      live.insert(use.regValue.id);
    }
  }
}

} // namespace

// 运行寄存器分配前的机器级 DCE。
// 输入:函数 mf 与提供活跃分析结果的分析管理器 am。
// 输出:就地删除死指令;返回是否发生过改动。
// 关键步骤:逐块处理,以块的 liveOut 为初值,自块尾向块首逆序扫描;
// 可删则跳过(标记 changed),否则保留并更新活跃集;最后把逆序结果翻转回正序。
bool runPreRAMachineDCE(MachineFunction &mf, MachineFunctionAnalysisManager &am) {
  const auto &live = am.getResult<MachineLivenessAnalysis>(mf);
  bool changed = false;

  for (std::size_t blockIndex = 0; blockIndex < mf.blocks.size();
       ++blockIndex) {
    // 从块出口活跃集出发,向块首方向逐指令回溯维护活跃状态。
    LiveSet currentLive = live.liveOut[blockIndex];
    MachineBasicBlock &block = mf.blocks[blockIndex];
    std::vector<MachineInstr> rewritten;
    rewritten.reserve(block.instrs.size());

    for (auto it = block.instrs.rbegin(); it != block.instrs.rend(); ++it) {
      // 定义无人使用的纯指令直接丢弃。
      if (isRemovablePureInstr(*it, currentLive)) {
        changed = true;
        continue;
      }

      updateLiveForKeptInstr(*it, currentLive);
      rewritten.push_back(std::move(*it));
    }

    // rewritten 是逆序收集的,翻转回正常执行顺序再写回块。
    std::reverse(rewritten.begin(), rewritten.end());
    block.instrs = std::move(rewritten);
  }

  return changed;
}

} // namespace riscv
