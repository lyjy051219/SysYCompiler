#include "riscv/opt/CodegenPasses.hpp"
#include "riscv/opt/MachineAnalysis.hpp"
#include "riscv/opt/MachineOptPasses.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace riscv {
// Pre-RA Affine Combine pass 的公共实现。
//
// 本 pass 有两大部分：
// 1. 带符号 2 的幂除法 bias 序列的折叠：指令选择为 "有符号数除以 2 的幂" 生成的
//    是一段规范的 "算术右移取符号 + 逻辑右移取偏置 + 加偏置 + 算术右移" 序列。
//    这里把链式的多次此类除法合并为一次（截断除法对正除数可传递，移位量相加）、
//    把 "除以 2" 的偏置 (x>>s31)&1 直接用一条 SRLIW 读取符号位、并在被除数已由更
//    宽算术右移窄化时消去中间的取符号右移。
// 2. 仿射表达式合成：把一串定义纯 GPR32 结果的 LI/COPY/ADDW/SUBW/ADDIW/SLLIW
//    指令折叠成一个仿射形式（常量 + 各寄存器系数之和），再用移位与加/减重新
//    合成等价序列；仅当新序列的指令代价严格低于被替换的死指令链代价时才采用。
//
// 数学前提与 guard：仿射合成在 32 位模运算（word 运算）下进行，系数与常量都按
// uint32 环绕，重构指令末尾用 ADDIW 0 做 word 规范化（符号扩展第 31 位）。除法
// bias 折叠的每处化简都附有其成立的取值范围前提（见各函数内注释），越界即放弃。
//
// 流水线位置：寄存器分配前（Pre-RA），产出带虚拟寄存器的新指令，保留块结构类
// 分析。RISC-V 与 ARM64 两后端使用相同实现
// 两后端各保留一份相同实现。

namespace {

// 仿射合成与依赖分析的规模上限，防止在病态输入上展开过大。
constexpr std::size_t kMaxAffineNodes = 64;
constexpr std::size_t kMaxAffineTerms = 8;
constexpr std::size_t kMaxGeneratedInstrs = 16;

// 某虚拟寄存器的定义信息：被定义次数，以及首次定义所在的块与指令下标。
struct DefinitionInfo {
  std::size_t count = 0;
  std::size_t blockIndex = 0;
  std::size_t instrIndex = 0;
};

using DefinitionTable = std::map<uint32_t, DefinitionInfo>;
using UseCountTable = std::map<uint32_t, std::size_t>;

// 逐字段比较两个寄存器是否相同。
bool sameRegister(const MachineReg &lhs, const MachineReg &rhs) {
  return lhs.physical == rhs.physical && lhs.id == rhs.id && lhs.rc == rhs.rc &&
         lhs.phys == rhs.phys;
}

// 操作数是否是 GPR32 类虚拟寄存器（仿射合成只处理 32 位整数运算）。
bool isVirtualGpr32(const MachineOperand &operand) {
  return operand.kind == OperandKind::Reg && !operand.regValue.physical &&
         operand.regValue.rc == RegClass::GPR32;
}

// 操作数是否是虚拟寄存器。
bool isVirtualRegister(const MachineOperand &operand) {
  return operand.kind == OperandKind::Reg && !operand.regValue.physical;
}

// 第 index 个 use 操作数是否是寄存器。
bool isRegOperand(const MachineInstr &instr, std::size_t index) {
  return index < instr.uses.size() &&
         instr.uses[index].kind == OperandKind::Reg;
}

// 第 index 个 use 操作数是否是立即数。
bool isImmOperand(const MachineInstr &instr, std::size_t index) {
  return index < instr.uses.size() &&
         instr.uses[index].kind == OperandKind::Imm;
}

// 指令是否恰好定义一个纯 GPR32 虚拟寄存器且无副作用/非终结/无隐式操作数。
// 这是参与仿射合成或 bias 折叠的基本前提（结果干净、可安全替换/删除）。
bool hasPureGpr32Def(const MachineInstr &instr) {
  return !instr.sideEffects && !instr.terminator &&
         instr.implicitDefs.empty() && instr.implicitUses.empty() &&
         instr.defs.size() == 1 && isVirtualGpr32(instr.defs.front());
}

// 指令是否是可展开进仿射形式的运算：LI（常量）、COPY、ADDW/SUBW（寄存器加减）、
// ADDIW（加立即数）、SLLIW（左移常量位，等价乘 2 的幂，移位量限 0..31）。
bool isExpandableAffineInstr(const MachineInstr &instr) {
  if (!hasPureGpr32Def(instr)) {
    return false;
  }

  switch (instr.opcode) {
  case Opcode::LI:
    return instr.uses.size() == 1 && isImmOperand(instr, 0);
  case Opcode::COPY:
    return instr.uses.size() == 1 && isRegOperand(instr, 0);
  case Opcode::ADDW:
  case Opcode::SUBW:
    return instr.uses.size() == 2 && isRegOperand(instr, 0) &&
           isRegOperand(instr, 1);
  case Opcode::ADDIW:
    return instr.uses.size() == 2 && isRegOperand(instr, 0) &&
           isImmOperand(instr, 1);
  case Opcode::SLLIW:
    return instr.uses.size() == 2 && isRegOperand(instr, 0) &&
           isImmOperand(instr, 1) && instr.uses[1].immValue >= 0 &&
           instr.uses[1].immValue < 32;
  default:
    return false;
  }
}

// 扫描全函数，为每个虚拟寄存器统计定义次数并记录首次定义位置。仿射合成只信任
// 唯一定义（count==1）的寄存器，因此这张表用于判定某值能否被内联展开。
DefinitionTable collectDefinitions(const MachineFunction &mf) {
  DefinitionTable definitions;
  for (std::size_t blockIndex = 0; blockIndex < mf.blocks.size();
       ++blockIndex) {
    const MachineBasicBlock &block = mf.blocks[blockIndex];
    for (std::size_t instrIndex = 0; instrIndex < block.instrs.size();
         ++instrIndex) {
      for (const MachineOperand &def : block.instrs[instrIndex].defs) {
        if (!isVirtualRegister(def)) {
          continue;
        }
        DefinitionInfo &info = definitions[def.regValue.id];
        if (info.count == 0) {
          info.blockIndex = blockIndex;
          info.instrIndex = instrIndex;
        }
        ++info.count;
      }
    }
  }
  return definitions;
}

// 统计每个虚拟寄存器的使用次数，用于判断展开某中间值后它是否变为死值可删除。
UseCountTable collectUseCounts(const MachineFunction &mf) {
  UseCountTable counts;
  for (const MachineBasicBlock &block : mf.blocks) {
    for (const MachineInstr &instr : block.instrs) {
      for (const MachineOperand &use : instr.uses) {
        if (isVirtualRegister(use)) {
          ++counts[use.regValue.id];
        }
      }
    }
  }
  return counts;
}

// 一个仿射形式：常量 + 各寄存器（按 id）系数之和（均按 uint32 模运算），并记录
// 展开过程中吸收的中间结点 id 集合（用于死链投影）。
struct AffineForm {
  uint32_t constant = 0;
  std::map<uint32_t, uint32_t> coefficients;
  std::set<uint32_t> expandedNodes;
};

// 删除系数为 0 的项，保持仿射形式规范。
void removeZeroCoefficients(AffineForm &form) {
  for (auto it = form.coefficients.begin(); it != form.coefficients.end();) {
    if (it->second == 0) {
      it = form.coefficients.erase(it);
    } else {
      ++it;
    }
  }
}

// 从某条根指令出发，沿唯一定义的可展开指令递归地把一段计算折叠成仿射形式。
// 展开时把叶子（物理 x0 视为 0、其他 GPR32 视为系数 1 的自由变量）与各运算按
// 加法/取负/数乘组合，超过项数或结点数上限即放弃。带 memo 缓存与 active 环检测。
class AffineFormBuilder {
public:
  AffineFormBuilder(const MachineFunction &mf,
                    const DefinitionTable &definitions, std::size_t blockIndex)
      : mf_(mf), definitions_(definitions), blockIndex_(blockIndex) {}

  // 以 instrIndex 处的指令为根构造仿射形式。要求该根是可展开指令、其定义寄存器
  // 恰在此处唯一定义，否则不作为根返回。
  std::optional<AffineForm> buildRoot(std::size_t instrIndex) {
    const MachineInstr &instr = mf_.blocks[blockIndex_].instrs[instrIndex];
    if (!isExpandableAffineInstr(instr)) {
      return std::nullopt;
    }
    const uint32_t root = instr.defs.front().regValue.id;
    auto found = definitions_.find(root);
    if (found == definitions_.end() || found->second.count != 1 ||
        found->second.blockIndex != blockIndex_ ||
        found->second.instrIndex != instrIndex) {
      return std::nullopt;
    }
    return buildDefinition(root, instrIndex);
  }

private:
  // 把一个寄存器当作仿射形式的叶子：x0 是常量 0，其他 GPR32 是系数 1 的自由项，
  // 非 GPR32 无法处理。
  std::optional<AffineForm> leaf(const MachineReg &reg) const {
    if (reg.physical) {
      if (reg.phys == PhysReg::X0) {
        return AffineForm{};
      }
      return std::nullopt;
    }
    if (reg.rc != RegClass::GPR32) {
      return std::nullopt;
    }
    AffineForm form;
    form.coefficients[reg.id] = 1;
    return form;
  }

  // 仿射形式是否仍在项数/结点数上限内。
  bool withinLimits(const AffineForm &form) const {
    return form.coefficients.size() <= kMaxAffineTerms &&
           form.expandedNodes.size() <= kMaxAffineNodes;
  }

  // 两个仿射形式相加（常量与同名系数按 uint32 相加），删零项后检查上限。
  std::optional<AffineForm> add(AffineForm lhs, const AffineForm &rhs) const {
    lhs.constant += rhs.constant;
    for (const auto &[id, coefficient] : rhs.coefficients) {
      lhs.coefficients[id] += coefficient;
    }
    lhs.expandedNodes.insert(rhs.expandedNodes.begin(),
                             rhs.expandedNodes.end());
    removeZeroCoefficients(lhs);
    return withinLimits(lhs) ? std::optional<AffineForm>(std::move(lhs))
                             : std::nullopt;
  }

  // 仿射形式整体数乘 factor（常量与各系数按 uint32 相乘）。SUBW 用 factor=UINT32_MAX
  // 即乘 -1 实现取负，SLLIW 用 factor=2^shift 实现左移。
  std::optional<AffineForm> scale(AffineForm form, uint32_t factor) const {
    form.constant *= factor;
    for (auto &[id, coefficient] : form.coefficients) {
      (void)id;
      coefficient *= factor;
    }
    removeZeroCoefficients(form);
    return withinLimits(form) ? std::optional<AffineForm>(std::move(form))
                              : std::nullopt;
  }

  // 求某个操作数寄存器的仿射形式：物理寄存器/非 GPR32 走叶子；虚拟 GPR32 若在本块
  // 内、在消费者之前唯一定义且定义指令可展开，则递归展开，否则作为叶子自由项。
  // consumerIndex 保证只向定义在使用点之前的指令展开，避免前向/环形引用。
  std::optional<AffineForm> formForOperand(const MachineOperand &operand,
                                           std::size_t consumerIndex) {
    if (operand.kind != OperandKind::Reg) {
      return std::nullopt;
    }
    const MachineReg reg = operand.regValue;
    if (reg.physical) {
      return leaf(reg);
    }
    if (reg.rc != RegClass::GPR32) {
      return std::nullopt;
    }

    auto found = definitions_.find(reg.id);
    if (found == definitions_.end() || found->second.count != 1 ||
        found->second.blockIndex != blockIndex_ ||
        found->second.instrIndex >= consumerIndex) {
      return leaf(reg);
    }
    const MachineInstr &definition =
        mf_.blocks[blockIndex_].instrs[found->second.instrIndex];
    if (!isExpandableAffineInstr(definition)) {
      return leaf(reg);
    }
    return buildDefinition(reg.id, found->second.instrIndex);
  }

  // 按操作码把一条定义指令展开为仿射形式：LI 给常量、COPY 透传、ADDW/SUBW 加减
  // （SUBW 先对右侧取负）、ADDIW 加立即数、SLLIW 数乘 2 的幂。带 memo 与 active
  // 环检测；结果记入 expandedNodes 并做上限检查。
  std::optional<AffineForm> buildDefinition(uint32_t id,
                                            std::size_t instrIndex) {
    auto memoized = memo_.find(id);
    if (memoized != memo_.end()) {
      return memoized->second;
    }
    if (!active_.insert(id).second) {
      return std::nullopt;
    }

    const MachineInstr &instr = mf_.blocks[blockIndex_].instrs[instrIndex];
    std::optional<AffineForm> result;
    switch (instr.opcode) {
    case Opcode::LI: {
      AffineForm form;
      form.constant = static_cast<uint32_t>(instr.uses[0].immValue);
      result = std::move(form);
      break;
    }
    case Opcode::COPY:
      result = formForOperand(instr.uses[0], instrIndex);
      break;
    case Opcode::ADDW:
    case Opcode::SUBW: {
      auto lhs = formForOperand(instr.uses[0], instrIndex);
      auto rhs = formForOperand(instr.uses[1], instrIndex);
      if (lhs && rhs) {
        if (instr.opcode == Opcode::SUBW) {
          rhs = scale(std::move(*rhs), UINT32_MAX);
        }
        if (rhs) {
          result = add(std::move(*lhs), *rhs);
        }
      }
      break;
    }
    case Opcode::ADDIW: {
      result = formForOperand(instr.uses[0], instrIndex);
      if (result) {
        result->constant += static_cast<uint32_t>(instr.uses[1].immValue);
      }
      break;
    }
    case Opcode::SLLIW: {
      result = formForOperand(instr.uses[0], instrIndex);
      if (result) {
        const unsigned shift = static_cast<unsigned>(instr.uses[1].immValue);
        result = scale(std::move(*result), uint32_t{1} << shift);
      }
      break;
    }
    default:
      break;
    }

    active_.erase(id);
    if (!result) {
      return std::nullopt;
    }
    result->expandedNodes.insert(id);
    if (!withinLimits(*result)) {
      return std::nullopt;
    }
    memo_[id] = *result;
    return result;
  }

  const MachineFunction &mf_;
  const DefinitionTable &definitions_;
  std::size_t blockIndex_;
  std::map<uint32_t, AffineForm> memo_;
  std::set<uint32_t> active_;
};

// 以下 make* 系列是构造具体机器指令的小工厂：一元带立即数、二元寄存器、加载
// 立即数。用于仿射合成阶段发射重构后的指令序列。
MachineInstr makeUnaryImm(Opcode opcode, MachineReg dst, MachineReg src,
                          int64_t immediate) {
  MachineInstr instr;
  instr.opcode = opcode;
  instr.defs.push_back(MachineOperand::reg(dst));
  instr.uses.push_back(MachineOperand::reg(src));
  instr.uses.push_back(MachineOperand::imm(immediate));
  return instr;
}

MachineInstr makeBinary(Opcode opcode, MachineReg dst, MachineReg lhs,
                        MachineReg rhs) {
  MachineInstr instr;
  instr.opcode = opcode;
  instr.defs.push_back(MachineOperand::reg(dst));
  instr.uses.push_back(MachineOperand::reg(lhs));
  instr.uses.push_back(MachineOperand::reg(rhs));
  return instr;
}

MachineInstr makeLoadImmediate(MachineReg dst, int64_t immediate) {
  MachineInstr instr;
  instr.opcode = Opcode::LI;
  instr.defs.push_back(MachineOperand::reg(dst));
  instr.uses.push_back(MachineOperand::imm(immediate));
  return instr;
}

// 统计 value 的二进制 1 的个数（用于估算按位重构一个乘数的代价）。
unsigned populationCount(uint32_t value) {
  unsigned count = 0;
  while (value != 0) {
    value &= value - 1;
    ++count;
  }
  return count;
}

// 估算用“移位 + 加法”重构一个乘数 magnitude 所需的指令条数：每个置位对应一个移位
// 结果（bit0 不需移位），再把这些部分和相加。用于在正负系数间择优选表示。
std::size_t magnitudeCost(uint32_t magnitude) {
  const unsigned pieces = populationCount(magnitude);
  if (pieces == 0) {
    return 0;
  }
  const std::size_t shifts = pieces - ((magnitude & 1U) != 0 ? 1U : 0U);
  return shifts + pieces - 1;
}

// 值是否落在 12 位有符号立即数范围（ADDIW 等能直接编码的范围）。
bool fitsSigned12(int64_t value) { return value >= -2048 && value <= 2047; }

// 估算加载一个 32 位立即数需要几条指令：能放入 12 位或低 12 位为零（可用一条 LUI
// 类指令）则 1 条，否则 2 条。
std::size_t loadImmediateCost(int64_t immediate) {
  const int64_t value = static_cast<int64_t>(
      static_cast<int32_t>(static_cast<uint32_t>(immediate)));
  if (fitsSigned12(value) || (static_cast<uint32_t>(value) & 0xfffU) == 0) {
    return 1;
  }
  return 2;
}

// 一条指令的代价：LI 按其立即数可能需要 1 或 2 条真实指令，其余按 1 计。
std::size_t instructionCost(const MachineInstr &instr) {
  if (instr.opcode == Opcode::LI && instr.uses.size() == 1 &&
      instr.uses[0].kind == OperandKind::Imm) {
    return loadImmediateCost(instr.uses[0].immValue);
  }
  return 1;
}

// 合成计划：重构出的指令序列，以及推进后的下一个可用虚拟寄存器号。
struct SynthesisPlan {
  std::vector<MachineInstr> instrs;
  uint32_t nextVReg = 0;
};

// 把一个仿射形式重新合成为一段等价的机器指令序列。每个系数项用“移位构造量级 +
// 加/减累加”实现（按正负两种量级表示择代价小者），再加上常量项，最后把结果写到
// destination_。可能复用最后一条指令的目标寄存器以省一条拷贝，否则用 ADDIW 0 做
// word 规范化收尾。生成指令超过上限则放弃（返回 nullopt）。
class AffineSynthesizer {
public:
  AffineSynthesizer(uint32_t nextVReg, MachineReg destination,
                    std::string comment)
      : nextVReg_(nextVReg), destination_(destination),
        comment_(std::move(comment)) {}

  // 由仿射形式构造指令序列。为每个系数项选择正/负量级中代价更小的表示并构造其
  // 乘积寄存器，然后把正项相加、减去负项（全负时以 x0 起头做减法），再并入常量项。
  std::optional<SynthesisPlan> build(const AffineForm &form) {
    struct SignedTerm {
      MachineReg value;
      bool negative = false;
    };

    std::vector<SignedTerm> terms;
    for (const auto &[id, coefficient] : form.coefficients) {
      const uint32_t negativeMagnitude = uint32_t{0} - coefficient;
      const bool useNegative =
          magnitudeCost(negativeMagnitude) < magnitudeCost(coefficient);
      const uint32_t magnitude = useNegative ? negativeMagnitude : coefficient;
      auto value =
          buildMagnitude(MachineReg::vreg(id, RegClass::GPR32), magnitude);
      if (!value) {
        return std::nullopt;
      }
      terms.push_back({*value, useNegative});
    }

    if (terms.empty()) {
      emit(makeLoadImmediate(destination_, signedConstant(form.constant)));
      finishComment();
      return finish();
    }

    std::vector<MachineReg> positive;
    std::vector<MachineReg> negative;
    for (const SignedTerm &term : terms) {
      (term.negative ? negative : positive).push_back(term.value);
    }

    MachineReg accumulator;
    if (!positive.empty()) {
      accumulator = positive.front();
      for (std::size_t i = 1; i < positive.size(); ++i) {
        MachineReg next = temp();
        emit(makeBinary(Opcode::ADDW, next, accumulator, positive[i]));
        accumulator = next;
      }
      for (MachineReg value : negative) {
        MachineReg next = temp();
        emit(makeBinary(Opcode::SUBW, next, accumulator, value));
        accumulator = next;
      }
    } else {
      const MachineReg zero = MachineReg::preg(PhysReg::X0, RegClass::GPR32);
      accumulator = temp();
      emit(makeBinary(Opcode::SUBW, accumulator, zero, negative.front()));
      for (std::size_t i = 1; i < negative.size(); ++i) {
        MachineReg next = temp();
        emit(makeBinary(Opcode::SUBW, next, accumulator, negative[i]));
        accumulator = next;
      }
    }

    if (form.constant != 0) {
      const int64_t constant = signedConstant(form.constant);
      MachineReg next = temp();
      if (fitsSigned12(constant)) {
        emit(makeUnaryImm(Opcode::ADDIW, next, accumulator, constant));
      } else {
        MachineReg constantReg = temp();
        emit(makeLoadImmediate(constantReg, constant));
        emit(makeBinary(Opcode::ADDW, next, accumulator, constantReg));
      }
      accumulator = next;
    }

    if (isTemporary(accumulator)) {
      // 最终结果落在某个临时寄存器上：把它的定义直接改名为目标寄存器，省去一条
      // 多余的拷贝。找不到可改名的定义则失败。
      bool renamed = false;
      for (auto it = instrs_.rbegin(); it != instrs_.rend(); ++it) {
        if (it->defs.size() == 1 && it->defs[0].kind == OperandKind::Reg &&
            sameRegister(it->defs[0].regValue, accumulator)) {
          it->defs[0].regValue = destination_;
          renamed = true;
          break;
        }
      }
      if (!renamed) {
        return std::nullopt;
      }
    } else {
      // Word operations canonicalize their result by sign-extending bit 31.
      emit(makeUnaryImm(Opcode::ADDIW, destination_, accumulator, 0));
    }

    finishComment();
    return finish();
  }

private:
  // 把按 uint32 保存的常量按有符号 32 位解释（word 语义）。
  static int64_t signedConstant(uint32_t value) {
    return static_cast<int64_t>(static_cast<int32_t>(value));
  }

  // 分配一个新的临时虚拟寄存器并记录它是本次合成产生的临时值。
  MachineReg temp() {
    MachineReg reg = MachineReg::vreg(nextVReg_++, RegClass::GPR32);
    temporaries_.insert(reg.id);
    return reg;
  }

  // 该寄存器是否是本次合成新造的临时值（可安全改名为最终目标以省一条收尾指令）。
  bool isTemporary(const MachineReg &reg) const {
    return !reg.physical && temporaries_.count(reg.id) != 0;
  }

  // 追加一条生成指令。
  void emit(MachineInstr instr) { instrs_.push_back(std::move(instr)); }

  // 用“移位 + 加法”构造 source * magnitude 的寄存器：对 magnitude 的每个置位，bit0
  // 直接用 source、其余用 SLLIW 左移得到，再把这些部分和依次相加。magnitude 为 0
  // 无法构造（返回 nullopt，调用方会另行处理零项）。
  std::optional<MachineReg> buildMagnitude(MachineReg source,
                                           uint32_t magnitude) {
    if (magnitude == 0) {
      return std::nullopt;
    }
    std::vector<MachineReg> pieces;
    for (unsigned bit = 0; bit < 32; ++bit) {
      if ((magnitude & (uint32_t{1} << bit)) == 0) {
        continue;
      }
      if (bit == 0) {
        pieces.push_back(source);
      } else {
        MachineReg shifted = temp();
        emit(makeUnaryImm(Opcode::SLLIW, shifted, source, bit));
        pieces.push_back(shifted);
      }
    }
    MachineReg result = pieces.front();
    for (std::size_t i = 1; i < pieces.size(); ++i) {
      MachineReg next = temp();
      emit(makeBinary(Opcode::ADDW, next, result, pieces[i]));
      result = next;
    }
    return result;
  }

  // 把原根指令的注释挂到生成序列的最后一条指令上，保留可读信息。
  void finishComment() {
    if (!instrs_.empty()) {
      instrs_.back().comment = comment_;
    }
  }

  // 收尾并返回合成计划：为空或超过生成指令上限则视为失败。
  std::optional<SynthesisPlan> finish() {
    if (instrs_.empty() || instrs_.size() > kMaxGeneratedInstrs) {
      return std::nullopt;
    }
    return SynthesisPlan{std::move(instrs_), nextVReg_};
  }

  uint32_t nextVReg_;
  MachineReg destination_;
  std::string comment_;
  std::set<uint32_t> temporaries_;
  std::vector<MachineInstr> instrs_;
};

// 死链投影结果：删除后的引用计数、变为死值的结点集合、被删指令的总代价。
struct DeadProjection {
  UseCountTable counts;
  std::set<uint32_t> deadNodes;
  std::size_t instructionCost = 0;
};

// 死链投影：假设把 root 的定义链删除，模拟递减各 use 的引用计数，找出连带变为
// 死值（且属于本次展开吸收的结点）的寄存器集合，并累计这些将被删除指令的总代价。
// 返回的 counts 是删除后的引用计数快照。用于与合成计划代价比较，判断是否划算。
DeadProjection projectDeadChain(const MachineFunction &mf,
                                const DefinitionTable &definitions,
                                const UseCountTable &useCounts,
                                const std::set<uint32_t> &expandedNodes,
                                uint32_t root) {
  DeadProjection projection;
  projection.counts = useCounts;
  projection.deadNodes.insert(root);
  std::vector<uint32_t> worklist{root};

  while (!worklist.empty()) {
    const uint32_t id = worklist.back();
    worklist.pop_back();
    auto definition = definitions.find(id);
    if (definition == definitions.end() || definition->second.count != 1) {
      continue;
    }
    const MachineInstr &instr = mf.blocks[definition->second.blockIndex]
                                    .instrs[definition->second.instrIndex];
    projection.instructionCost += instructionCost(instr);
    for (const MachineOperand &use : instr.uses) {
      if (!isVirtualRegister(use)) {
        continue;
      }
      auto count = projection.counts.find(use.regValue.id);
      if (count == projection.counts.end() || count->second == 0) {
        continue;
      }
      --count->second;
      if (count->second == 0 && expandedNodes.count(use.regValue.id) != 0 &&
          projection.deadNodes.insert(use.regValue.id).second) {
        worklist.push_back(use.regValue.id);
      }
    }
  }
  return projection;
}

// 合成计划的总指令代价。
std::size_t planCost(const SynthesisPlan &plan) {
  std::size_t cost = 0;
  for (const MachineInstr &instr : plan.instrs) {
    cost += instructionCost(instr);
  }
  return cost;
}

// 把合成计划里新指令的寄存器使用累加进引用计数表（替换后维护 use 计数一致）。
void addPlanUses(const SynthesisPlan &plan, UseCountTable &counts) {
  for (const MachineInstr &instr : plan.instrs) {
    for (const MachineOperand &use : instr.uses) {
      if (isVirtualRegister(use)) {
        ++counts[use.regValue.id];
      }
    }
  }
}

// 一组“有符号数除以 2 的幂”指令的匹配结果：起始下标、移位量，以及序列中被除数、
// 取符号值、偏置、加偏置后的中间值、最终商各自的寄存器。
struct SignedPow2DivisionGroup {
  std::size_t begin = 0;
  unsigned shift = 0;
  MachineReg dividend;
  MachineReg sign;
  MachineReg bias;
  MachineReg adjusted;
  MachineReg quotient;
};

// 从 block 的 begin 处尝试匹配指令选择为“有符号除以 2^shift”生成的规范四指令序列：
//   sign     = SRAIW dividend, 31        （全 0 或全 1 的符号掩码）
//   bias     = SRLIW sign, 32-shift       （下移得到 0 或 2^shift-1 的偏置）
//   adjusted = ADDW  dividend, bias        （向零截断的偏置修正）
//   quotient = SRAIW adjusted, shift       （右移得到商）
// 并要求各中间寄存器 def/use 计数与串联关系严格吻合，否则返回 nullopt。
std::optional<SignedPow2DivisionGroup>
matchSignedPow2DivisionGroup(const MachineBasicBlock &block,
                            std::size_t begin,
                            const DefinitionTable &definitions,
                            const UseCountTable &uses) {
  if (begin + 4 > block.instrs.size()) {
    return std::nullopt;
  }

  const MachineInstr &sign = block.instrs[begin];
  const MachineInstr &bias = block.instrs[begin + 1];
  const MachineInstr &adjusted = block.instrs[begin + 2];
  const MachineInstr &quotient = block.instrs[begin + 3];
  if (sign.opcode != Opcode::SRAIW || bias.opcode != Opcode::SRLIW ||
      adjusted.opcode != Opcode::ADDW || quotient.opcode != Opcode::SRAIW ||
      !hasPureGpr32Def(sign) || !hasPureGpr32Def(bias) ||
      !hasPureGpr32Def(adjusted) || !hasPureGpr32Def(quotient) ||
      sign.uses.size() != 2 || bias.uses.size() != 2 ||
      adjusted.uses.size() != 2 || quotient.uses.size() != 2 ||
      !isRegOperand(sign, 0) || !isImmOperand(sign, 1) ||
      !isRegOperand(bias, 0) || !isImmOperand(bias, 1) ||
      !isRegOperand(adjusted, 0) || !isRegOperand(adjusted, 1) ||
      !isRegOperand(quotient, 0) || !isImmOperand(quotient, 1) ||
      sign.uses[0].regValue.rc != RegClass::GPR32 ||
      sign.uses[1].immValue != 31) {
    return std::nullopt;
  }

  const int64_t shift = quotient.uses[1].immValue;
  if (shift <= 0 || shift >= 32 ||
      bias.uses[1].immValue != 32 - shift) {
    return std::nullopt;
  }

  SignedPow2DivisionGroup group;
  group.begin = begin;
  group.shift = static_cast<unsigned>(shift);
  group.dividend = sign.uses[0].regValue;
  group.sign = sign.defs.front().regValue;
  group.bias = bias.defs.front().regValue;
  group.adjusted = adjusted.defs.front().regValue;
  group.quotient = quotient.defs.front().regValue;

  if (!sameRegister(bias.uses[0].regValue, group.sign) ||
      !sameRegister(adjusted.uses[0].regValue, group.dividend) ||
      !sameRegister(adjusted.uses[1].regValue, group.bias) ||
      !sameRegister(quotient.uses[0].regValue, group.adjusted)) {
    return std::nullopt;
  }

  auto hasUniqueDefinition = [&](const MachineReg &reg) {
    const auto found = definitions.find(reg.id);
    return !reg.physical && found != definitions.end() &&
           found->second.count == 1;
  };
  auto hasUseCount = [&](const MachineReg &reg, std::size_t count) {
    const auto found = uses.find(reg.id);
    return found != uses.end() && found->second == count;
  };
  if (!hasUniqueDefinition(group.sign) ||
      !hasUniqueDefinition(group.bias) ||
      !hasUniqueDefinition(group.adjusted) ||
      !hasUniqueDefinition(group.quotient) ||
      !hasUseCount(group.sign, 1) || !hasUseCount(group.bias, 1) ||
      !hasUseCount(group.adjusted, 1)) {
    return std::nullopt;
  }
  return group;
}

// 把链式的连续“有符号除以 2 的幂”合并为一次。数学前提：向零截断的整数除法对正
// 除数可传递，trunc(trunc(x/2^a)/2^b) 等于 trunc(x/2^(a+b))，故可复用同一段四指令
// 序列而把移位量相加。 guard：总移位量必须小于 32，以保证每个 word 移位立即数良定义；
// 中间商必须恰好被使用 2 次（一次给偏置、一次给下一段除法），否则不能安全合并。
// 每合并一处即从头重算 def/use 表并重试，直到不再有可合并的链。
bool foldChainedSignedPow2Divisions(MachineFunction &mf) {
  if (std::getenv("BACKEND_ENABLE_SIGNED_DIV_FOLD") == nullptr) {
    return false;
  }

  bool changed = false;
  for (;;) {
    const DefinitionTable definitions = collectDefinitions(mf);
    const UseCountTable uses = collectUseCounts(mf);
    bool foldedOne = false;

    for (MachineBasicBlock &block : mf.blocks) {
      for (std::size_t begin = 0; begin < block.instrs.size(); ++begin) {
        auto first = matchSignedPow2DivisionGroup(block, begin, definitions,
                                                  uses);
        if (!first) {
          continue;
        }

        std::vector<SignedPow2DivisionGroup> chain{*first};
        unsigned totalShift = first->shift;
        for (std::size_t nextBegin = begin + 4;; nextBegin += 4) {
          auto next = matchSignedPow2DivisionGroup(block, nextBegin,
                                                   definitions, uses);
          if (!next || !sameRegister(next->dividend,
                                     chain.back().quotient) ||
              totalShift + next->shift > 31) {
            break;
          }

          const auto quotientUses = uses.find(chain.back().quotient.id);
          if (quotientUses == uses.end() || quotientUses->second != 2) {
            break;
          }
          totalShift += next->shift;
          chain.push_back(*next);
        }

        if (chain.size() < 2) {
          continue;
        }

        // Integer division truncating toward zero is transitive for positive
        // divisors: trunc(trunc(x / 2^a) / 2^b) equals
        // trunc(x / 2^(a+b)).  The canonical four-instruction sequence is
        // therefore reusable with the summed shift.  Keeping the total below
        // 32 also keeps every RISC-V word shift immediate well-defined.
        MachineInstr &firstBias = block.instrs[begin + 1];
        MachineInstr &firstQuotient = block.instrs[begin + 3];
        firstBias.uses[1] = MachineOperand::imm(32 - totalShift);
        firstQuotient.defs[0] =
            MachineOperand::reg(chain.back().quotient);
        firstQuotient.uses[1] = MachineOperand::imm(totalShift);
        if (!block.instrs[chain.back().begin + 3].comment.empty()) {
          firstQuotient.comment =
              block.instrs[chain.back().begin + 3].comment;
        }
        block.instrs.erase(
            block.instrs.begin() + static_cast<std::ptrdiff_t>(begin + 4),
            block.instrs.begin() + static_cast<std::ptrdiff_t>(begin +
                                                               4 * chain.size()));
        changed = true;
        foldedOne = true;
        break;
      }
      if (foldedOne) {
        break;
      }
    }

    if (!foldedOne) {
      return changed;
    }
  }
}

// 折叠“有符号数除以 2”的偏置计算。对 i32，(x >>s 31) & 1 恰好等于 x 的符号位，
// 可直接用一条 SRLIW（x >>u 31）读取，消去中间那个全 0 或全 1 的取符号值。 guard：
// 这只对宽度为 1 位的掩码成立，不能推广到更宽的掩码——例如正数 0x40000003 的
// x >>u 30 非零，而有符号偏置本应为 0。要求相关寄存器唯一定义、符号值仅被用一次。
bool foldSignedDivisionByTwoBias(MachineFunction &mf) {
  const DefinitionTable definitions = collectDefinitions(mf);
  const UseCountTable uses = collectUseCounts(mf);
  bool changed = false;

  for (MachineBasicBlock &block : mf.blocks) {
    for (std::size_t index = 0; index + 1 < block.instrs.size();) {
      const MachineInstr &sign = block.instrs[index];
      const MachineInstr &mask = block.instrs[index + 1];
      if (sign.opcode != Opcode::SRAIW || mask.opcode != Opcode::ANDI ||
          !hasPureGpr32Def(sign) || !hasPureGpr32Def(mask) ||
          sign.uses.size() != 2 || mask.uses.size() != 2 ||
          !isRegOperand(sign, 0) || !isImmOperand(sign, 1) ||
          !isRegOperand(mask, 0) || !isImmOperand(mask, 1) ||
          sign.uses[0].regValue.rc != RegClass::GPR32 ||
          sign.uses[1].immValue != 31 || mask.uses[1].immValue != 1) {
        ++index;
        continue;
      }

      const MachineReg signReg = sign.defs.front().regValue;
      if (!sameRegister(mask.uses[0].regValue, signReg)) {
        ++index;
        continue;
      }

      const auto definition = definitions.find(signReg.id);
      const auto use = uses.find(signReg.id);
      const MachineReg resultReg = mask.defs.front().regValue;
      const auto resultDefinition = definitions.find(resultReg.id);
      if (definition == definitions.end() || definition->second.count != 1 ||
          use == uses.end() || use->second != 1 ||
          resultDefinition == definitions.end() ||
          resultDefinition->second.count != 1) {
        ++index;
        continue;
      }

      // For i32 division by two, `(x >>s 31) & 1` is exactly the sign bit.
      // Reading it directly with SRLIW removes the intermediate all-zero/all-one
      // sign value.  Do not generalize this to masks wider than one bit: for a
      // positive x such as 0x40000003, `x >>u 30` is non-zero while the signed
      // bias must remain zero.
      MachineInstr folded = mask;
      folded.opcode = Opcode::SRLIW;
      folded.uses[0] = sign.uses[0];
      folded.uses[1] = sign.uses[1];
      if (folded.comment.empty()) {
        folded.comment = sign.comment;
      }
      block.instrs[index] = std::move(folded);
      block.instrs.erase(block.instrs.begin() +
                         static_cast<std::ptrdiff_t>(index + 1));
      changed = true;
      ++index;
    }
  }

  return changed;
}

// 消去窄化后有符号除法偏置里多余的取符号右移。设 q 是某次算术右移至少 k 位的
// 结果：非负 q 的高 k 位全 0，负 q 的高 k 位全 1，因此对 correctionBits=k 有
//   ((q sraiw 31) srliw (32-k)) == (q srliw (32-k))，
// 即偏置可直接以 q 为输入，跳过中间的取符号右移。 guard（范围证明必要）：必须存在
// 前置的算术右移把 q 窄化到至少 k 位，否则一个大正数可能高位非零使等式不成立。
bool foldNarrowSignedDivisionBias(MachineFunction &mf) {
  if (std::getenv("BACKEND_ENABLE_SIGNED_DIV_FOLD") == nullptr) {
    return false;
  }
  const DefinitionTable definitions = collectDefinitions(mf);
  bool changed = false;

  auto uniqueDefinition = [&](const MachineReg &reg)
      -> const MachineInstr * {
    if (reg.physical) {
      return nullptr;
    }
    const auto found = definitions.find(reg.id);
    if (found == definitions.end() || found->second.count != 1) {
      return nullptr;
    }
    return &mf.blocks[found->second.blockIndex]
                .instrs[found->second.instrIndex];
  };

  for (MachineBasicBlock &block : mf.blocks) {
    for (MachineInstr &bias : block.instrs) {
      if (bias.opcode != Opcode::SRLIW || !hasPureGpr32Def(bias) ||
          bias.uses.size() != 2 || !isRegOperand(bias, 0) ||
          !isImmOperand(bias, 1)) {
        continue;
      }

      const int64_t biasShift = bias.uses[1].immValue;
      if (biasShift <= 0 || biasShift >= 32) {
        continue;
      }
      const int64_t correctionBits = 32 - biasShift;

      const MachineInstr *sign = uniqueDefinition(bias.uses[0].regValue);
      if (sign == nullptr || sign->opcode != Opcode::SRAIW ||
          !hasPureGpr32Def(*sign) || sign->uses.size() != 2 ||
          !isRegOperand(*sign, 0) || !isImmOperand(*sign, 1) ||
          sign->uses[1].immValue != 31) {
        continue;
      }

      const MachineReg narrowed = sign->uses[0].regValue;
      const MachineInstr *narrowing = uniqueDefinition(narrowed);
      if (narrowing == nullptr || narrowing->opcode != Opcode::SRAIW ||
          !hasPureGpr32Def(*narrowing) || narrowing->uses.size() != 2 ||
          !isRegOperand(*narrowing, 0) || !isImmOperand(*narrowing, 1) ||
          narrowing->uses[1].immValue < correctionBits ||
          narrowing->uses[1].immValue >= 32) {
        continue;
      }

      // Let q be the result of SRAIW by at least k bits.  A non-negative q
      // cannot have any of its top k word bits set, while a negative q has all
      // of them set.  Therefore, for correctionBits=k,
      //
      //   ((q sraiw 31) srliw (32-k)) == (q srliw (32-k)).
      //
      // This is the signed power-of-two division bias used by instruction
      // selection.  The range proof is essential: without the preceding word
      // shift, a large positive value could have non-zero top bits.
      bias.uses[0] = MachineOperand::reg(narrowed);
      changed = true;
    }
  }

  return changed;
}

// 本 pass 只改指令、不动块结构，故保留 CFG/循环/块频率这几类块结构分析结果。
MachinePreservationStatus preserveBlockStructureAnalyses() {
  auto status = MachinePreservationStatus::none();
  status.keep<MachineCFGAnalysis>();
  status.keep<MachineLoopAnalysis>();
  status.keep<MachineBlockFrequencyAnalysis>();
  return status;
}

} // namespace

// pass 主入口：先跑三种有符号 2 的幂除法 bias 折叠，再做仿射合成。仿射合成对每个
// 块从后往前扫描每条可展开的根指令，把其计算链折叠成仿射形式并重新合成指令序列，
// 用死链投影估算删除旧链能省的代价，仅当合成代价严格更低时才采纳并记录替换，最后
// 一次性把块内被替换指令换成新序列。返回是否修改了函数。
bool runPreRAAffineCombine(MachineFunction &mf) {
  bool changed = false;
  // 除法链折叠匹配的是指令选择自身生成的规范序列，默认关闭以保持保守；
  // 需要时可设置环境变量 BACKEND_ENABLE_SIGNED_DIV_FOLD 打开。
  if (std::getenv("BACKEND_ENABLE_SIGNED_DIV_FOLD") != nullptr) {
    changed = foldChainedSignedPow2Divisions(mf);
    changed = foldSignedDivisionByTwoBias(mf) || changed;
    changed = foldNarrowSignedDivisionBias(mf) || changed;
  }
  const DefinitionTable definitions = collectDefinitions(mf);
  UseCountTable useCounts = collectUseCounts(mf);

  for (std::size_t blockIndex = 0; blockIndex < mf.blocks.size();
       ++blockIndex) {
    MachineBasicBlock &block = mf.blocks[blockIndex];
    std::map<std::size_t, std::vector<MachineInstr>> replacements;

    for (std::size_t index = block.instrs.size(); index-- > 0;) {
      const MachineInstr &rootInstr = block.instrs[index];
      if (!isExpandableAffineInstr(rootInstr)) {
        continue;
      }
      const MachineReg rootReg = rootInstr.defs.front().regValue;
      auto rootUses = useCounts.find(rootReg.id);
      if (rootUses == useCounts.end() || rootUses->second == 0) {
        continue;
      }

      AffineFormBuilder builder(mf, definitions, blockIndex);
      auto form = builder.buildRoot(index);
      if (!form) {
        continue;
      }
      AffineSynthesizer synthesizer(mf.nextVReg, rootReg, rootInstr.comment);
      auto plan = synthesizer.build(*form);
      if (!plan) {
        continue;
      }

      DeadProjection projection = projectDeadChain(
          mf, definitions, useCounts, form->expandedNodes, rootReg.id);
      if (planCost(*plan) >= projection.instructionCost) {
        continue;
      }

      useCounts = std::move(projection.counts);
      addPlanUses(*plan, useCounts);
      mf.nextVReg = plan->nextVReg;
      replacements.emplace(index, std::move(plan->instrs));
      changed = true;
    }

    if (replacements.empty()) {
      continue;
    }
    std::vector<MachineInstr> rewritten;
    rewritten.reserve(block.instrs.size());
    for (std::size_t index = 0; index < block.instrs.size(); ++index) {
      auto replacement = replacements.find(index);
      if (replacement == replacements.end()) {
        rewritten.push_back(std::move(block.instrs[index]));
        continue;
      }
      for (MachineInstr &instr : replacement->second) {
        rewritten.push_back(std::move(instr));
      }
    }
    block.instrs = std::move(rewritten);
  }

  return changed;
}

// pass 对象入口：运行 runPreRAAffineCombine，改动则保留块结构分析，未改动则保留
// 全部分析。
MachinePreservationStatus
PreRAAffineCombinePass::run(MachineFunction &mf,
                            MachineFunctionAnalysisManager &am) {
  (void)am;
  return runPreRAAffineCombine(mf) ? preserveBlockStructureAnalyses()
                                   : MachinePreservationStatus::all();
}

} // namespace riscv
