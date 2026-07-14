#pragma once

#include "ir/opt/NewPassManager.hpp"

#include <unordered_map>

class Function;

// Full loop unswitching for loop-invariant branch conditions.
//
// The pass deliberately remembers which form was selected for a function.
// buildLoopPipeline is itself a fixed point, and without this state a later
// round could first perform trivial unswitching and then clone the same
// function for non-trivial unswitching (or repeatedly double its loops).
class LoopUnswitchPass : public PassDescriptor<LoopUnswitchPass> {
public:
    PassResult run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    enum class SelectedKind {
        None,
        Trivial,
        NonTrivial
    };

    struct FunctionState {
        SelectedKind Kind = SelectedKind::None;
        unsigned FullCloneCount = 0;
    };

    std::unordered_map<Function *, FunctionState> States;
};
