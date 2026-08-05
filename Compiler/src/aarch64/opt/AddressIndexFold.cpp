#include "aarch64/opt/MachineOptPasses.hpp"

#include <cstddef>
#include <unordered_map>
#include <vector>

// ARM64 专用地址索引折叠：把「基址 + (索引<<scale)」的显式地址计算折进
// load/store 的索引寻址 [base, index, lsl/sxtw #scale]，消除 sxtw/lsl/add
// 指令链。SysY 数组访问几乎都是 base + sext(i32 index)*elemSize，正好对应
// ARM64 的 `ldr wD, [xBase, wIndex, sxtw #2]` 单指令寻址。
//
// 匹配（零偏移的访存）：
//   SXTW  t   = wIdx            (可选)
//   SLLI  s   = t << scale      (scale == log2(accessSize))
//   ADD   A   = base + s        (寄存器加)
//   LW/SW ... [A, #0]
// 全链单次使用、源在链内未被重定义时，改写访存为索引寻址，索引取 wIdx
// (若有 sxtw，32 位) 或 t (64 位)，并删除 add/slli/sxtw。

namespace aarch64 {
namespace {

bool isVReg(const MachineOperand &op) {
  return op.kind == OperandKind::Reg && !op.regValue.physical;
}

bool sameReg(const MachineOperand &a, const MachineOperand &b) {
  if (a.kind != OperandKind::Reg || b.kind != OperandKind::Reg) {
    return false;
  }
  return a.regValue.physical == b.regValue.physical &&
         a.regValue.id == b.regValue.id && a.regValue.phys == b.regValue.phys;
}

int accessScale(Opcode op) {
  switch (op) {
  case Opcode::LBU:
  case Opcode::SB:
    return 0;
  case Opcode::LW:
  case Opcode::SW:
  case Opcode::FLW:
  case Opcode::FSW:
    return 2;
  case Opcode::LD:
  case Opcode::SD:
    return 3;
  default:
    return -1;
  }
}

bool isStoreOp(Opcode op) {
  return op == Opcode::SB || op == Opcode::SW || op == Opcode::SD ||
         op == Opcode::FSW;
}

std::unordered_map<uint32_t, int> collectUseCounts(const MachineFunction &mf) {
  std::unordered_map<uint32_t, int> counts;
  for (const MachineBasicBlock &block : mf.blocks) {
    for (const MachineInstr &instr : block.instrs) {
      for (const MachineOperand &use : instr.uses) {
        if (isVReg(use)) {
          counts[use.regValue.id] += 1;
        }
      }
    }
  }
  return counts;
}

} // namespace

bool runAddressIndexFold(MachineFunction &mf) {
  const std::unordered_map<uint32_t, int> useCounts = collectUseCounts(mf);
  bool changed = false;

  for (MachineBasicBlock &block : mf.blocks) {
    std::vector<MachineInstr> &instrs = block.instrs;
    std::vector<std::size_t> removed;

    // 查找 reg 在 endIdx 之前、块内的唯一定义下标。
    auto findDef = [&](const MachineOperand &reg,
                       std::size_t endIdx) -> long {
      for (std::size_t a = endIdx; a-- > 0;) {
        for (const MachineOperand &d : instrs[a].defs) {
          if (isVReg(d) && sameReg(d, reg)) {
            return static_cast<long>(a);
          }
        }
      }
      return -1;
    };
    auto singleUse = [&](const MachineOperand &reg) {
      auto it = useCounts.find(reg.regValue.id);
      return it != useCounts.end() && it->second == 1;
    };
    auto clobberedBetween = [&](const MachineOperand &reg, std::size_t from,
                                std::size_t to) {
      for (std::size_t k = from + 1; k < to; ++k) {
        for (const MachineOperand &d : instrs[k].defs) {
          if (d.kind == OperandKind::Reg && sameReg(d, reg)) {
            return true;
          }
        }
      }
      return false;
    };

    // 折叠方案：把某个访存改写成索引寻址所需的信息。
    struct FoldPlan {
      std::size_t memIdx;
      MachineOperand ptr;
      MachineOperand index;
      int scale;
      std::vector<std::size_t> remove;
      // 归一化的地址键：同一 (base,index,scale) 折叠多次说明该地址是可共享/
      // 可外提的（典型为循环不变量），此时逐次折成索引寻址反而放弃了共享，
      // 实测更慢；仅折叠很少复用的地址。
      uint64_t key;
    };

    auto regKey = [](const MachineOperand &r) -> uint64_t {
      // 物理寄存器用 phys 编号高位区分，虚拟寄存器用 id。
      return r.regValue.physical
                 ? (0x8000000000000000ULL |
                    static_cast<uint64_t>(r.regValue.phys))
                 : static_cast<uint64_t>(r.regValue.id);
    };

    std::vector<FoldPlan> plans;

    for (std::size_t i = 0; i < instrs.size(); ++i) {
      MachineInstr &mem = instrs[i];
      const int scale = accessScale(mem.opcode);
      if (scale < 0 || scale == 0) {
        continue;
      }
      const bool store = isStoreOp(mem.opcode);
      const int offIdx = store ? 2 : 1;
      const int baseIdx = store ? 1 : 0;
      if (static_cast<int>(mem.uses.size()) != offIdx + 1 ||
          mem.uses[offIdx].kind != OperandKind::Imm ||
          mem.uses[offIdx].immValue != 0 ||
          mem.uses[baseIdx].kind != OperandKind::Reg) {
        continue;
      }
      const MachineOperand memBase = mem.uses[baseIdx];
      if (!isVReg(memBase) || !singleUse(memBase)) {
        continue;
      }
      const long addIdx = findDef(memBase, i);
      if (addIdx < 0) {
        continue;
      }
      const MachineInstr &add = instrs[addIdx];
      if (add.opcode != Opcode::ADD || add.uses.size() != 2 ||
          add.sideEffects || !isVReg(add.uses[0]) || !isVReg(add.uses[1])) {
        continue;
      }
      for (int shSlot = 0; shSlot < 2; ++shSlot) {
        const MachineOperand shifted = add.uses[shSlot];
        const MachineOperand ptr = add.uses[shSlot == 0 ? 1 : 0];
        if (!singleUse(shifted)) {
          continue;
        }
        const long shIdx = findDef(shifted, static_cast<std::size_t>(addIdx));
        if (shIdx < 0) {
          continue;
        }
        const MachineInstr &sh = instrs[shIdx];
        if (sh.opcode != Opcode::SLLI || sh.uses.size() != 2 ||
            sh.uses[1].kind != OperandKind::Imm ||
            sh.uses[1].immValue != scale || !isVReg(sh.uses[0]) ||
            sh.sideEffects) {
          continue;
        }
        if (clobberedBetween(ptr, static_cast<std::size_t>(addIdx), i)) {
          continue;
        }
        MachineOperand indexReg = sh.uses[0];
        std::vector<std::size_t> toRemove = {
            static_cast<std::size_t>(addIdx), static_cast<std::size_t>(shIdx)};

        if (singleUse(indexReg)) {
          const long sxIdx =
              findDef(indexReg, static_cast<std::size_t>(shIdx));
          if (sxIdx >= 0 && instrs[sxIdx].opcode == Opcode::SXTW &&
              instrs[sxIdx].uses.size() == 1 &&
              isVReg(instrs[sxIdx].uses[0]) && !instrs[sxIdx].sideEffects &&
              !clobberedBetween(instrs[sxIdx].uses[0],
                                static_cast<std::size_t>(sxIdx),
                                static_cast<std::size_t>(shIdx))) {
            indexReg = instrs[sxIdx].uses[0];
            toRemove.push_back(static_cast<std::size_t>(sxIdx));
          }
        }
        if (clobberedBetween(indexReg, static_cast<std::size_t>(shIdx), i)) {
          continue;
        }

        FoldPlan plan;
        plan.memIdx = i;
        plan.ptr = ptr;
        plan.index = indexReg;
        plan.scale = scale;
        plan.remove = std::move(toRemove);
        plan.key = regKey(ptr) * 1315423911u + regKey(indexReg) * 2654435761u +
                   static_cast<uint64_t>(scale);
        plans.push_back(std::move(plan));
        break;
      }
    }

    // 统计每个地址键的折叠次数，只折叠复用不超过 2 次的地址。
    std::unordered_map<uint64_t, int> keyCount;
    for (const FoldPlan &p : plans) {
      keyCount[p.key] += 1;
    }

    for (const FoldPlan &p : plans) {
      if (keyCount[p.key] > 2) {
        continue;
      }
      MachineInstr &mem = instrs[p.memIdx];
      const bool store = isStoreOp(mem.opcode);
      const int offIdx = store ? 2 : 1;
      const int baseIdx = store ? 1 : 0;
      mem.uses[baseIdx] = p.ptr;
      mem.uses[offIdx] = MachineOperand::imm(0);
      mem.uses.push_back(p.index);
      mem.uses.push_back(MachineOperand::imm(p.scale));
      for (std::size_t idx : p.remove) {
        removed.push_back(idx);
      }
      changed = true;
    }

    if (!removed.empty()) {
      std::vector<bool> drop(instrs.size(), false);
      for (std::size_t idx : removed) {
        drop[idx] = true;
      }
      std::vector<MachineInstr> kept;
      kept.reserve(instrs.size());
      for (std::size_t i = 0; i < instrs.size(); ++i) {
        if (!drop[i]) {
          kept.push_back(std::move(instrs[i]));
        }
      }
      instrs = std::move(kept);
    }
  }

  return changed;
}

} // namespace aarch64
