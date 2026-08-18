// Registers.cpp —— RISC-V 物理寄存器枚举到汇编名与 ABI 角色的映射表。
// 这里集中描述每个物理寄存器的汇编名(如 x10 写作 a0)、是否可参与寄存器分配、
// 是否 caller-saved(临时寄存器,调用会破坏)、是否 callee-saved(被调用者需保存)、
// 以及是否浮点寄存器。分配器和汇编发射都从这张表取信息。
#include "riscv/Registers.hpp"

#include <array>
#include <cstddef>
#include <vector>

namespace riscv {
namespace {

// 寄存器总数:以最后一个浮点寄存器 FT11 的枚举值加一为上界,用于开静态数组。
constexpr std::size_t kNumRegs =
    static_cast<std::size_t>(PhysReg::FT11) + 1;

// 物理寄存器信息表。每一项字段顺序为:
// {PhysReg 枚举, 汇编名, 可分配, callerSaved, calleeSaved, isFloat}。
// 表按 PhysReg 枚举值顺序排列,可用枚举值直接下标索引。
// ABI 角色约定:
//   zero 恒零,sp/gp/tp 为固定用途不参与分配;ra 为返回地址(caller-saved);
//   t0-t6 临时寄存器(caller-saved),a0-a7 参数/返回值寄存器(caller-saved),
//   s0-s11 被调用者保存寄存器(callee-saved,s0 亦即 fp,不参与分配);
//   浮点侧 ft* 为临时,fa* 为参数,fs* 为被调用者保存,规则与整型对称。
constexpr std::array<RegisterInfo, kNumRegs> kRegisterInfos{{
    {PhysReg::NoReg, "noreg", false, false, false, false},
    {PhysReg::X0, "zero", false, false, false, false},
    {PhysReg::RA, "ra", false, true, false, false},
    {PhysReg::SP, "sp", false, false, false, false},
    {PhysReg::GP, "gp", false, false, false, false},
    {PhysReg::TP, "tp", false, false, false, false},
    {PhysReg::T0, "t0", true, true, false, false},
    {PhysReg::T1, "t1", true, true, false, false},
    {PhysReg::T2, "t2", true, true, false, false},
    {PhysReg::S0, "s0", false, false, true, false},
    {PhysReg::S1, "s1", true, false, true, false},
    {PhysReg::A0, "a0", true, true, false, false},
    {PhysReg::A1, "a1", true, true, false, false},
    {PhysReg::A2, "a2", true, true, false, false},
    {PhysReg::A3, "a3", true, true, false, false},
    {PhysReg::A4, "a4", true, true, false, false},
    {PhysReg::A5, "a5", true, true, false, false},
    {PhysReg::A6, "a6", true, true, false, false},
    {PhysReg::A7, "a7", true, true, false, false},
    {PhysReg::S2, "s2", true, false, true, false},
    {PhysReg::S3, "s3", true, false, true, false},
    {PhysReg::S4, "s4", true, false, true, false},
    {PhysReg::S5, "s5", true, false, true, false},
    {PhysReg::S6, "s6", true, false, true, false},
    {PhysReg::S7, "s7", true, false, true, false},
    {PhysReg::S8, "s8", true, false, true, false},
    {PhysReg::S9, "s9", true, false, true, false},
    {PhysReg::S10, "s10", true, false, true, false},
    {PhysReg::S11, "s11", true, false, true, false},
    {PhysReg::T3, "t3", true, true, false, false},
    {PhysReg::T4, "t4", true, true, false, false},
    {PhysReg::T5, "t5", true, true, false, false},
    // t6 is reserved for late assembly expansion and must never participate in
    // register allocation.  In particular, MEMSET_INLINE names it as an
    // implicit clobber even though it is intentionally absent from
    // kGPRAllocatable.
    {PhysReg::T6, "t6", false, true, false, false},
    {PhysReg::FT0, "ft0", true, true, false, true},
    {PhysReg::FT1, "ft1", true, true, false, true},
    {PhysReg::FT2, "ft2", true, true, false, true},
    {PhysReg::FT3, "ft3", true, true, false, true},
    {PhysReg::FT4, "ft4", true, true, false, true},
    {PhysReg::FT5, "ft5", true, true, false, true},
    {PhysReg::FT6, "ft6", true, true, false, true},
    {PhysReg::FT7, "ft7", true, true, false, true},
    {PhysReg::FS0, "fs0", true, false, true, true},
    {PhysReg::FS1, "fs1", true, false, true, true},
    {PhysReg::FA0, "fa0", true, true, false, true},
    {PhysReg::FA1, "fa1", true, true, false, true},
    {PhysReg::FA2, "fa2", true, true, false, true},
    {PhysReg::FA3, "fa3", true, true, false, true},
    {PhysReg::FA4, "fa4", true, true, false, true},
    {PhysReg::FA5, "fa5", true, true, false, true},
    {PhysReg::FA6, "fa6", true, true, false, true},
    {PhysReg::FA7, "fa7", true, true, false, true},
    {PhysReg::FS2, "fs2", true, false, true, true},
    {PhysReg::FS3, "fs3", true, false, true, true},
    {PhysReg::FS4, "fs4", true, false, true, true},
    {PhysReg::FS5, "fs5", true, false, true, true},
    {PhysReg::FS6, "fs6", true, false, true, true},
    {PhysReg::FS7, "fs7", true, false, true, true},
    {PhysReg::FS8, "fs8", true, false, true, true},
    {PhysReg::FS9, "fs9", true, false, true, true},
    {PhysReg::FS10, "fs10", true, false, true, true},
    {PhysReg::FS11, "fs11", true, false, true, true},
    {PhysReg::FT8, "ft8", true, true, false, true},
    {PhysReg::FT9, "ft9", true, true, false, true},
    {PhysReg::FT10, "ft10", true, true, false, true},
    {PhysReg::FT11, "ft11", true, true, false, true},
}};

// 整型可分配寄存器池:寄存器分配器可自由指派的 GPR。
// 不含 zero/sp/gp/tp/ra/s0(fp)/t6 等有固定用途或被保留的寄存器。
const std::vector<PhysReg> kGPRAllocatable{
    PhysReg::T0, PhysReg::T1, PhysReg::T2, PhysReg::T3, PhysReg::T4,
    PhysReg::T5, PhysReg::A0, PhysReg::A1, PhysReg::A2, PhysReg::A3,
    PhysReg::A4, PhysReg::A5, PhysReg::A6, PhysReg::A7, PhysReg::S1,
    PhysReg::S2, PhysReg::S3, PhysReg::S4, PhysReg::S5, PhysReg::S6,
    PhysReg::S7, PhysReg::S8, PhysReg::S9, PhysReg::S10, PhysReg::S11};

// 浮点可分配寄存器池:所有 ft/fa/fs 浮点寄存器均可参与分配。
const std::vector<PhysReg> kFPRAllocatable{
    PhysReg::FT0, PhysReg::FT1, PhysReg::FT2, PhysReg::FT3, PhysReg::FT4,
    PhysReg::FT5, PhysReg::FT6, PhysReg::FT7, PhysReg::FT8, PhysReg::FT9,
    PhysReg::FT10, PhysReg::FT11, PhysReg::FA0, PhysReg::FA1, PhysReg::FA2,
    PhysReg::FA3, PhysReg::FA4, PhysReg::FA5, PhysReg::FA6, PhysReg::FA7,
    PhysReg::FS0, PhysReg::FS1, PhysReg::FS2, PhysReg::FS3, PhysReg::FS4,
    PhysReg::FS5, PhysReg::FS6, PhysReg::FS7, PhysReg::FS8, PhysReg::FS9,
    PhysReg::FS10, PhysReg::FS11};

// 整型 caller-saved(调用者保存)集合:ra 与所有 t/a 寄存器,跨调用不保留。
const std::vector<PhysReg> kGPRCallerSaved{
    PhysReg::RA, PhysReg::T0, PhysReg::T1, PhysReg::T2, PhysReg::T3,
    PhysReg::T4, PhysReg::T5, PhysReg::A0, PhysReg::A1,
    PhysReg::A2, PhysReg::A3, PhysReg::A4, PhysReg::A5, PhysReg::A6,
    PhysReg::A7};

// 浮点 caller-saved 集合:所有 ft 与 fa 寄存器。
const std::vector<PhysReg> kFPRCallerSaved{
    PhysReg::FT0, PhysReg::FT1, PhysReg::FT2, PhysReg::FT3, PhysReg::FT4,
    PhysReg::FT5, PhysReg::FT6, PhysReg::FT7, PhysReg::FT8, PhysReg::FT9,
    PhysReg::FT10, PhysReg::FT11, PhysReg::FA0, PhysReg::FA1, PhysReg::FA2,
    PhysReg::FA3, PhysReg::FA4, PhysReg::FA5, PhysReg::FA6, PhysReg::FA7};

// 整型 callee-saved(被调用者保存)集合:s0-s11,函数若用到需在 prologue 保存。
const std::vector<PhysReg> kGPRCalleeSaved{
    PhysReg::S0, PhysReg::S1, PhysReg::S2, PhysReg::S3, PhysReg::S4,
    PhysReg::S5, PhysReg::S6, PhysReg::S7, PhysReg::S8, PhysReg::S9,
    PhysReg::S10, PhysReg::S11};

// 浮点 callee-saved 集合:fs0-fs11。
const std::vector<PhysReg> kFPRCalleeSaved{
    PhysReg::FS0, PhysReg::FS1, PhysReg::FS2, PhysReg::FS3, PhysReg::FS4,
    PhysReg::FS5, PhysReg::FS6, PhysReg::FS7, PhysReg::FS8, PhysReg::FS9,
    PhysReg::FS10, PhysReg::FS11};

// 判断寄存器类是否为浮点类,用于在整型/浮点两套寄存器池之间选择。
bool isFloatClass(RegClass rc) { return rc == RegClass::FPR32; }

} // namespace

// 判断枚举值是否为有效物理寄存器:下标在范围内且表项自洽(表项记录的枚举与查询一致)。
bool isValidPhysReg(PhysReg reg) {
  const auto index = static_cast<std::size_t>(reg);
  return index < kRegisterInfos.size() && kRegisterInfos[index].reg == reg;
}

// 判断寄存器类是否为已知的合法类别(GPR32/GPR64/FPR32)。
bool isValidRegClass(RegClass rc) {
  switch (rc) {
  case RegClass::GPR32:
  case RegClass::GPR64:
  case RegClass::FPR32:
    return true;
  }
  return false;
}

// 按物理寄存器查表返回其信息;越界时回退到 NoReg 表项,保证不越界访问。
const RegisterInfo &getRegisterInfo(PhysReg reg) {
  const auto index = static_cast<std::size_t>(reg);
  if (index >= kRegisterInfos.size()) {
    return kRegisterInfos[static_cast<std::size_t>(PhysReg::NoReg)];
  }
  return kRegisterInfos[index];
}

// 按寄存器类返回对应的可分配寄存器池。
const std::vector<PhysReg> &allocatableRegs(RegClass rc) {
  return isFloatClass(rc) ? kFPRAllocatable : kGPRAllocatable;
}

// 按寄存器类返回对应的 caller-saved 集合。
const std::vector<PhysReg> &callerSavedRegs(RegClass rc) {
  return isFloatClass(rc) ? kFPRCallerSaved : kGPRCallerSaved;
}

// 按寄存器类返回对应的 callee-saved 集合。
const std::vector<PhysReg> &calleeSavedRegs(RegClass rc) {
  return isFloatClass(rc) ? kFPRCalleeSaved : kGPRCalleeSaved;
}

// 返回物理寄存器的汇编名字符串(如 "a0"),供汇编发射直接输出。
const char *regName(PhysReg reg) { return getRegisterInfo(reg).name; }
} // namespace riscv
