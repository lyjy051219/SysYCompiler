#include "riscv/Frame.hpp"

#include "lib/Target.hpp"
#include "lib/TypeSystem.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>

namespace riscv {
// 本文件实现 Frame Lowering：在寄存器分配确定了栈上对象与被调用者
// 保存寄存器之后,计算每个栈对象的偏移、整帧大小,并把引用入参的伪帧偏移
// 展开为相对当前帧的真实立即数。位于 RISC-V 后端流水线靠后的位置(寄存器分配与
// 栈对象收集完成之后、汇编发射之前)。

namespace {

// 把 value 向上对齐到 align 的整数倍。align 至少取 1,避免除零或负对齐。
int64_t alignTo(int64_t value, int64_t align) {
  align = std::max<int64_t>(1, align);
  return ((value + align - 1) / align) * align;
}

// 判断 IR 类型是否为布尔:整型且带有 isBool 标记。布尔在内存中占 1 字节。
bool isBoolType(const std::shared_ptr<IRType> &type) {
  auto intType = std::dynamic_pointer_cast<INTType>(type);
  return intType && intType->getIsBool();
}

// 扫描整个函数,判断是否含有 CALL 指令。含调用的函数不是叶函数,
// 需要在帧中为返回地址等预留空间。
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

// 判断操作码是否为访存加载指令(整型/浮点),用于识别入参加载。
bool isLoad(Opcode opcode) {
  return opcode == Opcode::LBU || opcode == Opcode::LW ||
         opcode == Opcode::LD || opcode == Opcode::FLW;
}

// 判断操作数是否为浮点虚拟寄存器(尚未分配物理寄存器、寄存器类为 FPR32)。
bool isFprVirtualReg(const MachineOperand &operand) {
  return operand.kind == OperandKind::Reg && !operand.regValue.physical &&
         operand.regValue.rc == RegClass::FPR32;
}

// 判断函数是否用到浮点虚拟寄存器。用于决定非叶函数是否需要为浮点被调用者
// 保存寄存器预留栈空间。遍历所有指令的 defs 与 uses。
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

// 从指令注释中解析"上一次记录的帧大小"。入参加载在早期以伪偏移标注,
// 注释形如 "incoming_arg" 或 "incoming_arg:<帧大小>"。
// 返回:纯 "incoming_arg" 返回 0(尚未展开);带数值的返回该帧大小;
// 非入参注释返回 -1(表示与本处理无关)。
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

// 计算 IR 类型的字节大小。布尔 1 字节,普通整型/浮点 4 字节,指针 8 字节,
// 数组按元素数乘以元素大小递归求值;其余类型退回类型自带的 getSize。
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
  case IR_VECTOR:
    return static_cast<int64_t>(
        TargetInfo::get().getDataLayout().getTypeAllocSize(type));
  case IR_ARRAY: {
    auto arrayType = std::dynamic_pointer_cast<ARRAYType>(type);
    return static_cast<int64_t>(arrayType->getElementCount()) *
           typeSizeBytes(arrayType->getElementType());
  }
  default:
    return static_cast<int64_t>(type->getSize());
  }
}

// 计算 IR 类型的对齐字节数。标量与大小一致;数组的对齐取元素对齐,
// 结果至少为 1。
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
  case IR_VECTOR:
    return static_cast<int64_t>(
        TargetInfo::get().getDataLayout().getABITypeAlign(type));
  case IR_ARRAY: {
    auto arrayType = std::dynamic_pointer_cast<ARRAYType>(type);
    return std::max<int64_t>(1, typeAlignBytes(arrayType->getElementType()));
  }
  default:
    return std::max<int64_t>(1, static_cast<int64_t>(type->getSize()));
  }
}

// 为函数分配栈帧布局并确定整帧大小。
// 输入:含有栈对象列表、被调用者保存寄存器集合、最大外传参数字节数的 mf。
// 输出:就地写入每个栈对象的 offset,以及 mf.frameSize。
// 关键步骤:先按各对象自身对齐依次排布局部栈对象;再对齐到 16 字节后,
// 为返回地址和被调用者保存寄存器预留空间;最后加上外传参数区并按 16 字节对齐。
void FrameLowering::assignFrameLayout(MachineFunction &mf) {
  // 帧底部预留外传参数区:调用点把溢出的栈参数写入 [sp, sp+maxOutgoingArgs),
  // 不再在调用点动态下调 sp,从而与相对 sp 寻址的溢出槽兼容。局部对象排在其之上。
  int64_t offset = mf.maxOutgoingArgs;
  // 依次为每个栈对象分配偏移:先按对象对齐向上取整,再累加其大小。
  for (StackObject &object : mf.stackObjects) {
    object.align = std::max<int64_t>(1, object.align);
    object.size = std::max<int64_t>(1, object.size);
    offset = alignTo(offset, object.align);
    object.offset = offset;
    offset += object.size;
  }

  offset = alignTo(offset, 16);
  const bool containsCall = hasCall(mf);
  // 只有含调用或使用了被调用者保存寄存器时,才需要保存区。
  if (containsCall || !mf.usedCalleeSaved.empty()) {
    offset = alignTo(offset, 8);
    if (containsCall) {
      offset += 8; // 返回地址
    }
    if (!mf.usedCalleeSaved.empty()) {
      // 已确定的被调用者保存寄存器,每个占 8 字节。
      offset += static_cast<int64_t>(mf.usedCalleeSaved.size()) * 8;
    } else if (containsCall) {
      // 尚未跑寄存器分配的临时保守估计:非叶函数最多可能用到 s1-s11。
      offset += 8 * 11; // 临时非叶映射器使用 s1-s11。
      if (usesFprVirtualRegs(mf)) {
        offset += 8 * 12; // 非叶浮点临时值使用 fs0-fs11。
      }
    }
  }
  // 帧末尾追加外传参数区,整帧按 16 字节对齐(栈指针对齐要求)。
  // 外传参数区已在帧底部预留(offset 起点已含),此处不再重复计入。
  mf.frameSize = alignTo(offset, 16);
}

// 把入参加载指令中的伪帧偏移展开为相对当前帧的真实立即数。
// 早期发射入参加载时,入参位于调用者帧,只能先记下相对占位;现在整帧大小
// 已知,补上 frameSize 差值并更新注释,记录本次已按哪个帧大小展开(便于幂等)。
// 输入/输出:就地修改 mf 中匹配指令的立即数与注释。
void FrameLowering::expandFrameIndices(MachineFunction &mf) {
  for (MachineBasicBlock &block : mf.blocks) {
    for (MachineInstr &instr : block.instrs) {
      const int64_t previousFrame = previousIncomingArgFrame(instr.comment);
      // 仅处理带入参注释、且形如 load reg, imm(base) 的加载指令,其余跳过。
      if (previousFrame < 0 || !isLoad(instr.opcode) || instr.uses.size() < 2 ||
          instr.uses[1].kind != OperandKind::Imm) {
        continue;
      }
      // 用新旧帧大小之差修正偏移,并把注释更新为当前帧大小以避免重复展开。
      instr.uses[1].immValue += mf.frameSize - previousFrame;
      instr.comment = "incoming_arg:" + std::to_string(mf.frameSize);
    }
  }
}

} // namespace riscv
