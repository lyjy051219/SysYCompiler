#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "ir/analysis/Dominant.hpp"
#include "lib/CFG.hpp"
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
class LoopInfo;

//Loop表示一个自然循环
class Loop {
    friend class LoopInfo;
public:
    //唯一循环头
    BasicBlock* Header = nullptr;
    //直接父循环
    Loop* ParentLoop = nullptr;
    //直接嵌套的子循环
    std::vector<Loop*> SubLoops;
    //直接属于本循环的基本块，不包含内层循环的基本块
    std::vector<BasicBlock*> Blocks;
    //所有通过回边回到Heaader的源基本块
    std::vector<BasicBlock*> BackedgeSrcs;

    Loop();
    BasicBlock* getHeader() const;
    Loop* getParentLoop() const;
    const std::vector<Loop*>& getSubLoops() const;
    const std::vector<BasicBlock*>& getBlocks() const;
    const std::vector<BasicBlock*>& getBackedgeSrcs() const;
    //从当前循环开始，沿着父循环向上计算深度（表层循环深度为1）
    unsigned getDepth() const;
    //判断当前循环是否是最内层循环
    bool isInnermost() const;
    //检查目标基本块是否存在于当前或者内层循环中
    bool contains(BasicBlock* BB) const;
    //返回当前循环及其所有子循环所有基本块
    std::vector<BasicBlock*> getBlocksIncludingSubLoops() const;
    //获取该循环所有出口块：先获得当前循环及所有子循环基本块
    //然后遍历这些块的后继，若后继不在当前循环及子循环中，则是出口块
    std::vector<BasicBlock*> getExitBlocks() const;
    //按照所属函数中的基本块顺序返回当前循环所有基本块
    std::vector<BasicBlock*> getBlocksInFunctionOrder() const;
    //RPO：对CFG后序遍历，然后反转结果得到RPO，只获得当前循环层的基本块 -- 只有这个接口是只返回当前循环层
    std::vector<BasicBlock*> getBlocksInRPO() const;
    //后序遍历返回当前循环及其所有子循环
    std::vector<Loop*> getLoopsInPostOrder(bool ReverseSiblingOrder = false) const;
    //判断该基本块是否是该循环的退出块：即该基本块有后继不在该循环中
    bool isExitingBlock(BasicBlock *BB) const;
    //检查当前循环是否具有唯一 preheader、唯一 latch 和专用出口，并在成功时返回 preheader 与 latch
    bool getLoopSimplifyForm(BasicBlock* &PreHeader, BasicBlock* &Latch) const;
};

//LoopInfo：函数级分析结果，返回循环森林
class LoopInfo {
    friend class LoopInfoBuilder;
public:
    //所有Loop对象由LoopInfo直接持有，禁止拷贝和移动
    std::vector<std::unique_ptr<Loop>> Loops;
    //循环森林中所有顶层循环
    std::vector<Loop*> TopLevelLoops;
    //将每个循环内基本块映射到直接拥有它的最内层循环
    std::unordered_map<BasicBlock*, Loop*> BBToLoop;

    LoopInfo();
    LoopInfo(LoopInfo&&);
    LoopInfo& operator=(LoopInfo&&);
    LoopInfo(const LoopInfo&) = delete;
    LoopInfo& operator=(const LoopInfo&) = delete;
    void clear();

    //返回给定基本块直接属于的最内层循环
    Loop* getLoopFor(BasicBlock* BB) const;
    //返回循环森林中所有顶层循环
    const std::vector<Loop*>& getTopLevelLoops() const;
    //后序遍历返回所有循环
    std::vector<Loop*> getLoopsInPostOrder(bool ReverseSiblingOrder = false) const;
    //是否是循环不变值：常量、参数、全局值和定义在循环外的指令
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

//最外层包装类
class LoopAnalysis : public AnalysisDescriptor<LoopAnalysis> {
public:
    using Result = LoopInfo;
    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;

private:
    friend AnalysisDescriptor<LoopAnalysis>;
    static PassID Key;
};
