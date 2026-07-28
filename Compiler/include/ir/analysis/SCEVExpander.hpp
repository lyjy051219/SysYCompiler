#pragma once

#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/SCEV.hpp"

#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// SCEVExpander 把循环不变的有限 SCEV 子语言物化到指定块终结指令之前。
// 流程先检查支配、表达式形状和成本预算，再事务式插入并记录新指令。
class SCEVExpander {
public:
    SCEVExpander(const Loop *Scope, BasicBlock *InsertBlock,
                 const DominatorTree &DT, unsigned Budget = 16);

    // canExpand/estimateCost 是无副作用预检；expand 失败时不会留下部分 IR。
    bool canExpand(const SCEVExpr *E) const;
    int estimateCost(const SCEVExpr *E) const;
    Value *expand(const SCEVExpr *E);

    const std::vector<Instruction *> &createdInstructions() const;

private:
    const Loop *Scope = nullptr;
    BasicBlock *InsertBlock = nullptr;
    const DominatorTree &DT;
    unsigned Budget = 0;
    unsigned Inserted = 0;
    std::unordered_map<const SCEVExpr *, Value *> Cache;
    std::vector<Instruction *> Created;

    bool isAvailable(Value *V) const;
    int estimateCostImpl(const SCEVExpr *E,
                         std::unordered_set<const SCEVExpr *> &Active) const;
    Value *expandImpl(const SCEVExpr *E);
    Value *makeConstant(const SCEVExpr *E) const;
    Instruction *insert(Instruction *I);
};
