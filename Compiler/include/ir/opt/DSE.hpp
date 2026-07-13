#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

class BasicAAResult;
class LoopInfo;
struct MemoryLocation;

enum class DSEBlockColor {
    Kill,
    Clobber,
    Transparent,
};

// 死存储消除
class DSEPass : public PassDescriptor<DSEPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    static bool eliminateInBlock(BasicBlock *BB);
    static DSEBlockColor classifyFrom(
        Instruction *First, const MemoryLocation &Location, BasicAAResult &AA,
        const LoopInfo &LI, FunctionAnalysisPassManager &FAM);
    static bool isStoreDead(StoreInst *Store, BasicAAResult &AA,
                            const LoopInfo &LI,
                            FunctionAnalysisPassManager &FAM);
};
