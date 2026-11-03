// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

//IndexComponent保存GEP下标的原始IR值和对应SCEV表达式
struct IndexComponent {
    //Raw记录GEP当前维度使用的原始下标值
    Value *Raw = nullptr;
    //Expr记录Raw按SCEV归一化后的符号表达式
    const SCEVExpr *Expr = nullptr;
};
 
//LSRCandidate描述一条可尝试改写为指针归纳变量的GEP
struct LSRCandidate {
    //OldGEP指向待替换的原始循环内GEP
    GetElementPtrInst *OldGEP = nullptr;
    //Base记录GEP地址族共享的底层基址
    Value *Base = nullptr;
    //Index记录当前循环轴上发生变化的下标值
    Value *Index = nullptr;
    //Start记录活动下标在循环入口处的SCEV起点
    const SCEVExpr *Start = nullptr;
    //StartConst在起点可折叠时记录其常量值
    std::optional<int64_t> StartConst;
    //Prefix记录活动下标之前保持不变的GEP维度
    std::vector<IndexComponent> Prefix;
    //Suffix记录活动下标之后保持不变的GEP维度
    std::vector<IndexComponent> Suffix;
    //L记录候选所属的自然循环
    Loop *L = nullptr;
    //PreHeader记录构造起始指针的循环预头块
    BasicBlock *PreHeader = nullptr;
    //Header记录放置指针PHI的循环头块
    BasicBlock *Header = nullptr;
    //Latch记录放置递推地址的唯一回边块
    BasicBlock *Latch = nullptr;
    //Step记录活动下标每次迭代变化的常量步长
    int64_t Step = 0;
    //LastIndexStride记录活动下标对应的类型化字节步长
    int64_t LastIndexStride = 0;
    //HasLeadingZero标记GEP是否包含数组寻址要求的前导零下标
    bool HasLeadingZero = false;
    //AllowOffsetReuse标记候选是否允许与常量起点不同的同形地址共享指针IV
    bool AllowOffsetReuse = true;
    //Used标记候选是否已经被某个成功改写的分组消费
    bool Used = false;
};

//LSRGroup保存起点和地址形状完全一致的一组GEP候选
struct LSRGroup {
    //Order记录该组在稳定候选顺序中的位置
    size_t Order = 0;
    //Items保存属于当前完全同形分组的候选
    std::vector<LSRCandidate *> Items;
    //Base记录分组共享的底层基址
    Value *Base = nullptr;
    //Start记录分组共享的SCEV起点
    const SCEVExpr *Start = nullptr;
    //StartConst记录能够折叠的共享常量起点
    std::optional<int64_t> StartConst;
    //L记录分组所属的自然循环
    Loop *L = nullptr;
    //PreHeader记录共享起始指针的插入块
    BasicBlock *PreHeader = nullptr;
    //Header记录共享指针PHI的插入块
    BasicBlock *Header = nullptr;
    //Latch记录共享递推GEP的插入块
    BasicBlock *Latch = nullptr;
    //Step记录分组共享的循环下标步长
    int64_t Step = 0;
    //Prefix记录活动下标之前的共享不变维度
    std::vector<IndexComponent> Prefix;
    //Suffix记录活动下标之后的共享不变维度
    std::vector<IndexComponent> Suffix;
    //ResultTy记录原GEP结果必须保持的指针类型
    std::shared_ptr<IRType> ResultTy;
};

//OffsetReuseGroup保存形状相同但常量起点允许不同的一组GEP候选
struct OffsetReuseGroup {
    //Order记录该组在稳定候选顺序中的位置
    size_t Order = 0;
    //Items保存通过常量偏移复用同一指针IV的候选
    std::vector<LSRCandidate *> Items;
    //Base记录分组共享的底层基址
    Value *Base = nullptr;
    //L记录分组所属的自然循环
    Loop *L = nullptr;
    //PreHeader记录锚点起始指针的插入块
    BasicBlock *PreHeader = nullptr;
    //Header记录锚点指针PHI的插入块
    BasicBlock *Header = nullptr;
    //Latch记录锚点递推GEP的插入块
    BasicBlock *Latch = nullptr;
    //Step记录分组共享的循环下标步长
    int64_t Step = 0;
    //AnchorStart记录用于构造共享指针IV的常量锚点起点
    int64_t AnchorStart = 0;
    //Prefix记录活动下标之前的共享不变维度
    std::vector<IndexComponent> Prefix;
    //ResultTy记录所有候选必须保持的结果指针类型
    std::shared_ptr<IRType> ResultTy;
};

//CrossBaseLSRGroup保存由同一个归纳下标驱动的两个不同基址地址流
struct CrossBaseLSRGroup {
    //Order记录该组在稳定候选顺序中的位置
    size_t Order = 0;
    //Items保存必须原子改写的两个跨基址候选
    std::vector<LSRCandidate *> Items;
    //L记录两个地址流共同所属的最内层循环
    Loop *L = nullptr;
    //PreHeader记录两个起始地址共同插入的循环预头块
    BasicBlock *PreHeader = nullptr;
    //Header记录两个指针PHI共同插入的循环头块
    BasicBlock *Header = nullptr;
    //Latch记录两个指针递推共同插入的循环回边块
    BasicBlock *Latch = nullptr;
    //Index记录两个地址流共享的原始整数归纳值
    Value *Index = nullptr;
    //Step记录按末级元素计量的单位地址步长
    int64_t Step = 0;
    //ElementByteStride记录每个末级元素占用的字节数
    int64_t ElementByteStride = 0;
};

//PointerIV保存强度削减后指针递推的起点、PHI和回边更新
struct PointerIV {
    //Start是在循环预头计算的初始地址
    GetElementPtrInst *Start = nullptr;
    //Phi是在循环头选择初始地址或回边地址的指针PHI
    PhiInst *Phi = nullptr;
    //Next是在回边块计算的下一迭代地址
    GetElementPtrInst *Next = nullptr;
};

//AddressGEPNode记录地址族中的派生GEP及其相对根GEP的常量偏移
struct AddressGEPNode {
    //GEP指向地址族中的一个GEP节点
    GetElementPtrInst *GEP = nullptr;
    //Offset记录该节点相对地址族根的元素偏移
    int64_t Offset = 0;
};

//LSRCostEstimate使用IR指令数近似一次地址强度削减的动态和设置成本
struct LSRCostEstimate {
    //OldLoopCost记录改写前每次迭代执行的地址与索引成本
    int64_t OldLoopCost = 0;
    //NewLoopCost记录改写后每次迭代执行的指针递推成本
    int64_t NewLoopCost = 0;
    //NewSetupCost记录预头和循环头新增的一次性设置成本
    int64_t NewSetupCost = 0;
    //RemovableIndexCost记录改写后能够删除的每轮索引计算成本
    int64_t RemovableIndexCost = 0;
    //Valid标记成本估计是否完整且未遇到不支持的物化
    bool Valid = false;
};

//把循环内仿射索引 GEP 改写为指针归纳变量：
//1. 从内到外扫描规范循环，发现仅一个循环变化维度的 GEP 地址族；
//2. 用 SCEV、支配关系和无回绕证明验证起点与步长可安全外提；
//3. 按完全同形或常量起点偏移分组，并比较旧索引链与新指针 IV 的动态成本；
//4. 在预头/头部/latch 构造起始地址、PHI 和递推 GEP，替换旧地址；
//5. 递归删除失去使用的旧索引链，并使数值、范围、别名分析失效。
class LoopStrengthReductionPass
    : public PassDescriptor<LoopStrengthReductionPass> {
public:
    //构造常规LSR或仅在流水线末尾运行的RV64跨基址模式
    explicit LoopStrengthReductionPass(bool EnableCrossBase = false);
    //运行循环地址强度削减并返回实际保留的分析集合
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    //EnableCrossBase只允许最终阶段创建non-inbounds跨基址指针IV
    bool EnableCrossBase = false;
    //UnsupportedMaterializeCost表示无法安全物化SCEV时使用的拒绝成本
    static constexpr int UnsupportedMaterializeCost = 1000000;
    //MaxPointerIVsPerInnerLoop限制最内层循环创建的指针IV数量
    static constexpr int MaxPointerIVsPerInnerLoop = 8;
    //MaxPointerIVsPerOuterLoop限制非最内层循环创建的指针IV数量
    static constexpr int MaxPointerIVsPerOuterLoop = 4;
    //CrossBaseStreamCount限制首版跨基址原子组只处理两个直接地址流
    static constexpr int CrossBaseStreamCount = 2;
    //RiscvUnknownTripEstimate是RV64未知次数内层循环的通用成本估计
    static constexpr int RiscvUnknownTripEstimate = 4;
    //返回SCEV表达式用于候选分组的稳定结构键
    static std::string scevKey(const SCEVExpr *E);
    //判断原始IR值是否能在循环预头直接使用
    static bool canUseInPreheader(Value *V, BasicBlock *PreHeader,
                                  const DominatorTree &DT);
    //判断SCEV表达式能否在预头和固定预算内安全物化
    static bool canMaterializeSCEVAtPreheader(
        const SCEVExpr *E, const Loop *L, BasicBlock *PreHeader,
        const DominatorTree &DT);
    //估算在预头物化SCEV表达式需要新增的指令数
    static int estimateMaterializeCost(
        const SCEVExpr *E, const Loop *L, BasicBlock *PreHeader,
        const DominatorTree &DT);
    //把SCEV整数常量转换为具有对应类型的IR常量
    static Value *makeSCEVConstant(int64_t V, const SCEVExpr *E);
    //在预头事务式物化SCEV表达式并返回生成的IR值
    static Value *materializeSCEVAtPreheader(
        const SCEVExpr *E, const Loop *L, BasicBlock *PreHeader,
        const DominatorTree &DT);
    //估算在预头物化全部不变前缀下标的指令成本
    static int estimatePrefixMaterializeCost(
        const std::vector<IndexComponent> &Prefix, const Loop *L,
        BasicBlock *PreHeader, const DominatorTree &DT);
    //在预头物化全部不变前缀下标并按原顺序返回IR值
    static bool materializePrefix(
        const std::vector<IndexComponent> &Prefix, const Loop *L,
        BasicBlock *PreHeader, const DominatorTree &DT,
        std::vector<Value *> &Indices);
    //判断Use是否把GEP直接作为Load或Store的地址操作数
    static bool isLoadStoreAddressUse(Instruction *UserI, Use *U);
    //识别从父GEP派生的单常量偏移GEP并返回偏移
    static bool getConstantOffsetGEP(
        GetElementPtrInst *Parent, Instruction *UserI, Use *U,
        GetElementPtrInst *&Child, int64_t &Offset);
    //递归收集只用于访存或常量偏移派生的完整GEP地址族
    static bool collectAddressGEPFamily(
        GetElementPtrInst *GEP, Loop *L, int64_t CurrentOffset,
        std::vector<AddressGEPNode> &Nodes,
        std::unordered_set<GetElementPtrInst *> &Visited);
    //判断GEP形状是否是能够按单下标递推的数组元素步进
    static bool isSingleIndexArrayStep(Value *Base, size_t NumIndices);
    //解析GEP并验证其只有一个随目标循环变化的仿射下标
    static bool matchGEP(
        GetElementPtrInst *GEP, Loop *L, SCEVResult &SE, Value *&Base,
        Value *&Index, std::vector<IndexComponent> &Prefix,
        std::vector<IndexComponent> &Suffix, int64_t &ActiveIndexStride,
        int64_t &FinalPointeeStride, bool &HasLeadingZero,
        bool &AllowOffsetReuse, bool AllowDirectScalarFallback);
    //证明两块头测单位步长循环中每次实际执行的整数更新都不会回绕
    static bool hasStrictGuardedUnitStepNoWrap(
        Loop *L, Value *Index, int64_t Step);
    //按照稳定函数顺序收集所有满足安全条件的地址强度削减候选
    static std::vector<LSRCandidate> collectCandidates(
        Function &F, LoopInfo &LI, SCEVResult &SE,
        const DominatorTree &DT, bool EnableCrossBase);
    //生成编码循环、基址、步长、布局和可选起点的候选形状键
    static std::string candidateShapeKey(const LSRCandidate &Candidate,
                                         bool IncludeStart);
    //按照忽略常量起点的形状键构造偏移复用分组
    static std::vector<OffsetReuseGroup> buildOffsetReuseGroups(
        std::vector<LSRCandidate> &Candidates);
    //按照包含起点的完整形状键构造完全同形分组
    static std::vector<LSRGroup> buildExactGroups(
        std::vector<LSRCandidate> &Candidates);
    //按照同一整数归纳值构造两个不同基址的RV64原子地址流分组
    static std::vector<CrossBaseLSRGroup> buildCrossBaseGroups(
        std::vector<LSRCandidate> &Candidates,
        const DominatorTree &DT);
    //判断指令是否属于能够递归删除的纯整数索引计算
    static bool isRemovableIndexInst(Instruction *I);
    //收集使用完全封闭在允许子图中的可删除索引计算链
    static bool collectRemovableIndexChain(
        Instruction *I, std::unordered_set<User *> &AllowedUsers,
        std::unordered_set<Instruction *> &Out);
    //估算候选分组改写后能够删除的每轮索引计算成本
    static int64_t estimateRemovableIndexCost(
        Loop *L, const std::vector<LSRCandidate *> &Items,
        const DominatorTree &DT);
    //估算候选分组改写前每次循环迭代的地址计算成本
    static int64_t estimateOldLoopCost(
        Loop *L, const std::vector<LSRCandidate *> &Items,
        const DominatorTree &DT);
    //估算完全同形分组使用共享指针IV后的新旧成本
    static LSRCostEstimate estimateExactGroupCost(
        const LSRGroup &Group, const DominatorTree &DT);
    //估算常量偏移分组使用锚点指针IV后的新旧成本
    static LSRCostEstimate estimateOffsetGroupCost(
        const OffsetReuseGroup &Group, const DominatorTree &DT);
    //估算RV64跨基址组共享缩放地址后的目标相关成本
    static LSRCostEstimate estimateCrossBaseGroupCost(
        const CrossBaseLSRGroup &Group);
    //结合循环次数判断一次性设置成本能否由每轮收益摊销
    static bool isCostModelProfitable(
        const LSRCostEstimate &Cost, Loop *L, SCEVResult &SE);
    //判断常量指针偏移是否能由目标IR的有符号索引表示
    static bool isRepresentablePointerOffset(int64_t Offset);
    //递归删除改写后失去所有使用的旧GEP和索引计算
    static void cleanupDeadIndexChains(
        const std::vector<Instruction *> &Seeds);
    //判断完全同形分组是否满足成本模型并值得改写
    static bool shouldRewriteExactGroup(const LSRGroup &Group,
                                        const DominatorTree &DT,
                                        SCEVResult &SE);
    //判断常量偏移分组是否满足成本模型并值得改写
    static bool shouldRewriteOffsetGroup(const OffsetReuseGroup &Group,
                                         const DominatorTree &DT,
                                         SCEVResult &SE);
    //判断跨基址原子组在RV64目标成本模型下是否值得改写
    static bool shouldRewriteCrossBaseGroup(
        const CrossBaseLSRGroup &Group, SCEVResult &SE);
    //在预头、循环头和回边块创建结构完整的指针归纳变量
    static PointerIV createPointerIV(
        Value *Base, const std::vector<Value *> &StartIndices, int64_t Step,
        const std::shared_ptr<IRType> &ResultTy, BasicBlock *PreHeader,
        BasicBlock *Header, BasicBlock *Latch, bool InBounds = true);
    //使用一个共享指针IV替换完全同形分组中的全部旧GEP
    static bool rewriteExactGroup(
        LSRGroup &Group, const DominatorTree &DT,
        std::vector<Instruction *> &DeadSeeds);
    //使用锚点指针IV和常量偏移替换偏移复用分组
    static bool rewriteOffsetGroup(
        OffsetReuseGroup &Group, const DominatorTree &DT,
        std::vector<Instruction *> &DeadSeeds);
    //原子创建两个non-inbounds指针IV并替换跨基址地址流
    static bool rewriteCrossBaseGroup(
        CrossBaseLSRGroup &Group, const DominatorTree &DT,
        std::vector<Instruction *> &DeadSeeds);
    //返回给定循环允许创建的最大指针IV数量
    static int pointerIVLimit(const Loop *L);
    //执行候选发现、分组、成本选择、改写和死索引清理主流程
    static bool runAddressLSR(Function &F, LoopInfo &LI, SCEVResult &SE,
                              DominatorTree &DT, bool EnableCrossBase);
};
