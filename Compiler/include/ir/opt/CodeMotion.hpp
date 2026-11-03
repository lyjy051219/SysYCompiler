// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>
 
class BasicAAResult;
class BlockFrequencyInfo;
class DominatorTree;
class LoopInfo;
class TargetCostInfo;

//CodeMotionPass只把可无条件执行的纯SSA计算下沉到全部使用点的共同支配区间。
//它不复制表达式，也不移动任何内存、调用、除余、危险移位或浮点转换。
class CodeMotionPass : public PassDescriptor<CodeMotionPass> {
public:
    //运行有预算的跨块CodeSink，并显式保留未受影响的CFG分析。
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    //UseSite把普通使用定位到用户块，把PHI使用定位到对应的入边前驱块。
    struct UseSite {
        //User记录真实持有Use的指令。
        Instruction *User = nullptr;
        //ExecutionBlock记录该Use在SSA语义上发生的基本块。
        BasicBlock *ExecutionBlock = nullptr;
        //IsPhiEdge区分普通块内使用与PHI入边使用。
        bool IsPhiEdge = false;
    };

    //Placement保存一处同时满足支配、循环层次和频率收益的候选位置。
    struct Placement {
        //Block记录候选插入块。
        BasicBlock *Block = nullptr;
        //Before记录块内最早普通用户；为空时插到终结指令之前。
        Instruction *Before = nullptr;
        //LoopDepth记录候选块的最内层循环深度。
        unsigned LoopDepth = 0;
        //Frequency记录BFI发布的相对静态执行频率。
        std::uint64_t Frequency = 0;
    };

    //限制单次运行检查的指令数，超大函数保守停止而不是产生编译时间尖峰。
    static constexpr std::size_t CandidateBudget = 32768;
    //限制单次运行实际搬移的指令数。
    static constexpr std::size_t MoveBudget = 8192;
    //限制一条表达式参与收益评估的Use数量。
    static constexpr std::size_t UseBudget = 256;
    //限制因下沉而延长活跃区间的不同SSA操作数数量。
    static constexpr unsigned MaxExtendedOperands = 4;
    //证明常量下标inbounds GEP的结果仍位于唯一已知对象或其one-past位置。
    static bool isProvablySafeGEP(GetElementPtrInst *GEP,
                                  BasicAAResult &AA);
    //筛选不会访问内存、不会产生异常/poison且不依赖undef的候选表达式。
    static bool isSafeCandidate(Instruction *I, BasicAAResult &AA);
    //按真实Use语义收集普通使用块和PHI入边块。
    static bool collectUseSites(Instruction *I,
                                std::vector<UseSite> &Sites);
    //计算两个块的最近公共支配者。
    static BasicBlock *nearestCommonDominator(BasicBlock *First,
                                               BasicBlock *Second,
                                               const DominatorTree &DT);
    //计算全部使用发生块的最近公共支配者，也就是本轮最晚块级放置点。
    static BasicBlock *computeLatestBlock(
        const std::vector<UseSite> &Sites, const DominatorTree &DT);
    //返回基本块所属最内层循环的深度。
    static unsigned getLoopDepth(BasicBlock *BB, const LoopInfo &LI);
    //寻找目标块内必须位于其前方的最早普通使用；PHI边使用只约束终结点。
    static Instruction *findInsertionAnchor(
        BasicBlock *Destination, const std::vector<UseSite> &Sites);
    //验证所有操作数定义在指令级均支配目标插入点。
    static bool operandsDominatePlacement(
        Instruction *I, BasicBlock *Destination, Instruction *Before,
        const DominatorTree &DT);
    //估计下沉后新增的跨块活跃SSA操作数数量。
    static unsigned estimateExtendedOperands(
        Instruction *I, BasicBlock *Destination);
    //结合循环深度、BFI动态收益、TTI代价和寄存器压力判断是否值得搬移。
    static bool isProfitable(
        Instruction *I, BasicBlock *Source, const Placement &Candidate,
        const LoopInfo &LI, const BlockFrequencyInfo &BFI,
        const TargetCostInfo &TargetCost);
    //在Source到Latest的支配链区间内选择循环最浅、频率最低的合法位置。
    static Placement choosePlacement(
        Instruction *I, BasicBlock *Latest,
        const std::vector<UseSite> &Sites, const DominatorTree &DT,
        const LoopInfo &LI, const BlockFrequencyInfo &BFI,
        const TargetCostInfo &TargetCost);
    //提交一次侵入式链表搬移。
    static bool moveToPlacement(Instruction *I,
                                const Placement &Destination);
};
