#pragma once

#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/SCEV.hpp"

#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Materialize the small, loop-invariant SCEV language used by this project.
//
// This is deliberately a separate utility instead of transform-specific
// recursive builders.  A preflight walk checks dominance, expression shape,
// division safety and a hard cost budget before any instruction is inserted,
// so callers never leave a half-expanded expression in the IR.
class SCEVExpander {
public:
    SCEVExpander(const Loop *Scope, BasicBlock *InsertBlock,
                 const DominatorTree &DT, unsigned Budget = 16);

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
