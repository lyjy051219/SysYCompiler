#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstdint>
#include <vector>

class DominatorNode;
class DominatorTree;
class RangeInfo;

// 规范约束表示 val op (bound + boundOffset)，常量边界同时缓存在 boundVal 中。
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

// 支配树 DFS frame 记录进入节点前的约束栈深度，用于兄弟子树间恢复路径事实。
struct ConstraintElimStackFrame {
    DominatorNode *node = nullptr;
    int constraintsBefore = 0;
};

struct ConstraintElimFoldCandidate {
    BrInst *branch = nullptr;
    BinaryInst *condition = nullptr;
    bool result = false;
};

 
// 流程：1. 在无回绕证明下规范比较与常量偏移；2. 沿支配树维护路径约束栈；
// 3. 结合范围上下界做直接/一跳蕴含和矛盾判断；4. 将已证明分支条件折为常量。
class ConstraintEliminationPass : public PassDescriptor<ConstraintEliminationPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    static bool isKnownNoWrapAddSub(
        BinaryInst *Inst, BasicBlock *ContextBlock, RangeInfo &RI,
        const DominatorTree &DT);
    static ConstraintElimOffsetExpr decomposeOffset(
        Value *V, BasicBlock *ContextBlock, RangeInfo &RI,
        const DominatorTree &DT, int Depth = 2);
    static bool rewriteOffsetConstraint(
        Value *LHS, Value *RHS, BinaryInst::Operation Op,
        BasicBlock *ContextBlock, RangeInfo &RI, const DominatorTree &DT,
        ConstraintElimConstraint &Out);
    static bool extractConstraint(
        BinaryInst *Cmp, bool Negate, RangeInfo &RI,
        const DominatorTree &DT, ConstraintElimConstraint &Out);
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
