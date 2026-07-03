#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

// Simplify small, non-escaping local aggregates:
//   1. forward constant elements copied from a constant global into same-block loads;
//   2. expand read-only small constant arrays into select chains for dynamic loads;
//   3. scalarize constant-initialized, constant-indexed aggregate allocas
//      through transparent aggregate/scalar GEPs so Mem2Reg can promote them;
//   4. delete aggregate allocas which are only written and never observed.
class LocalAggregateSimplifyPass
    : public PassDescriptor<LocalAggregateSimplifyPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);
};
