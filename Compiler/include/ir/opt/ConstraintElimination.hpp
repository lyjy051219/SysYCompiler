#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

// 约束消除：沿支配树传播分支条件约束，消除被蕴含的冗余比较
class ConstraintEliminationPass : public PassDescriptor<ConstraintEliminationPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);
};
