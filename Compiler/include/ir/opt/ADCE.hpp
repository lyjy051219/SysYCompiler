#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <deque>
#include <unordered_set>

struct ADCEContext {
    std::unordered_set<Instruction*> LiveInsts;
    std::unordered_set<BasicBlock*> LiveBlocks;
    std::deque<Instruction*> Worklist;
    std::unordered_set<BasicBlock*> CDProcessed;
};

// Aggressive Dead Code Elimination
// Mark-Sweep: 从 root-live 指令反向传播活性 + postdom 控制依赖补活
// Sweep: 死条件分支改写为无条件跳转 + 删死指令
class ADCEPass : public PassDescriptor<ADCEPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);
};
