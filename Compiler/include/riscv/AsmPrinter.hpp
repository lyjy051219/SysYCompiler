// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "MIR.hpp"
#include <cstdint>
#include <iosfwd>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace riscv {

class AsmPrinter {
  std::ostream &out_;
  int pcrelId_ = 0;
  const MachineFunction *currentFunction_ = nullptr;
  std::unordered_map<uint32_t, PhysReg> gprVRegMap_;
  std::unordered_map<uint32_t, PhysReg> fprVRegMap_;
  std::unordered_map<uint32_t, int> vregUseCounts_;
  std::unordered_map<uint32_t, int> vregDefCounts_;
  std::vector<PhysReg> freeGprTemps_;
  std::vector<PhysReg> freeFprTemps_;

public:
  explicit AsmPrinter(std::ostream &out);
  void emitModule(const MachineModule &module);

private:
  void emitGlobal(const GlobalObject &global);
  void emitFunction(const MachineFunction &function);
  void emitInstr(const MachineInstr &instr);
  void prepareVirtualRegTemps(const MachineFunction &function);
  void releaseInstrUses(const MachineInstr &instr);
  void releaseDeadDefs(const MachineInstr &instr);
  void releaseOperandUse(const MachineOperand &operand);
  void releaseVirtualReg(MachineReg reg);
  PhysReg acquireScratchGpr();
  void releaseScratchGpr(PhysReg reg);
  void emitLoadAddress(const MachineOperand &dst, const std::string &symbol);
  void emitStackAdjust(int64_t amount);
  void emitMemoryInstr(const MachineInstr &instr);
  std::pair<std::string, PhysReg> memoryBaseReg(const MachineOperand &base,
                                                int64_t &offset);
  PhysReg assignVirtualReg(MachineReg reg);
  std::string operandReg(const MachineOperand &operand);
};

} // namespace riscv
 