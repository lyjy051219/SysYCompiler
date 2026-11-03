// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#pragma once
#include "ir/analysis/LoopDependence.hpp"
#include "ir/opt/NewPassManager.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
 
//BasicAAResult提供底层对象和内存位置别名关系
class BasicAAResult;
//DominatorTree提供迭代路径上的支配关系
class DominatorTree;
//Loop表示一个自然循环及其嵌套关系
class Loop;
//LoopInfo提供函数内循环森林和基本块归属
class LoopInfo;
//RangeInfo提供指令上下文中的整数值域
class RangeInfo;
//SCEVResult提供循环次数、步长和无回绕证明
class SCEVResult;

//LoopAccessPairKind表示两个循环内存访问之间可证明的冲突范围
enum class LoopAccessPairKind {
    //NoConflict表示两个访问确定不会发生读写冲突
    NoConflict,
    //SameIterationOnly表示冲突只可能发生在相同逻辑迭代
    SameIterationOnly,
    //Unknown表示现有证据无法界定两个访问的冲突范围
    Unknown
};

//LoopAccessDimension描述访问足迹在单个循环轴上的次数和字节步长
struct LoopAccessDimension {
    //Axis指向当前足迹维度对应的循环轴
    Loop *Axis = nullptr;
    //TripCount保存该循环轴的符号迭代次数
    const SCEVExpr *TripCount = nullptr;
    //ConstantTripCount保存可证明的常量迭代次数
    std::optional<std::int64_t> ConstantTripCount;
    //ByteStride保存该循环轴每前进一步产生的字节位移
    std::optional<std::int64_t> ByteStride;
    //IsFinite标记该循环轴是否已证明有限
    bool IsFinite = false;
    //IsUnitStride标记该循环轴的步长绝对值是否等于元素大小
    bool IsUnitStride = false;
};

//LoopAccessFootprint汇总一个循环内存访问在完整迭代域上的地址足迹
struct LoopAccessFootprint {
    //Access指向LoopDependence保存的稳定内存访问记录
    const LoopMemoryAccess *Access = nullptr;
    //Base指向足迹能够归一化到的唯一底层对象
    Value *Base = nullptr;
    //StartByteOffset保存逻辑迭代原点相对底层对象的字节偏移
    std::optional<std::int64_t> StartByteOffset;
    //MinByteOffset保存常量足迹相对底层对象的最小字节偏移
    std::optional<std::int64_t> MinByteOffset;
    //MaxByteOffsetExclusive保存常量足迹半开区间的右端字节偏移
    std::optional<std::int64_t> MaxByteOffsetExclusive;
    //ObjectSize保存唯一底层对象的已知分配字节数
    std::optional<std::size_t> ObjectSize;
    //ElementSize保存单次内存访问覆盖的字节数
    std::size_t ElementSize = 0;
    //Alignment保存访问地址至少满足的字节对齐
    std::size_t Alignment = 1;
    //Dimensions按循环嵌套顺序保存足迹涉及的各个循环轴
    std::vector<LoopAccessDimension> Dimensions;
    //IsConsecutive标记最内层变化维度是否形成无洞连续访问
    bool IsConsecutive = false;
    //IsFullAccess标记整个已证明迭代域是否在足迹内无洞连续
    bool IsFullAccess = false;
    //IsEmpty标记常量零次迭代导致该访问实际不会执行
    bool IsEmpty = false;
    //Exact标记足迹的基址、起点、维度和执行条件是否全部精确
    bool Exact = false;
};

//LoopAccessSummary汇总一个循环作用域内的访问记录、足迹和保守状态
struct LoopAccessSummary {
    //Scope指向该访问摘要覆盖的循环作用域
    Loop *Scope = nullptr;
    //Accesses保存LoopDependence提供的稳定内存访问记录
    std::vector<const LoopMemoryAccess *> Accesses;
    //Footprints保存与访问记录一一对应的循环访问足迹
    std::vector<LoopAccessFootprint> Footprints;
    //HasUnknownMemory标记作用域中是否存在无法建模的内存访问
    bool HasUnknownMemory = true;
    //HasWrite标记作用域中是否至少存在一次写访问
    bool HasWrite = false;
};

//LoopFusionAccessResult汇总两个循环的访问融合合法性和冲突统计
struct LoopFusionAccessResult {
    //Legal标记所有潜在冲突是否均允许循环融合
    bool Legal = false;
    //Unknown标记分析是否因证据不足或预算耗尽而保守失败
    bool Unknown = false;
    //SameIterationFlowPairs记录仅同次迭代冲突的写后读访问对数量
    unsigned SameIterationFlowPairs = 0;
    //SameIterationConflictPairs记录所有仅同次迭代冲突的访问对数量
    unsigned SameIterationConflictPairs = 0;
};

//LoopAccessInfo把标量地址证明转换成循环变换可直接消费的访问判定
class LoopAccessInfo {
public:
    //构造一个尚未绑定任何基础分析的空访问分析结果
    LoopAccessInfo();
    //使用函数及各项基础分析构造循环访问查询器
    LoopAccessInfo(Function *F, LoopInfo *LI, SCEVResult *SE,
                   BasicAAResult *AA, LoopDependenceInfo *LD,
                   RangeInfo *RI, DominatorTree *DT);
    //移动构造访问分析结果并转移惰性摘要缓存
    LoopAccessInfo(LoopAccessInfo &&) noexcept;
    //移动赋值访问分析结果并转移惰性摘要缓存
    LoopAccessInfo &operator=(LoopAccessInfo &&) noexcept;
    //禁止拷贝访问分析结果以避免复制带有IR指针键的缓存
    LoopAccessInfo(const LoopAccessInfo &) = delete;
    //禁止拷贝赋值访问分析结果以避免复制带有IR指针键的缓存
    LoopAccessInfo &operator=(const LoopAccessInfo &) = delete;

    //返回循环作用域的访问摘要并在首次查询时构建缓存
    const LoopAccessSummary &getSummary(Loop *Scope);
    //返回指定稳定访问记录在循环作用域中的足迹
    const LoopAccessFootprint *getFootprint(
        Loop *Scope, const LoopMemoryAccess &Access);
    //返回指定指令在循环作用域中对应的全部访问足迹
    std::vector<const LoopAccessFootprint *> getFootprints(
        Loop *Scope, Instruction *I);
    //判断指定指令沿给定循环轴的全部访问是否连续
    bool isConsecutiveAccess(Loop *Scope, Instruction *I, Loop *Axis);
    //分类两个循环轴上的内存访问是否冲突或仅同次迭代冲突
    LoopAccessPairKind classifyPair(
        Loop *FirstScope, Loop *FirstAxis,
        const LoopMemoryAccess &First,
        Loop *SecondScope, Loop *SecondAxis,
        const LoopMemoryAccess &Second) const;
    //判断作用域内各次指定轴迭代之间是否不存在未知写冲突
    bool areIterationsIndependent(Loop *Scope, Loop *Axis);
    //分析两个循环的访问关系并生成融合合法性摘要
    LoopFusionAccessResult analyzeFusion(Loop *First, Loop *Second);
    //判断两个循环是否满足访问层面的融合合法性
    bool isFusionLegal(Loop *First, Loop *Second);
    //判断两个循环是否形成次数独立且有限的矩形循环巢
    bool isRectangularNest(Loop *Outer, Loop *Inner);
    //判断同一读取是否在一个循环轴复用并在另一轴变化
    bool hasCrossAxisReuse(Loop *Scope, Loop *ReusedAxis,
                           Loop *VaryingAxis);
    //返回指定指令沿给定循环轴的已知字节步长
    std::optional<std::int64_t> getByteStride(Instruction *I, Loop *Scope,
                                              Loop *Axis);
    //求源写到宿读在指定轴上的唯一常量依赖距离，宿迭代减源迭代为正。
    std::optional<std::int64_t> getConstantDependenceDistance(
        Instruction *Source, Instruction *Sink,
        Loop *Scope, Loop *Axis);
    //根据本Pass及其依赖分析的保留状态判断结果是否失效
    bool invalidate(Function &F, const PreservationStatus &PA);

private:
    //MaxTransformAccessPairs限制单次循环变换判定枚举的访问对数量
    static constexpr std::size_t MaxTransformAccessPairs = 8192;
    //MaxAddressNoWrapQueryNodes限制单次地址无回绕查询访问的SSA节点数
    static constexpr std::size_t MaxAddressNoWrapQueryNodes = 4096;

    //AddressNoWrapQueryState保存单次地址表达式无回绕查询的递归状态
    struct AddressNoWrapQueryState {
        //Active记录当前递归路径上的活动SSA节点以识别环
        std::unordered_set<Value *> Active;
        //Completed缓存本次查询中已完成节点的无回绕结论
        std::unordered_map<Value *, bool> Completed;
        //VisitedNodes记录本次查询已访问的SSA节点数量
        std::size_t VisitedNodes = 0;
    };

    //Func指向当前分析结果所属的函数
    Function *Func = nullptr;
    //LI指向函数级循环森林分析结果
    LoopInfo *LI = nullptr;
    //SE指向函数级标量演化分析结果
    SCEVResult *SE = nullptr;
    //AA指向函数级基础别名分析结果
    BasicAAResult *AA = nullptr;
    //LD指向函数级循环依赖分析结果
    LoopDependenceInfo *LD = nullptr;
    //RI指向函数级整数范围分析结果
    RangeInfo *RI = nullptr;
    //DT指向函数级支配树分析结果
    DominatorTree *DT = nullptr;
    //Cache按循环作用域保存按需构建的访问摘要
    std::unordered_map<Loop *, std::unique_ptr<LoopAccessSummary>> Cache;

    //为循环作用域构建访问记录及其足迹摘要
    std::unique_ptr<LoopAccessSummary> buildSummary(Loop *Scope);
    //为单个稳定内存访问构建完整迭代域上的访问足迹
    LoopAccessFootprint buildFootprint(
        Loop *Scope, const LoopMemoryAccess &Access);
    //按嵌套顺序收集作用域及其所有子循环轴
    static void collectLoopAxes(Loop *Scope, std::vector<Loop *> &Axes);
    //计算一个内存访问沿指定循环轴的总字节步长
    static std::optional<std::int64_t> computeAccessByteStride(
        const LoopMemoryAccess &Access, Loop *Axis);
    //计算仿射访问地址中不含符号项的常量起始偏移
    static std::optional<std::int64_t> computeConstantStartOffset(
        const LoopMemoryAccess &Access);
    //执行带溢出检查的有符号加法
    static bool checkedAdd(std::int64_t First, std::int64_t Second,
                           std::int64_t &Result);
    //执行带溢出检查的有符号乘法
    static bool checkedMul(std::int64_t First, std::int64_t Second,
                           std::int64_t &Result);
    //返回有符号整数绝对值对应的无符号幅值并处理最小值
    static std::uint64_t absoluteMagnitude(std::int64_t Value);
    //判断循环轴是否已由SCEV及可选范围事实证明有限
    bool isKnownFinite(Loop *Axis) const;
    //判断指定门控块是否支配循环轴所有有效迭代路径
    bool dominatesIterationPaths(BasicBlock *Gate, Loop *Axis) const;
    //判断直接父子循环轴是否构成次数不依赖外层的矩形维度
    bool isRectangularAxisPair(Loop *Outer, Loop *Inner) const;
    //判断内存访问是否在足迹覆盖的每个逻辑迭代中必定执行
    bool isMustExecuteInIterationDomain(
        const LoopMemoryAccess &Access,
        const std::vector<LoopAccessDimension> &Dimensions) const;
    //递归证明整数地址表达式在指令上下文中不会发生有符号回绕
    bool isIntegerAddressExpressionNoWrap(
        Value *Expression, Instruction *Context,
        AddressNoWrapQueryState &State, unsigned Depth) const;
    //判断内存访问完整地址计算链是否已证明不回绕
    bool isAddressArithmeticKnownNoWrap(
        const LoopMemoryAccess &Access) const;
    //判断两个内存访问是否至少包含一个写入并可能形成冲突
    static bool isWriteConflict(const LoopMemoryAccess &First,
                                const LoopMemoryAccess &Second);
    //判断两个仿射表达式的循环外符号项及其系数是否相同
    static bool sameInvariantTerms(const LoopAffineExpr &First,
                                   const LoopAffineExpr &Second);
    //在映射两个目标循环轴后判断其余循环项及系数是否相同
    static bool sameLoopTermsWithAxisMapping(
        const LoopAffineExpr &First, Loop *FirstAxis,
        const LoopAffineExpr &Second, Loop *SecondAxis);
    //判断内存访问是否满足完整混合基数地址布局约束
    static bool isMixedRadixAccess(const LoopMemoryAccess &Access);
    //判断仿射表达式是否仅包含目标循环轴并返回其非零系数
    static bool isolateAxis(const LoopAffineExpr &Expression, Loop *Axis,
                            std::int64_t &Coefficient);
    //判断两个访问地址相等是否强制两个循环轴位于相同逻辑迭代
    static bool equalityForcesAlignedIteration(
        const LoopMemoryAccess &First, Loop *FirstAxis,
        const LoopMemoryAccess &Second, Loop *SecondAxis);
};

//LoopAccessAnalysis聚合循环、范围、别名和依赖事实构建访问查询器
class LoopAccessAnalysis : public AnalysisDescriptor<LoopAccessAnalysis> {
public:
    //Result指定该分析Pass返回LoopAccessInfo
    using Result = LoopAccessInfo;
    //构建函数级LoopAccessInfo并绑定所需基础分析结果
    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;

private:
    //允许AnalysisDescriptor访问本Pass的唯一标识
    friend AnalysisDescriptor<LoopAccessAnalysis>;
    //Key是LoopAccessAnalysis在Pass管理器中的唯一标识
    inline static PassID Key;
};
