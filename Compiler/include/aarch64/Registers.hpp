#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace aarch64 {

enum class RegClass { GPR32, GPR64, FPR32 };

enum class PhysReg : uint16_t {
  NoReg = 0,
  X0, RA, SP, GP, TP, T0, T1, T2, S0, S1, A0, A1, A2, A3, A4, A5, A6, A7,
  S2, S3, S4, S5, S6, S7, S8, S9, S10, S11, T3, T4, T5, T6,
  FT0, FT1, FT2, FT3, FT4, FT5, FT6, FT7, FS0, FS1, FA0, FA1, FA2, FA3,
  FA4, FA5, FA6, FA7, FS2, FS3, FS4, FS5, FS6, FS7, FS8, FS9, FS10, FS11,
  FT8, FT9, FT10, FT11
};

struct RegisterInfo {
  PhysReg reg;
  const char *name;
  bool allocatable;
  bool callerSaved;
  bool calleeSaved;
  bool isFloat;
};

const RegisterInfo &getRegisterInfo(PhysReg reg);
const std::vector<PhysReg> &allocatableRegs(RegClass rc);
const std::vector<PhysReg> &callerSavedRegs(RegClass rc);
const std::vector<PhysReg> &calleeSavedRegs(RegClass rc);
const char *regName(PhysReg reg);
bool isValidPhysReg(PhysReg reg);
bool isValidRegClass(RegClass rc);

} // namespace aarch64
