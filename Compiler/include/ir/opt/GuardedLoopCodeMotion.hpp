// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <unordered_map>
#include <unordered_set>
#include <vector>

class DominatorTree;
class Loop;
class LoopInfo;
class RangeInfo;
class SCEVResult;
class TargetCostInfo;
struct InductionDescriptor;
struct LoopTripCountInfo;
 
//GuardedLoopCodeMotionPass 为可能零次执行的头测循环建立显式入口守卫，
//再把仅供循环使用的昂贵纯计算移入受保护预头。
class GuardedLoopCodeMotionPass
    : public PassDescriptor<GuardedLoopCodeMotionPass> {
public:
    //每次运行至多提交一个 CFG 计划，交由外层固定点重建全部结构分析。
    PreservationStatus run(Function &F,
                           FunctionAnalysisPassManager &FAM);

private:
    //GuardPlan 保存提交前已经完整验证的循环入口与指令集合。
    struct GuardPlan {
        Loop *TargetLoop = nullptr;
        BasicBlock *PreHeader = nullptr;
        BasicBlock *Header = nullptr;
        BasicBlock *Latch = nullptr;
        BasicBlock *EnterBlock = nullptr;
        BasicBlock *ExitBlock = nullptr;
        BrInst *HeaderBranch = nullptr;
        bool EnterOnTrue = false;
        std::vector<Instruction *> HeaderCloneOrder;
        std::vector<Instruction *> Candidates;
    };

    //SaturatedParentPlan 保存“子循环退出后父循环状态已不再变化”的完整
    //结构证明；提交阶段只需截断父回边并让 CFG 接口折叠退化 PHI。
    struct SaturatedParentPlan {
        Loop *ParentLoop = nullptr;
        Loop *ChildLoop = nullptr;
        BasicBlock *ParentHeader = nullptr;
        BasicBlock *ParentLatch = nullptr;
        BasicBlock *ParentExit = nullptr;
        BrInst *ParentLatchBranch = nullptr;
        std::vector<PhiInst *> ParentHeaderPhis;
    };

    //限制单次下沉闭包与入口条件克隆规模，避免异常 CFG 拉高编译时间。
    inline static constexpr std::size_t MaxCandidateCount = 64;
    inline static constexpr std::size_t MaxHeaderCloneCount = 16;
    //至少节省一组明显昂贵的目标指令才支付额外入口分支。
    inline static constexpr int MinimumMovedCost = 8;

    //判断指令是否为可延后执行、可克隆的无内存纯标量计算。
    static bool isSafeScalarInstruction(Instruction *I);
    //判断定义在循环入口守卫位置是否可用。
    static bool isAvailableAtPreheader(
        Value *V, BasicBlock *PreHeader,
        const DominatorTree &DT);
    //收集 Header 条件和零次出口值需要克隆的有序纯计算。
    static bool collectHeaderCloneOrder(
        BasicBlock *Header, BasicBlock *PreHeader,
        BasicBlock *ExitBlock, const DominatorTree &DT,
        std::vector<Instruction *> &Order,
        std::unordered_set<Instruction *> &ProtectedPreheaderValues);
    //递归保护入口条件或零次出口值依赖的预头 SSA 定义。
    static bool protectPreheaderDependency(
        Value *Root, BasicBlock *PreHeader,
        std::unordered_set<Instruction *> &Protected,
        std::size_t &Visited);
    //判断循环是否已经具有指向同一出口的显式入口守卫。
    static bool hasExistingEntryGuard(BasicBlock *PreHeader,
                                      BasicBlock *ExitBlock);
    //筛选所有用户都局限在循环中的纯计算闭包，并做目标成本门禁。
    static bool collectCandidates(
        Function &F, FunctionAnalysisPassManager &FAM,
        const Loop &L, BasicBlock *PreHeader,
        const std::unordered_set<Instruction *> &ProtectedPreheaderValues,
        const TargetCostInfo &TargetCost,
        const std::vector<Instruction *> &HeaderCloneOrder,
        std::vector<Instruction *> &Candidates);
    //为一个循环建立不改 IR 的完整计划。
    static bool buildPlan(
        Function &F, FunctionAnalysisPassManager &FAM,
        Loop &L, const DominatorTree &DT,
        const TargetCostInfo &TargetCost, GuardPlan &Plan);
    //原子提交入口条件克隆、零次旁路、PHI 修复和纯计算搬移。
    static bool applyPlan(Function &F, const GuardPlan &Plan);
    //判断两个标量值是否为同一 SSA 定义或同类型同值常量。
    static bool sameScalarValue(Value *Left, Value *Right);
    //验证父循环状态 PHI 经子循环运行或零次旁路后均形成原状态的不动点。
    static bool matchSaturatedStatePhi(
        PhiInst *ParentPhi, BasicBlock *ParentPreHeader,
        BasicBlock *ParentHeader, BasicBlock *ParentLatch,
        BasicBlock *ChildPreHeader, BasicBlock *ChildHeader,
        BasicBlock *ChildLatch, PhiInst *&ChildPhi,
        PhiInst *&MergePhi);
    //验证父入口条件与子循环继续条件在状态回传映射下完全相同。
    static bool matchSaturationCondition(
        BrInst *ParentHeaderBranch, BasicBlock *ChildPreHeader,
        BrInst *ChildHeaderBranch, const Loop &Child,
        const std::unordered_map<PhiInst *, PhiInst *> &StateMap);
    //验证父循环控制归纳值只服务于自身有限回边，不会被程序观察。
    static bool hasOnlyParentControlUses(
        const InductionDescriptor &Induction,
        const LoopTripCountInfo &Trip,
        BrInst *ParentLatchBranch);
    //构建子循环饱和后父迭代可安全保留一次的完整计划。
    static bool buildSaturatedParentPlan(
        Loop &Parent, SCEVResult &SE, RangeInfo &RI,
        DominatorTree &DT,
        SaturatedParentPlan &Plan);
    //截断已证明冗余的父回边，并用现有 PHI 接口恢复单入口 SSA。
    static bool applySaturatedParentPlan(
        const SaturatedParentPlan &Plan);
};
