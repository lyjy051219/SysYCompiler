// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstdint>
#include <unordered_set>
#include <vector>
 
class DominatorTree;
class BasicAAResult;
class Function;
class Loop;
class LoopAccessInfo;
class RangeInfo;
class SCEVResult;
class LoopPhaseRelayoutPass;
struct InductionDescriptor;
struct LoopTripCountInfo;

//LoopIdiomRecognizePass识别连续内存循环并用memset、memcpy或memmove替换
class LoopIdiomRecognizePass
    : public PassDescriptor<LoopIdiomRecognizePass> {
public:
    //遍历函数内循环并提交已证明安全且有收益的内存惯用法替换
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    //阶段布局Pass复用本Pass已经验证过的typed GEP与完整覆盖基础证明
    friend class LoopPhaseRelayoutPass;
    //AffineViewAccess保存一次二维typed GEP访问的根对象、完整索引和轴映射
    struct AffineViewAccess {
        //Address指向产生最终标量地址的GEP
        GetElementPtrInst *Address = nullptr;
        //Root记录展平零索引GEP链后得到的数组根对象
        Value *Root = nullptr;
        //Indices保存从根对象出发的完整typed GEP索引
        std::vector<Value *> Indices;
        //ByteStrides保存各索引在对象布局中的字节步长
        std::vector<int64_t> ByteStrides;
        //AxisAtIndex以-1表示常量索引，以0/1表示外层或内层归纳轴
        std::vector<int> AxisAtIndex;
    };

    //AffineViewRewrite保存一个临时数组读取及其等价源数组索引
    struct AffineViewRewrite {
        //Load指向需要改写地址的临时数组读取
        LoadInst *Load = nullptr;
        //SourceIndices保存改写后从源对象出发的完整typed GEP索引
        std::vector<Value *> SourceIndices;
    };

    //AffineViewCandidate保存完整二维复制视图消除所需的全部证明和改写计划
    struct AffineViewCandidate {
        //Outer和Inner记录生产者的直接父子矩形循环
        Loop *Outer = nullptr;
        Loop *Inner = nullptr;
        //ExitBlock记录生产循环完成后唯一到达的出口块
        BasicBlock *ExitBlock = nullptr;
        //OuterIV和InnerIV记录两条规范单位递增归纳轴
        PhiInst *OuterIV = nullptr;
        PhiInst *InnerIV = nullptr;
        //Store和Load记录生产者唯一的一次写入和源读取
        StoreInst *Store = nullptr;
        LoadInst *Load = nullptr;
        //Destination和Source保存生产者两端的typed GEP置换
        AffineViewAccess Destination;
        AffineViewAccess Source;
        //Rewrites保存所有被完整初始化支配的只读消费者
        std::vector<AffineViewRewrite> Rewrites;
        //ElementCount保存生产循环已证明覆盖的标量元素数量
        int64_t ElementCount = 0;
    };

    //MinAffineViewElements限制过小对象为一次视图消除增加的编译与代码扰动
    static constexpr int64_t MinAffineViewElements = 16;

    //LinearAccess保存相对循环归纳变量呈单位步长变化的线性GEP访问
    struct LinearAccess {
        //GEP指向被匹配的原始地址计算指令
        GetElementPtrInst *GEP = nullptr;
        //Base记录位于循环外的地址基址
        Value *Base = nullptr;
        //Indices保存GEP的完整索引序列
        std::vector<Value *> Indices;
        //InductionIndex记录归纳变量在索引序列中的位置
        size_t InductionIndex = 0;
        //ElementTy记录每次迭代访问的标量元素类型
        std::shared_ptr<IRType> ElementTy;
    };

    //CopyLoopMatch保存一个简单连续复制循环的完整匹配结果
    struct CopyLoopMatch {
        //L指向被匹配的复制循环
        Loop *L = nullptr;
        //PreHeader指向循环唯一前置块
        BasicBlock *PreHeader = nullptr;
        //Header指向循环唯一头块
        BasicBlock *Header = nullptr;
        //ExitBlock指向循环唯一出口块
        BasicBlock *ExitBlock = nullptr;
        //IndVar指向单位步长规范归纳PHI
        PhiInst *IndVar = nullptr;
        //Store指向循环内唯一目标写入
        StoreInst *Store = nullptr;
        //Load指向循环内唯一源读取
        LoadInst *Load = nullptr;
        //Destination保存目标连续地址模式
        LinearAccess Destination;
        //Source保存源连续地址模式
        LinearAccess Source;
        //RuntimeBound记录运行时循环上界
        Value *RuntimeBound = nullptr;
        //Start记录归纳变量的常量起始值
        int64_t Start = 0;
        //TripCount记录已证明的常量迭代次数
        int64_t TripCount = 0;
        //IsRuntime标记该循环是否使用运行时迭代次数
        bool IsRuntime = false;
    };

    //匹配由单个归纳索引驱动的连续同型GEP访问
    static bool matchLinearAccess(GetElementPtrInst *GEP, PhiInst *IndVar,
                                  Loop *L, LinearAccess &Access);
    //剥离GEP和BitCast并返回可证明独立的全局或栈对象根
    static Value *getDisjointObjectRoot(Value *V);
    //判断两个线性访问是否来自不同且确定不相交的对象
    static bool areProvablyDisjoint(const LinearAccess &A,
                                    const LinearAccess &B);
    //判断常量的全部字节是否相同并返回memset填充值
    static bool getRepeatedByte(Value *V, uint8_t &Byte);
    //证明循环仅含惯用法替换允许的归纳、地址、访存和控制指令
    static bool hasOnlyAllowedLoopInstructions(
        Loop *L, const LoopTripCountInfo &TCI,
        const InductionDescriptor &IV, StoreInst *Store,
        GetElementPtrInst *DestGEP, LoadInst *SourceLoad,
        GetElementPtrInst *SourceGEP, DominatorTree &DT);
    //在preheader中物化指定起始迭代对应的首元素地址
    static GetElementPtrInst *materializeStartPointer(
        const LinearAccess &Access, PhiInst *IndVar, int64_t Start,
        BasicBlock *PreHeader);
    //计算线性访问从指定起始迭代到已知底层对象末端的可用字节数
    static bool getKnownRemainingObjectBytes(
        const LinearAccess &Access, int64_t Start, BasicAAResult &AA,
        size_t &RemainingBytes);
    //匹配可参与临时数组双复制消除的简单连续复制循环
    static bool matchSimpleCopyLoop(Loop *L, SCEVResult &SE,
                                    DominatorTree &DT,
                                    CopyLoopMatch &Match);
    //把相邻的源到临时数组和临时数组到目标复制合并为memmove
    static bool transformTemporaryCopyPair(
        Function &F, const CopyLoopMatch &First,
        const CopyLoopMatch &Second, DominatorTree &DT, RangeInfo &RI,
        BasicAAResult &AA, bool &CFGChanged);
    //展平最终标量typed GEP并计算每个逻辑索引对应的字节步长
    static bool matchAffineViewAccess(Value *Pointer,
                                      AffineViewAccess &Access);
    //把生产者GEP中的两个动态索引绑定到外层与内层规范归纳轴
    static bool bindAffineViewAxes(AffineViewAccess &Access,
                                   PhiInst *OuterIV, PhiInst *InnerIV);
    //递归检查对象地址只经typed GEP到达load/store或允许的数组输入调用
    static bool collectAffineViewObjectUses(
        Value *Current, Function &F, bool AllowArrayInput,
        std::unordered_set<Value *> &Visited,
        std::vector<LoadInst *> &Loads,
        std::vector<StoreInst *> &Stores,
        std::vector<Instruction *> &Writers);
    //收集组成最终标量地址的全部typed GEP指令
    static bool collectAffineViewGEPChain(
        Value *Pointer, Value *ExpectedRoot,
        std::unordered_set<Instruction *> &Instructions);
    //判断生产者循环除两条归纳控制和唯一复制外不含其他计算或效果
    static bool hasOnlyAffineViewProducerInstructions(
        const AffineViewCandidate &Candidate, SCEVResult &SE);
    //判断访问足迹精确覆盖整个底层对象且没有遗漏或越界
    static bool affineViewCoversWholeObject(
        LoopAccessInfo &LA, Loop *Scope, Instruction *Access,
        Value *ExpectedRoot, int64_t ElementCount);
    //判断从生产循环出口出发是否还能到达指定基本块
    static bool isReachableFromAffineViewExit(BasicBlock *Exit,
                                              BasicBlock *Target);
    //为一个二维矩形复制循环构建完整只读仿射视图改写计划
    static bool buildAffineViewCandidate(
        Function &F, Loop *Outer, LoopAccessInfo &LA, SCEVResult &SE,
        DominatorTree &DT, AffineViewCandidate &Candidate);
    //提交已完成全部合法性与收益证明的仿射视图消除
    static bool transformAffineView(Function &F, Loop *Outer,
                                    LoopAccessInfo &LA, SCEVResult &SE,
                                    DominatorTree &DT);
    //识别并把单个连续填充或复制循环替换为memset或memcpy
    static bool transformLoop(Function &F, Loop *L, SCEVResult &SE,
                              DominatorTree &DT, RangeInfo &RI,
                              BasicAAResult &AA, bool &CFGChanged);
};
