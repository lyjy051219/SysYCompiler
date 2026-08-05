#include "aarch64/Registers.hpp"

#include <array>
#include <cstddef>
#include <vector>

// ARM64(AArch64) 寄存器定义。
// 为最大化复用既有的图着色寄存器分配器与栈帧逻辑，这里沿用 RISC-V 后端的
// PhysReg 枚举名（X0/RA/SP/T*/A*/S*/FT*/FA*/FS*），只把它们映射到实际的
// AArch64 物理寄存器，并按 AAPCS64 调用约定重新标注 caller/callee-saved。
// GPR 名字给出 64 位形式（x0..x30/sp/xzr）；AsmPrinter 会根据操作数的
// RegClass 自行选择 w/x 前缀，GPR32 与 GPR64 共用同一物理寄存器堆。
//
// 关键映射（保持可分配寄存器总数 25 个 GPR / 32 个 FPR 以维持分配器行为）：
//   A0-A7 对应 x0-x7，实参/返回，caller-saved 可分配
//   T0-T5 对应 x9-x14，临时，caller-saved 可分配
//   S11 对应 x15，AArch64 callee-saved 只有 10 个，故 S11 改为 caller-saved
//   S1-S10 对应 x19-x28，callee-saved 可分配
//   T6 对应 x16（IP0），保留给汇编阶段展开大偏移/memcpy，不可分配
//   S0 对应 x29（fp），callee-saved 但不可分配（占位，不实际使用）
//   RA 对应 x30（lr），返回地址，caller-saved 不可分配
//   X0（零）对应 xzr/wzr，由宽度决定，不可分配
//   GP/TP 对应 x18/x17，占位，代码生成中不会出现
//   FA0-FA7 对应 s0-s7，浮点实参/返回，caller-saved
//   FS0-FS7 对应 s8-s15，浮点 callee-saved（仅 v8-v15 为 callee-saved）
//   FS8-FS11 对应 s28-s31，超出 8 个名额的部分改为 caller-saved
//   FT0-FT11 对应 s16-s27，浮点临时，caller-saved

namespace aarch64 {
namespace {

// 寄存器总数:以最后一个浮点寄存器 FT11 的枚举值加一为上界,用于开静态数组。
constexpr std::size_t kNumRegs =
    static_cast<std::size_t>(PhysReg::FT11) + 1;

// 物理寄存器信息表。每一项字段顺序为:
// {PhysReg 枚举, AArch64 汇编名, 可分配, callerSaved, calleeSaved, isFloat}。
// 表按 PhysReg 枚举值顺序排列,可用枚举值直接下标索引。
// 汇编名用寄存器堆的 64 位/单精度形式(x0../s0..),w 前缀由 AsmPrinter 按
// RegClass 现场决定;X0 表项名为 xzr(零寄存器,宽度决定 wzr/xzr),SP 名为 sp。
// AAPCS64 角色见文件头映射说明。
constexpr std::array<RegisterInfo, kNumRegs> kRegisterInfos{{
    {PhysReg::NoReg, "noreg", false, false, false, false},
    {PhysReg::X0, "xzr", false, false, false, false},
    {PhysReg::RA, "x30", false, true, false, false},
    {PhysReg::SP, "sp", false, false, false, false},
    {PhysReg::GP, "x18", false, false, false, false},
    {PhysReg::TP, "x17", false, false, false, false},
    {PhysReg::T0, "x9", true, true, false, false},
    {PhysReg::T1, "x10", true, true, false, false},
    {PhysReg::T2, "x11", true, true, false, false},
    {PhysReg::S0, "x29", false, false, true, false},
    {PhysReg::S1, "x19", true, false, true, false},
    {PhysReg::A0, "x0", true, true, false, false},
    {PhysReg::A1, "x1", true, true, false, false},
    {PhysReg::A2, "x2", true, true, false, false},
    {PhysReg::A3, "x3", true, true, false, false},
    {PhysReg::A4, "x4", true, true, false, false},
    {PhysReg::A5, "x5", true, true, false, false},
    {PhysReg::A6, "x6", true, true, false, false},
    {PhysReg::A7, "x7", true, true, false, false},
    {PhysReg::S2, "x20", true, false, true, false},
    {PhysReg::S3, "x21", true, false, true, false},
    {PhysReg::S4, "x22", true, false, true, false},
    {PhysReg::S5, "x23", true, false, true, false},
    {PhysReg::S6, "x24", true, false, true, false},
    {PhysReg::S7, "x25", true, false, true, false},
    {PhysReg::S8, "x26", true, false, true, false},
    {PhysReg::S9, "x27", true, false, true, false},
    {PhysReg::S10, "x28", true, false, true, false},
    // S11 落到 caller-saved 的 x15：AArch64 callee-saved 比 RISC-V 少一个。
    {PhysReg::S11, "x15", true, true, false, false},
    {PhysReg::T3, "x12", true, true, false, false},
    {PhysReg::T4, "x13", true, true, false, false},
    {PhysReg::T5, "x14", true, true, false, false},
    // T6 对应 IP0（x16），保留给汇编阶段展开，绝不参与寄存器分配。
    {PhysReg::T6, "x16", false, true, false, false},
    {PhysReg::FT0, "s16", true, true, false, true},
    {PhysReg::FT1, "s17", true, true, false, true},
    {PhysReg::FT2, "s18", true, true, false, true},
    {PhysReg::FT3, "s19", true, true, false, true},
    {PhysReg::FT4, "s20", true, true, false, true},
    {PhysReg::FT5, "s21", true, true, false, true},
    {PhysReg::FT6, "s22", true, true, false, true},
    {PhysReg::FT7, "s23", true, true, false, true},
    {PhysReg::FS0, "s8", true, false, true, true},
    {PhysReg::FS1, "s9", true, false, true, true},
    {PhysReg::FA0, "s0", true, true, false, true},
    {PhysReg::FA1, "s1", true, true, false, true},
    {PhysReg::FA2, "s2", true, true, false, true},
    {PhysReg::FA3, "s3", true, true, false, true},
    {PhysReg::FA4, "s4", true, true, false, true},
    {PhysReg::FA5, "s5", true, true, false, true},
    {PhysReg::FA6, "s6", true, true, false, true},
    {PhysReg::FA7, "s7", true, true, false, true},
    {PhysReg::FS2, "s10", true, false, true, true},
    {PhysReg::FS3, "s11", true, false, true, true},
    {PhysReg::FS4, "s12", true, false, true, true},
    {PhysReg::FS5, "s13", true, false, true, true},
    {PhysReg::FS6, "s14", true, false, true, true},
    {PhysReg::FS7, "s15", true, false, true, true},
    // FS8-FS11 超出 8 个浮点 callee-saved 名额，改为 caller-saved。
    {PhysReg::FS8, "s28", true, true, false, true},
    {PhysReg::FS9, "s29", true, true, false, true},
    {PhysReg::FS10, "s30", true, true, false, true},
    {PhysReg::FS11, "s31", true, true, false, true},
    {PhysReg::FT8, "s24", true, true, false, true},
    {PhysReg::FT9, "s25", true, true, false, true},
    {PhysReg::FT10, "s26", true, true, false, true},
    {PhysReg::FT11, "s27", true, true, false, true},
}};

// 整型可分配寄存器池:寄存器分配器可自由指派的 GPR(共 25 个)。
// 不含 sp/x29(fp)/x30(lr)/IP0 等有固定用途或被保留的寄存器。
const std::vector<PhysReg> kGPRAllocatable{
    PhysReg::T0, PhysReg::T1, PhysReg::T2, PhysReg::T3, PhysReg::T4,
    PhysReg::T5, PhysReg::A0, PhysReg::A1, PhysReg::A2, PhysReg::A3,
    PhysReg::A4, PhysReg::A5, PhysReg::A6, PhysReg::A7, PhysReg::S1,
    PhysReg::S2, PhysReg::S3, PhysReg::S4, PhysReg::S5, PhysReg::S6,
    PhysReg::S7, PhysReg::S8, PhysReg::S9, PhysReg::S10, PhysReg::S11};

// 浮点可分配寄存器池:所有 ft/fa/fs 浮点寄存器均可参与分配(共 32 个)。
const std::vector<PhysReg> kFPRAllocatable{
    PhysReg::FT0, PhysReg::FT1, PhysReg::FT2, PhysReg::FT3, PhysReg::FT4,
    PhysReg::FT5, PhysReg::FT6, PhysReg::FT7, PhysReg::FT8, PhysReg::FT9,
    PhysReg::FT10, PhysReg::FT11, PhysReg::FA0, PhysReg::FA1, PhysReg::FA2,
    PhysReg::FA3, PhysReg::FA4, PhysReg::FA5, PhysReg::FA6, PhysReg::FA7,
    PhysReg::FS0, PhysReg::FS1, PhysReg::FS2, PhysReg::FS3, PhysReg::FS4,
    PhysReg::FS5, PhysReg::FS6, PhysReg::FS7, PhysReg::FS8, PhysReg::FS9,
    PhysReg::FS10, PhysReg::FS11};

// caller-saved 集合必须与 RegisterInfo 标注一致：RegAlloc 在 CALL 处按此集合
// 打入 clobber。S11(x15) 与 FS8-FS11(s28-s31) 已从 callee 改为 caller。
const std::vector<PhysReg> kGPRCallerSaved{
    PhysReg::RA, PhysReg::T0, PhysReg::T1, PhysReg::T2, PhysReg::T3,
    PhysReg::T4, PhysReg::T5, PhysReg::A0, PhysReg::A1,
    PhysReg::A2, PhysReg::A3, PhysReg::A4, PhysReg::A5, PhysReg::A6,
    PhysReg::A7, PhysReg::S11};

// 浮点 caller-saved 集合:所有 ft 与 fa 寄存器,外加溢出到 caller-saved 的 fs8-fs11。
const std::vector<PhysReg> kFPRCallerSaved{
    PhysReg::FT0, PhysReg::FT1, PhysReg::FT2, PhysReg::FT3, PhysReg::FT4,
    PhysReg::FT5, PhysReg::FT6, PhysReg::FT7, PhysReg::FT8, PhysReg::FT9,
    PhysReg::FT10, PhysReg::FT11, PhysReg::FA0, PhysReg::FA1, PhysReg::FA2,
    PhysReg::FA3, PhysReg::FA4, PhysReg::FA5, PhysReg::FA6, PhysReg::FA7,
    PhysReg::FS8, PhysReg::FS9, PhysReg::FS10, PhysReg::FS11};

// 整型 callee-saved(被调用者保存)集合:映射到 x19-x28 的 S0-S10,
// 函数若用到需在 prologue 保存(S0=x29/fp 占位不实际参与分配)。
// 注意 S11 已改为 caller-saved,不在此列。
const std::vector<PhysReg> kGPRCalleeSaved{
    PhysReg::S0, PhysReg::S1, PhysReg::S2, PhysReg::S3, PhysReg::S4,
    PhysReg::S5, PhysReg::S6, PhysReg::S7, PhysReg::S8, PhysReg::S9,
    PhysReg::S10};

// 浮点 callee-saved 集合:映射到 v8-v15 的 fs0-fs7(AAPCS64 只有这 8 个是
// callee-saved,且仅低 64 位需保存)。fs8-fs11 已溢出为 caller-saved。
const std::vector<PhysReg> kFPRCalleeSaved{
    PhysReg::FS0, PhysReg::FS1, PhysReg::FS2, PhysReg::FS3, PhysReg::FS4,
    PhysReg::FS5, PhysReg::FS6, PhysReg::FS7};

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

// 返回物理寄存器的 AArch64 汇编名(如 "x0"),供汇编发射据此再选 w/x/s 宽度。
const char *regName(PhysReg reg) { return getRegisterInfo(reg).name; }
} // namespace aarch64
