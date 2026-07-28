#include "ir/opt/Local2Global.hpp"

#include "lib/CFG.hpp"
#include "lib/IRUtils.hpp"
#include "lib/TypeSystem.hpp"

#include <memory>
#include <utility>
#include <vector>

bool Local2GlobalPass::isRecursiveFrom(
	Function *F, const FunctionCallGraph &CallGraph) {
	// 1.1 从 main 的直接调用边出发检查能否回到自身；递归会让静态对象破坏每次调用
	// 独立分配的语义，因此整个提升流程必须停止。
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

	// 1. 建立调用图并排除递归 main，确保入口 alloca 的生命周期等同于整次程序执行。
	std::vector<std::pair<AllocaInst *, std::shared_ptr<IRType>>> WorkList;

	Function *Main = M.getMainFunction();
	if (!Main) return PreservationStatus::all();

	FunctionCallGraph CallGraph = M.buildDirectCallGraph();
	if (isRecursiveFrom(Main, CallGraph)) {
		return PreservationStatus::all();
	}

	// 2. 只考察 main 入口块中的大对象，并预先判断函数是否可能到达计时屏障。
	BasicBlock *EntryBB = Main->front();
	if (!EntryBB) return PreservationStatus::all();
	const bool MainMayReachTiming = Main->mayReachTimingCall();

	for (AllocaInst *AI : collectInstructions<AllocaInst>(*EntryBB)) {
		std::shared_ptr<IRType> ElemTy =
			IRType::getPointeeType(AI->getIRType());
		if (!ElemTy) continue;

		// 2.1 若 alloca 的动态执行位置可能跨越计时调用，则保留栈分配，避免改变
		// 计时区间内的初始化/分配成本；其余对象按目标阈值进入工作表。
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

	// 3. 为每个候选创建同型全局量，先替换全部 use，再删除原 alloca。
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
