#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <deque>
#include <unordered_set>

class PostDominatorTree;

//标记阶段状态：活跃指令/块、反向数据依赖工作表和已展开控制依赖的块。
struct ADCEContext {
    //记录已经证明必须保留的活跃指令。
    std::unordered_set<Instruction*> LiveInsts;
    //记录至少包含一条活跃指令的基本块。
    std::unordered_set<BasicBlock*> LiveBlocks;
    //保存尚未展开数据和控制依赖的活跃指令。
    std::deque<Instruction*> Worklist;
    //记录已经展开过控制依赖的基本块以避免重复遍历。
    std::unordered_set<BasicBlock*> CDProcessed;
};

//流程：1. 以可观察操作和必要 CFG 为活跃根；2. 沿操作数和后支配控制依赖求闭包；
//3. 将死条件分支改为原后继无条件边；4. 用 undef 断开并删除其余死指令。
class ADCEPass : public PassDescriptor<ADCEPass> {
public:
    //从可观察根求活跃闭包并删除其余指令及死条件边。
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    //判断调用是否具有计时、I/O或其他可观察副作用。
    static bool callHasObservableSideEffect(CallInst *CI);
    //判断指令是否应作为激进死代码消除的初始活跃根。
    static bool isRootLive(
        Instruction *I, PostDominatorTree &PDT);
    //把指令及其所属基本块标活并将新指令加入工作表。
    static void markLive(Instruction *I, ADCEContext &Context);
    //利用后支配关系标记决定目标块是否执行的控制分支。
    static void markControlDependences(
        BasicBlock *BB, PostDominatorTree &PDT, ADCEContext &Context);
    //为死条件分支从原有后继中选择保持控制可达性的幸存边。
    static BasicBlock *pickSurvivorSuccessor(
        BasicBlock *BB, PostDominatorTree &PDT);
    //改写死条件分支并断开、删除所有未标活的非终结指令。
    static bool sweep(
        Function &F, PostDominatorTree &PDT, ADCEContext &Context,
        bool &CFGChanged);
};
