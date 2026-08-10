#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

//BasicAAResult提供内存位置、底层对象和对象边界证明
class BasicAAResult;
//DominatorTree提供候选load与循环预头之间的支配证明
class DominatorTree;
//LoopInfo提供自然循环归属和循环不变量查询
class LoopInfo;
//Loop表示一个自然循环
class Loop;
//MemorySSAResult提供循环和跨块路径上的稀疏内存访问
class MemorySSAResult;
//SCEVResult提供程序点归纳变量活动范围
class SCEVResult;
//RangeInfo提供控制流路径上的整数范围事实
class RangeInfo;
//MemoryLocation描述一次带真实字节宽度的内存访问
struct MemoryLocation;
//ClonedBlockRegion保存循环区域克隆映射
struct ClonedBlockRegion;

//LoopLoadEliminationPass消除经过循环、内存版本和对象边界证明的冗余load
class LoopLoadEliminationPass
    : public PassDescriptor<LoopLoadEliminationPass> {
public:
    //在函数中提交至多一次load复用或守卫版本化，供有界固定点安全重建分析
    PreservationStatus run(
        Function &F, FunctionAnalysisPassManager &FAM);

private:
    //MaxReachableBlocks限制一次跨循环路径证明访问的基本块数量
    static constexpr std::size_t MaxReachableBlocks = 96;
    //MaxMemoryAccesses限制一次循环或路径证明访问的MemorySSA节点数量
    static constexpr std::size_t MaxMemoryAccesses = 2048;
    //MaxSeedComparisons限制单条load尝试的支配seed数量
    static constexpr std::size_t MaxSeedComparisons = 128;
    //MaxVersionBlocks限制一次快慢版本化复制的最内层循环块数
    static constexpr std::size_t MaxVersionBlocks = 8;
    //MaxVersionInstructions限制一次快慢版本化复制的指令数
    static constexpr std::size_t MaxVersionInstructions = 96;
    //MaxVersionsPerFunction限制一个函数中由本pass生成的慢版本数量
    static constexpr std::size_t MaxVersionsPerFunction = 4;

    //VersionCandidate保存完成全部分析证明后的只读循环版本化计划
    struct VersionCandidate {
        Loop *L = nullptr;
        BasicBlock *PreHeader = nullptr;
        BasicBlock *Header = nullptr;
        BasicBlock *Latch = nullptr;
        LoadInst *Seed = nullptr;
        LoadInst *Target = nullptr;
        Value *Bound = nullptr;
        std::int64_t SafeBound = 0;
        std::vector<BasicBlock *> Blocks;
        std::vector<BasicBlock *> Exits;
    };

    //VersionedExitPhiPlan保存旧出口PHI在快慢两侧的完整输入快照
    struct VersionedExitPhiPlan {
        PhiInst *OldPhi = nullptr;
        std::vector<PhiInst::IncomingEdge> FastIncoming;
        std::vector<PhiInst::IncomingEdge> SlowIncoming;
    };

    //VersionedExitPlan保存一个共享出口的双代理改写计划
    struct VersionedExitPlan {
        BasicBlock *Exit = nullptr;
        std::vector<BasicBlock *> FastPreds;
        std::vector<BasicBlock *> SlowPreds;
        std::vector<VersionedExitPhiPlan> PhiPlans;
    };
    //判断两个load是否产生完全相同形状的标量值
    static bool isTypeCompatible(LoadInst *First, LoadInst *Second);
    //判断值能否在预头终结指令前直接使用
    static bool isAvailableAtPreheader(
        Value *V, BasicBlock *PreHeader,
        const DominatorTree &DT);
    //判断一条潜在写指令是否可证明不会修改候选位置
    static bool mayModifyLocation(
        const MemoryLocation &Location, Instruction *LocationContext,
        Instruction *I, BasicAAResult &AA, SCEVResult &SE,
        RangeInfo &RI, const DominatorTree &DT, const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM);
    //反向收集仍可能到达目标块的CFG区域
    static bool collectBlocksReachingTarget(
        BasicBlock *Target,
        std::unordered_set<BasicBlock *> &Blocks,
        BasicBlock *StopAt = nullptr);
    //证明支配seed到目标load的每条可达路径都没有相关写入
    static bool hasNoClobberOnPaths(
        LoadInst *Seed, LoadInst *Target, MemorySSAResult &MSSA,
        BasicAAResult &AA, SCEVResult &SE, RangeInfo &RI,
        const DominatorTree &DT, const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM);
    //寻找能够在循环路径上保持同一内存版本的支配load
    static LoadInst *findReusableSeed(
        LoadInst *Target, const std::vector<LoadInst *> &Loads,
        MemorySSAResult &MSSA, BasicAAResult &AA, SCEVResult &SE,
        RangeInfo &RI, const DominatorTree &DT, const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM);
    //提交一条不新增访存的跨循环load复用
    static bool eliminateOneDominatedLoad(
        Function &F, MemorySSAResult &MSSA, BasicAAResult &AA,
        SCEVResult &SE, RangeInfo &RI, const DominatorTree &DT,
        const LoopInfo &LI, FunctionAnalysisPassManager &FAM);
    //证明循环及其子循环不存在会修改候选位置的内存访问
    static bool hasNoLoopClobber(
        const MemoryLocation &Location, Instruction *LocationContext,
        Loop &L, MemorySSAResult &MSSA, BasicAAResult &AA,
        SCEVResult &SE, RangeInfo &RI, const DominatorTree &DT,
        const LoopInfo &LI, FunctionAnalysisPassManager &FAM);
    //判断循环是否已经属于本pass生成的快版本或慢版本
    static bool isGeneratedVersionLoop(Loop &L);
    //用带类型GEP、对象大小和访问宽度计算该位置允许的最大共同边界
    static bool accumulateLocationSafeBound(
        const MemoryLocation &Location, Instruction *LocationContext,
        BasicAAResult &AA, const DominatorTree &DT,
        const LoopInfo &LI, SCEVResult &SE,
        Value *&SharedBound, std::int64_t &SafeBound);
    //在给定候选位置下判断指令是否为静态无关写或可由共同边界守卫的写
    static bool acceptVersionedMemoryInstruction(
        const MemoryLocation &Location, Instruction *I,
        BasicAAResult &AA, SCEVResult &SE,
        const DominatorTree &DT, const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM,
        Value *&SharedBound, std::int64_t &SafeBound,
        bool &NeedsGuard);
    //寻找一个小型最内层、无调用且可由单一共同边界保护的版本化候选
    static std::optional<VersionCandidate> findVersionCandidate(
        Function &F, BasicAAResult &AA, SCEVResult &SE,
        const DominatorTree &DT, const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM);
    //销毁尚未接入原CFG的克隆块和入口块
    static void destroyDetachedBlocks(std::vector<BasicBlock *> Blocks);
    //在出口代理块中建立显式LCSSA汇总PHI
    static PhiInst *buildExitProxyPhi(
        const std::shared_ptr<IRType> &Ty,
        const std::vector<PhiInst::IncomingEdge> &Incoming,
        BasicBlock *Proxy, const std::string &Name);
    //在克隆后、提交前离线验证并规划一个共享出口
    static std::optional<VersionedExitPlan> planVersionedExit(
        BasicBlock *Exit, const VersionCandidate &Candidate,
        ClonedBlockRegion &Region);
    //提交一个已完整预检的共享出口双代理计划
    static void applyVersionedExitPlan(
        Function &F, VersionedExitPlan &Plan);
    //事务式建立快慢最内层循环版本和显式范围守卫
    static bool versionOneGuardedLoop(
        Function &F, const VersionCandidate &Candidate);
    //构造只保留CFG结构分析的失效声明
    static PreservationStatus changedPreservation();
};
