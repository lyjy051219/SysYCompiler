// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

class DominatorTree;
class Loop;
class LoopAccessInfo;
class LoopInfo;
class RangeInfo;
class SCEVResult;

//LoopPhaseRelayoutAccess保存跨多级typed GEP链恢复出的二维逻辑访问
struct LoopPhaseRelayoutAccess {
    //Address指向产生最终标量地址的GEP，Root记录完整分配对象
    GetElementPtrInst *Address = nullptr;
    Value *Root = nullptr;
    //Indices保存从Root出发的规范逻辑索引，ByteStrides保存对应字节步长
    std::vector<Value *> Indices;
    std::vector<std::int64_t> ByteStrides;
    //AxisAtIndex以-1表示常量，以0/1表示生产者外层或内层轴
    std::vector<int> AxisAtIndex;
};

//LoopPhaseRelayoutRewrite保存一次受生产阶段支配的二维读取及其转置索引
struct LoopPhaseRelayoutRewrite {
    //Load指向布局改写后应从shadow读取的原始load
    LoadInst *Load = nullptr;
    //TransposedIndices保存从转置对象出发的完整typed GEP索引
    std::vector<Value *> TransposedIndices;
    //EstimatedExecutions保存仅由循环结构估计的动态执行次数
    std::int64_t EstimatedExecutions = 0;
};

//LoopPhaseRelayoutCandidate保存二维阶段末镜像布局所需的完整改写计划
struct LoopPhaseRelayoutCandidate {
    //Target记录生产者与消费者共同所属的函数
    Function *Target = nullptr;
    //ProducerOuter和ProducerInner记录完整覆盖对象的二维矩形循环
    Loop *ProducerOuter = nullptr;
    Loop *ProducerInner = nullptr;
    //ProducerExit记录shadow完整初始化后唯一到达的循环出口
    BasicBlock *ProducerExit = nullptr;
    //Source记录原二维全局对象，FinalStore记录每元素最后一次必执行写入
    Variable *Source = nullptr;
    StoreInst *FinalStore = nullptr;
    //ShadowObjectType记录交换两维之后的完整数组类型
    std::shared_ptr<IRType> ShadowObjectType;
    //ProducerTransposedIndices保存镜像写入使用的typed GEP索引
    std::vector<Value *> ProducerTransposedIndices;
    //Rewrites保存所有确定受益且能安全读取shadow的后续消费者
    std::vector<LoopPhaseRelayoutRewrite> Rewrites;
    //Rows、Columns和ElementBytes记录源对象的精确二维布局
    std::int64_t Rows = 0;
    std::int64_t Columns = 0;
    std::int64_t ElementBytes = 0;
    //ShadowBytes记录新增对象大小，EstimatedNetBenefit记录缩放后的缓存收益
    std::size_t ShadowBytes = 0;
    std::int64_t EstimatedNetBenefit = 0;
};

//LoopPhaseConsistentShadowCandidate保存动态活动域内的原位更新转置计划
struct LoopPhaseConsistentShadowCandidate {
    //Target和三层循环共同限定需要版本化的完整计算区域
    Function *Target = nullptr;
    BasicBlock *PreHeader = nullptr;
    BasicBlock *Header = nullptr;
    BasicBlock *Exit = nullptr;
    //Source、Bound和FinalStore描述原对象、共同活动边界及同步写入点
    Variable *Source = nullptr;
    Value *Bound = nullptr;
    StoreInst *FinalStore = nullptr;
    //ShadowObjectType与转置索引只保存已经完成证明的提交快照
    std::shared_ptr<IRType> ShadowObjectType;
    std::vector<Value *> StoreTransposedIndices;
    std::vector<LoopPhaseRelayoutRewrite> Rewrites;
    //Blocks是保留语义的慢版本克隆区域
    std::vector<BasicBlock *> Blocks;
    std::int64_t Rows = 0;
    std::int64_t Columns = 0;
    std::size_t ShadowBytes = 0;
};

//LoopPhaseRelayoutPass把完整二维生产阶段的最终值镜像为消费者友好布局
class LoopPhaseRelayoutPass
    : public PassDescriptor<LoopPhaseRelayoutPass> {
public:
    //绑定模块变换期间查询与失效函数级分析所需的分析管理器
    explicit LoopPhaseRelayoutPass(FunctionAnalysisPassManager &FAM);
    //逐函数选择一个合法且有收益的阶段布局候选并提交改写
    PreservationStatus run(Module &M, ModuleAnalysisPassManager &MAM);

private:
    //CacheLineBytes采用当前RISC-V成本模型共用的64字节缓存行
    static constexpr std::int64_t CacheLineBytes = 64;
    //MaxShadowBytes限制单个布局镜像造成的静态内存增长
    static constexpr std::size_t MaxShadowBytes = 8 * 1024 * 1024;
    //MaxTotalShadowBytes限制本Pass在一个模块中累计创建的镜像对象
    static constexpr std::size_t MaxTotalShadowBytes = 16 * 1024 * 1024;
    //MinInnerTraversalBytes过滤不能摊薄缓存行转换成本的短扫描
    static constexpr std::int64_t MinInnerTraversalBytes = 256 * 1024;
    //MaxPathScanBlocks限制生产出口到消费者之间的路径扫描预算
    static constexpr std::size_t MaxPathScanBlocks = 4096;

    //沿typed GEP链恢复根对象、二维逻辑索引及其根类型字节步长
    static bool matchTwoDimensionalAccess(
        Value *Pointer, LoopPhaseRelayoutAccess &Access);
    //把生产者地址中的两个动态索引绑定到外层与内层归纳PHI
    static bool bindProducerAxes(
        LoopPhaseRelayoutAccess &Access, PhiInst *OuterIV,
        PhiInst *InnerIV);
    //从全局对象类型中提取精确二维标量数组形状并构造转置类型
    static bool getTwoDimensionalGlobalShape(
        Variable *Source, std::int64_t &Rows, std::int64_t &Columns,
        std::int64_t &ElementBytes, std::size_t &ObjectBytes,
        std::shared_ptr<IRType> &ShadowObjectType);
    //证明两层循环以零起点单位步长完整覆盖给定行列域
    static bool isCanonicalFullProducer(
        Loop *Outer, Loop *Inner, std::int64_t Rows,
        std::int64_t Columns, LoopAccessInfo &LA, SCEVResult &SE,
        BasicBlock *&ExitBlock);
    //证明候选是每个逻辑元素在本轮生产阶段的最后一次必执行访问
    static bool isFinalPerElementStore(
        Loop *Outer, Loop *Inner, StoreInst *Candidate,
        const std::vector<LoadInst *> &SourceLoads,
        const std::vector<StoreInst *> &SourceStores,
        DominatorTree &DT);
    //判断生产出口到指定消费者之前是否存在可能重入或形成顺序屏障的调用
    static bool hasCallOnPath(BasicBlock *Start, LoadInst *Target);
    //在真实访存点证明二维索引落在对象边界内
    static bool areIndicesProvablyInBounds(
        Value *RowIndex, Value *ColumnIndex, Instruction *Context,
        std::int64_t Rows, std::int64_t Columns, SCEVResult &SE,
        RangeInfo &RI, const DominatorTree &DT);
    //返回整数索引沿指定循环轴的常量步长，循环不变量返回零
    static std::optional<std::int64_t> getIndexStepForLoop(
        Value *Index, Loop *Axis, SCEVResult &SE);
    //仅依据包含当前指令的循环次数估计动态执行次数并做饱和保护
    static std::int64_t estimateExecutionCount(
        Instruction *I, LoopInfo &LI, SCEVResult &SE,
        RangeInfo &RI, DominatorTree &DT);
    //把有符号步长安全转换为无符号幅值
    static std::uint64_t absoluteMagnitude(std::int64_t Value);
    //从模块函数与全局符号中生成不会冲突的内部shadow名称
    static std::string makeUniqueShadowName(Module &M, unsigned Ordinal);
    //为一个二维生产循环构建完整的镜像与消费者改写计划
    static bool buildCandidate(
        Function &F, Loop *Outer, LoopAccessInfo &LA, LoopInfo &LI,
        SCEVResult &SE, RangeInfo &RI, DominatorTree &DT,
        LoopPhaseRelayoutCandidate &Candidate);
    //创建shadow并一次性提交已经完成全部证明的镜像和load改写
    static bool applyCandidate(
        Module &M, LoopPhaseRelayoutCandidate &Candidate,
        unsigned Ordinal);
    //识别零起点单位步长动态循环，并返回退出比较中的真实边界值
    static bool matchCanonicalDynamicAxis(
        Loop *Axis, SCEVResult &SE, PhiInst *&Induction,
        Value *&Bound, BasicBlock *&PreHeader,
        BasicBlock *&Latch, BasicBlock *&Exit);
    //识别动态三层原位更新并建立带完整慢路径的转置shadow计划
    static bool buildConsistentShadowCandidate(
        Function &F, Loop *Outer, LoopAccessInfo &LA,
        SCEVResult &SE, DominatorTree &DT,
        LoopPhaseConsistentShadowCandidate &Candidate);
    //建立边界守卫、活动域初始化、原位同步和未经改写的慢版本
    static bool applyConsistentShadowCandidate(
        Module &M, LoopPhaseConsistentShadowCandidate &Candidate,
        unsigned Ordinal);
 
    //FAM提供函数级循环、范围、支配与访问分析并负责精确失效
    FunctionAnalysisPassManager &FAM;
    //CreatedShadowBytes记录本Pass实例已经提交的静态镜像内存
    std::size_t CreatedShadowBytes = 0;
    //NextShadowOrdinal为内部全局符号提供稳定递增编号
    unsigned NextShadowOrdinal = 0;
    //VersionedSources避免固定点再次优化本次生成的语义慢路径
    std::unordered_set<Variable *> VersionedSources;
};
