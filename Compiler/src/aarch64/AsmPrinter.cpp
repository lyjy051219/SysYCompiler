// AsmPrinter.cpp —— 把已完成寄存器分配与frame lowering的 MIR 发射为 ARM64(ARMv8-A) 汇编文本。
// 这是 aarch64 后端里唯一真正做 ISA 翻译的地方:MIR 各操作码沿用 rv 命名(ADD/LW/BLT...),
// 但在这里被翻译成 AArch64 助记符,例如:
//   - 寄存器按操作数 RegClass 选宽度前缀:GPR32 用 w、GPR64 用 x、FPR32 用 s;
//     31 号编码在多数指令里是零寄存器(wzr/xzr),在 sp 相关形式里则是栈指针,需区分。
//   - LI 用 movz/movk 序列物化 64 位立即数;LA 用 adrp + add :lo12: 取全局地址。
//   - REM 无对应指令,展开成 sdiv/udiv 再 msub;SLT/浮点比较用 cmp/fcmp + cset;
//     两寄存器条件分支用 cmp + b.cond;逻辑立即数按位掩码可编码性决定直接编码或先物化。
//   - 融合指令 madd/msub/smaddl/smsubl、条件选择 csel、访存的 [base, index, lsl/sxtw] 索引寻址。
//   - prologue 与 epilogue 用 str/ldr 把 x30（lr）与用到的 callee-saved 存到 sp 相对槽。
// 另外还负责:发射模块头与全局数据段(.bss/.rodata/.data)、内联 memcpy/memset 展开,
// 以及为寄存器分配未覆盖时残留的虚拟寄存器做一个兜底的临时寄存器指派(基于使用计数回收)。
// 命名约定:局部标签统一用 .L 前缀并配自增 id 保证唯一,访存写作 [base, #offset]。
#include "aarch64/AsmPrinter.hpp"

#include "lib/CFG.hpp"
#include "aarch64/Target.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <ostream>
#include <string>

namespace aarch64 {
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

// ARM64 加/减立即数编码范围：0..4095，或该范围值左移 12 位。
bool fitsAddImm(int64_t value) {
  if (value < 0) {
    return false;
  }
  return value <= 0xfff || ((value & 0xfff) == 0 && (value >> 12) <= 0xfff);
}

// 保留寄存器：全部落在不可分配的 T3/T4/T5(实为 caller-saved 临时，
// 由 memcpy/memset 的 implicitDefs 声明 clobber) 与 T6(=x16, IP0)。
constexpr PhysReg kReservedMemcpyTmp = PhysReg::T3;
constexpr PhysReg kReservedMemcpySrc = PhysReg::T4;
constexpr PhysReg kReservedMemcpyDst = PhysReg::T5;
constexpr PhysReg kReservedGprScratch = PhysReg::T6;

// 取寄存器的 64 位 x 名（GPR）或 s 名（FPR）。
const char *rawRegName(PhysReg reg) { return regName(reg); }

// 按 RegClass 选择寄存器宽度前缀：GPR32 用 w，GPR64 用 x，FPR32 用 s。
std::string widthRegName(PhysReg reg, RegClass rc) {
  std::string base = rawRegName(reg);
  if (rc == RegClass::FPR32) {
    return base; // s 形式
  }
  if (rc == RegClass::GPR64) {
    return base; // x/sp/xzr 形式
  }
  // GPR32：把 x 前缀换成 w，特殊寄存器单独处理。
  if (base == "sp") {
    return "wsp";
  }
  if (base == "xzr") {
    return "wzr";
  }
  if (!base.empty() && base[0] == 'x') {
    return "w" + base.substr(1);
  }
  return base;
}

// 把单精度寄存器名 sN 转成对应的 dN（callee-saved 保存/恢复用 64 位形式）。
std::string doubleRegName(PhysReg reg) {
  std::string base = rawRegName(reg);
  if (!base.empty() && base[0] == 's') {
    return "d" + base.substr(1);
  }
  return base;
}

// 物化一个 64 位立即数到指定寄存器（x 形式），用 movz/movk 序列。
void emitMoveImm(std::ostream &out, const std::string &reg, int64_t value) {
  const uint64_t bits = static_cast<uint64_t>(value);
  // 处理 0
  if (bits == 0) {
    out << "    mov " << reg << ", #0\n";
    return;
  }
  bool first = true;
  for (int shift = 0; shift < 64; shift += 16) {
    const uint64_t part = (bits >> shift) & 0xffffULL;
    if (part == 0) {
      continue;
    }
    if (first) {
      out << "    movz " << reg << ", #" << part << ", lsl #" << shift << '\n';
      first = false;
    } else {
      out << "    movk " << reg << ", #" << part << ", lsl #" << shift << '\n';
    }
  }
  if (first) {
    out << "    mov " << reg << ", #0\n";
  }
}

// 判断一个立即数能否作为 ARM64 逻辑指令的位掩码立即数直接编码。
// 规则：在寄存器宽度内，该值必须是「某个 2 4 8 16 32 64 位元素内一段连续
// 的 1 经旋转后」再周期性复制而成，且不能全 0 或全 1。
// width 取 32 或 64。能编码返回 true，此时可直接写 and wd wn imm。
bool isLogicalImmediate(uint64_t imm, unsigned width) {
  if (width == 32) {
    imm &= 0xffffffffULL;
    imm |= imm << 32;
  }
  if (imm == 0 || imm == ~0ULL) {
    return false;
  }
  // 递归折半找出最小周期元素大小。
  unsigned elemSize = 64;
  for (unsigned sz = 32; sz >= 2; sz /= 2) {
    const uint64_t mask = (sz == 64) ? ~0ULL : ((1ULL << sz) - 1);
    if ((imm & mask) == ((imm >> sz) & mask)) {
      elemSize = sz;
    } else {
      break;
    }
  }
  const uint64_t elemMask = (elemSize == 64) ? ~0ULL : ((1ULL << elemSize) - 1);
  const uint64_t elem = imm & elemMask;
  const unsigned ones = static_cast<unsigned>(__builtin_popcountll(elem));
  if (ones == 0 || ones == elemSize) {
    return false;
  }
  // 元素旋转后若能变成低位 ones 个 1 的连续块，则合法。
  const uint64_t target = (ones == 64) ? ~0ULL : ((1ULL << ones) - 1);
  for (unsigned r = 0; r < elemSize; ++r) {
    const uint64_t rot =
        ((elem >> r) | (elem << (elemSize - r))) & elemMask;
    if (rot == target) {
      return true;
    }
  }
  return false;
}

// 保存/恢复 sp 相对偏移的寄存器。isFloat 时用 64 位 d 形式。
void emitSpRelative(std::ostream &out, bool isLoad, PhysReg reg,
                    int64_t offset, bool isFloat) {
  const char *mnem = isLoad ? "ldr" : "str";
  const std::string rn = isFloat ? doubleRegName(reg) : rawRegName(reg);
  if (offset >= 0 && offset <= 32760 && (offset % 8) == 0) {
    out << "    " << mnem << ' ' << rn << ", [sp, #" << offset << "]\n";
    return;
  }
  // 大偏移：先算出地址到 IP0(x16)。
  emitMoveImm(out, regName(kReservedGprScratch), offset);
  out << "    add " << regName(kReservedGprScratch) << ", sp, "
      << regName(kReservedGprScratch) << '\n';
  out << "    " << mnem << ' ' << rn << ", [" << regName(kReservedGprScratch)
      << "]\n";
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

// 判断函数是否含 CALL 指令(即非叶子函数),决定 prologue 是否保存 x30(lr) 及临时池选择。
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

// 发射整个模块:先输出后端标识头,再逐个发射全局对象(数据段),
// 然后进入 .text 段逐个发射函数。
void AsmPrinter::emitModule(const MachineModule &module) {
  out_ << "// myCompiler AArch64 (ARMv8-A) backend\n";
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
// 则从帧顶向下依次把 x30(lr)、整型/浮点被保存寄存器 str 到栈上(每个占 8 字节,
// 浮点用 64 位 d 形式)。函数体:逐块发射(首块不再打标签,直接接在符号后),
// 逐指令翻译;每条指令后更新虚拟寄存器的使用计数,及时回收不再活跃的兜底临时寄存器。
// 空函数体直接发射 ret。末尾用 .size 记录函数大小并清空本函数的临时状态。
void AsmPrinter::emitFunction(const MachineFunction &function) {
  prepareVirtualRegTemps(function);
  currentFunction_ = &function;
  const bool hasCall = functionContainsCall(function);
  const std::vector<PhysReg> savedGprs = gprCalleeSavedRegsToSave(function);
  const std::vector<PhysReg> savedFprs = fprCalleeSavedRegsToSave(function);

  out_ << "    .globl " << function.name << '\n';
  out_ << "    .type " << function.name << ", %function\n";
  out_ << function.name << ":\n";
  // 序言：下移 sp 开辟栈帧，再把返回地址 x30 与用到的 callee-saved 依次存入
  // 栈帧高地址侧（与 Frame 布局约定一致：每个 8 字节槽）。
  emitStackAdjust(-function.frameSize);
  if (hasCall || !savedGprs.empty() || !savedFprs.empty()) {
    int64_t offset = function.frameSize;
    if (hasCall) {
      offset -= 8;
      emitSpRelative(out_, /*isLoad=*/false, PhysReg::RA, offset,
                     /*isFloat=*/false);
    }
    for (PhysReg reg : savedGprs) {
      offset -= 8;
      emitSpRelative(out_, false, reg, offset, false);
    }
    for (PhysReg reg : savedFprs) {
      offset -= 8;
      emitSpRelative(out_, false, reg, offset, /*isFloat=*/true);
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

// 把单条 MIR 指令翻译成 ARM64 汇编，这是 ISA 翻译的核心。按操作码分派：
// mov/fmov 做寄存器与位模式搬移，movz+movk 物化立即数，add/sub 处理立即数，
// 逻辑立即数按位掩码编码，移位，smull 做 64 位乘法，madd/msub/smaddl/smsubl 乘加融合，
// sdiv+msub 求余，cmp+cset 比较置位，csel 条件选择，fcvtzs/scvtf 浮点与整数互转，
// 访存，内联 memcpy/memset，bl/ret/cbz/b.cond/b 控制流，sxtw 符号扩展。
// 每类先做操作数形态校验（不合法即抛 BackendError），再按对应助记符输出。
void AsmPrinter::emitInstr(const MachineInstr &instr) {
  switch (instr.opcode) {
  case Opcode::COPY:
    // 整数寄存器搬移。ISel 会混用 GPR32(i32) 与 GPR64(参数/返回寄存器)，
    // 两端宽度须一致：只要有一侧是 32 位就统一用 w 形式（写 w 会清零高位）。
    if (instr.defs.empty() || instr.uses.empty()) {
      throw BackendError("malformed COPY instruction");
    }
    {
      const RegClass rc = (instr.defs.front().regValue.rc == RegClass::GPR64 &&
                           instr.uses.front().regValue.rc == RegClass::GPR64)
                              ? RegClass::GPR64
                              : RegClass::GPR32;
      out_ << "    mov " << operandRegAt(instr.defs.front(), rc) << ", "
           << operandRegAt(instr.uses.front(), rc) << '\n';
    }
    return;
  case Opcode::F_COPY:
    // 浮点寄存器搬移。
    if (instr.defs.empty() || instr.uses.empty()) {
      throw BackendError("malformed F_COPY instruction");
    }
    out_ << "    fmov " << operandReg(instr.defs.front()) << ", "
         << operandReg(instr.uses.front()) << '\n';
    return;
  case Opcode::PARALLEL_COPY:
    if (instr.defs.empty() || instr.uses.empty()) {
      throw BackendError("malformed PARALLEL_COPY instruction");
    }
    if (instr.defs.front().regValue.rc == RegClass::FPR32) {
      out_ << "    fmov " << operandReg(instr.defs.front()) << ", "
           << operandReg(instr.uses.front()) << '\n';
    } else {
      const RegClass rc = (instr.defs.front().regValue.rc == RegClass::GPR64 &&
                           instr.uses.front().regValue.rc == RegClass::GPR64)
                              ? RegClass::GPR64
                              : RegClass::GPR32;
      out_ << "    mov " << operandRegAt(instr.defs.front(), rc) << ", "
           << operandRegAt(instr.uses.front(), rc) << '\n';
    }
    return;
  case Opcode::FMV_W_X:
    // 把整数寄存器的位模式搬进浮点寄存器（i32 到 f32 的 bitcast）。
    // GPR 侧恒为 32 位 w 形式（含 wzr），fmov s, w。
    if (instr.defs.empty() || instr.uses.empty()) {
      throw BackendError("malformed FMV_W_X instruction");
    }
    out_ << "    fmov " << operandReg(instr.defs.front()) << ", "
         << operandRegAt(instr.uses.front(), RegClass::GPR32) << '\n';
    return;
  case Opcode::FMV_X_W:
    // 把浮点寄存器的位模式搬进整数寄存器（f32 到 i32 的 bitcast）。
    // GPR 侧恒为 32 位 w 形式，fmov w, s。
    if (instr.defs.empty() || instr.uses.empty()) {
      throw BackendError("malformed FMV_X_W instruction");
    }
    out_ << "    fmov " << operandRegAt(instr.defs.front(), RegClass::GPR32)
         << ", " << operandReg(instr.uses.front()) << '\n';
    return;
  case Opcode::LI:
    // 物化立即数：一律用目标寄存器的 64 位形式做 movz/movk，
    // 32 位消费者取其低 32 位视图即可。
    if (instr.defs.empty() || instr.uses.empty() ||
        instr.uses.front().kind != OperandKind::Imm) {
      throw BackendError("malformed LI instruction");
    }
    emitMoveImm(out_, operandRegWide(instr.defs.front()),
                instr.uses.front().immValue);
    return;
  case Opcode::LA:
    // 取全局地址：adrp 取高 21 位页地址，再 add 低 12 位偏移。
    if (instr.defs.empty() || instr.uses.empty() ||
        instr.uses.front().kind != OperandKind::Global) {
      throw BackendError("malformed LA instruction");
    }
    emitLoadAddress(instr.defs.front(), instr.uses.front().symbol);
    return;
  case Opcode::ADDI:
  case Opcode::ADDIW:
    // 加立即数：ARM64 立即数为 0..4095（可左移12）。负数改用 sub，
    // 越界则先物化到 IP0 再用寄存器加法。
    // 特例：uses[0] 可能是 FrameIndex（取栈对象地址的 lea 形式），
    // 此时基址为 sp 加对象偏移，结果一定是 64 位地址。
    if (instr.defs.empty() || instr.uses.size() < 2 ||
        instr.uses[1].kind != OperandKind::Imm) {
      throw BackendError("malformed immediate instruction");
    }
    {
      int64_t imm = instr.uses[1].immValue;
      std::string dst;
      std::string src;
      if (instr.uses[0].kind == OperandKind::FrameIndex) {
        // lea：dst = sp + (对象偏移 + imm)，用 64 位。
        dst = operandRegWide(instr.defs.front());
        src = resolveMemoryBase(instr.uses[0], imm);
      } else {
        dst = operandReg(instr.defs.front());
        src = operandReg(instr.uses[0]);
      }
      // 源为零寄存器时该指令等价于加载立即数。
      // ARM64 加立即数形式里 31 号寄存器表示 sp 而不是零寄存器，
      // 直接输出 add wd wzr imm 会被汇编器拒绝。
      // 这里与 LI 一致，用 64 位目标寄存器做 movz 和 movk。
      if (imm != 0 && instr.uses[0].kind == OperandKind::Reg &&
          instr.uses[0].regValue.physical &&
          instr.uses[0].regValue.phys == PhysReg::X0) {
        emitMoveImm(out_, operandRegWide(instr.defs.front()), imm);
        return;
      }
      if (imm == 0) {
        out_ << "    mov " << dst << ", " << src << '\n';
      } else if (imm > 0 && fitsAddImm(imm)) {
        out_ << "    add " << dst << ", " << src << ", #" << imm << '\n';
      } else if (imm < 0 && fitsAddImm(-imm)) {
        out_ << "    sub " << dst << ", " << src << ", #" << (-imm) << '\n';
      } else {
        const std::string scratch =
            widthRegName(kReservedGprScratch, instr.defs.front().regValue.rc);
        emitMoveImm(out_, regName(kReservedGprScratch), imm);
        out_ << "    add " << dst << ", " << src << ", " << scratch << '\n';
      }
    }
    return;
  case Opcode::ANDI:
  case Opcode::ORI:
  case Opcode::XORI:
    // 逻辑立即数：ARM64 的位掩码立即数编码复杂，稳妥起见一律先把立即数
    // 物化到 IP0，再用寄存器形式的逻辑运算。
    if (instr.defs.empty() || instr.uses.size() < 2 ||
        instr.uses[1].kind != OperandKind::Imm) {
      throw BackendError("malformed immediate instruction");
    }
    {
      const char *mnemonic = instr.opcode == Opcode::ANDI  ? "and"
                             : instr.opcode == Opcode::ORI ? "orr"
                                                           : "eor";
      const RegClass rc = instr.defs.front().regValue.rc;
      const std::string dst = operandReg(instr.defs.front());
      const std::string src = operandReg(instr.uses[0]);
      const int64_t imm = instr.uses[1].immValue;
      const unsigned width = (rc == RegClass::GPR32) ? 32 : 64;
      // 立即数可作为位掩码立即数时直接编码，省去 movz/movk 物化。
      if (isLogicalImmediate(static_cast<uint64_t>(imm), width)) {
        const uint64_t enc =
            (width == 32) ? (static_cast<uint64_t>(imm) & 0xffffffffULL)
                          : static_cast<uint64_t>(imm);
        out_ << "    " << mnemonic << ' ' << dst << ", " << src << ", #" << enc
             << '\n';
      } else {
        const std::string scratch = widthRegName(kReservedGprScratch, rc);
        emitMoveImm(out_, regName(kReservedGprScratch), imm);
        out_ << "    " << mnemonic << ' ' << dst << ", " << src << ", "
             << scratch << '\n';
      }
    }
    return;
  case Opcode::SLLI:
  case Opcode::SLLIW:
  case Opcode::SRAI:
  case Opcode::SRAIW:
  case Opcode::SRLIW:
    // 移位立即数：直接映射到 lsl/asr/lsr，移位量在合法范围内。
    if (instr.defs.empty() || instr.uses.size() < 2 ||
        instr.uses[1].kind != OperandKind::Imm) {
      throw BackendError("malformed immediate instruction");
    }
    {
      const char *mnemonic = "";
      switch (instr.opcode) {
      case Opcode::SLLI:
      case Opcode::SLLIW:
        mnemonic = "lsl";
        break;
      case Opcode::SRAI:
      case Opcode::SRAIW:
        mnemonic = "asr";
        break;
      case Opcode::SRLIW:
        mnemonic = "lsr";
        break;
      default:
        break;
      }
      // 移位两端宽度一致，按 def 宽度选择 w/x。
      const RegClass w = instr.defs.front().regValue.rc;
      out_ << "    " << mnemonic << ' ' << operandRegAt(instr.defs.front(), w)
           << ", " << operandRegAt(instr.uses[0], w) << ", #"
           << instr.uses[1].immValue << '\n';
    }
    return;
  case Opcode::ADD:
  case Opcode::ADDW:
  case Opcode::SUB:
  case Opcode::SUBW:
  case Opcode::MUL:
  case Opcode::MULW:
  case Opcode::DIV:
  case Opcode::DIVW:
  case Opcode::DIVUW:
  case Opcode::AND:
  case Opcode::OR:
  case Opcode::XOR:
  case Opcode::SLL:
  case Opcode::SLLW:
  case Opcode::SRAW:
  case Opcode::SRLW:
  case Opcode::FADD_S:
  case Opcode::FSUB_S:
  case Opcode::FMUL_S:
  case Opcode::FDIV_S:
    // 直接映射到 ARM64 单条指令的算术/位/移位/浮点二元运算。
    // W 后缀在 ISel 里表示 32 位，operandReg 已据 RegClass 选 w/x。
    if (instr.defs.empty() || instr.uses.size() < 2) {
      throw BackendError("malformed binary instruction");
    }
    {
      const char *mnemonic = "";
      switch (instr.opcode) {
      case Opcode::ADD:
      case Opcode::ADDW:
        mnemonic = "add";
        break;
      case Opcode::SUB:
      case Opcode::SUBW:
        mnemonic = "sub";
        break;
      case Opcode::MUL:
      case Opcode::MULW:
        mnemonic = "mul";
        break;
      case Opcode::DIV:
      case Opcode::DIVW:
        mnemonic = "sdiv";
        break;
      case Opcode::DIVUW:
        mnemonic = "udiv";
        break;
      case Opcode::AND:
        mnemonic = "and";
        break;
      case Opcode::OR:
        mnemonic = "orr";
        break;
      case Opcode::XOR:
        mnemonic = "eor";
        break;
      case Opcode::SLL:
      case Opcode::SLLW:
        mnemonic = "lsl";
        break;
      case Opcode::SRAW:
        mnemonic = "asr";
        break;
      case Opcode::SRLW:
        mnemonic = "lsr";
        break;
      case Opcode::FADD_S:
        mnemonic = "fadd";
        break;
      case Opcode::FSUB_S:
        mnemonic = "fsub";
        break;
      case Opcode::FMUL_S:
        mnemonic = "fmul";
        break;
      case Opcode::FDIV_S:
        mnemonic = "fdiv";
        break;
      default:
        break;
      }
      // MUL 为真 64 位乘法（mul xd, xn, xm），MULW 为 32 位乘法（mul wd, wn, wm），
      // 均按 def 的寄存器类选择宽度。32×32→64 的有符号长乘是独立的 SMULL
      // opcode（下方单独分支发射 smull xd, wn, wm）：ISel 只在常量除法 magic
      // multiply 与 GEP 索引×步长时生成 SMULL。宽度语义在 MIR 层显式区分后，
      // 任何把真 64 位 MUL 当作 smull 消费的 Pass 都会被消除。
      const RegClass w = instr.defs.front().regValue.rc;
      out_ << "    " << mnemonic << ' ' << operandRegAt(instr.defs.front(), w)
           << ", " << operandRegAt(instr.uses[0], w) << ", "
           << operandRegAt(instr.uses[1], w) << '\n';
    }
    return;
  case Opcode::SMULL:
    // 32×32→64 有符号长乘：smull xd, wn, wm。乘数固定取 w 视图（符号扩展），
    // 结果写入 64 位 x 寄存器。
    if (instr.defs.empty() || instr.uses.size() < 2) {
      throw BackendError("malformed smull instruction");
    }
    out_ << "    smull " << operandRegAt(instr.defs.front(), RegClass::GPR64)
         << ", " << operandRegAt(instr.uses[0], RegClass::GPR32) << ", "
         << operandRegAt(instr.uses[1], RegClass::GPR32) << '\n';
    return;
  case Opcode::MADD:
  case Opcode::MSUB:
    // 32 位乘加/乘减融合：madd/msub wd, wn, wm, wa。
    if (instr.defs.empty() || instr.uses.size() < 3) {
      throw BackendError("malformed madd/msub instruction");
    }
    out_ << "    " << (instr.opcode == Opcode::MADD ? "madd" : "msub") << ' '
         << operandRegAt(instr.defs.front(), RegClass::GPR32) << ", "
         << operandRegAt(instr.uses[0], RegClass::GPR32) << ", "
         << operandRegAt(instr.uses[1], RegClass::GPR32) << ", "
         << operandRegAt(instr.uses[2], RegClass::GPR32) << '\n';
    return;
  case Opcode::SMADDL:
  case Opcode::SMSUBL:
    // 32x32+64 乘加/乘减融合：smaddl/smsubl xd, wn, wm, xa。
    // 与 64 位 MUL 用 smull 同理：乘数取 w 视图符号扩展，累加数与结果为 x。
    if (instr.defs.empty() || instr.uses.size() < 3) {
      throw BackendError("malformed smaddl/smsubl instruction");
    }
    out_ << "    " << (instr.opcode == Opcode::SMADDL ? "smaddl" : "smsubl")
         << ' ' << operandRegAt(instr.defs.front(), RegClass::GPR64) << ", "
         << operandRegAt(instr.uses[0], RegClass::GPR32) << ", "
         << operandRegAt(instr.uses[1], RegClass::GPR32) << ", "
         << operandRegAt(instr.uses[2], RegClass::GPR64) << '\n';
    return;
  case Opcode::REM:
  case Opcode::REMW:
  case Opcode::REMUW:
    // ARM64 无求余指令：先 sdiv/udiv 到 IP0，再 msub 还原余数
    // rem = a - (a/b)*b。
    if (instr.defs.empty() || instr.uses.size() < 2) {
      throw BackendError("malformed remainder instruction");
    }
    {
      const RegClass rc = instr.defs.front().regValue.rc;
      const std::string dst = operandReg(instr.defs.front());
      const std::string a = operandReg(instr.uses[0]);
      const std::string b = operandReg(instr.uses[1]);
      const std::string q = widthRegName(kReservedGprScratch, rc);
      const char *divOp = instr.opcode == Opcode::REMUW ? "udiv" : "sdiv";
      out_ << "    " << divOp << ' ' << q << ", " << a << ", " << b << '\n';
      out_ << "    msub " << dst << ", " << q << ", " << b << ", " << a << '\n';
    }
    return;
  case Opcode::SLT:
  case Opcode::SLTU:
    // set-less-than：cmp 后按有/无符号条件码 cset 出 0/1（结果为 GPR32）。
    if (instr.defs.empty() || instr.uses.size() < 2) {
      throw BackendError("malformed set-less-than instruction");
    }
    out_ << "    cmp " << operandReg(instr.uses[0]) << ", "
         << operandReg(instr.uses[1]) << '\n';
    out_ << "    cset " << operandReg(instr.defs.front()) << ", "
         << (instr.opcode == Opcode::SLT ? "lt" : "lo") << '\n';
    return;
  case Opcode::FEQ_S:
  case Opcode::FLT_S:
  case Opcode::FLE_S:
    // 浮点比较：fcmp 设标志后 cset 出 0/1（结果为 GPR32）。
    // 有序比较用 eq / mi(小于) / ls(小于等于)。
    if (instr.defs.empty() || instr.uses.size() < 2) {
      throw BackendError("malformed float compare instruction");
    }
    out_ << "    fcmp " << operandReg(instr.uses[0]) << ", "
         << operandReg(instr.uses[1]) << '\n';
    {
      const char *cond = instr.opcode == Opcode::FEQ_S   ? "eq"
                         : instr.opcode == Opcode::FLT_S ? "mi"
                                                         : "ls";
      out_ << "    cset " << operandReg(instr.defs.front()) << ", " << cond
           << '\n';
    }
    return;
  case Opcode::SEQZ:
  case Opcode::SNEZ:
    // 与零比较：cmp #0 后 cset eq/ne。
    if (instr.defs.empty() || instr.uses.empty()) {
      throw BackendError("malformed zero-compare instruction");
    }
    out_ << "    cmp " << operandReg(instr.uses.front()) << ", #0\n";
    out_ << "    cset " << operandReg(instr.defs.front()) << ", "
         << (instr.opcode == Opcode::SEQZ ? "eq" : "ne") << '\n';
    return;
  case Opcode::CSEL: {
    // 条件选择：先按内嵌的比较 Opcode 发射 cmp/fcmp 设标志，再 csel。
    // uses = {trueVal, falseVal, cmpLhs, cmpRhs, Imm(cmpOpcode)}。
    if (instr.defs.empty() || instr.uses.size() < 5 ||
        instr.uses[4].kind != OperandKind::Imm) {
      throw BackendError("malformed csel instruction");
    }
    const Opcode cmpOp = static_cast<Opcode>(instr.uses[4].immValue);
    const char *cond = "";
    bool isFloatCmp = false;
    bool cmpZero = false;
    switch (cmpOp) {
    case Opcode::SLT:  cond = "lt"; break;
    case Opcode::SLTU: cond = "lo"; break;
    case Opcode::SEQZ: cond = "eq"; cmpZero = true; break;
    case Opcode::SNEZ: cond = "ne"; cmpZero = true; break;
    case Opcode::FEQ_S: cond = "eq"; isFloatCmp = true; break;
    case Opcode::FLT_S: cond = "mi"; isFloatCmp = true; break;
    case Opcode::FLE_S: cond = "ls"; isFloatCmp = true; break;
    default:
      throw BackendError("unsupported compare opcode in csel");
    }
    if (cmpZero) {
      // tst 融合：cmpRhs 为 Imm 掩码时，用 tst reg,#mask 一条指令代替
      // and+cmp，等价测试 (reg & mask) 与 0 的关系（eq=为零/ne=非零）。
      if (instr.uses[3].kind == OperandKind::Imm) {
        out_ << "    tst " << operandReg(instr.uses[2]) << ", #"
             << static_cast<uint64_t>(instr.uses[3].immValue) << '\n';
      } else {
        out_ << "    cmp " << operandReg(instr.uses[2]) << ", #0\n";
      }
    } else {
      out_ << "    " << (isFloatCmp ? "fcmp" : "cmp") << ' '
           << operandReg(instr.uses[2]) << ", " << operandReg(instr.uses[3])
           << '\n';
    }
    const RegClass rc = instr.defs.front().regValue.rc;
    out_ << "    csel " << operandRegAt(instr.defs.front(), rc) << ", "
         << operandRegAt(instr.uses[0], rc) << ", "
         << operandRegAt(instr.uses[1], rc) << ", " << cond << '\n';
    return;
  }
  case Opcode::FCVT_W_S:
  case Opcode::FCVT_S_W:
    // 浮点/整数互转：fcvtzs 向零取整转有符号整数，scvtf 有符号整数转浮点。
    if (instr.defs.empty() || instr.uses.empty()) {
      throw BackendError("malformed float conversion instruction");
    }
    if (instr.opcode == Opcode::FCVT_W_S) {
      out_ << "    fcvtzs " << operandReg(instr.defs.front()) << ", "
           << operandReg(instr.uses.front()) << '\n';
    } else {
      out_ << "    scvtf " << operandReg(instr.defs.front()) << ", "
           << operandReg(instr.uses.front()) << '\n';
    }
    return;
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

    // 保留寄存器：T5/T4 作 dst/src(x 形式)，T3 临时值，T6 计数器。
    {
      const std::string dstReg = rawRegName(kReservedMemcpyDst);
      const std::string srcReg = rawRegName(kReservedMemcpySrc);
      const std::string tmpW = widthRegName(kReservedMemcpyTmp, RegClass::GPR32);
      const std::string cnt = rawRegName(kReservedGprScratch);
      out_ << "    mov " << dstReg << ", " << dst << '\n';
      out_ << "    mov " << srcReg << ", " << src << '\n';
      emitMoveImm(out_, cnt, wordCount);
      out_ << "    cbz " << cnt << ", " << byteStart << '\n';
      out_ << wordLoop << ":\n";
      out_ << "    ldr " << tmpW << ", [" << srcReg << "]\n";
      out_ << "    str " << tmpW << ", [" << dstReg << "]\n";
      out_ << "    add " << srcReg << ", " << srcReg << ", #4\n";
      out_ << "    add " << dstReg << ", " << dstReg << ", #4\n";
      out_ << "    sub " << cnt << ", " << cnt << ", #1\n";
      out_ << "    cbnz " << cnt << ", " << wordLoop << '\n';
      out_ << byteStart << ":\n";
      emitMoveImm(out_, cnt, byteCount);
      out_ << "    cbz " << cnt << ", " << done << '\n';
      out_ << byteLoop << ":\n";
      out_ << "    ldrb " << tmpW << ", [" << srcReg << "]\n";
      out_ << "    strb " << tmpW << ", [" << dstReg << "]\n";
      out_ << "    add " << srcReg << ", " << srcReg << ", #1\n";
      out_ << "    add " << dstReg << ", " << dstReg << ", #1\n";
      out_ << "    sub " << cnt << ", " << cnt << ", #1\n";
      out_ << "    cbnz " << cnt << ", " << byteLoop << '\n';
      out_ << done << ":\n";
    }
    return;
  }
  // MEMSET_INLINE:把 [dst, size) 填充为字节值 byte。
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

    {
      const std::string dstReg = rawRegName(kReservedMemcpyDst);
      const std::string valW = widthRegName(kReservedMemcpyTmp, RegClass::GPR32);
      const std::string valX = rawRegName(kReservedMemcpyTmp);
      const std::string cnt = rawRegName(kReservedGprScratch);
      out_ << "    mov " << dstReg << ", " << dst << '\n';
      emitMoveImm(out_, valX, static_cast<int64_t>(repeated));
      emitMoveImm(out_, cnt, wordCount);
      out_ << "    cbz " << cnt << ", " << byteStart << '\n';
      out_ << wordLoop << ":\n";
      out_ << "    str " << valW << ", [" << dstReg << "]\n";
      out_ << "    add " << dstReg << ", " << dstReg << ", #4\n";
      out_ << "    sub " << cnt << ", " << cnt << ", #1\n";
      out_ << "    cbnz " << cnt << ", " << wordLoop << '\n';
      out_ << byteStart << ":\n";
      emitMoveImm(out_, cnt, byteCount);
      out_ << "    cbz " << cnt << ", " << done << '\n';
      out_ << byteLoop << ":\n";
      out_ << "    strb " << valW << ", [" << dstReg << "]\n";
      out_ << "    add " << dstReg << ", " << dstReg << ", #1\n";
      out_ << "    sub " << cnt << ", " << cnt << ", #1\n";
      out_ << "    cbnz " << cnt << ", " << byteLoop << '\n';
      out_ << done << ":\n";
    }
    return;
  }
  case Opcode::RET:
    // 尾声：恢复返回地址 x30 与 callee-saved，回收栈帧，ret 返回。
    if (currentFunction_) {
      const bool containsCall = functionContainsCall(*currentFunction_);
      const std::vector<PhysReg> savedGprs =
          gprCalleeSavedRegsToSave(*currentFunction_);
      const std::vector<PhysReg> savedFprs =
          fprCalleeSavedRegsToSave(*currentFunction_);
      int64_t offset = currentFunction_->frameSize;
      if (containsCall) {
        offset -= 8;
        emitSpRelative(out_, /*isLoad=*/true, PhysReg::RA, offset,
                       /*isFloat=*/false);
      }
      for (PhysReg reg : savedGprs) {
        offset -= 8;
        emitSpRelative(out_, true, reg, offset, false);
      }
      for (PhysReg reg : savedFprs) {
        offset -= 8;
        emitSpRelative(out_, true, reg, offset, /*isFloat=*/true);
      }
      emitStackAdjust(currentFunction_->frameSize);
    }
    out_ << "    ret\n";
    return;
  case Opcode::CALL:
    // 直接调用：bl 会把返回地址写入 x30。
    if (instr.uses.empty() || instr.uses.front().kind != OperandKind::Global) {
      throw BackendError("malformed CALL instruction");
    }
    out_ << "    bl " << instr.uses.front().symbol << '\n';
    return;
  case Opcode::BNEZ:
    // 非零跳转：cbnz。
    if (instr.uses.size() < 2 || instr.uses[1].kind != OperandKind::Block) {
      throw BackendError("malformed BNEZ instruction");
    }
    out_ << "    cbnz " << operandReg(instr.uses[0]) << ", "
         << instr.uses[1].blockLabel << '\n';
    return;
  case Opcode::BEQZ:
    // 为零跳转：cbz。
    if (instr.uses.size() < 2 || instr.uses[1].kind != OperandKind::Block) {
      throw BackendError("malformed BEQZ instruction");
    }
    out_ << "    cbz " << operandReg(instr.uses[0]) << ", "
         << instr.uses[1].blockLabel << '\n';
    return;
  case Opcode::BEQ:
  case Opcode::BNE:
  case Opcode::BLT:
  case Opcode::BGE:
    // 两寄存器条件分支：cmp 设标志后按条件码 b.cond 跳转。
    if (instr.uses.size() < 3 || instr.uses[2].kind != OperandKind::Block) {
      throw BackendError("malformed conditional branch instruction");
    }
    {
      const char *cond = "";
      switch (instr.opcode) {
      case Opcode::BEQ:
        cond = "eq";
        break;
      case Opcode::BNE:
        cond = "ne";
        break;
      case Opcode::BLT:
        cond = "lt";
        break;
      case Opcode::BGE:
        cond = "ge";
        break;
      default:
        break;
      }
      out_ << "    cmp " << operandReg(instr.uses[0]) << ", "
           << operandReg(instr.uses[1]) << '\n';
      out_ << "    b." << cond << ' ' << instr.uses[2].blockLabel << '\n';
    }
    return;
  case Opcode::J:
    // 无条件跳转：b。
    if (instr.uses.empty() || instr.uses.front().kind != OperandKind::Block) {
      throw BackendError("malformed J instruction");
    }
    out_ << "    b " << instr.uses.front().blockLabel << '\n';
    return;
  case Opcode::SXTW:
    // 符号扩展 32 位到 64 位：sxtw xd, ws。
    if (instr.defs.empty() || instr.uses.empty()) {
      throw BackendError("malformed SXTW instruction");
    }
    out_ << "    sxtw " << operandRegWide(instr.defs.front()) << ", "
         << widthRegName(regForOperand(instr.uses.front()), RegClass::GPR32)
         << '\n';
    return;
  case Opcode::NOP:
    out_ << "    nop\n";
    return;
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

void AsmPrinter::emitLoadAddress(const MachineOperand &dst,
                                 const std::string &symbol) {
  // 取全局符号地址：adrp 取 4KB 页基址，再 add 上页内低 12 位偏移。
  const std::string reg = operandRegWide(dst);
  out_ << "    adrp " << reg << ", " << symbol << '\n';
  out_ << "    add " << reg << ", " << reg << ", :lo12:" << symbol << '\n';
}

void AsmPrinter::emitStackAdjust(int64_t amount) {
  // 调整 sp：正数为回收(加)，负数为开辟(减)；越界则先物化到 IP0。
  if (amount == 0) {
    return;
  }
  const int64_t magnitude = amount < 0 ? -amount : amount;
  const char *mnemonic = amount < 0 ? "sub" : "add";
  if (fitsAddImm(magnitude)) {
    out_ << "    " << mnemonic << " sp, sp, #" << magnitude << '\n';
    return;
  }
  emitMoveImm(out_, regName(kReservedGprScratch), magnitude);
  out_ << "    " << mnemonic << " sp, sp, " << regName(kReservedGprScratch)
       << '\n';
}

void AsmPrinter::emitMemoryInstr(const MachineInstr &instr) {
  // 内存访问统一发射为 ARM64 的 [base, #offset] 形式。
  const bool isStore =
      instr.opcode == Opcode::SB || instr.opcode == Opcode::SW ||
      instr.opcode == Opcode::SD || instr.opcode == Opcode::FSW;
  const char *mnemonic = "";
  int64_t accessSize = 4;
  switch (instr.opcode) {
  case Opcode::LBU:
    mnemonic = "ldrb";
    accessSize = 1;
    break;
  case Opcode::SB:
    mnemonic = "strb";
    accessSize = 1;
    break;
  case Opcode::LW:
    mnemonic = "ldr";
    accessSize = 4;
    break;
  case Opcode::SW:
    mnemonic = "str";
    accessSize = 4;
    break;
  case Opcode::LD:
    mnemonic = "ldr";
    accessSize = 8;
    break;
  case Opcode::SD:
    mnemonic = "str";
    accessSize = 8;
    break;
  case Opcode::FLW:
    mnemonic = "ldr";
    accessSize = 4;
    break;
  case Opcode::FSW:
    mnemonic = "str";
    accessSize = 4;
    break;
  default:
    throw BackendError("unsupported memory instruction");
  }

  // ldrb/strb 的数据寄存器用 32 位 w 形式，其余按操作数 RegClass 决定。
  const MachineOperand &valueOperand =
      isStore ? instr.uses[0]
              : (instr.defs.empty() ? throw BackendError("malformed load")
                                    : instr.defs.front());
  const int offsetIdx = isStore ? 2 : 1;
  const int baseIdx = isStore ? 1 : 0;
  if (static_cast<int>(instr.uses.size()) <= offsetIdx ||
      instr.uses[offsetIdx].kind != OperandKind::Imm) {
    throw BackendError("malformed memory instruction");
  }

  int64_t offset = instr.uses[offsetIdx].immValue;

  // 索引寻址形式：地址折叠 pass 会在偏移之后追加 {indexReg, scaleImm}，
  // 表示有效地址 = base + index<<scale。此时偏移恒为 0（ARM64 不能同时带
  // 立即偏移与寄存器索引）。index 为 32 位则用 sxtw 扩展，64 位则用 lsl。
  const int indexIdx = offsetIdx + 1;
  const int scaleIdx = offsetIdx + 2;
  if (static_cast<int>(instr.uses.size()) > scaleIdx &&
      instr.uses[indexIdx].kind == OperandKind::Reg &&
      instr.uses[scaleIdx].kind == OperandKind::Imm && offset == 0) {
    const std::string valueReg2 =
        (accessSize == 1) ? widthRegName(regForOperand(valueOperand),
                                         RegClass::GPR32)
                          : operandReg(valueOperand);
    const std::string base2 = operandRegWide(instr.uses[baseIdx]);
    const MachineOperand &idx = instr.uses[indexIdx];
    const int64_t scale = instr.uses[scaleIdx].immValue;
    const bool idx32 = idx.regValue.rc == RegClass::GPR32;
    const std::string idxReg =
        idx32 ? operandRegAt(idx, RegClass::GPR32) : operandRegWide(idx);
    const char *extend = idx32 ? "sxtw" : "lsl";
    out_ << "    " << mnemonic << ' ' << valueReg2 << ", [" << base2 << ", "
         << idxReg << ", " << extend << " #" << scale << "]\n";
    return;
  }

  std::string base = resolveMemoryBase(instr.uses[baseIdx], offset);
  const std::string valueReg =
      (accessSize == 1) ? widthRegName(regForOperand(valueOperand),
                                       RegClass::GPR32)
                        : operandReg(valueOperand);

  // 偏移可直接编码：0，或非负且按 accessSize 对齐且缩放后 <=4095。
  const bool encodable =
      offset == 0 ||
      (offset > 0 && offset % accessSize == 0 && offset / accessSize <= 0xfff);
  if (encodable) {
    if (offset == 0) {
      out_ << "    " << mnemonic << ' ' << valueReg << ", [" << base << "]\n";
    } else {
      out_ << "    " << mnemonic << ' ' << valueReg << ", [" << base << ", #"
           << offset << "]\n";
    }
    return;
  }

  // 偏移越界：把有效地址算到 IP0 再零偏移访问。
  emitMoveImm(out_, regName(kReservedGprScratch), offset);
  out_ << "    add " << regName(kReservedGprScratch) << ", " << base << ", "
       << regName(kReservedGprScratch) << '\n';
  out_ << "    " << mnemonic << ' ' << valueReg << ", ["
       << regName(kReservedGprScratch) << "]\n";
}

// 解析内存/地址计算的基址：寄存器直接取 x 形式；FrameIndex 折算成 sp+对象偏移。
std::string AsmPrinter::resolveMemoryBase(const MachineOperand &base,
                                          int64_t &offset) {
  if (base.kind == OperandKind::Reg) {
    return operandRegWide(base);
  }
  if (base.kind == OperandKind::FrameIndex) {
    if (!currentFunction_) {
      throw BackendError("frame-index operand without active function");
    }
    const int frameIndex = base.frameIndexValue;
    if (frameIndex < 0 ||
        frameIndex >= static_cast<int>(currentFunction_->stackObjects.size())) {
      throw BackendError("invalid frame-index operand");
    }
    offset += currentFunction_->stackObjects[frameIndex].offset;
    return "sp";
  }
  throw BackendError("expected register or frame-index memory base");
}

std::pair<std::string, PhysReg>
AsmPrinter::memoryBaseReg(const MachineOperand &base, int64_t &offset) {
  // 兼容旧接口：仅折算基址与偏移，不再自行物化。
  return {resolveMemoryBase(base, offset), PhysReg::NoReg};
}

// 把寄存器操作数解析成物理寄存器:物理寄存器直接取,虚拟寄存器走兜底指派。
// 这是需要拿到 PhysReg 本身(而非名字)的统一入口,如挑宽度/判浮点时。
PhysReg AsmPrinter::regForOperand(const MachineOperand &operand) {
  if (operand.kind != OperandKind::Reg) {
    throw BackendError("expected register operand");
  }
  const MachineReg reg = operand.regValue;
  if (!reg.isValid()) {
    throw BackendError("invalid register operand");
  }
  return reg.physical ? reg.phys : assignVirtualReg(reg);
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
      throw BackendError("AArch64 FPR temporary register pool exhausted");
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
      throw BackendError("AArch64 GPR temporary register pool exhausted");
    }
    PhysReg phys = freeGprTemps_.back();
    freeGprTemps_.pop_back();
    gprVRegMap_.emplace(reg.id, phys);
    return phys;
  }

  throw BackendError("unsupported virtual register class");
}

std::string AsmPrinter::operandReg(const MachineOperand &operand) {
  // 按操作数的 RegClass 选择寄存器宽度：GPR32 用 w，GPR64 用 x，FPR32 用 s。
  if (operand.kind != OperandKind::Reg) {
    throw BackendError("expected register operand");
  }
  const MachineReg reg = operand.regValue;
  if (!reg.isValid()) {
    throw BackendError("invalid register operand");
  }
  const PhysReg phys = reg.physical ? reg.phys : assignVirtualReg(reg);
  return widthRegName(phys, reg.rc);
}

std::string AsmPrinter::operandRegWide(const MachineOperand &operand) {
  // 取寄存器的 64 位 x 形式（地址计算/立即数物化用）。
  const PhysReg phys = regForOperand(operand);
  if (getRegisterInfo(phys).isFloat) {
    return rawRegName(phys);
  }
  return widthRegName(phys, RegClass::GPR64);
}

std::string AsmPrinter::operandRegAt(const MachineOperand &operand,
                                     RegClass rc) {
  // 以指定宽度输出寄存器名（用于 COPY 两端宽度可能不一致的情形）。
  const PhysReg phys = regForOperand(operand);
  if (getRegisterInfo(phys).isFloat) {
    return rawRegName(phys);
  }
  return widthRegName(phys, rc);
}

} // namespace aarch64
