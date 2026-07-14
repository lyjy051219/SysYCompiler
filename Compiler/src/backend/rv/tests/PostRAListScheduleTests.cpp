#include "lib/CFG.hpp"
#include "riscv/opt/CodegenPasses.hpp"
#include "riscv/opt/MachineAnalysis.hpp"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

std::unique_ptr<Module> ModuleIR = std::make_unique<Module>();

namespace {

using namespace riscv;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      throw std::runtime_error(std::string("CHECK failed: ") + #condition +    \
                               " at line " + std::to_string(__LINE__));        \
    }                                                                          \
  } while (false)

MachineReg gpr(PhysReg reg) { return MachineReg::preg(reg, RegClass::GPR64); }

MachineInstr makeRet() {
  MachineInstr instr;
  instr.opcode = Opcode::RET;
  instr.terminator = true;
  return instr;
}

MachineInstr makeAdd(MachineReg dst, MachineReg lhs, MachineReg rhs) {
  MachineInstr instr;
  instr.opcode = Opcode::ADD;
  instr.defs.push_back(MachineOperand::reg(dst));
  instr.uses.push_back(MachineOperand::reg(lhs));
  instr.uses.push_back(MachineOperand::reg(rhs));
  return instr;
}

MachineInstr makeMul(MachineReg dst, MachineReg lhs, MachineReg rhs) {
  MachineInstr instr;
  instr.opcode = Opcode::MUL;
  instr.defs.push_back(MachineOperand::reg(dst));
  instr.uses.push_back(MachineOperand::reg(lhs));
  instr.uses.push_back(MachineOperand::reg(rhs));
  return instr;
}

MachineInstr makeDiv(MachineReg dst, MachineReg lhs, MachineReg rhs) {
  MachineInstr instr;
  instr.opcode = Opcode::DIV;
  instr.defs.push_back(MachineOperand::reg(dst));
  instr.uses.push_back(MachineOperand::reg(lhs));
  instr.uses.push_back(MachineOperand::reg(rhs));
  return instr;
}

MachineInstr makeLoad(MachineReg dst, MachineReg base, int64_t offset) {
  MachineInstr instr;
  instr.opcode = Opcode::LW;
  instr.defs.push_back(MachineOperand::reg(dst));
  instr.uses.push_back(MachineOperand::reg(base));
  instr.uses.push_back(MachineOperand::imm(offset));
  return instr;
}

MachineInstr makeStore(MachineReg src, MachineReg base, int64_t offset) {
  MachineInstr instr;
  instr.opcode = Opcode::SW;
  instr.uses.push_back(MachineOperand::reg(src));
  instr.uses.push_back(MachineOperand::reg(base));
  instr.uses.push_back(MachineOperand::imm(offset));
  instr.sideEffects = true;
  return instr;
}

MachineInstr makeCall(const std::string &name) {
  MachineInstr instr;
  instr.opcode = Opcode::CALL;
  instr.uses.push_back(MachineOperand::global(name));
  instr.implicitDefs.push_back(PhysReg::RA);
  instr.sideEffects = true;
  return instr;
}

MachineBasicBlock makeBlock(std::string label,
                           std::vector<MachineInstr> instrs) {
  MachineBasicBlock block;
  block.label = std::move(label);
  block.instrs = std::move(instrs);
  return block;
}

MachineFunctionAnalysisManager makeAnalysisManager() {
  MachineFunctionAnalysisManager am;
  am.registerPass([] { return MachineInstrStatsAnalysis(); });
  am.registerPass([] { return MachineCFGAnalysis(); });
  am.registerPass([] { return MachineLivenessAnalysis(); });
  am.registerPass([] { return MachineLoopAnalysis(); });
  am.registerPass([] { return MachineBlockFrequencyAnalysis(); });
  return am;
}

void primeAnalyses(MachineFunction &mf, MachineFunctionAnalysisManager &am) {
  am.getResult<MachineInstrStatsAnalysis>(mf);
  am.getResult<MachineCFGAnalysis>(mf);
  am.getResult<MachineLivenessAnalysis>(mf);
  am.getResult<MachineLoopAnalysis>(mf);
  am.getResult<MachineBlockFrequencyAnalysis>(mf);
}

std::vector<Opcode> opcodes(const MachineFunction &mf) {
  std::vector<Opcode> result;
  for (const MachineInstr &instr : mf.blocks.front().instrs) {
    result.push_back(instr.opcode);
  }
  return result;
}

void runSchedule(MachineFunction &mf, MachineFunctionAnalysisManager &am,
                 bool expectChange) {
  MachineFunctionPassManager pm;
  pm.addPass(PostRAListSchedulePass());
  const auto PA = pm.run(mf, am);
  CHECK(PA.isKept(MachineCFGAnalysis::ID()));
  CHECK(PA.isKept(MachineInstrStatsAnalysis::ID()));
  CHECK(PA.isKept(MachineLivenessAnalysis::ID()));
  CHECK(PA.isKept(MachineLoopAnalysis::ID()));
  CHECK(PA.isKept(MachineBlockFrequencyAnalysis::ID()));
  CHECK(expectChange ? !PA.areAllKept() : PA.areAllKept());
  CHECK(am.getCachedResult<MachineInstrStatsAnalysis>(mf) != nullptr);
  CHECK(am.getCachedResult<MachineCFGAnalysis>(mf) != nullptr);
  CHECK(am.getCachedResult<MachineLivenessAnalysis>(mf) != nullptr);
  CHECK(am.getCachedResult<MachineLoopAnalysis>(mf) != nullptr);
  CHECK(am.getCachedResult<MachineBlockFrequencyAnalysis>(mf) != nullptr);
}

void testLatencyDrivenReorder() {
  MachineFunction mf;
  mf.blocks = {
      makeBlock("entry",
                {makeAdd(gpr(PhysReg::T0), gpr(PhysReg::S1), gpr(PhysReg::S2)),
                 makeMul(gpr(PhysReg::T1), gpr(PhysReg::S3), gpr(PhysReg::S4)),
                 makeAdd(gpr(PhysReg::T2), gpr(PhysReg::T0), gpr(PhysReg::T1)),
                 makeRet()})};

  auto am = makeAnalysisManager();
  primeAnalyses(mf, am);
  runSchedule(mf, am, true);

  CHECK(opcodes(mf) ==
        std::vector<Opcode>({Opcode::MUL, Opcode::ADD, Opcode::ADD,
                             Opcode::RET}));
}

void testMemoryChainAndGapFilling() {
  MachineFunction mf;
  mf.blocks = {
      makeBlock("entry",
                {makeAdd(gpr(PhysReg::T6), gpr(PhysReg::S1), gpr(PhysReg::S2)),
                 makeLoad(gpr(PhysReg::T3), gpr(PhysReg::SP), 0),
                 makeStore(gpr(PhysReg::T0), gpr(PhysReg::SP), 4), makeRet()})};

  auto am = makeAnalysisManager();
  primeAnalyses(mf, am);
  runSchedule(mf, am, true);

  CHECK(opcodes(mf) ==
        std::vector<Opcode>({Opcode::LW, Opcode::ADD, Opcode::SW,
                             Opcode::RET}));
}

void testAllUsesStayBeforeOverwrite() {
  MachineFunction mf;
  mf.blocks = {
      makeBlock("entry",
                {makeAdd(gpr(PhysReg::T1), gpr(PhysReg::T0), gpr(PhysReg::S1)),
                 makeDiv(gpr(PhysReg::T2), gpr(PhysReg::T0), gpr(PhysReg::S2)),
                 makeMul(gpr(PhysReg::T0), gpr(PhysReg::S3), gpr(PhysReg::S4)),
                 makeRet()})};

  auto am = makeAnalysisManager();
  primeAnalyses(mf, am);
  runSchedule(mf, am, true);

  CHECK(opcodes(mf) ==
        std::vector<Opcode>({Opcode::DIV, Opcode::ADD, Opcode::MUL,
                             Opcode::RET}));
}

void testCallBarrierAndPreservation() {
  MachineFunction mf;
  mf.blocks = {
      makeBlock("entry",
                {makeAdd(gpr(PhysReg::T0), gpr(PhysReg::S1), gpr(PhysReg::S2)),
                 makeMul(gpr(PhysReg::T1), gpr(PhysReg::S3), gpr(PhysReg::S4)),
                 makeCall("callee"),
                 makeAdd(gpr(PhysReg::T2), gpr(PhysReg::S5), gpr(PhysReg::S6)),
                 makeMul(gpr(PhysReg::T3), gpr(PhysReg::S7), gpr(PhysReg::S8)),
                 makeRet()})};

  auto am = makeAnalysisManager();
  primeAnalyses(mf, am);
  runSchedule(mf, am, false);

  CHECK(opcodes(mf) ==
        std::vector<Opcode>({Opcode::ADD, Opcode::MUL, Opcode::CALL,
                             Opcode::ADD, Opcode::MUL, Opcode::RET}));
}

void testPureSingleCycleBlockSkipped() {
  MachineFunction mf;
  mf.blocks = {
      makeBlock("entry",
                {makeAdd(gpr(PhysReg::T0), gpr(PhysReg::S1), gpr(PhysReg::S2)),
                 makeAdd(gpr(PhysReg::T1), gpr(PhysReg::S3), gpr(PhysReg::S4)),
                 makeAdd(gpr(PhysReg::T2), gpr(PhysReg::S5), gpr(PhysReg::S6)),
                 makeRet()})};

  auto am = makeAnalysisManager();
  primeAnalyses(mf, am);

  MachineFunctionPassManager pm;
  pm.addPass(PostRAListSchedulePass());
  const auto PA = pm.run(mf, am);

  CHECK(PA.areAllKept());
  CHECK(opcodes(mf) ==
        std::vector<Opcode>({Opcode::ADD, Opcode::ADD, Opcode::ADD,
                             Opcode::RET}));
}

} // namespace

int main() {
  testLatencyDrivenReorder();
  testMemoryChainAndGapFilling();
  testAllUsesStayBeforeOverwrite();
  testCallBarrierAndPreservation();
  testPureSingleCycleBlockSkipped();
  std::cout << "PostRAListScheduleTests passed\n";
  return 0;
}
