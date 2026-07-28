#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <deque>
#include <unordered_set>

class PostDominatorTree;

// 标记阶段状态：活跃指令/块、反向数据依赖工作表和已展开控制依赖的块。
struct ADCEContext {
    std::unordered_set<Instruction*> LiveInsts;
    std::unordered_set<BasicBlock*> LiveBlocks;
    std::deque<Instruction*> Worklist;
    std::unordered_set<BasicBlock*> CDProcessed;
};

// 流程：1. 以可观察操作和必要 CFG 为活跃根；2. 沿操作数和后支配控制依赖求闭包；
// 3. 将死条件分支改为原后继无条件边；4. 用 undef 断开并删除其余死指令。
class ADCEPass : public PassDescriptor<ADCEPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    static bool callHasObservableSideEffect(CallInst *CI);
    static bool isRootLive(
        Instruction *I, PostDominatorTree &PDT);
    static void markLive(Instruction *I, ADCEContext &Context);
    static void markControlDependences(
        BasicBlock *BB, PostDominatorTree &PDT, ADCEContext &Context);
    static BasicBlock *pickSurvivorSuccessor(
        BasicBlock *BB, PostDominatorTree &PDT);
    static bool sweep(
        Function &F, PostDominatorTree &PDT, ADCEContext &Context,
        bool &CFGChanged);
};
