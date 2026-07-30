#pragma once

#include "ir/opt/NewPassManager.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class BasicBlock;
class Instruction;
class Loop;
class LoopDependenceInfo;
class SCEVResult;
class StoreInst;
class Value;
struct ClonedBlockRegion;

//按连续 store 序列的反向切片保守分布循环：
//1. 选择有限、LCSSA、单出口且无 live-out/内存相关控制的规范循环；
//2. 为每个 store 建立循环内反向切片，枚举保持原 store 次序的连续切分点；
//3. 验证共享切片可复制、危险指令只属于一个分区，并由统一依赖分析证明
//两分区串行执行合法；
//4. 用多维访问步长差异评估局部性收益，选择距离最大的计划；
//5. 克隆完整循环，原循环保留第一分区、克隆循环保留第二分区，再串行接 CFG。
class LoopDistributionPass : public PassDescriptor<LoopDistributionPass> {
public:
    //在函数中寻找一个合法且有局部性收益的循环分布计划并提交。
    PassResult run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    //限制单个函数在固定点迭代期间最多执行的循环分布次数。
    static constexpr unsigned MaxDistributionsPerFunction = 2;
    //限制可被克隆的候选循环基本块数量。
    static constexpr std::size_t MaxDistributionBlocks = 24;
    //限制候选循环允许包含的总指令数量。
    static constexpr std::size_t MaxDistributionInstructions = 160;
    //限制可作为独立语句根参与切分的store数量。
    static constexpr std::size_t MaxStatementRoots = 24;

    //InstructionSet表示循环分区对应的闭合指令切片。
    using InstructionSet = std::unordered_set<Instruction *>;
    //BlockSet用于常数时间判断指令定义是否位于候选循环内。
    using BlockSet = std::unordered_set<BasicBlock *>;

    //DistributionShape保存循环分布候选的规范CFG形态和语句根。
    struct DistributionShape {
        //记录待分布的循环。
        Loop *L = nullptr;
        //记录循环的唯一预头块。
        BasicBlock *PreHeader = nullptr;
        //记录循环的唯一循环头。
        BasicBlock *Header = nullptr;
        //记录循环的唯一回边块。
        BasicBlock *Latch = nullptr;
        //记录产生唯一退出边的循环内基本块。
        BasicBlock *Exiting = nullptr;
        //记录循环外的唯一退出目标块。
        BasicBlock *Exit = nullptr;
        //按函数顺序记录循环及其子循环的全部基本块。
        std::vector<BasicBlock *> Blocks;
        //按程序顺序记录作为分区语句根的全部store。
        std::vector<StoreInst *> Stores;
    };

    //DistributionPlan保存连续切点两侧的语句根、闭合切片和收益。
    struct DistributionPlan {
        //保存该计划已经验证的循环形态。
        DistributionShape Shape;
        //记录串行执行的第一分区store根。
        std::vector<StoreInst *> FirstStores;
        //记录串行执行的第二分区store根。
        std::vector<StoreInst *> SecondStores;
        //保存第一分区执行所需的闭合指令切片。
        InstructionSet FirstSlice;
        //保存第二分区执行所需的闭合指令切片。
        InstructionSet SecondSlice;
        //记录两分区多维访存模式的局部性距离。
        unsigned LocalityDistance = 0;
    };

    //DimensionLocality累计单个循环维度上的加权访存步长信息。
    struct DimensionLocality {
        //累计各已知访存的步长桶乘访问权重。
        std::uint64_t WeightedBucketSum = 0;
        //累计具有已知步长的load和store访问权重。
        std::uint64_t KnownWeight = 0;
    };

    //LocalityProfile按外到内循环维度保存局部性统计。
    using LocalityProfile = std::vector<DimensionLocality>;

    //服务步骤 1：控制条件若反向依赖循环内 load/call，则两个分区可能走不同
    //路径；深度预算耗尽也按“依赖内存”处理，不能当作纯净证明。
    //判断指令是否包含克隆路径尚未建模的可观察或未知语义。
    static bool hasUnsupportedInstruction(Instruction *I);
    //递归判断值是否依赖候选循环内部的load或call。
    static bool valueDependsOnLoad(Value *V,
                                   std::unordered_set<Value *> &Seen,
                                   const BlockSet &LoopBlocks,
                                   unsigned Depth = 0);
    //检查任一循环内条件分支是否依赖循环内存状态。
    static bool hasMemoryDependentControl(
        const std::vector<BasicBlock *> &Blocks);
    //递归检查候选循环及全部子循环是否均可证明有限。
    static bool hasFiniteLoopTree(Loop *L, SCEVResult &SE);
    //检查候选循环内是否存在需要在分布后重建的活跃输出。
    static bool hasLoopLiveOut(const DistributionShape &Shape);
    //识别满足LCSSA、唯一退出和规模限制的循环分布形态。
    static std::optional<DistributionShape> getDistributionShape(
        Function &F, Loop *L);
    //服务步骤 2：反向切片只沿循环内 SSA 定义传播；共享节点随后单独验证能否
    //在两个克隆循环中各执行一次。
    //从一个store根沿循环内定义建立闭合反向指令切片。
    static InstructionSet collectBackwardSlice(
        StoreInst *Root, const BlockSet &LoopBlocks);
    //计算待丢弃切片中不与保留切片共享的专属指令。
    static InstructionSet collectExclusiveSlice(
        const InstructionSet &Discarded, const InstructionSet &Retained);
    //验证两分区共享切片可安全复制且所有单次副作用具有唯一所有者。
    static bool canDuplicateSharedSlice(
        const DistributionShape &Shape, const InstructionSet &First,
        const InstructionSet &Second);
    //将原循环指令集合映射为对应的克隆循环指令集合。
    static bool mapInstructionSet(
        const InstructionSet &Original, const ClonedBlockRegion &Region,
        InstructionSet &Mapped);
    //保活仍有外部使用的节点并删除其余封闭专属切片。
    static void deleteExclusiveSlice(InstructionSet &Candidates);
    //把一个语句切片合并到目标分区切片中。
    static void mergeSlice(InstructionSet &Into,
                           const InstructionSet &From);
    //把字节步长绝对值归并到局部性比较使用的粗粒度桶。
    static unsigned strideBucket(std::int64_t Stride);
    //按循环维度建立一个分区专属访存的加权局部性画像。
    static LocalityProfile buildLocalityProfile(
        const InstructionSet &Slice, const InstructionSet &OtherSlice,
        const std::vector<Loop *> &Dimensions, LoopDependenceInfo &LD);
    //计算两个分区在多维访存步长上的局部性距离。
    static unsigned localityDistance(const InstructionSet &First,
                                     const InstructionSet &Second, Loop *L,
                                     LoopDependenceInfo &LD);
    //枚举连续store切点并选择合法且局部性距离最大的分布计划。
    static std::optional<DistributionPlan> findDistributionPlan(
        Function &F, Loop *L, LoopDependenceInfo &LD);
    //断开并销毁尚未提交到CFG中的整组循环克隆块。
    static void destroyClonedBlocks(std::vector<BasicBlock *> &Blocks);
    //事务式克隆循环、串接CFG并删除两侧各自不属于本分区的切片。
    static bool executeDistribution(Function &F,
                                    const DistributionPlan &Plan);

    //外层流水线会重复调用本 pass；按函数和 pass 对象累计限制分布次数，避免
    //每轮重新获得克隆预算而无界增长代码。
    std::unordered_map<Function *, unsigned> TransformCounts;
};
