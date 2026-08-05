#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include <deque>
#include <unordered_set>
class PostDominatorTree;

//ADCEContext：用于保存一次执行过程中的状态
struct ADCEContext {
    //所有已经证明必须保留的指令
    std::unordered_set<Instruction*> LiveInsts;
    //至少包含一条活跃指令的基本块
    std::unordered_set<BasicBlock*> LiveBlocks;
    //尚未完成依赖传播的活跃指令队列
    std::deque<Instruction*> Worklist;
    //已经分析过控制依赖的基本块
    std::unordered_set<BasicBlock*> CDProcessed;
};
class ADCEPass : public PassDescriptor<ADCEPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    //接口1：ADCE一开始就活跃的根指令
    static bool isRootLive(
        Instruction *I, PostDominatorTree &PDT,
        FunctionAnalysisPassManager &FAM);
    //接口2：标记活跃性（加入到LiveInsts，WorkList，LiveBlocks）
    static void markLive(Instruction *I, ADCEContext &Context);
    //接口3：标记控制依赖（沿后支配树从目标块的前驱向上遍历，直到遇到目标块的立即后支配者）
    static void markControlDependences(
        BasicBlock *BB, PostDominatorTree &PDT, ADCEContext &Context);
    //为死条件分支从原有后继中选择保持控制可达性的幸存边。
    static BasicBlock *pickSurvivorSuccessor(BasicBlock *BB, PostDominatorTree &PDT);
    //执行清扫
    static bool sweep(
        Function &F, PostDominatorTree &PDT, ADCEContext &Context,
        bool &CFGChanged);
};
