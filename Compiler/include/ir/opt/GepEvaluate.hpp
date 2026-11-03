// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

//GepEvaluate：折叠掉空索引的GEP替换为基址，拆分GEP
class GepEvaluatePass : public PassDescriptor<GepEvaluatePass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    static bool evaluateGEP(Function &F);
};
 