// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

// UnifyFuncExitsPass: 将函数内多个返回出口统一为单一出口块
class UnifyFuncExitsPass : public PassDescriptor<UnifyFuncExitsPass> {
public:
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);
};
 