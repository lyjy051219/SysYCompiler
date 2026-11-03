// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstdint>
#include <unordered_set>
#include <vector>

class BasicAAResult;
class BlockFrequencyInfo;
class DominatorTree;
class Loop;
class LoopInfo;
class PostDominatorTree;
class RangeInfo;
class SCEVResult;
class TargetCostInfo;

//InvariantPrivateStorePredicateEdge记录到达候选store必须采取的一条不变分支边
struct InvariantPrivateStorePredicateEdge {
    //Condition是已在循环预头可用的i1分支条件
    Value *Condition = nullptr;
    //TakeTrue标记路径是否经过该分支的true边
    bool TakeTrue = true;
};
 
//InvariantPrivateStoreCandidate保存一条反复写入及其精确执行谓词
struct InvariantPrivateStoreCandidate {
    //Store指向循环内原始写入
    StoreInst *Store = nullptr;
    //Path按从循环Header到store的顺序保存全部分支边
    std::vector<InvariantPrivateStorePredicateEdge> Path;
};

//InvariantPrivateStoreHeaderState记录回边绕过前缀时需要在汇合块续接的循环状态
struct InvariantPrivateStoreHeaderState {
    //HeaderPhi是原循环头中的状态PHI
    PhiInst *HeaderPhi = nullptr;
    //InitialValue是首次从预头进入循环时选择的状态
    Value *InitialValue = nullptr;
    //BackedgeValue是原回边提供的下一轮状态
    Value *BackedgeValue = nullptr;
    //SuffixUses是在汇合块支配区域中需要改由新PHI供值的use
    std::vector<Use *> SuffixUses;
};

//InvariantPrivateStoreHoistPlan描述一个可原子提交的首轮前缀旁路计划
struct InvariantPrivateStoreHoistPlan {
    //TargetLoop是候选循环
    Loop *TargetLoop = nullptr;
    //PreHeader是循环唯一预头
    BasicBlock *PreHeader = nullptr;
    //Header和Latch界定原始回边
    BasicBlock *Header = nullptr;
    BasicBlock *Latch = nullptr;
    //RegionExit是条件写入前缀的共同汇合块，也是新回边目标
    BasicBlock *RegionExit = nullptr;
    //Root是地址未逃逸的局部聚合根
    AllocaInst *Root = nullptr;
    //ZeroInitializer是原对象的完整非volatile清零
    CallInst *ZeroInitializer = nullptr;
    //Stores保存全部地址互不重叠的候选
    std::vector<InvariantPrivateStoreCandidate> Stores;
    //HeaderStates保存旁路后需要重建的循环携带状态
    std::vector<InvariantPrivateStoreHeaderState> HeaderStates;
    //RegionPredecessors是提交前汇合块已有的全部前驱
    std::vector<BasicBlock *> RegionPredecessors;
};

//InvariantPrivateStoreHoistPass让私有对象上的不变条件写入前缀只在首轮执行
class InvariantPrivateStoreHoistPass
    : public PassDescriptor<InvariantPrivateStoreHoistPass> {
public:
    //在函数中寻找一个合法计划并提交
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    //判断memset是否对给定alloca执行完整非volatile零字节覆盖
    static bool isWholeZeroMemset(CallInst *CI, AllocaInst *AI);
    //判断值是否不依赖循环且在预头终结前可用
    static bool isInvariantAvailableAtPreHeader(
        Value *V, Loop *L, BasicBlock *PreHeader,
        const DominatorTree &DT);
    //递归证明值在本轮前缀中由循环不变的纯表达式形成
    static bool isStablePrefixValue(
        Value *V, Instruction *UsePoint, Loop *L, LoopInfo &LI,
        const DominatorTree &DT,
        std::unordered_set<Value *> &Active,
        std::unordered_set<Value *> &Proven, int &Budget);
    //从Header沿唯一前驱链提取到达基本块的精确不变谓词
    static bool collectPredicatePath(
        BasicBlock *Block, Loop *L, BasicBlock *PreHeader,
        const DominatorTree &DT,
        std::vector<InvariantPrivateStorePredicateEdge> &Path);
    //完整遍历私有alloca的指针use闭包并分类读写与清零
    static bool collectPrivateAggregateUses(
        Value *Pointer, AllocaInst *Root, Loop *L,
        BasicAAResult &AA, std::unordered_set<Value *> &Seen,
        std::vector<LoadInst *> &Loads,
        std::vector<StoreInst *> &Stores,
        CallInst *&ZeroInitializer, int &Budget);
    //为单个循环构建完整合法性和收益计划
    static bool buildPlan(
        Loop *L, LoopInfo &LI, SCEVResult &SE, RangeInfo &RI,
        BasicAAResult &AA, DominatorTree &DT,
        const PostDominatorTree &PDT, const BlockFrequencyInfo &BFI,
        const TargetCostInfo &TargetCost,
        InvariantPrivateStoreHoistPlan &Plan);
    //在全部检查完成后把原回边改接到前缀汇合块并续接循环状态
    static bool applyPlan(InvariantPrivateStoreHoistPlan &Plan);
};
