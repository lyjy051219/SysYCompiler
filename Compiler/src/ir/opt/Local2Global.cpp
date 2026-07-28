#include "ir/opt/Local2Global.hpp"

#include "lib/CFG.hpp"
#include "lib/IRUtils.hpp"
#include "lib/TypeSystem.hpp"

#include <memory>
#include <utility>
#include <vector>

// main 通常由运行时只调用一次，但语法并没有阻止 main 直接
// 或间接递归。一旦递归，把 alloca 换成全局对象就会让不同栈帧
// 共享同一份存储。
bool Local2GlobalPass::isRecursiveFrom(
	Function *F, const FunctionCallGraph &CallGraph) {
	if (!F) return false;

	auto RootIt = CallGraph.find(F);
	if (RootIt == CallGraph.end()) return false;
	return isReachableFromRootsCustomSucc<Function>(
		RootIt->second, F,
		[&CallGraph](Function *Current) -> std::vector<Function *> {
			auto It = CallGraph.find(Current);
			return It == CallGraph.end() ? std::vector<Function *>{}
			                            : It->second;
		});
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
	const bool MainMayReachTiming = Main->mayReachTimingCall();

	for (AllocaInst *AI : collectInstructions<AllocaInst>(*EntryBB)) {
		std::shared_ptr<IRType> ElemTy =
			IRType::getPointeeType(AI->getIRType());
		if (!ElemTy) continue;

		// 全局化会把栈分配移到程序启动阶段。若入口到 alloca 之间，或
		// 从该 alloca 绕回自身的循环路径上存在计时边界，就会把原本
		// 计入测量区间的分配移出区间，因此保留原局部对象。
		if (MainMayReachTiming &&
			(Main->hasTimingBarrierOnPath(EntryBB, nullptr, AI) ||
			 Main->hasTimingBarrierOnPath(AI, AI))) {
			continue;
		}

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
