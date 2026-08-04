#include "aarch64/opt/MachineOptPasses.hpp"

#include <cstddef>
#include <unordered_map>
#include <vector>

// ARM64 专用乘加融合：把「乘法结果只被一条加/减消费」的模式融合成单条
// madd/msub（32 位）或 smaddl/smsubl（32x32+64，用于地址累加与 magic
// multiply 取高位后的加法）。适用于任意乘累加与数组地址计算的通用模式。
//
// 融合规则（均要求乘法结果为单次使用，且乘法在加/减之前、同一基本块内，
// 两者之间乘数寄存器未被重定义）：
//   形如 d = x + mul(n,m) 或 d = mul(n,m) + x 的加法，合并为 MADD/SMADDL d, n, m, x；
//   形如 d = x - mul(n,m) 的减法，合并为 MSUB/SMSUBL d, n, m, x。
// SUB 只在减数一侧是乘法时才可融合（d = x - n*m）。
//
// 位宽语义契约（P0 修复）：
//   MIR 中 MULW = 32 位乘法（mul wd, wn, wm），SMULL = 32×32→64 有符号长乘
//   （smull xd, wn, wm），MUL = 真 64 位乘法（mul xd, xn, xm）。
//   SMADDL/SMSUBL 的 ISA 语义是 32×32→64 乘加/乘减，与「SMULL + 64 位
//   ADD/SUB」逐位等价（含模 2^64 回绕），因此只有 SMULL 可参与长乘加融合；
//   真 64 位 MUL 在 AArch64 上没有对应的长乘加指令，一律不融合。
//   旧实现把 Opcode::MUL 一律按 smull 语义处理，依赖 ISel 只在 magic multiply
//   与 GEP 索引×步长两处生成 64 位乘法的内部约定；一旦出现真 64 位乘法，
//   MUL→SMADDL 会静默截断乘数为 w 视图，构成潜在错误编译。本次通过新增
//   SMULL opcode 把宽度语义显式化，MUL 不再参与任何长乘加融合。

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
bool isSmull(Opcode op) { return op == Opcode::SMULL; }
// 可融合的乘法只有 32 位 MULW 与 32×32→64 SMULL；真 64 位 MUL 不在集合内。
bool isMulOp(Opcode op) { return isMul32(op) || isSmull(op); }

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

// 乘法指令的宽度结构校验：SMULL 定义 64 位 x 结果、MULW 定义 32 位 w 结果，
// 且两个乘数槽必须是寄存器（虚拟或物理均可）。乘数槽若为立即数等非寄存器
// 操作数，SMADDL/SMSUBL/MADD/MSUB 无法编码，必须拒绝。
bool structurallyValidMul(const MachineInstr &mul) {
  if (mul.defs.size() != 1 || mul.defs.front().kind != OperandKind::Reg ||
      mul.uses.size() < 2 || mul.uses[0].kind != OperandKind::Reg ||
      mul.uses[1].kind != OperandKind::Reg) {
    return false;
  }
  const RegClass defRc = mul.defs.front().regValue.rc;
  if (isSmull(mul.opcode) && defRc != RegClass::GPR64) {
    return false;
  }
  if (isMul32(mul.opcode) && defRc != RegClass::GPR32) {
    return false;
  }
  return true;
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
        if (!structurallyValidMul(mul)) {
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
