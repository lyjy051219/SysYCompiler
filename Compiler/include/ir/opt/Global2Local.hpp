// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include <cstddef>
#include <memory>
#include <vector>

//候选Info
struct Global2LocalCandidateInfo {
	//待转换的全局变量
	Variable *GV = nullptr;
	//该全局变量的类型
	std::shared_ptr<IRType> ElemTy;
	//需要被替换的main内Use
	std::vector<Use *> MainUses;
};
class Global2LocalPass : public PassDescriptor<Global2LocalPass> {
public:
	explicit Global2LocalPass(bool RelaxScalarUses = false);
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);
 
private:
	//阈值
	static constexpr size_t Threshold = 32;
	//记录是否放宽标量全局的直接load/store使用限制
	bool RelaxScalarUses = false;
	//检查标量全局在main中的使用是否为安全直接访存
	static bool isSafeScalarUseInMain(Use *U, Variable *GV);
};
