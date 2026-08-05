#pragma once

#include "MIR.hpp"
#include <cstdint>
#include <iosfwd>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aarch64 {

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
  // 折算基址与偏移（寄存器或 FrameIndex），返回 64 位基址寄存器名。
  std::string resolveMemoryBase(const MachineOperand &base, int64_t &offset);
  PhysReg assignVirtualReg(MachineReg reg);
  // 解析操作数对应的物理寄存器（物理直取，虚拟走临时分配）。
  PhysReg regForOperand(const MachineOperand &operand);
  std::string operandReg(const MachineOperand &operand);
  // 取操作数寄存器的 64 位 x 形式（地址计算/立即数物化用）。
  std::string operandRegWide(const MachineOperand &operand);
  // 以指定 RegClass 宽度输出寄存器名（COPY 两端宽度可能不同）。
  std::string operandRegAt(const MachineOperand &operand, RegClass rc);
};

} // namespace aarch64
