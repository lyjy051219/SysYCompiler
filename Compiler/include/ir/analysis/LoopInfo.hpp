#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "ir/analysis/Dominant.hpp"
#include "lib/CFG.hpp"
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>

class LoopInfo;
//LoopInfo：参考llvm算法实现，但是对于第三步循环做了改进，改为基于RPO顺序进行遍历
//同时对其做了简化，和本地适配

// 表示一个自然循环
class Loop {
    friend class LoopInfo;
public:
    // 循环头 (header)
    BasicBlock* Header = nullptr;
    // 父循环 (nullptr 表示是最外层循环)
    Loop* ParentLoop = nullptr;
    // 子循环列表
    std::vector<Loop*> SubLoops;

    // 该循环拥有的 BasicBlock 集合
    std::vector<BasicBlock*> Blocks;

    // 回边起始节点列表（回边终点即 Header）
    std::vector<BasicBlock*> BackedgeSrcs;

    Loop() = default;

    // 获取循环头
    BasicBlock* getHeader() const { return Header; }
    // 获取父循环
    Loop* getParentLoop() const { return ParentLoop; }
    // 获取所有子循环
    const std::vector<Loop*>& getSubLoops() const { return SubLoops; }
    // 获取直接属于该循环的块（不含子循环的块）
    const std::vector<BasicBlock*>& getBlocks() const { return Blocks; }
    // 获取回边起始块
    const std::vector<BasicBlock*>& getBackedgeSrcs() const { return BackedgeSrcs; }

    // 获取循环深度（1 表示最外层）
    unsigned getDepth() const {
        unsigned depth = 1;
        for (Loop* p = ParentLoop; p != nullptr; p = p->ParentLoop)
            ++depth;
        return depth;
    }
    // 该块是否在本循环中（包括子循环）
    bool contains(BasicBlock* BB) const;
    // 收集本循环（含子循环）通向循环外部的出口块
    std::vector<BasicBlock*> getExitBlocks() const;
    // 检查是否满足 LoopSimplify 的基础形状：唯一 PreHeader、唯一 Latch、Dedicated Exits
    bool getLoopSimplifyForm(BasicBlock* &PreHeader, BasicBlock* &Latch) const;
    // 是否是最内层循环（没有子循环）
    bool isInnermost() const { return SubLoops.empty(); }
};

// 分析结果：整个函数的循环结构
class LoopInfo {
    friend class LoopInfoBuilder;
public:
    // 所有循环对象，拥有所有权
    std::vector<std::unique_ptr<Loop>> Loops;
    // 顶层循环（没有父循环的循环）
    std::vector<Loop*> TopLevelLoops;
    // BasicBlock -> 直接所属的最内层 Loop（nullptr 表示不在任何循环中）
    std::unordered_map<BasicBlock*, Loop*> BBToLoop;

    LoopInfo() = default;
    LoopInfo(LoopInfo&&) = default;
    LoopInfo& operator=(LoopInfo&&) = default;
    LoopInfo(const LoopInfo&) = delete;
    LoopInfo& operator=(const LoopInfo&) = delete;

    void clear() {
        Loops.clear();
        TopLevelLoops.clear();
        BBToLoop.clear();
    }

    // 查询某块所在的最内层循环（nullptr 表示不在循环中）
    Loop* getLoopFor(BasicBlock* BB) const {
        auto it = BBToLoop.find(BB);
        return it != BBToLoop.end() ? it->second : nullptr;
    }

    // 获取顶层循环列表
    const std::vector<Loop*>& getTopLevelLoops() const { return TopLevelLoops; }

    bool invalidate(Function &F, const PreservationStatus &PA);
};

// 内部构建器：负责执行算法
class LoopInfoBuilder {
public:
    LoopInfo build(Function &F, const DominatorTree &DT);
private:
    // 步骤1：后序遍历支配树，识别回边和循环头，创建 Loop 对象
    void discoverLoopHeaders(Function &F, const DominatorTree &DT,
                             LoopInfo &LI,
                             std::vector<Loop*> &RPOLoops);
    // 步骤2：从回边起点反向遍历 CFG，将 BasicBlock 归属到循环
    void LoopsCFG(LoopInfo &LI, const DominatorTree &DT);
    // 步骤3：RPO 遍历建立父子循环关系
    void LoopsDFS(LoopInfo &LI, const std::vector<Loop*> &RPOLoops);
};

// 分析 Pass
class LoopAnalysis : public AnalysisDescriptor<LoopAnalysis> {
public:
    using Result = LoopInfo;
    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;

private:
    friend AnalysisDescriptor<LoopAnalysis>;
    static PassID Key;
};
