// VectorLegalize.cpp - 通用向量 MIR 的标量化 legalizer。
//
// 在官方 SIMD 指令文档发布之前，RV64GC 没有可用的原生向量指令。此实现把
// ISel 生成的 G_V* 语义伪指令逐 lane 展开为标量 RV64GC MIR：
//   - 向量寄存器在寄存器分配之前消失，不再需要物理向量寄存器；
//   - 向量 load/store 按元素宽度拆成 LW/FLW 与 SW/FSW；
//   - 向量算术/比较/select/shuffle/extract/insert/phi 全部按 lane 处理；
//   - 动态 lane 索引用“索引 == 常量 lane”条件 + SELECT 实现，不引入额外栈槽。
//
// 将来拿到官方 SIMD 指令文档后，保留本文件作为正确性 fallback，并在同一接口
// 处按 MachineVectorType 选择原生 SIMD 路径即可。
#include "riscv/VectorLegalize.hpp"

#include "lib/CFG.hpp"
#include "riscv/Target.hpp"

#include <cstdint>
#include <initializer_list>
#include <unordered_map>
#include <utility>
#include <vector>

namespace riscv {
namespace {

RegClass scalarRegClassFor(MachineVectorElementKind element) {
  return element == MachineVectorElementKind::F32 ? RegClass::FPR32
                                                  : RegClass::GPR32;
}

Opcode scalarCopyOpcode(MachineVectorElementKind element) {
  return element == MachineVectorElementKind::F32 ? Opcode::F_COPY
                                                  : Opcode::COPY;
}

Opcode scalarLoadOpcode(MachineVectorElementKind element) {
  if (element == MachineVectorElementKind::F32) {
    return Opcode::FLW;
  }
  if (element == MachineVectorElementKind::I32) {
    return Opcode::LW;
  }
  throw BackendError("cannot scalarize vector load with mask element type");
}

Opcode scalarStoreOpcode(MachineVectorElementKind element) {
  if (element == MachineVectorElementKind::F32) {
    return Opcode::FSW;
  }
  if (element == MachineVectorElementKind::I32) {
    return Opcode::SW;
  }
  throw BackendError("cannot scalarize vector store with mask element type");
}

void appendRegInstr(MachineBasicBlock &block, Opcode opcode, MachineReg def,
                    std::initializer_list<MachineReg> uses) {
  MachineInstr instr;
  instr.opcode = opcode;
  if (def.isValid()) {
    instr.defs.push_back(MachineOperand::reg(def));
  }
  for (MachineReg use : uses) {
    instr.uses.push_back(MachineOperand::reg(use));
  }
  block.instrs.push_back(std::move(instr));
}

void appendImmInstr(MachineBasicBlock &block, Opcode opcode, MachineReg def,
                    MachineOperand base, int64_t imm) {
  MachineInstr instr;
  instr.opcode = opcode;
  if (def.isValid()) {
    instr.defs.push_back(MachineOperand::reg(def));
  }
  instr.uses.push_back(std::move(base));
  instr.uses.push_back(MachineOperand::imm(imm));
  block.instrs.push_back(std::move(instr));
}

void appendLoadInstr(MachineBasicBlock &block, Opcode opcode, MachineReg def,
                     MachineOperand base, int64_t offset) {
  MachineInstr instr;
  instr.opcode = opcode;
  instr.defs.push_back(MachineOperand::reg(def));
  instr.uses.push_back(std::move(base));
  instr.uses.push_back(MachineOperand::imm(offset));
  block.instrs.push_back(std::move(instr));
}

void appendStoreInstr(MachineBasicBlock &block, Opcode opcode, MachineReg src,
                      MachineOperand base, int64_t offset) {
  MachineInstr instr;
  instr.opcode = opcode;
  instr.uses.push_back(MachineOperand::reg(src));
  instr.uses.push_back(std::move(base));
  instr.uses.push_back(MachineOperand::imm(offset));
  instr.sideEffects = true;
  block.instrs.push_back(std::move(instr));
}

MachineReg materializeLaneConstant(MachineFunction &mf,
                                   MachineBasicBlock &block,
                                   MachineVectorElementKind element,
                                   uint32_t bits) {
  if (element == MachineVectorElementKind::F32) {
    MachineReg bitsReg = mf.createVReg(RegClass::GPR64);
    MachineInstr li;
    li.opcode = Opcode::LI;
    li.defs.push_back(MachineOperand::reg(bitsReg));
    li.uses.push_back(
        MachineOperand::imm(static_cast<int64_t>(bits)));
    block.instrs.push_back(std::move(li));

    MachineReg floatReg = mf.createVReg(RegClass::FPR32);
    appendRegInstr(block, Opcode::FMV_W_X, floatReg, {bitsReg});
    return floatReg;
  }

  if (bits == 0) {
    return MachineReg::preg(PhysReg::X0, RegClass::GPR32);
  }
  MachineReg reg = mf.createVReg(RegClass::GPR32);
  MachineInstr li;
  li.opcode = Opcode::LI;
  li.defs.push_back(MachineOperand::reg(reg));
  li.uses.push_back(MachineOperand::imm(static_cast<int64_t>(bits)));
  block.instrs.push_back(std::move(li));
  return reg;
}

// 比较 GPR32 index 与常量 lane，返回 GPR32 的 0/1 条件。
MachineReg emitIndexEqualsLane(MachineFunction &mf, MachineBasicBlock &block,
                               MachineReg indexReg, int64_t lane) {
  MachineReg diff = mf.createVReg(RegClass::GPR32);
  appendImmInstr(block, Opcode::XORI, diff, MachineOperand::reg(indexReg), lane);
  MachineReg cond = mf.createVReg(RegClass::GPR32);
  appendRegInstr(block, Opcode::SEQZ, cond, {diff});
  return cond;
}

MachineInstr makeSelect(MachineReg dst, MachineReg cond, MachineReg trueValue,
                        MachineReg falseValue) {
  MachineInstr select;
  select.opcode = Opcode::SELECT;
  select.defs.push_back(MachineOperand::reg(dst));
  select.uses.push_back(MachineOperand::reg(cond));
  select.uses.push_back(MachineOperand::reg(trueValue));
  select.uses.push_back(MachineOperand::reg(falseValue));
  return select;
}

class ScalarizingVectorLegalizer {
public:
  explicit ScalarizingVectorLegalizer(MachineFunction &function)
      : mf(function) {}

  void runOnBlock(MachineBasicBlock &block) {
    currentBlock_ = &block;
    legalizeBlock(block);
    currentBlock_ = nullptr;
  }

  void finish() {
    rebuildValueLaneMap();
    mf.vectorVRegTypes.clear();
  }

private:
  const MachineVectorType &typeOf(const MachineOperand &operand,
                                  const char *context) const {
    if (operand.kind != OperandKind::Reg) {
      throw BackendError(std::string(context) +
                         " requires a vector register operand");
    }
    const MachineVectorType *type = mf.getVectorType(operand.regValue);
    if (!type || !type->isValid()) {
      throw BackendError(std::string(context) +
                         " has an untyped vector register");
    }
    return *type;
  }

  RegClass scalarClass(const MachineVectorType &type) const {
    return scalarRegClassFor(type.element);
  }

  std::vector<MachineReg> newLaneRegs(const MachineVectorType &type) {
    std::vector<MachineReg> result;
    result.reserve(type.lanes);
    for (uint16_t lane = 0; lane < type.lanes; ++lane) {
      result.push_back(mf.createVReg(scalarClass(type)));
    }
    return result;
  }

  std::vector<MachineReg> &lanesForDef(const MachineOperand &def,
                                       const MachineVectorType &type) {
    if (def.kind != OperandKind::Reg ||
        def.regValue.rc != RegClass::GenericVector) {
      throw BackendError("vector definition must be a GenericVector register");
    }
    auto found = laneMap.find(def.regValue.id);
    if (found == laneMap.end()) {
      auto inserted = laneMap.emplace(def.regValue.id, newLaneRegs(type));
      return inserted.first->second;
    }
    if (found->second.size() != type.lanes) {
      throw BackendError("vector register lane map is inconsistent");
    }
    return found->second;
  }

  const std::vector<MachineReg> &lanesForUse(const MachineOperand &use) {
    if (use.kind != OperandKind::Reg ||
        use.regValue.rc != RegClass::GenericVector) {
      throw BackendError("vector use must be a GenericVector register");
    }
    auto found = laneMap.find(use.regValue.id);
    if (found == laneMap.end()) {
      throw BackendError("vector register used before it was scalarized");
    }
    return found->second;
  }

  void legalizeBlock(MachineBasicBlock &block) {
    // 标量化会 1:N 展开指令。先移到独立输出块，避免在遍历原指令向量的同时
    // 向其中追加指令，否则 vector 扩容会使当前 MachineInstr 引用悬空。
    MachineBasicBlock output;
    currentBlock_ = &output;
    output.instrs.reserve(block.instrs.size() * 2);

    for (MachineInstr &instr : block.instrs) {
      if (instr.opcode == Opcode::PHI) {
        if (!instr.defs.empty() && instr.defs[0].kind == OperandKind::Reg &&
            instr.defs[0].regValue.rc == RegClass::GenericVector) {
          legalizeVectorPhi(instr);
          continue;
        }
        output.instrs.push_back(std::move(instr));
        continue;
      }

      if (!isGenericVectorOpcode(instr.opcode)) {
        output.instrs.push_back(std::move(instr));
        continue;
      }

      legalizeVectorInstr(instr);
    }

    currentBlock_ = nullptr;
    block.instrs = std::move(output.instrs);
  }

  void legalizeVectorPhi(const MachineInstr &phi) {
    const MachineVectorType &type = typeOf(phi.defs[0], "vector PHI");
    std::vector<MachineReg> &dst = lanesForDef(phi.defs[0], type);

    for (uint16_t lane = 0; lane < type.lanes; ++lane) {
      MachineInstr scalarPhi;
      scalarPhi.opcode = Opcode::PHI;
      scalarPhi.defs.push_back(MachineOperand::reg(dst[lane]));
      for (std::size_t i = 0; i < phi.uses.size(); ++i) {
        MachineOperand use = phi.uses[i];
        if (use.kind == OperandKind::ValueRef) {
          use.valueLane = lane;
        }
        scalarPhi.uses.push_back(std::move(use));
      }
      currentBlock()->instrs.push_back(std::move(scalarPhi));
    }
  }

  void legalizeVectorInstr(const MachineInstr &instr) {
    switch (instr.opcode) {
    case Opcode::G_VUNDEF:
      legalizeVectorUndef(instr);
      return;
    case Opcode::G_VCONST:
      legalizeVectorConst(instr);
      return;
    case Opcode::G_VCOPY:
      legalizeVectorCopy(instr);
      return;
    case Opcode::G_VLOAD:
      legalizeVectorLoad(instr);
      return;
    case Opcode::G_VSTORE:
      legalizeVectorStore(instr);
      return;
    case Opcode::G_VADD:
    case Opcode::G_VSUB:
    case Opcode::G_VMUL:
    case Opcode::G_VDIV:
    case Opcode::G_VUDIV:
    case Opcode::G_VREM:
    case Opcode::G_VUREM:
    case Opcode::G_VAND:
    case Opcode::G_VOR:
    case Opcode::G_VXOR:
    case Opcode::G_VSHL:
    case Opcode::G_VLSHR:
    case Opcode::G_VASHR:
    case Opcode::G_VFADD:
    case Opcode::G_VFSUB:
    case Opcode::G_VFMUL:
    case Opcode::G_VFDIV:
      legalizeVectorBinary(instr);
      return;
    case Opcode::G_VFREM:
      throw BackendError(
          "vector frem requires a runtime/libcall; official SIMD lowering is "
          "not available");
    case Opcode::G_VICMP:
    case Opcode::G_VFCMP:
      legalizeVectorCompare(instr);
      return;
    case Opcode::G_VSELECT:
      legalizeVectorSelect(instr);
      return;
    case Opcode::G_VEXTRACT:
      legalizeVectorExtract(instr);
      return;
    case Opcode::G_VINSERT:
      legalizeVectorInsert(instr);
      return;
    case Opcode::G_VSHUFFLE:
      legalizeVectorShuffle(instr);
      return;
    case Opcode::G_VTRUNC:
    case Opcode::G_VZEXT:
    case Opcode::G_VSEXT:
      legalizeVectorIntCast(instr);
      return;
    case Opcode::G_VFPTOSI:
    case Opcode::G_VSITOFP:
    case Opcode::G_VBITCAST:
      legalizeVectorCast(instr);
      return;
    default:
      throw BackendError("unhandled generic vector opcode during scalarization");
    }
  }

  void legalizeVectorUndef(const MachineInstr &instr) {
    const MachineVectorType &type = typeOf(instr.defs[0], "G_VUNDEF");
    std::vector<MachineReg> &dst = lanesForDef(instr.defs[0], type);
    for (uint16_t lane = 0; lane < type.lanes; ++lane) {
      dst[lane] = materializeLaneConstant(mf, *currentBlock(), type.element, 0);
    }
  }

  void legalizeVectorConst(const MachineInstr &instr) {
    const MachineVectorType &type = typeOf(instr.defs[0], "G_VCONST");
    std::vector<MachineReg> &dst = lanesForDef(instr.defs[0], type);
    if (instr.uses.size() != type.lanes) {
      throw BackendError("G_VCONST lane count mismatch");
    }
    for (uint16_t lane = 0; lane < type.lanes; ++lane) {
      const uint32_t bits =
          static_cast<uint32_t>(static_cast<uint64_t>(instr.uses[lane].immValue));
      dst[lane] =
          materializeLaneConstant(mf, *currentBlock(), type.element, bits);
    }
  }

  void legalizeVectorCopy(const MachineInstr &instr) {
    const MachineVectorType &type = typeOf(instr.defs[0], "G_VCOPY");
    const std::vector<MachineReg> &src = lanesForUse(instr.uses[0]);
    std::vector<MachineReg> &dst = lanesForDef(instr.defs[0], type);
    for (uint16_t lane = 0; lane < type.lanes; ++lane) {
      appendRegInstr(*currentBlock(), scalarCopyOpcode(type.element), dst[lane],
                     {src[lane]});
    }
  }

  void legalizeVectorLoad(const MachineInstr &instr) {
    const MachineVectorType &type = typeOf(instr.defs[0], "G_VLOAD");
    std::vector<MachineReg> &dst = lanesForDef(instr.defs[0], type);
    const int64_t elementBytes = type.elementBits() / 8U;
    for (uint16_t lane = 0; lane < type.lanes; ++lane) {
      appendLoadInstr(*currentBlock(), scalarLoadOpcode(type.element), dst[lane],
                      instr.uses[0], instr.uses[1].immValue +
                                        static_cast<int64_t>(lane) * elementBytes);
    }
  }

  void legalizeVectorStore(const MachineInstr &instr) {
    const MachineVectorType &type = typeOf(instr.uses[0], "G_VSTORE");
    const std::vector<MachineReg> &src = lanesForUse(instr.uses[0]);
    const int64_t elementBytes = type.elementBits() / 8U;
    for (uint16_t lane = 0; lane < type.lanes; ++lane) {
      appendStoreInstr(*currentBlock(), scalarStoreOpcode(type.element),
                       src[lane], instr.uses[1],
                       instr.uses[2].immValue +
                           static_cast<int64_t>(lane) * elementBytes);
    }
  }

  Opcode scalarBinaryOpcode(Opcode vectorOpcode,
                            const MachineVectorType &type) const {
    if (type.element == MachineVectorElementKind::F32) {
      switch (vectorOpcode) {
      case Opcode::G_VFADD:
        return Opcode::FADD_S;
      case Opcode::G_VFSUB:
        return Opcode::FSUB_S;
      case Opcode::G_VFMUL:
        return Opcode::FMUL_S;
      case Opcode::G_VFDIV:
        return Opcode::FDIV_S;
      default:
        break;
      }
    } else if (type.element == MachineVectorElementKind::I32) {
      switch (vectorOpcode) {
      case Opcode::G_VADD:
        return Opcode::ADDW;
      case Opcode::G_VSUB:
        return Opcode::SUBW;
      case Opcode::G_VMUL:
        return Opcode::MULW;
      case Opcode::G_VDIV:
        return Opcode::DIVW;
      case Opcode::G_VUDIV:
        return Opcode::DIVUW;
      case Opcode::G_VREM:
        return Opcode::REMW;
      case Opcode::G_VUREM:
        return Opcode::REMUW;
      case Opcode::G_VAND:
        return Opcode::AND;
      case Opcode::G_VOR:
        return Opcode::OR;
      case Opcode::G_VXOR:
        return Opcode::XOR;
      case Opcode::G_VSHL:
        return Opcode::SLLW;
      case Opcode::G_VLSHR:
        return Opcode::SRLW;
      case Opcode::G_VASHR:
        return Opcode::SRAW;
      default:
        break;
      }
    } else if (type.isMask() && (vectorOpcode == Opcode::G_VAND ||
                                 vectorOpcode == Opcode::G_VOR ||
                                 vectorOpcode == Opcode::G_VXOR)) {
      return vectorOpcode == Opcode::G_VAND
                 ? Opcode::AND
                 : (vectorOpcode == Opcode::G_VOR ? Opcode::OR : Opcode::XOR);
    }
    throw BackendError("vector binary opcode has no scalar RV64GC lowering");
  }

  void legalizeVectorBinary(const MachineInstr &instr) {
    const MachineVectorType &type = typeOf(instr.defs[0], "vector binary");
    const std::vector<MachineReg> &lhs = lanesForUse(instr.uses[0]);
    const std::vector<MachineReg> &rhs = lanesForUse(instr.uses[1]);
    std::vector<MachineReg> &dst = lanesForDef(instr.defs[0], type);
    const Opcode scalar = scalarBinaryOpcode(instr.opcode, type);
    for (uint16_t lane = 0; lane < type.lanes; ++lane) {
      appendRegInstr(*currentBlock(), scalar, dst[lane],
                     {lhs[lane], rhs[lane]});
    }
  }

  void appendIntCompare(MachineBasicBlock &block, BinaryInst::Operation pred,
                        MachineReg dst, MachineReg lhs, MachineReg rhs) {
    switch (pred) {
    case BinaryInst::E: {
      MachineReg tmp = mf.createVReg(RegClass::GPR32);
      appendRegInstr(block, Opcode::XOR, tmp, {lhs, rhs});
      appendRegInstr(block, Opcode::SEQZ, dst, {tmp});
      return;
    }
    case BinaryInst::NE: {
      MachineReg tmp = mf.createVReg(RegClass::GPR32);
      appendRegInstr(block, Opcode::XOR, tmp, {lhs, rhs});
      appendRegInstr(block, Opcode::SNEZ, dst, {tmp});
      return;
    }
    case BinaryInst::L:
      appendRegInstr(block, Opcode::SLT, dst, {lhs, rhs});
      return;
    case BinaryInst::G:
      appendRegInstr(block, Opcode::SLT, dst, {rhs, lhs});
      return;
    case BinaryInst::LE: {
      MachineReg tmp = mf.createVReg(RegClass::GPR32);
      appendRegInstr(block, Opcode::SLT, tmp, {rhs, lhs});
      appendImmInstr(block, Opcode::XORI, dst, MachineOperand::reg(tmp), 1);
      return;
    }
    case BinaryInst::GE: {
      MachineReg tmp = mf.createVReg(RegClass::GPR32);
      appendRegInstr(block, Opcode::SLT, tmp, {lhs, rhs});
      appendImmInstr(block, Opcode::XORI, dst, MachineOperand::reg(tmp), 1);
      return;
    }
    default:
      throw BackendError("unsupported integer vector comparison predicate");
    }
  }

  void appendFloatCompare(MachineBasicBlock &block, BinaryInst::Operation pred,
                          MachineReg dst, MachineReg lhs, MachineReg rhs) {
    switch (pred) {
    case BinaryInst::E:
      appendRegInstr(block, Opcode::FEQ_S, dst, {lhs, rhs});
      return;
    case BinaryInst::NE: {
      MachineReg tmp = mf.createVReg(RegClass::GPR32);
      appendRegInstr(block, Opcode::FEQ_S, tmp, {lhs, rhs});
      appendImmInstr(block, Opcode::XORI, dst, MachineOperand::reg(tmp), 1);
      return;
    }
    case BinaryInst::L:
      appendRegInstr(block, Opcode::FLT_S, dst, {lhs, rhs});
      return;
    case BinaryInst::G:
      appendRegInstr(block, Opcode::FLT_S, dst, {rhs, lhs});
      return;
    case BinaryInst::LE:
      appendRegInstr(block, Opcode::FLE_S, dst, {lhs, rhs});
      return;
    case BinaryInst::GE:
      appendRegInstr(block, Opcode::FLE_S, dst, {rhs, lhs});
      return;
    default:
      throw BackendError("unsupported float vector comparison predicate");
    }
  }

  void legalizeVectorCompare(const MachineInstr &instr) {
    const MachineVectorType &maskType = typeOf(instr.defs[0], "vector compare");
    const MachineVectorType &operandType =
        typeOf(instr.uses[0], "vector compare operand");
    const std::vector<MachineReg> &lhs = lanesForUse(instr.uses[0]);
    const std::vector<MachineReg> &rhs = lanesForUse(instr.uses[1]);
    std::vector<MachineReg> &dst = lanesForDef(instr.defs[0], maskType);
    const auto pred =
        static_cast<BinaryInst::Operation>(instr.uses[2].immValue);
    for (uint16_t lane = 0; lane < operandType.lanes; ++lane) {
      if (operandType.element == MachineVectorElementKind::F32) {
        appendFloatCompare(*currentBlock(), pred, dst[lane], lhs[lane],
                           rhs[lane]);
      } else {
        appendIntCompare(*currentBlock(), pred, dst[lane], lhs[lane],
                         rhs[lane]);
      }
    }
  }

  void legalizeVectorSelect(const MachineInstr &instr) {
    const MachineVectorType &type = typeOf(instr.defs[0], "vector select");
    const std::vector<MachineReg> &trueLanes = lanesForUse(instr.uses[1]);
    const std::vector<MachineReg> &falseLanes = lanesForUse(instr.uses[2]);
    std::vector<MachineReg> &dst = lanesForDef(instr.defs[0], type);

    std::vector<MachineReg> condLanes;
    if (instr.uses[0].regValue.rc == RegClass::GenericVector) {
      condLanes = lanesForUse(instr.uses[0]);
    } else {
      condLanes.assign(type.lanes, instr.uses[0].regValue);
    }
    for (uint16_t lane = 0; lane < type.lanes; ++lane) {
      currentBlock()->instrs.push_back(
          makeSelect(dst[lane], condLanes[lane], trueLanes[lane], falseLanes[lane]));
    }
  }

  // 动态 lane 索引:index == lane ? lane_value : old_value。旧值初始为 0，
  // 后续每个 lane 的选择串行累积出最终结果。
  MachineReg emitDynamicLaneSelect(const std::vector<MachineReg> &laneValues,
                                   const MachineOperand &indexOperand,
                                   uint16_t lanes,
                                   const MachineVectorType &type) {
    if (indexOperand.kind == OperandKind::Reg &&
        indexOperand.regValue.rc == RegClass::GPR32) {
      MachineReg current = materializeLaneConstant(
          mf, *currentBlock(), type.element, 0);
      for (uint16_t lane = 0; lane < lanes; ++lane) {
        MachineReg cond =
            emitIndexEqualsLane(mf, *currentBlock(), indexOperand.regValue, lane);
        current = mf.createVReg(scalarClass(type));
        currentBlock()->instrs.push_back(
            makeSelect(current, cond, laneValues[lane], current));
      }
      return current;
    }
    throw BackendError("dynamic vector lane index must be a GPR32 register");
  }

  void legalizeVectorExtract(const MachineInstr &instr) {
    const MachineVectorType &type = typeOf(instr.uses[0], "vector extract");
    const std::vector<MachineReg> &src = lanesForUse(instr.uses[0]);
    if (instr.uses[1].kind == OperandKind::Imm) {
      const int64_t lane = instr.uses[1].immValue;
      if (lane < 0 || static_cast<uint16_t>(lane) >= type.lanes) {
        throw BackendError("constant vector extract lane out of range");
      }
      appendRegInstr(*currentBlock(), scalarCopyOpcode(type.element),
                     instr.defs[0].regValue, {src[static_cast<uint16_t>(lane)]});
      return;
    }
    MachineReg selected =
        emitDynamicLaneSelect(src, instr.uses[1], type.lanes, type);
    appendRegInstr(*currentBlock(), scalarCopyOpcode(type.element),
                   instr.defs[0].regValue, {selected});
  }

  void legalizeVectorInsert(const MachineInstr &instr) {
    const MachineVectorType &type = typeOf(instr.defs[0], "vector insert");
    const std::vector<MachineReg> &oldLanes = lanesForUse(instr.uses[0]);
    std::vector<MachineReg> &dst = lanesForDef(instr.defs[0], type);

    if (instr.uses[2].kind == OperandKind::Imm) {
      const int64_t lane = instr.uses[2].immValue;
      if (lane < 0 || static_cast<uint16_t>(lane) >= type.lanes) {
        throw BackendError("constant vector insert lane out of range");
      }
      for (uint16_t i = 0; i < type.lanes; ++i) {
        appendRegInstr(*currentBlock(), scalarCopyOpcode(type.element), dst[i],
                       {oldLanes[i]});
      }
      appendRegInstr(*currentBlock(), scalarCopyOpcode(type.element),
                     dst[static_cast<uint16_t>(lane)],
                     {instr.uses[1].regValue});
      return;
    }

    for (uint16_t lane = 0; lane < type.lanes; ++lane) {
      MachineReg cond = emitIndexEqualsLane(
          mf, *currentBlock(), instr.uses[2].regValue, lane);
      currentBlock()->instrs.push_back(
          makeSelect(dst[lane], cond, instr.uses[1].regValue, oldLanes[lane]));
    }
  }

  void legalizeVectorShuffle(const MachineInstr &instr) {
    const MachineVectorType &type = typeOf(instr.defs[0], "vector shuffle");
    const std::vector<MachineReg> &left = lanesForUse(instr.uses[0]);
    const std::vector<MachineReg> &right = lanesForUse(instr.uses[1]);
    std::vector<MachineReg> &dst = lanesForDef(instr.defs[0], type);
    for (uint16_t lane = 0; lane < type.lanes; ++lane) {
      const int64_t mask = instr.uses[static_cast<std::size_t>(lane) + 2].immValue;
      if (mask < 0) {
        dst[lane] =
            materializeLaneConstant(mf, *currentBlock(), type.element, 0);
        continue;
      }
      const auto source = static_cast<std::size_t>(mask);
      if (source < left.size()) {
        appendRegInstr(*currentBlock(), scalarCopyOpcode(type.element), dst[lane],
                       {left[source]});
      } else if (source - left.size() < right.size()) {
        appendRegInstr(*currentBlock(), scalarCopyOpcode(type.element), dst[lane],
                       {right[source - left.size()]});
      } else {
        throw BackendError("vector shuffle mask out of range");
      }
    }
  }

  void legalizeVectorIntCast(const MachineInstr &instr) {
    const MachineVectorType &type = typeOf(instr.defs[0], "vector int cast");
    const MachineVectorType &sourceType =
        typeOf(instr.uses[0], "vector int cast source");
    if (type.lanes != sourceType.lanes) {
      throw BackendError("vector cast changes lane count");
    }
    const std::vector<MachineReg> &src = lanesForUse(instr.uses[0]);
    std::vector<MachineReg> &dst = lanesForDef(instr.defs[0], type);
    for (uint16_t lane = 0; lane < type.lanes; ++lane) {
      if (type.element == sourceType.element) {
        appendRegInstr(*currentBlock(), scalarCopyOpcode(type.element), dst[lane],
                       {src[lane]});
      } else if (sourceType.isMask() &&
                 type.element == MachineVectorElementKind::I32) {
        appendRegInstr(*currentBlock(), Opcode::COPY, dst[lane], {src[lane]});
      } else if (type.isMask() &&
                 sourceType.element == MachineVectorElementKind::I32 &&
                 instr.opcode != Opcode::G_VSEXT) {
        appendImmInstr(*currentBlock(), Opcode::ANDI, dst[lane],
                       MachineOperand::reg(src[lane]), 1);
      } else {
        throw BackendError("unsupported vector integer cast during scalarization");
      }
    }
  }

  void legalizeVectorCast(const MachineInstr &instr) {
    const MachineVectorType &type = typeOf(instr.defs[0], "vector cast");
    const MachineVectorType &sourceType =
        typeOf(instr.uses[0], "vector cast source");
    if (type.lanes != sourceType.lanes) {
      throw BackendError("vector cast changes lane count");
    }
    const std::vector<MachineReg> &src = lanesForUse(instr.uses[0]);
    std::vector<MachineReg> &dst = lanesForDef(instr.defs[0], type);

    Opcode opcode = Opcode::NOP;
    if (instr.opcode == Opcode::G_VFPTOSI &&
        sourceType.element == MachineVectorElementKind::F32 &&
        type.element == MachineVectorElementKind::I32) {
      opcode = Opcode::FCVT_W_S;
    } else if (instr.opcode == Opcode::G_VSITOFP &&
               sourceType.element == MachineVectorElementKind::I32 &&
               type.element == MachineVectorElementKind::F32) {
      opcode = Opcode::FCVT_S_W;
    } else if (instr.opcode == Opcode::G_VBITCAST) {
      if (sourceType.element == MachineVectorElementKind::F32 &&
          type.element == MachineVectorElementKind::I32) {
        opcode = Opcode::FMV_X_W;
      } else if (sourceType.element == MachineVectorElementKind::I32 &&
                 type.element == MachineVectorElementKind::F32) {
        opcode = Opcode::FMV_W_X;
      } else if (sourceType.element == type.element) {
        opcode = scalarCopyOpcode(type.element);
      }
    }
    if (opcode == Opcode::NOP) {
      throw BackendError("unsupported vector cast during scalarization");
    }
    for (uint16_t lane = 0; lane < type.lanes; ++lane) {
      appendRegInstr(*currentBlock(), opcode, dst[lane], {src[lane]});
    }
  }

  MachineBasicBlock *currentBlock() { return currentBlock_; }
  const MachineBasicBlock *currentBlock() const { return currentBlock_; }

  void rebuildValueLaneMap() {
    mf.valueVectorLanes.clear();
    for (const auto &[value, reg] : mf.valueRegs) {
      if (reg.rc != RegClass::GenericVector) {
        continue;
      }
      auto found = laneMap.find(reg.id);
      if (found != laneMap.end()) {
        mf.valueVectorLanes[value] = found->second;
      }
    }
  }

  MachineFunction &mf;
  MachineBasicBlock *currentBlock_ = nullptr;
  std::unordered_map<uint32_t, std::vector<MachineReg>> laneMap;
};

} // namespace

void legalizeAndSelectGenericVectors(MachineModule &module) {
  validateGenericVectorMIR(module);

  for (MachineFunction &function : module.functions) {
    ScalarizingVectorLegalizer legalizer(function);
    for (MachineBasicBlock &block : function.blocks) {
      legalizer.runOnBlock(block);
    }
    legalizer.finish();
  }

  // 向量全局对象在 AsmPrinter 中按数据段逐 lane 发射；legalize 之后不再有
  // GenericVector MIR，这里清除标记避免 fail-closed 逻辑误报。
  for (GlobalObject &global : module.globals) {
    global.requiresVectorLowering = false;
  }

  if (containsGenericVectorMIR(module)) {
    throw BackendError("vector scalarization left generic vector MIR behind");
  }
}

} // namespace riscv
