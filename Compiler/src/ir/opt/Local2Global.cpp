#include "ir/opt/Local2Global.hpp"

#include "lib/CFG.hpp"
#include "lib/TypeSystem.hpp"

#include <memory>
#include <unordered_set>
#include <utility>
#include <vector>

// main 通常由运行时只调用一次，但语法并没有阻止 main 直接
// 或间接递归。一旦递归，把 alloca 换成全局对象就会让不同栈帧
// 共享同一份存储。
bool Local2GlobalPass::isRecursiveFrom(
	Function *F, const FunctionCallGraph &CallGraph) {
	if (!F) return false;

	std::vector<Function *> WorkList;
	std::unordered_set<Function *> Visited;
	auto RootIt = CallGraph.find(F);
	if (RootIt == CallGraph.end()) return false;
	for (Function *Callee : RootIt->second) {
		if (Callee) WorkList.push_back(Callee);
	}

	while (!WorkList.empty()) {
		Function *Current = WorkList.back();
		WorkList.pop_back();
		if (Current == F) return true;
		if (!Current || !Visited.insert(Current).second) continue;

		auto It = CallGraph.find(Current);
		if (It == CallGraph.end()) continue;
		for (Function *Callee : It->second) {
			if (Callee) WorkList.push_back(Callee);
		}
	}
	return false;
}

PreservationStatus Local2GlobalPass::run(Module &M, ModuleAnalysisPassManager &MAM) {
	(void)MAM;

	std::vector<std::pair<AllocaInst *, std::shared_ptr<IRType>>> WorkList;

	Function *Main = M.getMainFunction();
	if (!Main) return PreservationStatus::all();

	FunctionCallGraph CallGraph = M.buildDirectCallGraph();
	if (isRecursiveFrom(Main, CallGraph)) {
		return PreservationStatus::all();
	}

	// 只有程序入口 main 的生存期能在现有 IR 信息下可靠地
	// 视为单次执行。普通函数即使非递归，也可能在循环中或多个
	// 调用点重入，不能只凭“非递归”就全局化其局部对象。
	// 同时只处理 entry block alloca，避免改变循环/分支内动态
	// alloca 每次执行都创建新对象的语义。
	BasicBlock *EntryBB = Main->front();
	if (!EntryBB) return PreservationStatus::all();

	for (auto IIt = EntryBB->begin(); IIt != EntryBB->end(); ++IIt) {
		auto *AI = dynamic_cast<AllocaInst *>(*IIt);
		if (!AI) continue;

		auto PtrTy = std::dynamic_pointer_cast<POINTERType>(AI->getIRType());
		if (!PtrTy) continue;

		std::shared_ptr<IRType> ElemTy = PtrTy->getPointerType();
		if (!ElemTy) continue;

		if (ElemTy->getSize() > RISCVThreshold) {
			WorkList.emplace_back(AI, ElemTy);
		}
	}

	if (WorkList.empty()) return PreservationStatus::all();

	for (auto &Entry : WorkList) {
		AllocaInst *AI = Entry.first;
		std::shared_ptr<IRType> ElemTy = Entry.second;
		if (!AI || !ElemTy) continue;

		// 复用 alloca 的标识符名创建全局变量。
		auto *GV = new Variable(Variable::GlobalVariable, ElemTy, AI->getIdent());
		AI->replaceAllUsesWith(GV);
		delete AI;
	}

	return PreservationStatus::none();
}
