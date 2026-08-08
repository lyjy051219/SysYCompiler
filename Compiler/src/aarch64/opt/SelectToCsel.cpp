#include "aarch64/opt/MachineOptPasses.hpp"

#include <cstddef>
#include <cstdlib>
#include <unordered_map>
#include <vector>

// ARM64 专用条件选择 lowering：把 SELECT 伪指令转成 CSEL，尽量把产生条件的比较
// 直接折进 csel 的 cmp/fcmp，从而用「[f]cmp + csel」两条指令替代原本的
// 无分支掩码序列（sub 掩码 + and + xor/add，3~4 条）或菱形分支。
// 这是完全通用的优化：所有整型条件选择（min/max、按条件累加等）都受益。
//
// 策略：
//   - 条件由同块内单次使用的比较（SLT/SLTU/SEQZ/SNEZ/FEQ/FLT/FLE）定义，
//     且比较操作数在比较与 SELECT 之间未被重定义时，折进 CSEL 并删除比较；
//   - 否则回退为「与零比较」：CSEL 内嵌 SNEZ，发射 cmp cond,#0 + csel ne。
//   - tst 融合（沿单次使用恒等拷贝链追溯 ANDI）默认关闭，
//     设置环境变量 BACKEND_ENABLE_CSEL_TST_FUSION 才启用。
// 只处理整型 GPR 结果的 SELECT；浮点结果的 SELECT 留给原有 lowering。

namespace aarch64 {
namespace {

bool isVReg(const MachineOperand &op) {
  return op.kind == OperandKind::Reg && !op.regValue.physical;
}

bool isCompareOpcode(Opcode op) {
  return op == Opcode::SLT || op == Opcode::SLTU || op == Opcode::SEQZ ||
         op == Opcode::SNEZ || op == Opcode::FEQ_S || op == Opcode::FLT_S ||
         op == Opcode::FLE_S;
}

bool isZeroCompare(Opcode op) {
  return op == Opcode::SEQZ || op == Opcode::SNEZ;
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

bool sameReg(const MachineOperand &a, const MachineOperand &b) {
  if (a.kind != OperandKind::Reg || b.kind != OperandKind::Reg) {
    return false;
  }
  return a.regValue.physical == b.regValue.physical &&
         a.regValue.id == b.regValue.id && a.regValue.phys == b.regValue.phys;
}

} // namespace

bool runSelectToCsel(MachineFunction &mf) {
  const std::unordered_map<uint32_t, int> useCounts = collectUseCounts(mf);
  bool changed = false;

  for (MachineBasicBlock &block : mf.blocks) {
    std::vector<MachineInstr> &instrs = block.instrs;
    std::vector<std::size_t> removed;

    for (std::size_t i = 0; i < instrs.size(); ++i) {
      MachineInstr &sel = instrs[i];
      if (sel.opcode != Opcode::SELECT || sel.defs.size() != 1 ||
          sel.uses.size() < 3) {
        continue;
      }
      // 只处理整型 GPR 结果。
      const RegClass rc = sel.defs.front().regValue.rc;
      if (rc == RegClass::FPR32) {
        continue;
      }
      if (!isVReg(sel.uses[0]) || sel.uses[1].kind != OperandKind::Reg ||
          sel.uses[2].kind != OperandKind::Reg) {
        continue;
      }

      const MachineOperand cond = sel.uses[0];
      const MachineOperand trueVal = sel.uses[1];
      const MachineOperand falseVal = sel.uses[2];

      // 尝试在同块、SELECT 之前找到定义 cond 的单次使用比较。
      Opcode cmpOp = Opcode::SNEZ;
      MachineOperand cmpLhs = cond;
      MachineOperand cmpRhs = MachineOperand::reg(
          MachineReg::preg(PhysReg::X0, RegClass::GPR32));
      bool foldedCompare = false;

      auto ucIt = useCounts.find(cond.regValue.id);
      const bool condSingleUse = ucIt != useCounts.end() && ucIt->second == 1;
      if (condSingleUse) {
        for (std::size_t j = i; j-- > 0;) {
          const MachineInstr &def = instrs[j];
          bool defsCond = false;
          for (const MachineOperand &d : def.defs) {
            if (isVReg(d) && sameReg(d, cond)) {
              defsCond = true;
              break;
            }
          }
          if (!defsCond) {
            continue;
          }
          if (isCompareOpcode(def.opcode) && !def.sideEffects &&
              !def.uses.empty()) {
            // 检查比较操作数在 (j, i) 之间未被重定义。
            const MachineInstr cmp = def;
            bool clobbered = false;
            for (std::size_t k = j + 1; k < i && !clobbered; ++k) {
              for (const MachineOperand &d : instrs[k].defs) {
                if (d.kind != OperandKind::Reg) {
                  continue;
                }
                for (const MachineOperand &cu : cmp.uses) {
                  if (cu.kind == OperandKind::Reg && sameReg(d, cu)) {
                    clobbered = true;
                    break;
                  }
                }
              }
            }
            if (!clobbered) {
              cmpOp = cmp.opcode;
              cmpLhs = cmp.uses[0];
              cmpRhs = isZeroCompare(cmp.opcode)
                           ? MachineOperand::reg(MachineReg::preg(
                                 PhysReg::X0, RegClass::GPR32))
                           : cmp.uses[1];
              foldedCompare = true;
              removed.push_back(j);

              // tst 融合：与零比较时，沿「单次使用的恒等拷贝」（等值比较 lowering
              // 留下的 XORI #0、以及 COPY）向上追溯 cmpLhs 的真正定义，若最终
              // 是单次使用的 ANDI(立即数)，则把 (x & mask) 折进 tst：cmpLhs 改
              // 为 x、cmpRhs 改为 Imm(mask)，并删除沿途的恒等拷贝与该 ANDI。
              // 全程要求单次使用且源在中间未被重定义，故安全。
              if (std::getenv("BACKEND_ENABLE_CSEL_TST_FUSION") != nullptr &&
                  isZeroCompare(cmp.opcode)) {
                MachineOperand cur = cmpLhs;
                std::size_t searchEnd = j; // 追溯定义须在此下标之前
                std::vector<std::size_t> chainRemoved;
                int64_t tstMask = 0;
                bool gotTst = false;
                for (int hop = 0; hop < 8 && isVReg(cur); ++hop) {
                  auto uc = useCounts.find(cur.regValue.id);
                  if (uc == useCounts.end() || uc->second != 1) {
                    break; // 非单次使用，不能安全改写
                  }
                  // 找 cur 在 searchEnd 之前的唯一定义。
                  std::size_t defIdx = 0;
                  bool found2 = false;
                  for (std::size_t a = searchEnd; a-- > 0;) {
                    bool defsCur = false;
                    for (const MachineOperand &d : instrs[a].defs) {
                      if (isVReg(d) && sameReg(d, cur)) {
                        defsCur = true;
                        break;
                      }
                    }
                    if (defsCur) {
                      defIdx = a;
                      found2 = true;
                      break;
                    }
                  }
                  if (!found2) {
                    break;
                  }
                  const MachineInstr &d2 = instrs[defIdx];
                  if (d2.sideEffects || d2.defs.size() != 1) {
                    break;
                  }
                  // 源在 (defIdx, searchEnd) 之间不得被重定义。
                  auto clobberedBetween = [&](const MachineOperand &src) {
                    for (std::size_t k = defIdx + 1; k < searchEnd; ++k) {
                      for (const MachineOperand &dd : instrs[k].defs) {
                        if (dd.kind == OperandKind::Reg && sameReg(dd, src)) {
                          return true;
                        }
                      }
                    }
                    return false;
                  };
                  // ANDI 立即数：命中 tst。
                  if (d2.opcode == Opcode::ANDI && d2.uses.size() >= 2 &&
                      d2.uses[0].kind == OperandKind::Reg &&
                      d2.uses[1].kind == OperandKind::Imm &&
                      !clobberedBetween(d2.uses[0])) {
                    cmpLhs = d2.uses[0];
                    tstMask = d2.uses[1].immValue;
                    chainRemoved.push_back(defIdx);
                    gotTst = true;
                    break;
                  }
                  // 恒等拷贝：XORI/ORI #0 或 COPY，继续向上追溯。
                  const bool isXorOr0 =
                      (d2.opcode == Opcode::XORI || d2.opcode == Opcode::ORI) &&
                      d2.uses.size() >= 2 &&
                      d2.uses[0].kind == OperandKind::Reg &&
                      d2.uses[1].kind == OperandKind::Imm &&
                      d2.uses[1].immValue == 0;
                  const bool isCopy = d2.opcode == Opcode::COPY &&
                                      d2.uses.size() == 1 &&
                                      d2.uses[0].kind == OperandKind::Reg;
                  if ((isXorOr0 || isCopy) && !clobberedBetween(d2.uses[0])) {
                    chainRemoved.push_back(defIdx);
                    cur = d2.uses[0];
                    searchEnd = defIdx;
                    continue;
                  }
                  break;
                }
                if (gotTst) {
                  cmpRhs = MachineOperand::imm(tstMask);
                  for (std::size_t idx : chainRemoved) {
                    removed.push_back(idx);
                  }
                }
              }
            }
          }
          break; // 只看最近一次定义。
        }
      }

      // 改写为 CSEL：uses = {trueVal, falseVal, cmpLhs, cmpRhs, Imm(cmpOp)}。
      sel.opcode = Opcode::CSEL;
      sel.uses.clear();
      sel.uses.push_back(trueVal);
      sel.uses.push_back(falseVal);
      sel.uses.push_back(cmpLhs);
      sel.uses.push_back(cmpRhs);
      sel.uses.push_back(MachineOperand::imm(static_cast<int64_t>(cmpOp)));
      (void)foldedCompare;
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
