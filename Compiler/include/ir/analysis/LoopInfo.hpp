#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "ir/analysis/Dominant.hpp"
#include "lib/CFG.hpp"
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>

class LoopInfo;

// Loop 表示一个自然循环；Blocks 只含直接成员，SubLoops 形成完整嵌套树。
class Loop {
    friend class LoopInfo;
public:

    BasicBlock* Header = nullptr;

    Loop* ParentLoop = nullptr;

    std::vector<Loop*> SubLoops;

    std::vector<BasicBlock*> Blocks;

    std::vector<BasicBlock*> BackedgeSrcs;

    Loop();

    // 结构与顺序查询均基于构建阶段确定的归属关系，不重新推导循环。
    BasicBlock* getHeader() const;

    Loop* getParentLoop() const;

    const std::vector<Loop*>& getSubLoops() const;

    const std::vector<BasicBlock*>& getBlocks() const;

    const std::vector<BasicBlock*>& getBackedgeSrcs() const;

    unsigned getDepth() const;

    bool contains(BasicBlock* BB) const;

    std::vector<BasicBlock*> getExitBlocks() const;

    std::vector<BasicBlock*> getBlocksIncludingSubLoops() const;

    std::vector<BasicBlock*> getBlocksInFunctionOrder() const;

    std::vector<BasicBlock*> getBlocksInRPO() const;

    std::vector<Loop*> getLoopsInPostOrder(
        bool ReverseSiblingOrder = false) const;
    bool isExitingBlock(BasicBlock *BB) const;

    bool getLoopSimplifyForm(BasicBlock* &PreHeader, BasicBlock* &Latch) const;

    bool isInnermost() const;
};

// 函数级结果拥有全部 Loop，并把每个块映射到其最内层循环。
class LoopInfo {
    friend class LoopInfoBuilder;
public:

    std::vector<std::unique_ptr<Loop>> Loops;

    std::vector<Loop*> TopLevelLoops;

    std::unordered_map<BasicBlock*, Loop*> BBToLoop;

    LoopInfo();
    LoopInfo(LoopInfo&&);
    LoopInfo& operator=(LoopInfo&&);
    LoopInfo(const LoopInfo&) = delete;
    LoopInfo& operator=(const LoopInfo&) = delete;

    void clear();

    Loop* getLoopFor(BasicBlock* BB) const;

    const std::vector<Loop*>& getTopLevelLoops() const;
    std::vector<Loop*> getLoopsInPostOrder(
        bool ReverseSiblingOrder = false) const;

    // 仅按 SSA 定义位置判断循环不变性：常量、参数、全局值和循环外定义均不变。
    static bool isLoopInvariant(Value *V, const Loop *L);

    bool invalidate(Function &F, const PreservationStatus &PA);
};

// 构建流程依次发现回边头、逆向归属循环体、整理顶层和直接块列表。
class LoopInfoBuilder {
public:
    LoopInfo build(Function &F, const DominatorTree &DT);
private:

    void discoverLoopHeaders(Function &F, const DominatorTree &DT,
                             LoopInfo &LI,
                             std::vector<Loop*> &RPOLoops);

    void LoopsCFG(LoopInfo &LI, const DominatorTree &DT);

    void LoopsDFS(LoopInfo &LI, const std::vector<Loop*> &RPOLoops);
};

// Pass 入口依赖支配树，并返回自然循环森林。
class LoopAnalysis : public AnalysisDescriptor<LoopAnalysis> {
public:
    using Result = LoopInfo;
    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;

private:
    friend AnalysisDescriptor<LoopAnalysis>;
    static PassID Key;
};
