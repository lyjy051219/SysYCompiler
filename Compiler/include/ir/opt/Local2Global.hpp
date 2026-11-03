// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include <cstddef>

//Local2Global：在非递归main中把大型入口alloca转换为同型全局对象
class Local2GlobalPass : public PassDescriptor<Local2GlobalPass> {
public:
	PreservationStatus run(Module &M, ModuleAnalysisPassManager &MAM);
private:
	//阈值
	static constexpr size_t Threshold = 1024;
};
 