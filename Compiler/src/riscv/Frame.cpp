#include "riscv/Frame.hpp"

#include "lib/TypeSystem.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>

namespace riscv {
namespace {

int64_t alignTo(int64_t value, int64_t align) {
  align = std::max<int64_t>(1, align);
  return ((value + align - 1) / align) * align;
}

bool isBoolType(const std::shared_ptr<IRType> &type) {
  auto intType = std::dynamic_pointer_cast<INTType>(type);
  return intType && intType->getIsBool();
}

bool hasCall(const MachineFunction &mf) {
  for (const MachineBasicBlock &block : mf.blocks) {
    for (const MachineInstr &instr : block.instrs) {
      if (instr.opcode == Opcode::CALL) {
        return true;
      }
    }
  }
  return false;
}

bool isLoad(Opcode opcode) {
  return opcode == Opcode::LBU || opcode == Opcode::LW ||
         opcode == Opcode::LD || opcode == Opcode::FLW;
}

bool isFprVirtualReg(const MachineOperand &operand) {
  return operand.kind == OperandKind::Reg && !operand.regValue.physical &&
         operand.regValue.rc == RegClass::FPR32;
}

bool usesFprVirtualRegs(const MachineFunction &mf) {
  for (const MachineBasicBlock &block : mf.blocks) {
    for (const MachineInstr &instr : block.instrs) {
      for (const MachineOperand &def : instr.defs) {
        if (isFprVirtualReg(def)) {
          return true;
        }
      }
      for (const MachineOperand &use : instr.uses) {
        if (isFprVirtualReg(use)) {
          return true;
        }
      }
    }
  }
  return false;
}

int64_t previousIncomingArgFrame(const std::string &comment) {
  constexpr const char *prefix = "incoming_arg:";
  if (comment == "incoming_arg") {
    return 0;
  }
  if (comment.rfind(prefix, 0) != 0) {
    return -1;
  }
  return std::stoll(comment.substr(std::string(prefix).size()));
}

} // namespace

int64_t typeSizeBytes(const std::shared_ptr<IRType> &type) {
  if (!type) {
    return 0;
  }

  switch (type->getTypeSystem()) {
  case IR_INT:
    return isBoolType(type) ? 1 : 4;
  case IR_FLOAT:
    return 4;
  case IR_POINTER:
    return 8;
  case IR_ARRAY: {
    auto arrayType = std::dynamic_pointer_cast<ARRAYType>(type);
    return static_cast<int64_t>(arrayType->getElementCount()) *
           typeSizeBytes(arrayType->getElementType());
  }
  default:
    return static_cast<int64_t>(type->getSize());
  }
}

int64_t typeAlignBytes(const std::shared_ptr<IRType> &type) {
  if (!type) {
    return 1;
  }

  switch (type->getTypeSystem()) {
  case IR_INT:
    return isBoolType(type) ? 1 : 4;
  case IR_FLOAT:
    return 4;
  case IR_POINTER:
    return 8;
  case IR_ARRAY: {
    auto arrayType = std::dynamic_pointer_cast<ARRAYType>(type);
    return std::max<int64_t>(1, typeAlignBytes(arrayType->getElementType()));
  }
  default:
    return std::max<int64_t>(1, static_cast<int64_t>(type->getSize()));
  }
}

void FrameLowering::assignFrameLayout(MachineFunction &mf) {
  int64_t offset = 0;
  for (StackObject &object : mf.stackObjects) {
    object.align = std::max<int64_t>(1, object.align);
    object.size = std::max<int64_t>(1, object.size);
    offset = alignTo(offset, object.align);
    object.offset = offset;
    offset += object.size;
  }

  offset = alignTo(offset, 16);
  const bool containsCall = hasCall(mf);
  if (containsCall || !mf.usedCalleeSaved.empty()) {
    offset = alignTo(offset, 8);
    if (containsCall) {
      offset += 8; // 返回地址
    }
    if (!mf.usedCalleeSaved.empty()) {
      offset += static_cast<int64_t>(mf.usedCalleeSaved.size()) * 8;
    } else if (containsCall) {
      offset += 8 * 11; // 临时非叶映射器使用 s1-s11。
      if (usesFprVirtualRegs(mf)) {
        offset += 8 * 12; // 非叶浮点临时值使用 fs0-fs11。
      }
    }
  }
  mf.frameSize = alignTo(offset + mf.maxOutgoingArgs, 16);
}

void FrameLowering::expandFrameIndices(MachineFunction &mf) {
  for (MachineBasicBlock &block : mf.blocks) {
    for (MachineInstr &instr : block.instrs) {
      const int64_t previousFrame = previousIncomingArgFrame(instr.comment);
      if (previousFrame < 0 || !isLoad(instr.opcode) || instr.uses.size() < 2 ||
          instr.uses[1].kind != OperandKind::Imm) {
        continue;
      }
      instr.uses[1].immValue += mf.frameSize - previousFrame;
      instr.comment = "incoming_arg:" + std::to_string(mf.frameSize);
    }
  }
}

} // namespace riscv
