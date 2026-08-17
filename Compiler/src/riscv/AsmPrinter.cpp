// AsmPrinter.cpp —— 把已完成寄存器分配与frame lowering的 MIR 发射为 RV64GC 汇编文本。
// 主要职责:
//   - 发射模块头、全局数据段(.bss/.rodata/.data 及初值)与文本段。
//   - 逐函数发射符号、prologue(下调 sp、保存 ra 与被调用者保存寄存器)、
//     函数体逐指令翻译、以及 RET 处的 epilogue(恢复寄存器、还原 sp、ret)。
//   - 把每条 MIR 指令翻译成对应 RISC-V 助记符,处理立即数/标签/栈帧偏移的格式化,
//     并展开 la、memcpy/memset 内联、超出 12 位立即数范围的访存等伪指令序列。
//   - 作为兜底,为仍残留的虚拟寄存器做一个简单的临时寄存器指派(基于使用计数回收)。
// 命名约定:寄存器打印为 ABI 名(a0/t0/fs1...),访存写作 offset(base),
// 局部标签统一用 .L 前缀并配自增 id 保证唯一。
#include "riscv/AsmPrinter.hpp"

#include "lib/CFG.hpp"
#include "riscv/Target.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <ostream>
#include <string>

namespace riscv {
namespace {

// 判断 IR 类型是否为布尔(1 字节整型)。
bool isBoolType(const std::shared_ptr<IRType> &type) {
  auto intType = std::dynamic_pointer_cast<INTType>(type);
  return intType && intType->getIsBool();
}

// 判断 IR 类型是否为指针类型。
bool isPointerType(const std::shared_ptr<IRType> &type) {
  return type && type->getTypeSystem() == IR_POINTER;
}

// 计算 IR 类型在内存中占用的字节数:布尔 1、int/float 4、指针 8,
// 数组按元素数乘元素大小递归,其余回退到类型自身报告的 size。
int64_t sizeOfType(const std::shared_ptr<IRType> &type) {
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
           sizeOfType(arrayType->getElementType());
  }
  default:
    return static_cast<int64_t>(type->getSize());
  }
}

// 求以 2 为底的对齐幂:给定字节对齐值返回其 log2(向上取整),供 .p2align 使用。
int p2Align(int64_t align) {
  align = std::max<int64_t>(1, align);
  int power = 0;
  int64_t value = 1;
  while (value < align) {
    value <<= 1;
    ++power;
  }
  return power;
}

// 判断数值是否落在有符号 12 位立即数范围([-2048, 2047]),这是多数 I 型指令的限制。
bool fitsSigned12(int64_t value) { return value >= -2048 && value <= 2047; }

// 内联 memcpy/memset 展开时保留使用的固定物理寄存器:临时值、源指针、目的指针;
// kReservedGprScratch（t6）是发射期通用的 scratch 寄存器，不参与寄存器分配。
constexpr PhysReg kReservedMemcpyTmp = PhysReg::T3;
constexpr PhysReg kReservedMemcpySrc = PhysReg::T4;
constexpr PhysReg kReservedMemcpyDst = PhysReg::T5;
constexpr PhysReg kReservedGprScratch = PhysReg::T6;

// 发射一条相对 sp 的访存(如保存/恢复 ra)。偏移落在 12 位内时直接写 offset(sp);
// 否则先把偏移 li 到 scratch 寄存器，再 add 到 sp 上，最后以 scratch 为基址访存。
void emitSpRelative(std::ostream &out, const char *mnemonic, PhysReg reg,
                    int64_t offset) {
  if (fitsSigned12(offset)) {
    out << "    " << mnemonic << ' ' << regName(reg) << ", " << offset
        << "(sp)\n";
    return;
  }
  out << "    li " << regName(kReservedGprScratch) << ", " << offset << '\n';
  out << "    add " << regName(kReservedGprScratch) << ", sp, "
      << regName(kReservedGprScratch) << '\n';
  out << "    " << mnemonic << ' ' << regName(reg) << ", 0("
      << regName(kReservedGprScratch) << ")\n";
}

// 兜底临时寄存器池。当 MIR 中仍残留虚拟寄存器时,AsmPrinter 自己按类别指派物理寄存器。
// 叶子函数(不含 call)可用的整型/浮点scratch 寄存器:caller-saved 的 t/a、ft 系列。
constexpr std::array<PhysReg, 11> kGprScratchRegs{
    PhysReg::T0, PhysReg::T1, PhysReg::T2, PhysReg::A0,
    PhysReg::A1, PhysReg::A2, PhysReg::A3, PhysReg::A4,
    PhysReg::A5, PhysReg::A6, PhysReg::A7};

constexpr std::array<PhysReg, 8> kFprScratchRegs{
    PhysReg::FT0, PhysReg::FT1, PhysReg::FT2, PhysReg::FT3,
    PhysReg::FT4, PhysReg::FT5, PhysReg::FT6, PhysReg::FT7};

// 含 call 的非叶子函数改用 callee-saved 的 s/fs 系列,避免临时值跨调用被破坏
// (代价是需在 prologue 保存这些寄存器)。
constexpr std::array<PhysReg, 11> kNonLeafGprScratchRegs{
    PhysReg::S1, PhysReg::S2,  PhysReg::S3, PhysReg::S4,
    PhysReg::S5, PhysReg::S6,  PhysReg::S7, PhysReg::S8,
    PhysReg::S9, PhysReg::S10, PhysReg::S11};

constexpr std::array<PhysReg, 12> kNonLeafFprScratchRegs{
    PhysReg::FS0, PhysReg::FS1, PhysReg::FS2,  PhysReg::FS3,
    PhysReg::FS4, PhysReg::FS5, PhysReg::FS6,  PhysReg::FS7,
    PhysReg::FS8, PhysReg::FS9, PhysReg::FS10, PhysReg::FS11};

// 判断函数是否含 CALL 指令(即非叶子函数),决定 prologue 是否保存 ra 及临时池选择。
bool functionContainsCall(const MachineFunction &function) {
  for (const MachineBasicBlock &block : function.blocks) {
    for (const MachineInstr &instr : block.instrs) {
      if (instr.opcode == Opcode::CALL) {
        return true;
      }
    }
  }
  return false;
}

// 判断操作数是否为浮点类虚拟寄存器。
bool isFprVirtualReg(const MachineOperand &operand) {
  return operand.kind == OperandKind::Reg && !operand.regValue.physical &&
         operand.regValue.rc == RegClass::FPR32;
}

// 判断操作数是否为整型类(GPR32/64)虚拟寄存器。
bool isGprVirtualReg(const MachineOperand &operand) {
  return operand.kind == OperandKind::Reg && !operand.regValue.physical &&
         (operand.regValue.rc == RegClass::GPR32 ||
          operand.regValue.rc == RegClass::GPR64);
}

// 扫描函数是否用到整型虚拟寄存器(用于判断非叶子函数是否需保存整型临时池)。
bool functionUsesGprVirtualRegs(const MachineFunction &function) {
  for (const MachineBasicBlock &block : function.blocks) {
    for (const MachineInstr &instr : block.instrs) {
      for (const MachineOperand &def : instr.defs) {
        if (isGprVirtualReg(def)) {
          return true;
        }
      }
      for (const MachineOperand &use : instr.uses) {
        if (isGprVirtualReg(use)) {
          return true;
        }
      }
    }
  }
  return false;
}

// 扫描函数是否用到浮点虚拟寄存器。
bool functionUsesFprVirtualRegs(const MachineFunction &function) {
  for (const MachineBasicBlock &block : function.blocks) {
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

// 判断某物理寄存器是否属于非叶子浮点scratch 池(fs0-fs11)。
bool isNonLeafFprScratchReg(PhysReg reg) {
  return std::find(kNonLeafFprScratchRegs.begin(), kNonLeafFprScratchRegs.end(),
                   reg) != kNonLeafFprScratchRegs.end();
}

// 判断操作数是否为非叶子浮点scratch 池中的物理寄存器。
bool isNonLeafFprScratchOperand(const MachineOperand &operand) {
  return operand.kind == OperandKind::Reg && operand.regValue.physical &&
         isNonLeafFprScratchReg(operand.regValue.phys);
}

// 扫描函数是否已经使用了非叶子浮点scratch 寄存器(这些属 callee-saved,需在 prologue 保存)。
bool functionUsesNonLeafFprScratchRegs(const MachineFunction &function) {
  for (const MachineBasicBlock &block : function.blocks) {
    for (const MachineInstr &instr : block.instrs) {
      for (const MachineOperand &def : instr.defs) {
        if (isNonLeafFprScratchOperand(def)) {
          return true;
        }
      }
      for (const MachineOperand &use : instr.uses) {
        if (isNonLeafFprScratchOperand(use)) {
          return true;
        }
      }
    }
  }
  return false;
}

// 函数是否需要保存浮点临时:用到浮点虚拟寄存器,或已占用非叶子浮点scratch 寄存器。
bool functionNeedsFprTempSaves(const MachineFunction &function) {
  return functionUsesFprVirtualRegs(function) ||
         functionUsesNonLeafFprScratchRegs(function);
}

// 收集函数实际用到的 callee-saved 寄存器,按整型/浮点(wantFloat)过滤。
std::vector<PhysReg> usedCalleeSavedRegs(const MachineFunction &function,
                                         bool wantFloat) {
  std::vector<PhysReg> regs;
  for (PhysReg reg : function.usedCalleeSaved) {
    const RegisterInfo &info = getRegisterInfo(reg);
    if (info.calleeSaved && info.isFloat == wantFloat) {
      regs.push_back(reg);
    }
  }
  return regs;
}

// 确定 prologue 中需保存的整型 callee-saved 寄存器集合。
// 常规情况用寄存器分配记录的 usedCalleeSaved;若该集合为空但函数含 call 且
// 用到整型虚拟寄存器,说明兜底临时池会占用非叶子scratch 寄存器,则整批保存它们。
std::vector<PhysReg> gprCalleeSavedRegsToSave(const MachineFunction &function) {
  std::vector<PhysReg> regs = usedCalleeSavedRegs(function, false);
  if (!regs.empty() || !functionContainsCall(function)) {
    return regs;
  }
  if (function.usedCalleeSaved.empty() && functionUsesGprVirtualRegs(function)) {
    return std::vector<PhysReg>(kNonLeafGprScratchRegs.begin(),
                                kNonLeafGprScratchRegs.end());
  }
  return regs;
}

// 与上面对称,确定需保存的浮点 callee-saved 寄存器集合。
std::vector<PhysReg> fprCalleeSavedRegsToSave(const MachineFunction &function) {
  std::vector<PhysReg> regs = usedCalleeSavedRegs(function, true);
  if (!regs.empty() || !functionContainsCall(function)) {
    return regs;
  }
  if (function.usedCalleeSaved.empty() && functionNeedsFprTempSaves(function)) {
    return std::vector<PhysReg>(kNonLeafFprScratchRegs.begin(),
                                kNonLeafFprScratchRegs.end());
  }
  return regs;
}

// 取全局对象的被指对象类型:全局在 IR 里以指针建模,这里剥掉指针层拿到真正的数据类型。
std::shared_ptr<IRType> pointeeType(const GlobalObject &global) {
  if (!global.ir) {
    return nullptr;
  }

  auto pointerType =
      std::dynamic_pointer_cast<POINTERType>(global.ir->getIRType());
  if (!pointerType) {
    return global.ir->getIRType();
  }
  return pointerType->getPointerType();
}

// 判断全局初值是否全零(含空初值、零常量、以及数组/嵌套初值逐元素全零),
// 用于决定该全局放入 .bss(全零,只记大小)还是需要逐字节发射实际数据。
bool isZeroInitializer(Value *value, const std::shared_ptr<IRType> &type) {
  if (!value) {
    return true;
  }
  if (isPointerType(type)) {
    return dynamic_cast<ConstPtr *>(value) != nullptr;
  }
  if (isBoolType(type)) {
    if (auto boolValue = dynamic_cast<ConstBool *>(value)) {
      return !boolValue->getVal();
    }
    if (auto intValue = dynamic_cast<ConstInt *>(value)) {
      return intValue->getVal() == 0;
    }
  }
  if (auto intValue = dynamic_cast<ConstInt *>(value)) {
    return intValue->getVal() == 0;
  }
  if (auto floatValue = dynamic_cast<ConstFloat *>(value)) {
    return floatValue->getVal() == 0.0f;
  }
  if (auto boolValue = dynamic_cast<ConstBool *>(value)) {
    return !boolValue->getVal();
  }
  if (auto vectorValue = dynamic_cast<ConstantVector *>(value)) {
    for (ConstantSystem *element : vectorValue->getElements()) {
      if (!element || !isZeroInitializer(element, element->getIRType())) {
        return false;
      }
    }
    return vectorValue->getNumElements() != 0;
  }

  auto init = dynamic_cast<Initializer *>(value);
  if (!init) {
    return false;
  }
  if (init->empty()) {
    return true;
  }

  auto arrayType = std::dynamic_pointer_cast<ARRAYType>(type);
  if (!arrayType) {
    for (Value *element : *init) {
      if (!isZeroInitializer(element,
                             element ? element->getIRType() : nullptr)) {
        return false;
      }
    }
    return true;
  }

  auto elementType = arrayType->getElementType();
  const int count = arrayType->getElementCount();
  for (int i = 0; i < count && i < static_cast<int>(init->size()); ++i) {
    if (!isZeroInitializer((*init)[i], elementType)) {
      return false;
    }
  }
  return true;
}

// 发射 size 字节的零填充(.zero 指令),size 非正时不输出。
void emitZeroBytes(std::ostream &out, int64_t size) {
  if (size > 0) {
    out << "    .zero " << size << '\n';
  }
}

// 按类型大小发射相应字节数的零填充。
void emitZeroForType(std::ostream &out, const std::shared_ptr<IRType> &type) {
  emitZeroBytes(out, sizeOfType(type));
}

// 递归发射一个全局初值到数据段。
// 数组:逐元素发射,不足元素数的尾部补零;指针:仅支持空指针(补零);
// 布尔:发射 .byte 0/1;整型/布尔常量发射 .word;
// 浮点常量按 IEEE-754 位模式重解释为无符号整数发射 .word;其余情况补零。
void emitDataValue(std::ostream &out, Value *value,
                   const std::shared_ptr<IRType> &type) {
  if (auto vectorType = std::dynamic_pointer_cast<VECTORType>(type)) {
    auto vectorValue = dynamic_cast<ConstantVector *>(value);
    const std::size_t count = vectorType->getElementCount();
    for (std::size_t lane = 0; lane < count; ++lane) {
      emitDataValue(out, vectorValue ? vectorValue->getElement(lane) : nullptr,
                    vectorType->getElementType());
    }
    return;
  }

  if (auto arrayType = std::dynamic_pointer_cast<ARRAYType>(type)) {
    auto init = dynamic_cast<Initializer *>(value);
    if (!init) {
      emitZeroForType(out, type);
      return;
    }

    auto elementType = arrayType->getElementType();
    const int count = arrayType->getElementCount();
    int i = 0;
    for (; i < count && i < static_cast<int>(init->size()); ++i) {
      emitDataValue(out, (*init)[i], elementType);
    }
    if (i < count) {
      emitZeroBytes(out,
                    static_cast<int64_t>(count - i) * sizeOfType(elementType));
    }
    return;
  }

  if (isPointerType(type)) {
    if (!value || dynamic_cast<ConstPtr *>(value)) {
      emitZeroForType(out, type);
      return;
    }
    throw BackendError("unsupported pointer global initializer");
  }

  if (isBoolType(type)) {
    if (auto boolValue = dynamic_cast<ConstBool *>(value)) {
      out << "    .byte " << (boolValue->getVal() ? 1 : 0) << '\n';
      return;
    }
    if (auto intValue = dynamic_cast<ConstInt *>(value)) {
      out << "    .byte " << (intValue->getVal() != 0 ? 1 : 0) << '\n';
      return;
    }
    emitZeroForType(out, type);
    return;
  }

  if (auto init = dynamic_cast<Initializer *>(value)) {
    if (init->empty()) {
      emitZeroForType(out, type);
      return;
    }
    emitDataValue(out, init->front(), type);
    return;
  }

  if (auto intValue = dynamic_cast<ConstInt *>(value)) {
    out << "    .word " << intValue->getVal() << '\n';
    return;
  }
  if (auto boolValue = dynamic_cast<ConstBool *>(value)) {
    out << "    .word " << (boolValue->getVal() ? 1 : 0) << '\n';
    return;
  }
  if (auto floatValue = dynamic_cast<ConstFloat *>(value)) {
    uint32_t bits = 0;
    float raw = floatValue->getVal();
    std::memcpy(&bits, &raw, sizeof(bits));
    out << "    .word " << bits << '\n';
    return;
  }

  emitZeroForType(out, type);
}

} // namespace

// 构造:绑定输出流。
AsmPrinter::AsmPrinter(std::ostream &out) : out_(out) {}

// 发射整个模块:先输出架构属性头,再逐个发射全局对象(数据段),
// 然后进入 .text 段逐个发射函数。
void AsmPrinter::emitModule(const MachineModule &module) {
  out_ << "# myCompiler RV64GC backend\n";
  out_ << "    .attribute arch, \"rv64gc\"\n";
  out_ << "    .option rvc\n";
  for (const GlobalObject &global : module.globals) {
    emitGlobal(global);
  }
  out_ << "    .text\n";
  for (const MachineFunction &function : module.functions) {
    emitFunction(function);
  }
}

// 发射一个全局对象。按初值特性选择段:全零入 .bss,常量入 .rodata,其余入 .data。
// 依次输出 .globl 声明、.p2align 对齐、符号标签,再发射初值(全零走 .zero,
// 否则逐元素发射),最后用 .size 记录符号大小。
void AsmPrinter::emitGlobal(const GlobalObject &global) {
  auto type = pointeeType(global);
  Value *init = global.ir ? global.ir->getUsee() : nullptr;
  const bool zero = isZeroInitializer(init, type);

  if (zero) {
    out_ << "    .section .bss\n";
  } else if (global.constant) {
    out_ << "    .section .rodata\n";
  } else {
    out_ << "    .section .data\n";
  }

  out_ << "    .globl " << global.name << '\n';
  out_ << "    .p2align " << p2Align(global.align) << '\n';
  out_ << global.name << ":\n";
  if (zero) {
    emitZeroBytes(out_, global.size);
  } else {
    emitDataValue(out_, init, type);
  }
  out_ << "    .size " << global.name << ", " << global.size << '\n';
}

// 发射单个函数,含 prologue、函数体、以及退出状态清理。
// prologue:先按 frameSize 下调 sp;若函数含 call 或有 callee-saved 需保存,
// 则从帧顶向下依次把 ra、整型/浮点被保存寄存器 sd/fsd 到栈上(每个占 8 字节)。
// 函数体:逐块发射(首块不再打标签,直接接在符号后),逐指令翻译;每条指令后
// 更新虚拟寄存器的使用计数,及时回收不再活跃的兜底临时寄存器。
// 空函数体直接发射 ret。末尾用 .size 记录函数大小并清空本函数的临时状态。
void AsmPrinter::emitFunction(const MachineFunction &function) {
  prepareVirtualRegTemps(function);
  currentFunction_ = &function;
  const bool hasCall = functionContainsCall(function);
  const std::vector<PhysReg> savedGprs = gprCalleeSavedRegsToSave(function);
  const std::vector<PhysReg> savedFprs = fprCalleeSavedRegsToSave(function);

  out_ << "    .globl " << function.name << '\n';
  out_ << "    .type " << function.name << ", @function\n";
  out_ << function.name << ":\n";
  emitStackAdjust(-function.frameSize);
  if (hasCall || !savedGprs.empty() || !savedFprs.empty()) {
    int64_t offset = function.frameSize;
    if (hasCall) {
      offset -= 8;
      emitSpRelative(out_, "sd", PhysReg::RA, offset);
    }
    for (PhysReg reg : savedGprs) {
      offset -= 8;
      emitSpRelative(out_, "sd", reg, offset);
    }
    for (PhysReg reg : savedFprs) {
      offset -= 8;
      emitSpRelative(out_, "fsd", reg, offset);
    }
  }

  if (function.blocks.empty()) {
    out_ << "    ret\n";
  } else {
    bool firstBlock = true;
    for (const MachineBasicBlock &block : function.blocks) {
      if (!firstBlock && !block.label.empty()) {
        out_ << block.label << ":\n";
      }
      for (const MachineInstr &instr : block.instrs) {
        emitInstr(instr);
        releaseInstrUses(instr);
        releaseDeadDefs(instr);
      }
      firstBlock = false;
    }
  }

  out_ << "    .size " << function.name << ", .-" << function.name << '\n';
  currentFunction_ = nullptr;
  gprVRegMap_.clear();
  fprVRegMap_.clear();
  vregUseCounts_.clear();
  vregDefCounts_.clear();
  freeGprTemps_.clear();
  freeFprTemps_.clear();
}

// 把单条 MIR 指令翻译成 RISC-V 汇编。按操作码分派:寄存器拷贝类、立即数运算类、
// 三寄存器运算类、比较/转换类、访存类、内联 memcpy/memset、控制流(call/ret/分支/跳转)等。
// 每类先做操作数形态校验(不合法即抛 BackendError),再按对应助记符和格式输出。
void AsmPrinter::emitInstr(const MachineInstr &instr) {
  switch (instr.opcode) {
  // COPY:整型寄存器拷贝,发射为伪指令 mv。
  case Opcode::COPY:
    if (instr.defs.empty() || instr.uses.empty()) {
      throw BackendError("malformed COPY instruction");
    }
    out_ << "    mv " << operandReg(instr.defs.front()) << ", "
         << operandReg(instr.uses.front()) << '\n';
    return;
  // F_COPY:浮点寄存器拷贝,发射 fmv.s。
  case Opcode::F_COPY:
    if (instr.defs.empty() || instr.uses.empty()) {
      throw BackendError("malformed F_COPY instruction");
    }
    out_ << "    fmv.s " << operandReg(instr.defs.front()) << ", "
         << operandReg(instr.uses.front()) << '\n';
    return;
  // PARALLEL_COPY:并行拷贝(用于消 phi/换位),按目的寄存器类别选 fmv.s 或 mv。
  case Opcode::PARALLEL_COPY:
    if (instr.defs.empty() || instr.uses.empty()) {
      throw BackendError("malformed PARALLEL_COPY instruction");
    }
    if (instr.defs.front().regValue.rc == RegClass::FPR32) {
      out_ << "    fmv.s " << operandReg(instr.defs.front()) << ", "
           << operandReg(instr.uses.front()) << '\n';
    } else {
      out_ << "    mv " << operandReg(instr.defs.front()) << ", "
           << operandReg(instr.uses.front()) << '\n';
    }
    return;
  // FMV_W_X / FMV_X_W:整型与浮点寄存器间按位搬移(不做数值转换)。
  case Opcode::FMV_W_X:
    if (instr.defs.empty() || instr.uses.empty()) {
      throw BackendError("malformed FMV_W_X instruction");
    }
    out_ << "    fmv.w.x " << operandReg(instr.defs.front()) << ", "
         << operandReg(instr.uses.front()) << '\n';
    return;
  case Opcode::FMV_X_W:
    if (instr.defs.empty() || instr.uses.empty()) {
      throw BackendError("malformed FMV_X_W instruction");
    }
    out_ << "    fmv.x.w " << operandReg(instr.defs.front()) << ", "
         << operandReg(instr.uses.front()) << '\n';
    return;
  // LI:加载立即数,发射伪指令 li(汇编器会展开成 lui/addi 等)。
  case Opcode::LI:
    if (instr.defs.empty() || instr.uses.empty() ||
        instr.uses.front().kind != OperandKind::Imm) {
      throw BackendError("malformed LI instruction");
    }
    out_ << "    li " << operandReg(instr.defs.front()) << ", "
         << instr.uses.front().immValue << '\n';
    return;
  // LA:加载全局符号地址,委托 emitLoadAddress 展开成 auipc+addi 的 PC 相对序列。
  case Opcode::LA:
    if (instr.defs.empty() || instr.uses.empty() ||
        instr.uses.front().kind != OperandKind::Global) {
      throw BackendError("malformed LA instruction");
    }
    emitLoadAddress(instr.defs.front(), instr.uses.front().symbol);
    return;
  // 立即数运算类(addi/andi/slli/srai 等):形如 op rd, rs, imm。
  // 先由操作码选出助记符,再校验立即数在 12 位范围内后发射;
  // 基址可能来自栈帧下标,memoryBaseReg 负责把它换成寄存器名并把偏移并入 imm。
  case Opcode::ADDI:
  case Opcode::ADDIW:
  case Opcode::ANDI:
  case Opcode::ORI:
  case Opcode::XORI:
  case Opcode::SLLI:
  case Opcode::SLLIW:
  case Opcode::SRAI:
  case Opcode::SRAIW:
  case Opcode::SRLIW:
    if (instr.defs.empty() || instr.uses.size() < 2 ||
        instr.uses[1].kind != OperandKind::Imm) {
      throw BackendError("malformed immediate instruction");
    }
    {
      const char *mnemonic = "";
      switch (instr.opcode) {
      case Opcode::ADDI:
        mnemonic = "addi";
        break;
      case Opcode::ADDIW:
        mnemonic = "addiw";
        break;
      case Opcode::ANDI:
        mnemonic = "andi";
        break;
      case Opcode::ORI:
        mnemonic = "ori";
        break;
      case Opcode::XORI:
        mnemonic = "xori";
        break;
      case Opcode::SLLI:
        mnemonic = "slli";
        break;
      case Opcode::SLLIW:
        mnemonic = "slliw";
        break;
      case Opcode::SRAI:
        mnemonic = "srai";
        break;
      case Opcode::SRAIW:
        mnemonic = "sraiw";
        break;
      case Opcode::SRLIW:
        mnemonic = "srliw";
        break;
      default:
        break;
      }

      int64_t imm = instr.uses[1].immValue;
      auto [base, scratch] = memoryBaseReg(instr.uses[0], imm);
      if (!fitsSigned12(imm)) {
        throw BackendError("immediate operand out of signed 12-bit range");
      }
      out_ << "    " << mnemonic << ' ' << operandReg(instr.defs.front())
           << ", " << base << ", " << imm << '\n';
      releaseScratchGpr(scratch);
    }
    return;
  // 三寄存器运算类(算术/逻辑/移位/比较/浮点二元):形如 op rd, rs1, rs2。
  // 涵盖整型 add/sub/mul/div/rem、位运算、变量移位、slt,以及浮点 fadd.s/feq.s 等。
  case Opcode::ADD:
  case Opcode::ADDW:
  case Opcode::SUB:
  case Opcode::SUBW:
  case Opcode::MUL:
  case Opcode::MULW:
  case Opcode::DIV:
  case Opcode::DIVW:
  case Opcode::DIVUW:
  case Opcode::REM:
  case Opcode::REMW:
  case Opcode::REMUW:
  case Opcode::AND:
  case Opcode::OR:
  case Opcode::XOR:
  case Opcode::SLL:
  case Opcode::SLLW:
  case Opcode::SRAW:
  case Opcode::SRLW:
  case Opcode::SLT:
  case Opcode::SLTU:
  case Opcode::FEQ_S:
  case Opcode::FLT_S:
  case Opcode::FLE_S:
  case Opcode::FADD_S:
  case Opcode::FSUB_S:
  case Opcode::FMUL_S:
  case Opcode::FDIV_S:
    if (instr.defs.empty() || instr.uses.size() < 2) {
      throw BackendError("malformed binary instruction");
    }
    {
      const char *mnemonic = "";
      switch (instr.opcode) {
      case Opcode::ADD:
        mnemonic = "add";
        break;
      case Opcode::ADDW:
        mnemonic = "addw";
        break;
      case Opcode::SUB:
        mnemonic = "sub";
        break;
      case Opcode::SUBW:
        mnemonic = "subw";
        break;
      case Opcode::MUL:
        mnemonic = "mul";
        break;
      case Opcode::MULW:
        mnemonic = "mulw";
        break;
      case Opcode::DIV:
        mnemonic = "div";
        break;
      case Opcode::DIVW:
        mnemonic = "divw";
        break;
      case Opcode::DIVUW:
        mnemonic = "divuw";
        break;
      case Opcode::REM:
        mnemonic = "rem";
        break;
      case Opcode::REMW:
        mnemonic = "remw";
        break;
      case Opcode::REMUW:
        mnemonic = "remuw";
        break;
      case Opcode::AND:
        mnemonic = "and";
        break;
      case Opcode::OR:
        mnemonic = "or";
        break;
      case Opcode::XOR:
        mnemonic = "xor";
        break;
      case Opcode::SLL:
        mnemonic = "sll";
        break;
      case Opcode::SLLW:
        mnemonic = "sllw";
        break;
      case Opcode::SRAW:
        mnemonic = "sraw";
        break;
      case Opcode::SRLW:
        mnemonic = "srlw";
        break;
      case Opcode::SLT:
        mnemonic = "slt";
        break;
      case Opcode::SLTU:
        mnemonic = "sltu";
        break;
      case Opcode::FEQ_S:
        mnemonic = "feq.s";
        break;
      case Opcode::FLT_S:
        mnemonic = "flt.s";
        break;
      case Opcode::FLE_S:
        mnemonic = "fle.s";
        break;
      case Opcode::FADD_S:
        mnemonic = "fadd.s";
        break;
      case Opcode::FSUB_S:
        mnemonic = "fsub.s";
        break;
      case Opcode::FMUL_S:
        mnemonic = "fmul.s";
        break;
      case Opcode::FDIV_S:
        mnemonic = "fdiv.s";
        break;
      default:
        break;
      }
      out_ << "    " << mnemonic << ' ' << operandReg(instr.defs.front())
           << ", " << operandReg(instr.uses[0]) << ", "
           << operandReg(instr.uses[1]) << '\n';
    }
    return;
  // SEQZ/SNEZ:与零比较的伪指令,rd = (rs == 0) 或 (rs != 0)。
  case Opcode::SEQZ:
  case Opcode::SNEZ:
    if (instr.defs.empty() || instr.uses.empty()) {
      throw BackendError("malformed zero-compare instruction");
    }
    out_ << "    " << (instr.opcode == Opcode::SEQZ ? "seqz" : "snez") << ' '
         << operandReg(instr.defs.front()) << ", "
         << operandReg(instr.uses.front()) << '\n';
    return;
  // 浮点与整型互转:FCVT_W_S 用 rtz(向零舍入)取整为 int,FCVT_S_W 反向转。
  case Opcode::FCVT_W_S:
  case Opcode::FCVT_S_W:
    if (instr.defs.empty() || instr.uses.empty()) {
      throw BackendError("malformed float conversion instruction");
    }
    if (instr.opcode == Opcode::FCVT_W_S) {
      out_ << "    fcvt.w.s " << operandReg(instr.defs.front()) << ", "
           << operandReg(instr.uses.front()) << ", rtz\n";
    } else {
      out_ << "    fcvt.s.w " << operandReg(instr.defs.front()) << ", "
           << operandReg(instr.uses.front()) << '\n';
    }
    return;
  // 访存类(lbu/sb/lw/sw/ld/sd/flw/fsw):统一委托 emitMemoryInstr 处理偏移与基址。
  case Opcode::LBU:
  case Opcode::SB:
  case Opcode::LW:
  case Opcode::SW:
  case Opcode::LD:
  case Opcode::SD:
  case Opcode::FLW:
  case Opcode::FSW:
    emitMemoryInstr(instr);
    return;
  // MEMCPY_INLINE:把 [src, size) 复制到 [dst, size)。
  // 就地展开成两段循环:先按 4 字节字循环搬 size/4 个字,再按字节循环搬余下 size%4 个字节。
  // 使用保留寄存器(dst/src/tmp/计数器)与本指令唯一的一组 .L 局部标签,不占用分配寄存器。
  // 第 4 个操作数是 ISel 静态证明的对齐字节数;align<4 时只允许字节访存,跳过字循环。
  case Opcode::MEMCPY_INLINE: {
    if (instr.uses.size() < 4 || instr.uses[2].kind != OperandKind::Imm ||
        instr.uses[3].kind != OperandKind::Imm) {
      throw BackendError("malformed MEMCPY_INLINE instruction");
    }
    const std::string dst = operandReg(instr.uses[0]);
    const std::string src = operandReg(instr.uses[1]);
    const int64_t size = instr.uses[2].immValue;
    const int64_t align = instr.uses[3].immValue;
    if (size < 0 || align < 1 || (align & (align - 1)) != 0) {
      throw BackendError("negative MEMCPY_INLINE size");
    }

    const int id = pcrelId_++;
    const std::string wordLoop = ".Lmemcpy" + std::to_string(id) + ".loopw";
    const std::string byteLoop = ".Lmemcpy" + std::to_string(id) + ".loopb";
    const std::string byteStart = ".Lmemcpy" + std::to_string(id) + ".bytes";
    const std::string done = ".Lmemcpy" + std::to_string(id) + ".done";
    const bool wordAligned = align >= 4;
    const int64_t wordCount = wordAligned ? size / 4 : 0;
    const int64_t byteCount = wordAligned ? size % 4 : size;

    out_ << "    mv " << regName(kReservedMemcpyDst) << ", " << dst << '\n';
    out_ << "    mv " << regName(kReservedMemcpySrc) << ", " << src << '\n';
    out_ << "    li " << regName(kReservedGprScratch) << ", " << wordCount
         << '\n';
    out_ << "    beqz " << regName(kReservedGprScratch) << ", " << byteStart
         << '\n';
    out_ << wordLoop << ":\n";
    out_ << "    lw " << regName(kReservedMemcpyTmp) << ", 0("
         << regName(kReservedMemcpySrc) << ")\n";
    out_ << "    sw " << regName(kReservedMemcpyTmp) << ", 0("
         << regName(kReservedMemcpyDst) << ")\n";
    out_ << "    addi " << regName(kReservedMemcpySrc) << ", "
         << regName(kReservedMemcpySrc) << ", 4\n";
    out_ << "    addi " << regName(kReservedMemcpyDst) << ", "
         << regName(kReservedMemcpyDst) << ", 4\n";
    out_ << "    addi " << regName(kReservedGprScratch) << ", "
         << regName(kReservedGprScratch) << ", -1\n";
    out_ << "    bnez " << regName(kReservedGprScratch) << ", " << wordLoop
         << '\n';
    out_ << byteStart << ":\n";
    out_ << "    li " << regName(kReservedGprScratch) << ", " << byteCount
         << '\n';
    out_ << "    beqz " << regName(kReservedGprScratch) << ", " << done << '\n';
    out_ << byteLoop << ":\n";
    out_ << "    lbu " << regName(kReservedMemcpyTmp) << ", 0("
         << regName(kReservedMemcpySrc) << ")\n";
    out_ << "    sb " << regName(kReservedMemcpyTmp) << ", 0("
         << regName(kReservedMemcpyDst) << ")\n";
    out_ << "    addi " << regName(kReservedMemcpySrc) << ", "
         << regName(kReservedMemcpySrc) << ", 1\n";
    out_ << "    addi " << regName(kReservedMemcpyDst) << ", "
         << regName(kReservedMemcpyDst) << ", 1\n";
    out_ << "    addi " << regName(kReservedGprScratch) << ", "
         << regName(kReservedGprScratch) << ", -1\n";
    out_ << "    bnez " << regName(kReservedGprScratch) << ", " << byteLoop
         << '\n';
    out_ << done << ":\n";
    return;
  }
  // MEMSET_INLINE:把 [dst, size) 填充为字节值 byte。
  // 把单字节复制成 4 份得到一个字模式(byte*0x01010101),先按字循环填 size/4 个字,
  // 再按字节循环填余下字节。结构与 MEMCPY_INLINE 对称。
  // 第 4 个操作数是 ISel 静态证明的对齐字节数;align<4 时只允许字节访存,跳过字循环。
  case Opcode::MEMSET_INLINE: {
    if (instr.uses.size() < 4 || instr.uses[1].kind != OperandKind::Imm ||
        instr.uses[2].kind != OperandKind::Imm ||
        instr.uses[3].kind != OperandKind::Imm) {
      throw BackendError("malformed MEMSET_INLINE instruction");
    }
    const std::string dst = operandReg(instr.uses[0]);
    const int64_t byte = instr.uses[1].immValue;
    const int64_t size = instr.uses[2].immValue;
    const int64_t align = instr.uses[3].immValue;
    if (byte < 0 || byte > 255 || size < 0 || align < 1 ||
        (align & (align - 1)) != 0) {
      throw BackendError("invalid MEMSET_INLINE operands");
    }

    const int id = pcrelId_++;
    const std::string wordLoop = ".Lmemset" + std::to_string(id) + ".loopw";
    const std::string byteLoop = ".Lmemset" + std::to_string(id) + ".loopb";
    const std::string byteStart = ".Lmemset" + std::to_string(id) + ".bytes";
    const std::string done = ".Lmemset" + std::to_string(id) + ".done";
    const bool wordAligned = align >= 4;
    const int64_t wordCount = wordAligned ? size / 4 : 0;
    const int64_t byteCount = wordAligned ? size % 4 : size;
    const uint32_t repeated = static_cast<uint32_t>(byte) * 0x01010101u;

    out_ << "    mv " << regName(kReservedMemcpyDst) << ", " << dst << '\n';
    out_ << "    li " << regName(kReservedMemcpyTmp) << ", "
         << static_cast<uint64_t>(repeated) << '\n';
    out_ << "    li " << regName(kReservedGprScratch) << ", " << wordCount
         << '\n';
    out_ << "    beqz " << regName(kReservedGprScratch) << ", " << byteStart
         << '\n';
    out_ << wordLoop << ":\n";
    out_ << "    sw " << regName(kReservedMemcpyTmp) << ", 0("
         << regName(kReservedMemcpyDst) << ")\n";
    out_ << "    addi " << regName(kReservedMemcpyDst) << ", "
         << regName(kReservedMemcpyDst) << ", 4\n";
    out_ << "    addi " << regName(kReservedGprScratch) << ", "
         << regName(kReservedGprScratch) << ", -1\n";
    out_ << "    bnez " << regName(kReservedGprScratch) << ", " << wordLoop
         << '\n';
    out_ << byteStart << ":\n";
    out_ << "    li " << regName(kReservedGprScratch) << ", " << byteCount
         << '\n';
    out_ << "    beqz " << regName(kReservedGprScratch) << ", " << done << '\n';
    out_ << byteLoop << ":\n";
    out_ << "    sb " << regName(kReservedMemcpyTmp) << ", 0("
         << regName(kReservedMemcpyDst) << ")\n";
    out_ << "    addi " << regName(kReservedMemcpyDst) << ", "
         << regName(kReservedMemcpyDst) << ", 1\n";
    out_ << "    addi " << regName(kReservedGprScratch) << ", "
         << regName(kReservedGprScratch) << ", -1\n";
    out_ << "    bnez " << regName(kReservedGprScratch) << ", " << byteLoop
         << '\n';
    out_ << done << ":\n";
    return;
  }
  // RET:发射 epilogue 再返回。按 prologue 的相反顺序,从相同偏移处
  // ld/fld 恢复 ra 与被保存寄存器,然后上调 sp 还原帧,最后 ret。
  case Opcode::RET:
    if (currentFunction_) {
      const bool containsCall = functionContainsCall(*currentFunction_);
      const std::vector<PhysReg> savedGprs =
          gprCalleeSavedRegsToSave(*currentFunction_);
      const std::vector<PhysReg> savedFprs =
          fprCalleeSavedRegsToSave(*currentFunction_);
      int64_t offset = currentFunction_->frameSize;
      if (containsCall) {
        offset -= 8;
        emitSpRelative(out_, "ld", PhysReg::RA, offset);
      }
      for (PhysReg reg : savedGprs) {
        offset -= 8;
        emitSpRelative(out_, "ld", reg, offset);
      }
      for (PhysReg reg : savedFprs) {
        offset -= 8;
        emitSpRelative(out_, "fld", reg, offset);
      }
    }
    if (currentFunction_) {
      emitStackAdjust(currentFunction_->frameSize);
    }
    out_ << "    ret\n";
    return;
  // CALL:直接调用全局符号,发射 call symbol。
  case Opcode::CALL:
    if (instr.uses.empty() || instr.uses.front().kind != OperandKind::Global) {
      throw BackendError("malformed CALL instruction");
    }
    out_ << "    call " << instr.uses.front().symbol << '\n';
    return;
  // BNEZ/BEQZ:与零比较的条件分支,操作数为 rs 与目标块标签。
  case Opcode::BNEZ:
    if (instr.uses.size() < 2 || instr.uses[1].kind != OperandKind::Block) {
      throw BackendError("malformed BNEZ instruction");
    }
    out_ << "    bnez " << operandReg(instr.uses[0]) << ", "
         << instr.uses[1].blockLabel << '\n';
    return;
  case Opcode::BEQZ:
    if (instr.uses.size() < 2 || instr.uses[1].kind != OperandKind::Block) {
      throw BackendError("malformed BEQZ instruction");
    }
    out_ << "    beqz " << operandReg(instr.uses[0]) << ", "
         << instr.uses[1].blockLabel << '\n';
    return;
  // 两寄存器条件分支(beq/bne/blt/bge):操作数为 rs1、rs2 与目标块标签。
  case Opcode::BEQ:
  case Opcode::BNE:
  case Opcode::BLT:
  case Opcode::BGE:
    if (instr.uses.size() < 3 || instr.uses[2].kind != OperandKind::Block) {
      throw BackendError("malformed conditional branch instruction");
    }
    switch (instr.opcode) {
    case Opcode::BEQ:
      out_ << "    beq ";
      break;
    case Opcode::BNE:
      out_ << "    bne ";
      break;
    case Opcode::BLT:
      out_ << "    blt ";
      break;
    case Opcode::BGE:
      out_ << "    bge ";
      break;
    default:
      break;
    }
    out_ << operandReg(instr.uses[0]) << ", " << operandReg(instr.uses[1])
         << ", " << instr.uses[2].blockLabel << '\n';
    return;
  // J:无条件跳转到目标块标签。
  case Opcode::J:
    if (instr.uses.empty() || instr.uses.front().kind != OperandKind::Block) {
      throw BackendError("malformed J instruction");
    }
    out_ << "    j " << instr.uses.front().blockLabel << '\n';
    return;
  // NOP:空操作。
  case Opcode::NOP:
    out_ << "    nop\n";
    return;
  // 到这里说明存在未被 lower 的临时 MIR 操作码，属于后端 bug，抛错。
  default:
    throw BackendError("unsupported temporary MIR opcode " +
                       std::to_string(static_cast<int>(instr.opcode)));
  }
}

// 为兜底虚拟寄存器指派做准备(在寄存器分配未覆盖时才用到)。
// 清空映射与计数,按是否叶子函数选择整型/浮点临时寄存器池(逆序压栈,便于从尾部弹出),
// 再统计每个虚拟寄存器的 use/def 次数,供后续按活跃度回收临时寄存器。
// 注:PARALLEL_COPY 的 def 计数额外加一,以避免其目的寄存器被过早回收。
void AsmPrinter::prepareVirtualRegTemps(const MachineFunction &function) {
  gprVRegMap_.clear();
  fprVRegMap_.clear();
  vregUseCounts_.clear();
  vregDefCounts_.clear();
  if (functionContainsCall(function)) {
    freeGprTemps_.assign(kNonLeafGprScratchRegs.rbegin(),
                         kNonLeafGprScratchRegs.rend());
  } else {
    freeGprTemps_.assign(kGprScratchRegs.rbegin(), kGprScratchRegs.rend());
  }
  if (functionContainsCall(function) && functionUsesFprVirtualRegs(function)) {
    freeFprTemps_.assign(kNonLeafFprScratchRegs.rbegin(),
                         kNonLeafFprScratchRegs.rend());
  } else {
    freeFprTemps_.assign(kFprScratchRegs.rbegin(), kFprScratchRegs.rend());
  }

  for (const MachineBasicBlock &block : function.blocks) {
    for (const MachineInstr &instr : block.instrs) {
      for (const MachineOperand &use : instr.uses) {
        if (use.kind != OperandKind::Reg || use.regValue.physical) {
          continue;
        }
        ++vregUseCounts_[use.regValue.id];
      }
      for (const MachineOperand &def : instr.defs) {
        if (def.kind != OperandKind::Reg || def.regValue.physical) {
          continue;
        }
        ++vregDefCounts_[def.regValue.id];
        if (instr.opcode == Opcode::PARALLEL_COPY) {
          ++vregDefCounts_[def.regValue.id];
        }
      }
    }
  }
}

// 指令发射后:对其每个 use 递减使用计数,计数归零则回收对应临时寄存器。
void AsmPrinter::releaseInstrUses(const MachineInstr &instr) {
  for (const MachineOperand &use : instr.uses) {
    releaseOperandUse(use);
  }
}

// 指令发射后:若某 def 的虚拟寄存器只被定义一次且已无剩余使用,则回收它(死定义)。
void AsmPrinter::releaseDeadDefs(const MachineInstr &instr) {
  for (const MachineOperand &def : instr.defs) {
    if (def.kind != OperandKind::Reg || def.regValue.physical) {
      continue;
    }
    auto defs = vregDefCounts_.find(def.regValue.id);
    if (defs != vregDefCounts_.end() && defs->second > 1) {
      continue;
    }
    auto remainingUses = vregUseCounts_.find(def.regValue.id);
    if (remainingUses == vregUseCounts_.end() || remainingUses->second <= 0) {
      releaseVirtualReg(def.regValue);
    }
  }
}

// 递减单个 use 操作数的剩余使用计数;归零时若无多重定义待处理则回收其临时寄存器。
void AsmPrinter::releaseOperandUse(const MachineOperand &operand) {
  if (operand.kind != OperandKind::Reg || operand.regValue.physical) {
    return;
  }

  auto remainingUses = vregUseCounts_.find(operand.regValue.id);
  if (remainingUses == vregUseCounts_.end()) {
    return;
  }

  --remainingUses->second;
  if (remainingUses->second <= 0) {
    vregUseCounts_.erase(remainingUses);
    auto defs = vregDefCounts_.find(operand.regValue.id);
    if (defs != vregDefCounts_.end() && defs->second > 1) {
      return;
    }
    releaseVirtualReg(operand.regValue);
  }
}

// 把某虚拟寄存器占用的物理临时寄存器归还对应空闲池,并从映射表中移除。
void AsmPrinter::releaseVirtualReg(MachineReg reg) {
  if (reg.rc == RegClass::FPR32) {
    auto mapped = fprVRegMap_.find(reg.id);
    if (mapped == fprVRegMap_.end()) {
      return;
    }
    freeFprTemps_.push_back(mapped->second);
    fprVRegMap_.erase(mapped);
    return;
  }

  auto mapped = gprVRegMap_.find(reg.id);
  if (mapped == gprVRegMap_.end()) {
    return;
  }
  freeGprTemps_.push_back(mapped->second);
  gprVRegMap_.erase(mapped);
}

// 从整型临时池借一个物理寄存器(池空则报错),用于发射期临时值。
PhysReg AsmPrinter::acquireScratchGpr() {
  if (freeGprTemps_.empty()) {
    throw BackendError("RISC-V GPR scratch register pool exhausted");
  }
  PhysReg reg = freeGprTemps_.back();
  freeGprTemps_.pop_back();
  return reg;
}

// 归还借出的整型临时寄存器(NoReg 表示未借,忽略)。
void AsmPrinter::releaseScratchGpr(PhysReg reg) {
  if (reg == PhysReg::NoReg) {
    return;
  }
  freeGprTemps_.push_back(reg);
}

// 展开取全局地址(la 的手工实现)。发射一个 .L 局部锚点标签,再用
// auipc rd, %pcrel_hi(symbol) 取高位,addi rd, rd, %pcrel_lo(锚点) 补低位,
// 得到 symbol 的 PC 相对地址。锚点标签用自增 id 保证唯一,%pcrel_lo 需回指它。
void AsmPrinter::emitLoadAddress(const MachineOperand &dst,
                                 const std::string &symbol) {
  const std::string reg = operandReg(dst);
  const int id = pcrelId_++;
  out_ << ".Lpcrel" << id << ":\n";
  out_ << "    auipc " << reg << ", %pcrel_hi(" << symbol << ")\n";
  out_ << "    addi " << reg << ", " << reg << ", %pcrel_lo(.Lpcrel" << id
       << ")\n";
}

// 调整栈指针 sp(prologue 传负值下调、epilogue 传正值还原)。
// 量为零不发射;落在 12 位内直接 addi sp, sp, amount;否则先 li 到scratch 寄存器再 add。
void AsmPrinter::emitStackAdjust(int64_t amount) {
  if (amount == 0) {
    return;
  }

  if (fitsSigned12(amount)) {
    out_ << "    addi sp, sp, " << amount << '\n';
    return;
  }

  out_ << "    li " << regName(kReservedGprScratch) << ", " << amount << '\n';
  out_ << "    add sp, sp, " << regName(kReservedGprScratch) << '\n';
}

// 发射访存指令(load/store)。先按操作码判定读写方向并选助记符,
// store 形如 op rs, offset(base),load 形如 op rd, offset(base)。
// 基址与偏移交给 memoryBaseReg 归一(可能来自栈帧下标或需大偏移展开),
// 归一后偏移仍须落在 12 位范围内,否则报错。用毕归还可能借出的scratch 寄存器。
void AsmPrinter::emitMemoryInstr(const MachineInstr &instr) {
  const bool isStore =
      instr.opcode == Opcode::SB || instr.opcode == Opcode::SW ||
      instr.opcode == Opcode::SD || instr.opcode == Opcode::FSW;
  const char *mnemonic = "";
  switch (instr.opcode) {
  case Opcode::LBU:
    mnemonic = "lbu";
    break;
  case Opcode::SB:
    mnemonic = "sb";
    break;
  case Opcode::LW:
    mnemonic = "lw";
    break;
  case Opcode::SW:
    mnemonic = "sw";
    break;
  case Opcode::LD:
    mnemonic = "ld";
    break;
  case Opcode::SD:
    mnemonic = "sd";
    break;
  case Opcode::FLW:
    mnemonic = "flw";
    break;
  case Opcode::FSW:
    mnemonic = "fsw";
    break;
  default:
    throw BackendError("unsupported memory instruction");
  }

  if (isStore) {
    if (instr.uses.size() < 3 || instr.uses[2].kind != OperandKind::Imm) {
      throw BackendError("malformed store instruction");
    }
    int64_t offset = instr.uses[2].immValue;
    auto [base, scratch] = memoryBaseReg(instr.uses[1], offset);
    if (!fitsSigned12(offset)) {
      throw BackendError("store offset out of signed 12-bit range");
    }
    out_ << "    " << mnemonic << ' ' << operandReg(instr.uses[0]) << ", "
         << offset << '(' << base << ")\n";
    releaseScratchGpr(scratch);
    return;
  }

  if (instr.defs.empty() || instr.uses.size() < 2 ||
      instr.uses[1].kind != OperandKind::Imm) {
    throw BackendError("malformed load instruction");
  }
  int64_t offset = instr.uses[1].immValue;
  auto [base, scratch] = memoryBaseReg(instr.uses[0], offset);
  if (!fitsSigned12(offset)) {
    throw BackendError("load offset out of signed 12-bit range");
  }
  out_ << "    " << mnemonic << ' ' << operandReg(instr.defs.front()) << ", "
       << offset << '(' << base << ")\n";
  releaseScratchGpr(scratch);
}

// 归一化访存基址,返回 {基址寄存器名, 需归还的scratch 寄存器}。
// 基址为寄存器时直接用其名;为栈帧下标时,校验下标有效后把该栈对象的偏移
// 累加进 offset 并以 sp 为基址。若最终偏移超出 12 位范围,则先把偏移 li 到 t6、
// 再 add 到基址得到新基址,并把 offset 清零(改用 0(newbase) 的形式)。
std::pair<std::string, PhysReg>
AsmPrinter::memoryBaseReg(const MachineOperand &base, int64_t &offset) {
  std::string baseReg;
  if (base.kind == OperandKind::Reg) {
    baseReg = operandReg(base);
  } else if (base.kind == OperandKind::FrameIndex) {
    if (!currentFunction_) {
      throw BackendError("frame-index operand without active function");
    }

    const int frameIndex = base.frameIndexValue;
    if (frameIndex < 0 ||
        frameIndex >= static_cast<int>(currentFunction_->stackObjects.size())) {
      throw BackendError("invalid frame-index operand");
    }

    offset += currentFunction_->stackObjects[frameIndex].offset;
    baseReg = regName(PhysReg::SP);
  } else {
    throw BackendError("expected register or frame-index memory base");
  }

  if (fitsSigned12(offset)) {
    return {baseReg, PhysReg::NoReg};
  }

  PhysReg scratch = kReservedGprScratch;
  const std::string scratchReg = regName(scratch);
  out_ << "    li " << scratchReg << ", " << offset << '\n';
  out_ << "    add " << scratchReg << ", " << baseReg << ", " << scratchReg
       << '\n';
  offset = 0;
  return {scratchReg, PhysReg::NoReg};
}

// 为一个虚拟寄存器指派物理临时寄存器(兜底路径)。已指派过则复用映射;
// 否则从对应类别(浮点/整型)的空闲池取一个记入映射,池空则报错。
PhysReg AsmPrinter::assignVirtualReg(MachineReg reg) {
  if (reg.rc == RegClass::FPR32) {
    auto existing = fprVRegMap_.find(reg.id);
    if (existing != fprVRegMap_.end()) {
      return existing->second;
    }
    if (freeFprTemps_.empty()) {
      throw BackendError("RISC-V FPR temporary register pool exhausted");
    }
    PhysReg phys = freeFprTemps_.back();
    freeFprTemps_.pop_back();
    fprVRegMap_.emplace(reg.id, phys);
    return phys;
  }

  if (reg.rc == RegClass::GPR32 || reg.rc == RegClass::GPR64) {
    auto existing = gprVRegMap_.find(reg.id);
    if (existing != gprVRegMap_.end()) {
      return existing->second;
    }
    if (freeGprTemps_.empty()) {
      throw BackendError("RISC-V GPR temporary register pool exhausted");
    }
    PhysReg phys = freeGprTemps_.back();
    freeGprTemps_.pop_back();
    gprVRegMap_.emplace(reg.id, phys);
    return phys;
  }

  throw BackendError("unsupported virtual register class");
}

// 把寄存器操作数转成汇编寄存器名。要求确为寄存器且有效;物理寄存器直接返回 ABI 名,
// 虚拟寄存器则通过兜底指派拿到物理寄存器后返回其名。这是所有指令打印寄存器的统一入口。
std::string AsmPrinter::operandReg(const MachineOperand &operand) {
  if (operand.kind != OperandKind::Reg) {
    throw BackendError("expected register operand");
  }
  const MachineReg reg = operand.regValue;
  if (!reg.isValid()) {
    throw BackendError("invalid register operand");
  }
  if (reg.physical) {
    return reg.str();
  }
  return regName(assignVirtualReg(reg));
}

} // namespace riscv
