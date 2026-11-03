// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "ir/opt/NewPassManager.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
 
class BasicBlock;
class BinaryInst;
class BrInst;
class DominatorTree;
class Instruction;
class Loop;
class LoopDependenceInfo;
class LoopInfo;
class PhiInst;
class PostDominatorTree;
class SCEVResult;
struct SCEVExpr;
class Use;
class Value;

//LoopInterchange 的整体流程：
//1. 从内到外枚举相邻循环，并用函数级次数上限与循环头记录阻止重复交换。
//2. 优先识别旋转后的受限非完美嵌套，完整证明 CFG、SSA、依赖方向和局部性后物理重排循环轴。
//3. 其余矩形同域嵌套走角色交换路径，只交换两个规范 IV 的负载使用，必要时下沉外轴纯计算前缀。
//4. 先构造游离克隆并完成所有可失败检查，再集中提交 CFG/SSA 改写；失败时销毁未挂接对象。
//5. 角色交换保留支配、后支配和循环结构，物理交换则使所有函数分析失效。
class LoopInterchangePass : public PassDescriptor<LoopInterchangePass> {
public:
    //在函数中寻找相邻循环并执行一次合法且有收益的循环交换。
    PassResult run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    //限制单个函数在固定点迭代期间最多执行的交换次数。
    static constexpr unsigned MaxInterchangesPerFunction = 4;
    //限制角色交换路径可以下沉到内循环的纯计算指令数量。
    static constexpr unsigned MaxSinkInstructions = 8;
    //限定执行循环交换所需达到的最小访存局部性收益。
    static constexpr long long MinLocalityBenefit = 6;

    //HeaderPair以交换前的外、内循环头唯一标识一对处理对象。
    struct HeaderPair {
        //记录候选外循环的循环头。
        BasicBlock *Outer = nullptr;
        //记录候选内循环的循环头。
        BasicBlock *Inner = nullptr;

        //判断两个循环头对是否完全相同。
        bool operator==(const HeaderPair &Other) const {
            return Outer == Other.Outer && Inner == Other.Inner;
        }
    };

    //HeaderPairHash为循环头对生成无序容器所需的组合哈希。
    struct HeaderPairHash {
        //组合外、内循环头地址并返回稳定的哈希值。
        std::size_t operator()(const HeaderPair &Pair) const;
    };

    //Candidate保存不改写CFG的归纳变量角色交换所需的全部证明与使用快照。
    struct Candidate {
        //记录当前候选的外循环。
        Loop *Outer = nullptr;
        //记录当前候选的内循环。
        Loop *Inner = nullptr;
        //记录外循环的规范归纳变量。
        PhiInst *OuterIV = nullptr;
        //记录内循环的规范归纳变量。
        PhiInst *InnerIV = nullptr;
        //记录内循环的唯一预头块。
        BasicBlock *InnerPreHeader = nullptr;
        //缓存内循环及其后代循环的全部基本块。
        std::unordered_set<BasicBlock *> InnerBlocks;
        //记录必须继续使用外归纳变量的外循环控制切片。
        std::unordered_set<Instruction *> OuterControl;
        //记录必须继续使用内归纳变量的内循环控制切片。
        std::unordered_set<Instruction *> InnerControl;
        //记录需要克隆并下沉到内循环头的外轴纯计算前缀。
        std::vector<Instruction *> SinkablePrefix;
        //快照需要由内归纳变量替换的外归纳变量负载使用。
        std::vector<Use *> OuterUses;
        //快照需要由外归纳变量替换的内归纳变量负载使用。
        std::vector<Use *> InnerUses;
        //记录交换两轴后估算得到的访存局部性收益。
        long long LocalityBenefit = 0;
    };

    //RotatedNestCandidate保存旋转后非完美嵌套进行物理CFG交换所需的完整状态。
    struct RotatedNestCandidate {
        //记录物理交换候选的外循环。
        Loop *Outer = nullptr;
        //记录物理交换候选的内循环。
        Loop *Inner = nullptr;
        //记录进入外循环并承担零次迭代旁路的守卫块。
        BasicBlock *Guard = nullptr;
        //记录交换前的外循环头。
        BasicBlock *OuterHeader = nullptr;
        //记录位于两层循环头之间的外循环前缀块。
        BasicBlock *Prefix = nullptr;
        //记录交换前的内循环头。
        BasicBlock *InnerHeader = nullptr;
        //记录内循环的唯一循环体块。
        BasicBlock *InnerBody = nullptr;
        //记录外循环的唯一回边块。
        BasicBlock *OuterLatch = nullptr;
        //记录外循环的唯一退出块。
        BasicBlock *Exit = nullptr;
        //记录外循环的规范归纳变量。
        PhiInst *OuterIV = nullptr;
        //记录内循环的规范归纳变量。
        PhiInst *InnerIV = nullptr;
        //记录外循环首次迭代需要在新CFG中复现的退出比较。
        BinaryInst *OuterFirstCmp = nullptr;
        //记录外归纳变量的初始值。
        Value *OuterStartValue = nullptr;
        //记录外归纳变量起始入边在PHI中的索引。
        unsigned OuterStartIndex = 0;
        //记录内归纳变量起始入边在PHI中的索引。
        unsigned InnerStartIndex = 0;
        //记录内归纳变量回边入值在PHI中的索引。
        unsigned InnerBackedgeIndex = 0;
        //记录外循环条件为真时是否继续迭代。
        bool OuterContinueOnTrue = true;
        //记录内循环条件为真时是否继续迭代。
        bool InnerContinueOnTrue = true;
        //记录物理交换两轴后估算得到的访存局部性收益。
        long long LocalityBenefit = 0;
    };

    //3.1 证明两轴起点、步长、迭代次数与零次语义一致，并收集角色交换的 SSA 切片。
    static bool sameIterationDomain(Loop *Outer, Loop *Inner,
                                    SCEVResult &SE,
                                    PhiInst *&OuterIV,
                                    PhiInst *&InnerIV);
    //检查循环头是否只含指定的规范归纳PHI。
    static bool hasOnlyCanonicalHeaderPhi(Loop *L, PhiInst *IV);
    //检查内循环的后代循环是否存在无法由SCEV安全描述的迭代域。
    static bool hasUnsupportedDescendantDomain(Loop *Inner,
                                               SCEVResult &SE);
    //判断指令是否为可无条件克隆和下沉的无副作用纯计算。
    static bool isPureSinkInstruction(Instruction *I);
    //收集依赖外轴且需要随角色交换下沉的纯计算前缀。
    static bool collectSinkablePrefix(Candidate &C);
    //收集排除控制切片后真正需要交换的两组归纳变量使用。
    static bool collectRoleUses(Candidate &C);
    //递归收集归纳变量更新或退出条件所依赖的循环内控制切片。
    static bool collectControlSlice(
        Value *V, PhiInst *IV, Loop *L,
        std::unordered_set<Instruction *> &Control,
        std::unordered_set<Value *> &Active, unsigned Depth = 0);
    //根据两轴字节步长变化估算角色交换的访存局部性收益。
    static long long computeLocalityBenefit(const Candidate &C,
                                            LoopDependenceInfo &LD);
    //3.2 要求 LoopSimplify/LCSSA 形态、必经内循环与无外层内存负载，再提交 IV 角色交换。
    static bool validateNestShape(const Candidate &C, const LoopInfo &LI,
                                  const DominatorTree &DT,
                                  const PostDominatorTree &PDT);
    //克隆必要前缀并以使用快照原子地提交归纳变量角色交换。
    static bool applyCandidate(Function &F, Candidate &C);
    //2.1 为物理交换建立完整候选：固定六块 CFG，核验归纳 PHI、可移动控制与依赖方向。
    static bool buildRotatedNestCandidate(
        Loop *Outer, Loop *Inner, LoopInfo &LI, SCEVResult &SE,
        DominatorTree &DT, PostDominatorTree &PDT,
        LoopDependenceInfo &LD, RotatedNestCandidate &C);
    //创建辅助块并提交旋转后循环嵌套的物理CFG交换。
    static bool applyRotatedNestCandidate(
        Function &F, RotatedNestCandidate &C);
    //比较两个SCEV表达式是否具有相同且有效的规范键。
    static bool expressionKeysEqual(const SCEVExpr *A,
                                    const SCEVExpr *B);
    //把访存字节步长的量级换算为局部性代价。
    static unsigned stridePenalty(std::int64_t Stride);
    //判断指令是否定义在给定基本块集合中。
    static bool isInstructionInBlocks(
        Instruction *I,
        const std::unordered_set<BasicBlock *> &Blocks);
    //清理尚未挂接到函数中的游离克隆指令。
    static void destroyDetachedInstructions(
        std::vector<Instruction *> &Instructions);

    //1.1 状态跨 pass 固定点轮次保留，同时约束同一循环对和整个函数的变换次数。
    std::unordered_set<HeaderPair, HeaderPairHash> Transformed;
    //记录每个函数在固定点迭代期间已经完成的循环交换次数。
    std::unordered_map<Function *, unsigned> TransformCount;
};
