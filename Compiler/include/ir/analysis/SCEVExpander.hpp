// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/SCEV.hpp"

#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <vector>
 
//SCEVExpander将循环不变的可支持SCEV表达式物化到指定基本块中
class SCEVExpander {
public:
    //构造绑定循环作用域、插入块、支配树和指令预算的表达式展开器
    SCEVExpander(const Loop *Scope, BasicBlock *InsertBlock,
                 const DominatorTree &DT, unsigned Budget = 16);

    //判断给定表达式是否能够在当前插入点和剩余预算内展开
    bool canExpand(const SCEVExpr *E) const;
    //估算展开给定表达式需要新增的IR指令数
    int estimateCost(const SCEVExpr *E) const;
    //事务式展开给定表达式并在失败时回滚本次新增指令
    Value *expand(const SCEVExpr *E);

    //返回当前展开器已经成功插入的全部指令
    const std::vector<Instruction *> &createdInstructions() const;

private:
    //限制不可展开引用该循环的递推表达式
    const Loop *Scope = nullptr;
    //接收新指令的目标基本块
    BasicBlock *InsertBlock = nullptr;
    //用于验证原始IR值在插入点可用性的支配树
    const DominatorTree &DT;
    //当前展开器最多允许插入的指令数
    unsigned Budget = 0;
    //当前展开器已经插入的指令数
    unsigned Inserted = 0;
    //缓存每个SCEV子表达式已经物化得到的IR值
    std::unordered_map<const SCEVExpr *, Value *> Cache;
    //按照插入顺序记录当前展开器创建的指令
    std::vector<Instruction *> Created;

    //判断给定原始IR值是否在当前插入点可用
    bool isAvailable(Value *V) const;
    //递归验证表达式形状并估算实际新增指令数
    int estimateCostImpl(const SCEVExpr *E,
                         std::unordered_set<const SCEVExpr *> &Active) const;
    //递归物化给定SCEV表达式并复用已经展开的子表达式
    Value *expandImpl(const SCEVExpr *E);
    //将SCEV整数常量转换为对应类型的IR常量
    Value *makeConstant(const SCEVExpr *E) const;
    //在目标块终结指令前插入新指令并更新预算记录
    Instruction *insert(Instruction *I);
};
