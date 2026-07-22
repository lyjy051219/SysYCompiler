#include "aarch64/opt/CodegenPasses.hpp"
#include "aarch64/opt/MachineAnalysis.hpp"
#include "aarch64/opt/MachineOptPasses.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace aarch64 {
namespace {

constexpr std::size_t kMaxAffineNodes = 64;
constexpr std::size_t kMaxAffineTerms = 8;
constexpr std::size_t kMaxGeneratedInstrs = 16;

struct DefinitionInfo {
  std::size_t count = 0;
  std::size_t blockIndex = 0;
  std::size_t instrIndex = 0;
};

using DefinitionTable = std::map<uint32_t, DefinitionInfo>;
using UseCountTable = std::map<uint32_t, std::size_t>;

bool sameRegister(const MachineReg &lhs, const MachineReg &rhs) {
  return lhs.physical == rhs.physical && lhs.id == rhs.id && lhs.rc == rhs.rc &&
         lhs.phys == rhs.phys;
}

bool isVirtualGpr32(const MachineOperand &operand) {
  return operand.kind == OperandKind::Reg && !operand.regValue.physical &&
         operand.regValue.rc == RegClass::GPR32;
}

bool isVirtualRegister(const MachineOperand &operand) {
  return operand.kind == OperandKind::Reg && !operand.regValue.physical;
}

bool isRegOperand(const MachineInstr &instr, std::size_t index) {
  return index < instr.uses.size() &&
         instr.uses[index].kind == OperandKind::Reg;
}

bool isImmOperand(const MachineInstr &instr, std::size_t index) {
  return index < instr.uses.size() &&
         instr.uses[index].kind == OperandKind::Imm;
}

bool hasPureGpr32Def(const MachineInstr &instr) {
  return !instr.sideEffects && !instr.terminator &&
         instr.implicitDefs.empty() && instr.implicitUses.empty() &&
         instr.defs.size() == 1 && isVirtualGpr32(instr.defs.front());
}

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

struct AffineForm {
  uint32_t constant = 0;
  std::map<uint32_t, uint32_t> coefficients;
  std::set<uint32_t> expandedNodes;
};

void removeZeroCoefficients(AffineForm &form) {
  for (auto it = form.coefficients.begin(); it != form.coefficients.end();) {
    if (it->second == 0) {
      it = form.coefficients.erase(it);
    } else {
      ++it;
    }
  }
}

class AffineFormBuilder {
public:
  AffineFormBuilder(const MachineFunction &mf,
                    const DefinitionTable &definitions, std::size_t blockIndex)
      : mf_(mf), definitions_(definitions), blockIndex_(blockIndex) {}

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

  bool withinLimits(const AffineForm &form) const {
    return form.coefficients.size() <= kMaxAffineTerms &&
           form.expandedNodes.size() <= kMaxAffineNodes;
  }

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

unsigned populationCount(uint32_t value) {
  unsigned count = 0;
  while (value != 0) {
    value &= value - 1;
    ++count;
  }
  return count;
}

std::size_t magnitudeCost(uint32_t magnitude) {
  const unsigned pieces = populationCount(magnitude);
  if (pieces == 0) {
    return 0;
  }
  const std::size_t shifts = pieces - ((magnitude & 1U) != 0 ? 1U : 0U);
  return shifts + pieces - 1;
}

bool fitsSigned12(int64_t value) { return value >= -2048 && value <= 2047; }

std::size_t loadImmediateCost(int64_t immediate) {
  const int64_t value = static_cast<int64_t>(
      static_cast<int32_t>(static_cast<uint32_t>(immediate)));
  if (fitsSigned12(value) || (static_cast<uint32_t>(value) & 0xfffU) == 0) {
    return 1;
  }
  return 2;
}

std::size_t instructionCost(const MachineInstr &instr) {
  if (instr.opcode == Opcode::LI && instr.uses.size() == 1 &&
      instr.uses[0].kind == OperandKind::Imm) {
    return loadImmediateCost(instr.uses[0].immValue);
  }
  return 1;
}

struct SynthesisPlan {
  std::vector<MachineInstr> instrs;
  uint32_t nextVReg = 0;
};

class AffineSynthesizer {
public:
  AffineSynthesizer(uint32_t nextVReg, MachineReg destination,
                    std::string comment)
      : nextVReg_(nextVReg), destination_(destination),
        comment_(std::move(comment)) {}

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
  static int64_t signedConstant(uint32_t value) {
    return static_cast<int64_t>(static_cast<int32_t>(value));
  }

  MachineReg temp() {
    MachineReg reg = MachineReg::vreg(nextVReg_++, RegClass::GPR32);
    temporaries_.insert(reg.id);
    return reg;
  }

  bool isTemporary(const MachineReg &reg) const {
    return !reg.physical && temporaries_.count(reg.id) != 0;
  }

  void emit(MachineInstr instr) { instrs_.push_back(std::move(instr)); }

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

  void finishComment() {
    if (!instrs_.empty()) {
      instrs_.back().comment = comment_;
    }
  }

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

struct DeadProjection {
  UseCountTable counts;
  std::set<uint32_t> deadNodes;
  std::size_t instructionCost = 0;
};

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

std::size_t planCost(const SynthesisPlan &plan) {
  std::size_t cost = 0;
  for (const MachineInstr &instr : plan.instrs) {
    cost += instructionCost(instr);
  }
  return cost;
}

void addPlanUses(const SynthesisPlan &plan, UseCountTable &counts) {
  for (const MachineInstr &instr : plan.instrs) {
    for (const MachineOperand &use : instr.uses) {
      if (isVirtualRegister(use)) {
        ++counts[use.regValue.id];
      }
    }
  }
}

struct SignedPow2DivisionGroup {
  std::size_t begin = 0;
  unsigned shift = 0;
  MachineReg dividend;
  MachineReg sign;
  MachineReg bias;
  MachineReg adjusted;
  MachineReg quotient;
};

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

bool foldChainedSignedPow2Divisions(MachineFunction &mf) {
  if (std::getenv("BACKEND_DISABLE_CHAINED_SIGNED_DIV_FOLD") != nullptr) {
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

bool foldNarrowSignedDivisionBias(MachineFunction &mf) {
  if (std::getenv("BACKEND_DISABLE_NARROW_SIGNED_DIV_BIAS_FOLD") != nullptr) {
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

MachinePreservationStatus preserveBlockStructureAnalyses() {
  auto status = MachinePreservationStatus::none();
  status.keep<MachineCFGAnalysis>();
  status.keep<MachineLoopAnalysis>();
  status.keep<MachineBlockFrequencyAnalysis>();
  return status;
}

} // namespace

bool runPreRAAffineCombine(MachineFunction &mf) {
  bool changed = foldChainedSignedPow2Divisions(mf);
  changed = foldSignedDivisionByTwoBias(mf) || changed;
  changed = foldNarrowSignedDivisionBias(mf) || changed;
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

MachinePreservationStatus
PreRAAffineCombinePass::run(MachineFunction &mf,
                            MachineFunctionAnalysisManager &am) {
  (void)am;
  return runPreRAAffineCombine(mf) ? preserveBlockStructureAnalyses()
                                   : MachinePreservationStatus::all();
}

} // namespace aarch64
