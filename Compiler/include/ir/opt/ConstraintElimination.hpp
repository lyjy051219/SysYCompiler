#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstdint>
#include <vector>

class DominatorNode;

struct ConstraintElimConstraint {
    Value *val = nullptr;
    BinaryInst::Operation op = BinaryInst::E;
    Value *bound = nullptr;
    bool boundIsConst = false;
    int64_t boundVal = 0;
    int64_t boundOffset = 0;
};

struct ConstraintElimOffsetExpr {
    Value *Base = nullptr;
    int64_t Offset = 0;
    bool Valid = false;
};

struct ConstraintElimStackFrame {
    DominatorNode *node = nullptr;
    int constraintsBefore = 0;
};

struct ConstraintElimFoldCandidate {
    BinaryInst *cmp = nullptr;
    bool result = false;
};

// 约束消除：沿支配树传播分支条件约束，消除被蕴含的冗余比较
class ConstraintEliminationPass : public PassDescriptor<ConstraintEliminationPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    static ConstraintElimOffsetExpr decomposeOffset(Value *V, int Depth = 2);
    static bool rewriteOffsetConstraint(
        Value *LHS, Value *RHS, BinaryInst::Operation Op,
        ConstraintElimConstraint &Out);
    static bool extractConstraint(
        BinaryInst *Cmp, bool Negate, ConstraintElimConstraint &Out);
    static bool comparableBoundValues(
        const ConstraintElimConstraint &A,
        const ConstraintElimConstraint &B, int64_t &AV, int64_t &BV);
    static bool implies(
        const ConstraintElimConstraint &Known,
        const ConstraintElimConstraint &Target);
    static bool isImplied(
        const std::vector<ConstraintElimConstraint> &Stack,
        const ConstraintElimConstraint &Target);
    static bool impliesTransitive(
        const std::vector<ConstraintElimConstraint> &Stack,
        const ConstraintElimConstraint &Target);
    static bool isImpliedChained(
        const std::vector<ConstraintElimConstraint> &Stack,
        const ConstraintElimConstraint &Target);
    static bool isContradicted(
        const std::vector<ConstraintElimConstraint> &Stack,
        const ConstraintElimConstraint &Target);
};
