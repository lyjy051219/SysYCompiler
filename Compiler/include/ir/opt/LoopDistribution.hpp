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

// 按连续 store 序列的反向切片保守分布循环：
// 1. 选择有限、LCSSA、单出口且无 live-out/内存相关控制的规范循环；
// 2. 为每个 store 建立循环内反向切片，枚举保持原 store 次序的连续切分点；
// 3. 验证共享切片可复制、危险指令只属于一个分区，并由统一依赖分析证明
//    两分区串行执行合法；
// 4. 用多维访问步长差异评估局部性收益，选择距离最大的计划；
// 5. 克隆完整循环，原循环保留第一分区、克隆循环保留第二分区，再串行接 CFG。
class LoopDistributionPass : public PassDescriptor<LoopDistributionPass> {
public:
    PassResult run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    static constexpr unsigned MaxDistributionsPerFunction = 2;
    static constexpr std::size_t MaxDistributionBlocks = 24;
    static constexpr std::size_t MaxDistributionInstructions = 160;
    static constexpr std::size_t MaxStatementRoots = 24;

    using InstructionSet = std::unordered_set<Instruction *>;
    using BlockSet = std::unordered_set<BasicBlock *>;

    struct DistributionShape {
        Loop *L = nullptr;
        BasicBlock *PreHeader = nullptr;
        BasicBlock *Header = nullptr;
        BasicBlock *Latch = nullptr;
        BasicBlock *Exiting = nullptr;
        BasicBlock *Exit = nullptr;
        std::vector<BasicBlock *> Blocks;
        std::vector<StoreInst *> Stores;
    };

    struct DistributionPlan {
        DistributionShape Shape;
        std::vector<StoreInst *> FirstStores;
        std::vector<StoreInst *> SecondStores;
        InstructionSet FirstSlice;
        InstructionSet SecondSlice;
        unsigned LocalityDistance = 0;
    };

    struct DimensionLocality {
        std::uint64_t WeightedBucketSum = 0;
        std::uint64_t KnownWeight = 0;
    };

    using LocalityProfile = std::vector<DimensionLocality>;

    // 服务步骤 1：控制条件若反向依赖循环内 load/call，则两个分区可能走不同
    // 路径；深度预算耗尽也按“依赖内存”处理，不能当作纯净证明。
    static bool hasUnsupportedInstruction(Instruction *I);
    static bool valueDependsOnLoad(Value *V,
                                   std::unordered_set<Value *> &Seen,
                                   const BlockSet &LoopBlocks,
                                   unsigned Depth = 0);
    static bool hasMemoryDependentControl(
        const std::vector<BasicBlock *> &Blocks);
    static bool hasFiniteLoopTree(Loop *L, SCEVResult &SE);
    static bool hasLoopLiveOut(const DistributionShape &Shape);
    static std::optional<DistributionShape> getDistributionShape(
        Function &F, Loop *L);
    // 服务步骤 2：反向切片只沿循环内 SSA 定义传播；共享节点随后单独验证能否
    // 在两个克隆循环中各执行一次。
    static InstructionSet collectBackwardSlice(
        StoreInst *Root, const BlockSet &LoopBlocks);
    static InstructionSet collectExclusiveSlice(
        const InstructionSet &Discarded, const InstructionSet &Retained);
    static bool canDuplicateSharedSlice(
        const DistributionShape &Shape, const InstructionSet &First,
        const InstructionSet &Second);
    static bool mapInstructionSet(
        const InstructionSet &Original, const ClonedBlockRegion &Region,
        InstructionSet &Mapped);
    static void deleteExclusiveSlice(InstructionSet &Candidates);
    static void mergeSlice(InstructionSet &Into,
                           const InstructionSet &From);
    static unsigned strideBucket(std::int64_t Stride);
    static LocalityProfile buildLocalityProfile(
        const InstructionSet &Slice, const InstructionSet &OtherSlice,
        const std::vector<Loop *> &Dimensions, LoopDependenceInfo &LD);
    static unsigned localityDistance(const InstructionSet &First,
                                     const InstructionSet &Second, Loop *L,
                                     LoopDependenceInfo &LD);
    static std::optional<DistributionPlan> findDistributionPlan(
        Function &F, Loop *L, LoopDependenceInfo &LD);
    static void destroyClonedBlocks(std::vector<BasicBlock *> &Blocks);
    static bool executeDistribution(Function &F,
                                    const DistributionPlan &Plan);

    // 外层流水线会重复调用本 pass；按函数和 pass 对象累计限制分布次数，避免
    // 每轮重新获得克隆预算而无界增长代码。
    std::unordered_map<Function *, unsigned> TransformCounts;
};
