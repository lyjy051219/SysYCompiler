#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstdint>
#include <vector>

class DominatorNode;
class DominatorTree;
class RangeInfo;

//规范约束表示 val op (bound + boundOffset)，常量边界同时缓存在 boundVal 中。
struct ConstraintElimConstraint {
    //记录规范约束左侧被限制的值。
    Value *val = nullptr;
    //记录左值与边界之间的规范比较谓词。
    BinaryInst::Operation op = BinaryInst::E;
    //记录常量或符号形式的边界基值。
    Value *bound = nullptr;
    //标记边界是否已被解析为整数常量。
    bool boundIsConst = false;
    //缓存常量边界值以避免依赖临时常量对象身份。
    int64_t boundVal = 0;
    //记录附加在边界基值上的常量偏移。
    int64_t boundOffset = 0;
};

//ConstraintElimOffsetExpr表示在无回绕证明下分解出的Base+Offset表达式。
struct ConstraintElimOffsetExpr {
    //记录剥离常量加减后的基值。
    Value *Base = nullptr;
    //累计表达式相对基值的有符号常量偏移。
    int64_t Offset = 0;
    //标记偏移分解结果是否有效。
    bool Valid = false;
};

//支配树 DFS frame 记录进入节点前的约束栈深度，用于兄弟子树间恢复路径事实。
struct ConstraintElimStackFrame {
    //记录当前显式DFS帧对应的支配树节点。
    DominatorNode *node = nullptr;
    //记录进入该节点前路径约束栈的深度。
    int constraintsBefore = 0;
};

//ConstraintElimFoldCandidate保存遍历结束后需要统一提交的分支折叠。
struct ConstraintElimFoldCandidate {
    //记录待改写条件的分支指令。
    BrInst *branch = nullptr;
    //记录收集候选时分支引用的原比较指令。
    BinaryInst *condition = nullptr;
    //记录证明得到的恒定布尔结果。
    bool result = false;
};

//流程：1. 在无回绕证明下规范比较与常量偏移；2. 沿支配树维护路径约束栈；
//3. 结合范围上下界做直接/一跳蕴含和矛盾判断；4. 将已证明分支条件折为常量。
class ConstraintEliminationPass : public PassDescriptor<ConstraintEliminationPass> {
public:
    //沿支配树传播路径约束并折叠能够证明恒真或恒假的条件分支。
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    //利用程序点范围证明i32加减在所有可能输入下均不回绕。
    static bool isKnownNoWrapAddSub(
        BinaryInst *Inst, BasicBlock *ContextBlock, RangeInfo &RI,
        const DominatorTree &DT);
    //有界递归地把无回绕常量加减表达式分解为Base+Offset。
    static ConstraintElimOffsetExpr decomposeOffset(
        Value *V, BasicBlock *ContextBlock, RangeInfo &RI,
        const DominatorTree &DT, int Depth = 2);
    //把比较两侧的偏移吸收到谓词或边界中并生成规范约束。
    static bool rewriteOffsetConstraint(
        Value *LHS, Value *RHS, BinaryInst::Operation Op,
        BasicBlock *ContextBlock, RangeInfo &RI, const DominatorTree &DT,
        ConstraintElimConstraint &Out);
    //从比较及所选真假边提取无回绕的规范路径约束。
    static bool extractConstraint(
        BinaryInst *Cmp, bool Negate, RangeInfo &RI,
        const DominatorTree &DT, ConstraintElimConstraint &Out);
    //将两个常量或同基值边界换算为可直接比较的整数。
    static bool comparableBoundValues(
        const ConstraintElimConstraint &A,
        const ConstraintElimConstraint &B, int64_t &AV, int64_t &BV);
    //判断一条已知约束是否直接蕴含目标约束。
    static bool implies(
        const ConstraintElimConstraint &Known,
        const ConstraintElimConstraint &Target);
    //检查路径约束栈中是否存在目标约束的直接证明。
    static bool isImplied(
        const std::vector<ConstraintElimConstraint> &Stack,
        const ConstraintElimConstraint &Target);
    //通过一个中间值组合一跳序关系并尝试证明目标约束。
    static bool impliesTransitive(
        const std::vector<ConstraintElimConstraint> &Stack,
        const ConstraintElimConstraint &Target);
    //依次尝试直接蕴含和一跳传递蕴含。
    static bool isImpliedChained(
        const std::vector<ConstraintElimConstraint> &Stack,
        const ConstraintElimConstraint &Target);
    //通过证明目标约束的否定来判断其是否与当前路径矛盾。
    static bool isContradicted(
        const std::vector<ConstraintElimConstraint> &Stack,
        const ConstraintElimConstraint &Target);
};
