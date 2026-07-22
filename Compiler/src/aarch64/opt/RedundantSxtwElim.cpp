#include "aarch64/opt/MachineOptPasses.hpp"

#include <cstddef>
#include <unordered_map>
#include <vector>

// ARM64 专用冗余符号扩展消除：SXTW 把 32 位有符号数扩到 64 位，但其低 32 位
// （w 视图）与源寄存器完全相同。乘法类指令 mul/mulw/madd/msub/smaddl/smsubl
// 的乘数操作数都按 w 视图读取（smull/smaddl 会对 w 视图自行符号扩展），因此
// 当一个 SXTW 结果仅被这些指令当作乘数使用时，可直接改用 SXTW 的源寄存器并
// 删除该 SXTW。常见于 GEP 索引地址计算（sxtw 索引后立即参与 smaddl 步长乘）。

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

// 这些指令的乘数操作数（uses[0]/uses[1]）按 w 视图消费，可安全替换。
bool isMulLikeWConsumer(Opcode op) {
  return op == Opcode::MUL || op == Opcode::MULW || op == Opcode::MADD ||
         op == Opcode::MSUB || op == Opcode::SMADDL || op == Opcode::SMSUBL;
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

bool runRedundantSxtwElim(MachineFunction &mf) {
  const std::unordered_map<uint32_t, int> useCounts = collectUseCounts(mf);
  bool changed = false;

  for (MachineBasicBlock &block : mf.blocks) {
    std::vector<MachineInstr> &instrs = block.instrs;
    std::vector<std::size_t> removed;

    for (std::size_t s = 0; s < instrs.size(); ++s) {
      const MachineInstr &sxtw = instrs[s];
      if (sxtw.opcode != Opcode::SXTW || sxtw.defs.size() != 1 ||
          sxtw.uses.size() != 1 || sxtw.sideEffects) {
        continue;
      }
      if (!isVReg(sxtw.defs.front()) || sxtw.uses.front().kind != OperandKind::Reg) {
        continue;
      }
      const MachineOperand dst = sxtw.defs.front();
      const MachineOperand srcReg = sxtw.uses.front();

      // 该 SXTW 结果必须恰好被使用一次。
      auto ucIt = useCounts.find(dst.regValue.id);
      if (ucIt == useCounts.end() || ucIt->second != 1) {
        continue;
      }

      // 在其后同块内找到唯一使用点，且必须是乘法类指令的乘数（uses[0]/[1]）。
      bool rewritten = false;
      for (std::size_t k = s + 1; k < instrs.size() && !rewritten; ++k) {
        MachineInstr &user = instrs[k];
        if (!isMulLikeWConsumer(user.opcode)) {
          // 若在到达使用点前源寄存器被重定义，则不安全，放弃。
          for (const MachineOperand &d : user.defs) {
            if (sameReg(d, srcReg)) {
              k = instrs.size(); // 触发外层结束
              break;
            }
          }
          continue;
        }
        for (std::size_t u = 0; u < user.uses.size() && u < 2; ++u) {
          if (sameReg(user.uses[u], dst)) {
            user.uses[u] = srcReg;
            removed.push_back(s);
            changed = true;
            rewritten = true;
            break;
          }
        }
        // 使用点在乘法但不是乘数槽（例如 smaddl 的累加数 uses[2]）：那是 x
        // 视图，不能替换，放弃这个 SXTW。
        if (!rewritten) {
          break;
        }
      }
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
