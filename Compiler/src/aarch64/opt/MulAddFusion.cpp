#include "aarch64/opt/MachineOptPasses.hpp"

#include <cstddef>
#include <unordered_map>
#include <vector>

// ARM64 专用乘加融合：把「乘法结果只被一条加/减消费」的模式融合成单条
// madd/msub（32 位）或 smaddl/smsubl（32x32+64，用于地址累加与 magic
// multiply 取高位后的加法）。收益覆盖矩阵乘、fft、稀疏矩阵向量乘等所有
// 乘累加与数组地址计算热点。
//
// 融合规则（均要求乘法结果为单次使用，且乘法在加/减之前、同一基本块内，
// 两者之间乘数寄存器未被重定义）：
//   ADD  d, x, mul(n,m)  或  ADD  d, mul(n,m), x  ->  MADD/SMADDL d, n, m, x
//   SUB  d, x, mul(n,m)                            ->  MSUB/SMSUBL d, n, m, x
// SUB 只在减数一侧是乘法时才可融合（d = x - n*m）。

namespace aarch64 {
namespace {

bool isVReg(const MachineOperand &op) {
  return op.kind == OperandKind::Reg && !op.regValue.physical;
}

// 全函数范围统计每个虚拟寄存器被用作 use 的次数。
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

bool isMul32(Opcode op) { return op == Opcode::MULW; }
bool isMul64(Opcode op) { return op == Opcode::MUL; }
bool isMulOp(Opcode op) { return isMul32(op) || isMul64(op); }

// 加法（可交换）与减法的宽度是否与乘法匹配。
bool addMatchesMul(Opcode addOp, Opcode mulOp) {
  if (isMul32(mulOp)) {
    return addOp == Opcode::ADDW;
  }
  return addOp == Opcode::ADD; // 64 位
}
bool subMatchesMul(Opcode subOp, Opcode mulOp) {
  if (isMul32(mulOp)) {
    return subOp == Opcode::SUBW;
  }
  return subOp == Opcode::SUB;
}

Opcode fusedAdd(Opcode mulOp) {
  return isMul32(mulOp) ? Opcode::MADD : Opcode::SMADDL;
}
Opcode fusedSub(Opcode mulOp) {
  return isMul32(mulOp) ? Opcode::MSUB : Opcode::SMSUBL;
}

} // namespace

bool runMulAddFusion(MachineFunction &mf) {
  const std::unordered_map<uint32_t, int> useCounts = collectUseCounts(mf);
  bool changed = false;

  for (MachineBasicBlock &block : mf.blocks) {
    std::vector<MachineInstr> &instrs = block.instrs;
    // 记录本块内每个虚拟寄存器最近一次被定义的指令下标，便于回溯乘法定义
    // 并检查乘数是否在乘法与加法之间被重定义。
    std::vector<std::size_t> removed;

    for (std::size_t i = 0; i < instrs.size(); ++i) {
      MachineInstr &add = instrs[i];
      const bool isAdd =
          add.opcode == Opcode::ADD || add.opcode == Opcode::ADDW;
      const bool isSub =
          add.opcode == Opcode::SUB || add.opcode == Opcode::SUBW;
      if (!isAdd && !isSub) {
        continue;
      }
      if (add.defs.size() != 1 || add.uses.size() < 2 || add.sideEffects) {
        continue;
      }

      // 候选乘法操作数下标：加法两侧都可；减法只看减数（uses[1]）。
      const int candidateSlots[2] = {isSub ? 1 : 0, isSub ? -1 : 1};
      for (int slotIdx = 0; slotIdx < 2; ++slotIdx) {
        const int slot = candidateSlots[slotIdx];
        if (slot < 0) {
          break;
        }
        const MachineOperand &prod = add.uses[slot];
        // 注意：acc 必须是拷贝而非引用。下面会 add.uses.clear() 再 push_back，
        // 引用会在 clear 后悬空导致累加数错误。
        const MachineOperand acc = add.uses[slot == 0 ? 1 : 0];
        if (!isVReg(prod) || !isVReg(acc)) {
          continue;
        }
        // 乘法结果必须单次使用，融合后原乘法即成死指令可安全移除。
        auto cntIt = useCounts.find(prod.regValue.id);
        if (cntIt == useCounts.end() || cntIt->second != 1) {
          continue;
        }

        // 在本块内、加法之前回溯定义该乘积的乘法指令。
        std::size_t mulIdx = i;
        bool found = false;
        for (std::size_t j = i; j-- > 0;) {
          const MachineInstr &cand = instrs[j];
          bool defsProd = false;
          for (const MachineOperand &d : cand.defs) {
            if (isVReg(d) && d.regValue.id == prod.regValue.id) {
              defsProd = true;
              break;
            }
          }
          if (defsProd) {
            mulIdx = j;
            found = isMulOp(cand.opcode) && cand.uses.size() >= 2 &&
                    !cand.sideEffects;
            break;
          }
        }
        if (!found) {
          continue;
        }
        MachineInstr &mul = instrs[mulIdx];
        const Opcode mulOp = mul.opcode;
        if (isAdd && !addMatchesMul(add.opcode, mulOp)) {
          continue;
        }
        if (isSub && !subMatchesMul(add.opcode, mulOp)) {
          continue;
        }
        if (!isVReg(mul.uses[0]) && mul.uses[0].kind != OperandKind::Reg) {
          continue;
        }

        // 检查乘数在 (mulIdx, i) 之间未被重定义，保证语义不变。
        bool clobbered = false;
        for (std::size_t k = mulIdx + 1; k < i && !clobbered; ++k) {
          for (const MachineOperand &d : instrs[k].defs) {
            if (d.kind != OperandKind::Reg) {
              continue;
            }
            for (const MachineOperand &mu : mul.uses) {
              if (mu.kind == OperandKind::Reg &&
                  d.regValue.physical == mu.regValue.physical &&
                  d.regValue.id == mu.regValue.id &&
                  d.regValue.phys == mu.regValue.phys) {
                clobbered = true;
                break;
              }
            }
          }
        }
        if (clobbered) {
          continue;
        }

        // 改写加法为融合指令：defs 不变，uses = {n, m, acc}。
        const MachineOperand n = mul.uses[0];
        const MachineOperand m = mul.uses[1];
        add.opcode = isSub ? fusedSub(mulOp) : fusedAdd(mulOp);
        add.uses.clear();
        add.uses.push_back(n);
        add.uses.push_back(m);
        add.uses.push_back(acc);
        removed.push_back(mulIdx);
        changed = true;
        break;
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
