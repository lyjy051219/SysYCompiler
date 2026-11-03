// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#include "ir/opt/Local2Global.hpp"
#include "lib/CFG.hpp"
#include "lib/IRUtils.hpp"
#include "lib/TypeSystem.hpp"
#include <memory>
#include <utility>
#include <vector>
//在非递归main中把大型入口alloca转换为同型全局对象
PreservationStatus Local2GlobalPass::run(Module &M, ModuleAnalysisPassManager &MAM) {
	(void)MAM;
	//1.排除递归main
	std::vector<std::pair<AllocaInst *, std::shared_ptr<IRType>>> WorkList;
	Function *Main = M.getMainFunction();
	if (!Main) return PreservationStatus::all();
	FunctionCallGraph CallGraph = M.buildDirectCallGraph();
	if (Module::isFunctionRecursive(Main, CallGraph)) {
		return PreservationStatus::all();
	}
	//2.从main从入口alloca中筛选大对象
	BasicBlock *EntryBB = Main->front();
	if (!EntryBB) return PreservationStatus::all();
	for (AllocaInst *AI : collectInstructions<AllocaInst>(*EntryBB)) {
		std::shared_ptr<IRType> ElemTy =
			IRType::getPointeeType(AI->getIRType());
		if (!ElemTy) continue;
		if (ElemTy->getSize() > Threshold) {
			WorkList.emplace_back(AI, ElemTy);
		}
	} 
	if (WorkList.empty()) return PreservationStatus::all();
	//3.alloca->全局变量
	for (auto &Entry : WorkList) {
		AllocaInst *AI = Entry.first;
		std::shared_ptr<IRType> ElemTy = Entry.second;
		if (!AI || !ElemTy) continue;
		auto *GV = new Variable(Variable::GlobalVariable, ElemTy, AI->getIdent());
		AI->replaceAllUsesWith(GV);
		delete AI;
	}
	return PreservationStatus::none();
}
