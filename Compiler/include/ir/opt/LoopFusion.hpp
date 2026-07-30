#pragma once

#include "ir/opt/NewPassManager.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

//BasicBlock表示控制流图中的基本块
class BasicBlock;
//BrInst表示条件或无条件分支指令
class BrInst;
//DominatorTree提供支配关系查询
class DominatorTree;
//Instruction表示一条IR指令
class Instruction;
//Loop表示循环树中的一个自然循环
class Loop;
//LoopAccessInfo提供循环间访存依赖证明
class LoopAccessInfo;
//LoopInfo提供函数内循环森林与块归属查询
class LoopInfo;
//PhiInst表示SSA汇合节点
class PhiInst;
//PostDominatorTree提供后支配关系查询
class PostDominatorTree;
//SCEVResult提供归纳变量与迭代次数分析结果
class SCEVResult;
//SCEVExpr表示可比较结构键的标量演化表达式
struct SCEVExpr;
//Value表示IR中的值
class Value;

//LoopFusionPass融合串行相邻、迭代域相同且访存次序可证明安全的同级循环
class LoopFusionPass : public PassDescriptor<LoopFusionPass> {
public:
    //在函数内寻找并提交至多一个合法且有收益的循环融合
    PassResult run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    //单个函数允许累计提交的最大融合次数
    static constexpr unsigned MaxFusionsPerFunction = 4;
    //一对候选循环允许包含的最大基本块数
    static constexpr std::size_t MaxFusionBlocks = 128;
    //一对候选循环允许包含的最大指令数
    static constexpr std::size_t MaxFusionInstructions = 512;
    //存在同迭代真依赖时允许融合的最大指令数
    static constexpr std::size_t MaxFlowFusionInstructions = 128;
    //退出桥中允许外提的最大指令数
    static constexpr std::size_t MaxBridgeInstructions = 24;
    //允许从第二循环头移动的最大非归纳PHI数
    static constexpr std::size_t MaxMovedHeaderPhis = 32;

    //BlockSet用于常数时间判断基本块是否属于循环摘要
    using BlockSet = std::unordered_set<BasicBlock *>;
    //InstructionSet用于判断桥内定义是否已按顺序纳入外提计划
    using InstructionSet = std::unordered_set<Instruction *>;

    //LoopShape记录验证循环融合所需的规范控制流、迭代域和规模摘要
    struct LoopShape {
        //L指向摘要对应的循环
        Loop *L = nullptr;
        //PreHeader指向循环唯一预头
        BasicBlock *PreHeader = nullptr;
        //Header指向循环唯一头部
        BasicBlock *Header = nullptr;
        //BodyEntry指向头部分支进入循环体的唯一后继
        BasicBlock *BodyEntry = nullptr;
        //Latch指向循环唯一回边块
        BasicBlock *Latch = nullptr;
        //Exit指向循环唯一退出目标
        BasicBlock *Exit = nullptr;
        //HeaderBranch指向头部的条件退出分支
        BrInst *HeaderBranch = nullptr;
        //LatchBranch指向latch回到头部的无条件分支
        BrInst *LatchBranch = nullptr;
        //Induction指向循环的规范归纳PHI
        PhiInst *Induction = nullptr;
        //InductionUpdate指向归纳变量的回边更新指令
        Instruction *InductionUpdate = nullptr;
        //ExitCompare指向控制头部分支的退出比较
        Instruction *ExitCompare = nullptr;
        //Start记录归纳变量起点的SCEV表达式
        const SCEVExpr *Start = nullptr;
        //Step记录归纳变量步长的SCEV表达式
        const SCEVExpr *Step = nullptr;
        //TripCount记录循环迭代次数的SCEV表达式
        const SCEVExpr *TripCount = nullptr;
        //ConstantStep记录经SCEV证明的常量步长
        std::optional<std::int64_t> ConstantStep;
        //MayBeZeroTrip标记循环是否可能零次执行
        bool MayBeZeroTrip = true;
        //Blocks按函数顺序保存循环及其子循环的全部基本块
        std::vector<BasicBlock *> Blocks;
        //BlockLookup提供Blocks的快速成员查询
        BlockSet BlockLookup;
        //InstructionCount记录Blocks中的总指令数
        std::size_t InstructionCount = 0;
    };

    //EdgeRewrite记录一次提交阶段的分支后继替换
    struct EdgeRewrite {
        //Branch指向待修改的分支指令
        BrInst *Branch = nullptr;
        //SuccessorIndex记录待替换后继在分支中的槽位
        unsigned SuccessorIndex = 0;
        //OldSuccessor记录提交前必须仍存在的旧后继
        BasicBlock *OldSuccessor = nullptr;
        //NewSuccessor记录融合后接入的新后继
        BasicBlock *NewSuccessor = nullptr;
    };

    //PhiRewrite记录一次提交阶段的PHI前驱替换
    struct PhiRewrite {
        //Phi指向待修改的PHI节点
        PhiInst *Phi = nullptr;
        //IncomingIndex记录待替换incoming的槽位
        unsigned IncomingIndex = 0;
        //OldPredecessor记录提交前必须仍存在的旧前驱
        BasicBlock *OldPredecessor = nullptr;
        //NewPredecessor记录融合后接入的新前驱
        BasicBlock *NewPredecessor = nullptr;
    };

    //FusionCandidate保存一对循环通过全部验证后的原子改写计划
    struct FusionCandidate {
        //First保存第一循环的结构与迭代域摘要
        LoopShape First;
        //Second保存第二循环的结构与迭代域摘要
        LoopShape Second;
        //Bridge指向第一循环退出与第二循环预头共用的桥接块
        BasicBlock *Bridge = nullptr;
        //HoistedBridgeInstructions按原顺序保存需外提至第一预头的桥指令
        std::vector<Instruction *> HoistedBridgeInstructions;
        //MovedSecondPhis保存需移入第一循环头的第二循环非归纳PHI
        std::vector<PhiInst *> MovedSecondPhis;
        //EdgeRewrites保存提交时执行的全部CFG边替换
        std::vector<EdgeRewrite> EdgeRewrites;
        //PhiRewrites保存提交时执行的全部PHI前驱替换
        std::vector<PhiRewrite> PhiRewrites;
        //SameIterationFlowPairs记录LoopAccess证明的同迭代真依赖对数
        unsigned SameIterationFlowPairs = 0;
    };

    //判断两个SCEV表达式是否具有相同且非空的结构键
    static bool expressionKeysEqual(const SCEVExpr *A,
                                    const SCEVExpr *B);
    //递归检查循环及其全部子循环是否均已证明有限
    static bool hasFiniteLoopTree(Loop *L, SCEVResult &SE);
    //检查循环内定义是否完全没有循环外使用
    static bool hasNoLiveOut(const LoopShape &Shape);
    //判断指令移入融合后的交错迭代顺序是否安全
    static bool isSafeReorderedInstruction(Instruction *I);
    //检查循环是否包含融合不支持的指令
    static bool hasUnsupportedLoopInstruction(const LoopShape &Shape);
    //检查循环是否带有阻止结构Pass振荡的分布来源标记
    static bool hasDistributionProvenance(const LoopShape &Shape);
    //判断桥接块中的指令是否可以安全外提
    static bool isSafeBridgeInstruction(Instruction *I);
    //判断值在第一循环预头是否可用
    static bool valueDominatesPreheader(
        Value *V, BasicBlock *PreHeader, BasicBlock *Bridge,
        const InstructionSet &EarlierBridgeInstructions,
        const DominatorTree &DT);
    //验证桥接块并按程序顺序收集待外提指令
    static bool collectBridgeInstructions(
        FusionCandidate &Candidate, const DominatorTree &DT);
    //验证两循环头部PHI并记录所需incoming改写与移动
    static bool collectHeaderPhiRewrites(FusionCandidate &Candidate,
                                         const DominatorTree &DT);
    //检查第二循环头是否只包含可删除的控制逻辑
    static bool secondHeaderContainsOnlyControl(
        const FusionCandidate &Candidate);
    //检查两循环是否具有完全相同的规范迭代域
    static bool sameIterationDomain(const LoopShape &First,
                                    const LoopShape &Second);
    //判断值是否是可由memset表示的重复字节常量
    static bool isRepeatedByteConstant(Value *V);
    //判断指针是否按规范归纳变量进行单位步长连续访问
    static bool isUnitStrideAccess(Value *Pointer, PhiInst *Induction);
    //判断循环是否是应留给LoopIdiomRecognize的单store惯用模式
    static bool isSingleStoreIdiomCandidate(const LoopShape &Shape);
    //根据同迭代真依赖、代码规模和惯用模式判断融合收益
    static bool isProfitable(const FusionCandidate &Candidate);
    //检查两循环是否同级、满足LCSSA并由唯一桥严格相邻
    static bool isLCSSAAndStructurallyAdjacent(
        const FusionCandidate &Candidate, const LoopInfo &LI);
    //查找分支中唯一指向指定后继的槽位
    static std::optional<unsigned> findUniqueSuccessorIndex(
        BrInst *Branch, BasicBlock *Successor);
    //构建并验证单个循环的规范结构与迭代域摘要
    static std::optional<LoopShape> buildLoopShape(
        Function &F, Loop *L, SCEVResult &SE,
        const PostDominatorTree &PDT);
    //为相邻循环执行全部合法性、依赖和收益验证并生成改写计划
    static std::optional<FusionCandidate> buildCandidate(
        Function &F, Loop *First, LoopInfo &LI, SCEVResult &SE,
        const DominatorTree &DT, const PostDominatorTree &PDT,
        LoopAccessInfo &LA);
    //原子提交已验证候选的指令移动、PHI修复与CFG重接
    static void commitCandidate(FusionCandidate &Candidate);

    //TransformCounts记录每个函数已提交的融合次数
    std::unordered_map<Function *, unsigned> TransformCounts;
};
