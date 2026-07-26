#include "ir/opt/DeadFunctionElimination.hpp"

#include <unordered_set>
#include <vector>

DeadFunctionEliminationPass::DeadFunctionEliminationPass(
	FunctionAnalysisPassManager &FAM)
	: FAM(FAM) {}

PreservationStatus DeadFunctionEliminationPass::run(
	Module &M,
	ModuleAnalysisPassManager &MAM)
{
	(void)MAM;

	auto &Functions = M.getFunctions();
	if (Functions.empty()) return PreservationStatus::all();
    //1.Live集合：存活函数集合，WorkList：待处理函数队列
	std::unordered_set<Function *> Live;
    //提前扩容，避免扩容和重复哈希，影响性能
	Live.reserve(Functions.size() * 2 + 1);
	std::vector<Function *> WorkList;
	WorkList.reserve(Functions.size());

	auto markLive = [&](Function *F) {
		if (F && Live.insert(F).second) WorkList.push_back(F);
	};

	// 2.直接从main函数开始，main函数一定live
	markLive(M.getMainFunction());

	// 当前生成的 IR 中，Function 只作为 CallInst 的直接被调目标出现，
	// 3.从Live函数出发，遍历其可达基本块中的CallInst，标记被调用的函数为Live
	std::unordered_set<BasicBlock *> ReachableBlocks;
	while (!WorkList.empty()) {
		Function *F = WorkList.back();
		WorkList.pop_back();

		F->collectReachableBlocks(ReachableBlocks);
		for (BasicBlock *BB : *F) {
			if (!BB || !ReachableBlocks.count(BB)) continue;
			for (Instruction *I : *BB) {
				auto *Call = dynamic_cast<CallInst *>(I);
				if (Call) markLive(Call->getDirectCallee());
			}
		}
	}

    //4.收集Dead函数：不在Live集合中的函数
	std::vector<Function *> Dead;
	Dead.reserve(Functions.size());
	for (auto &FPtr : Functions) {
		Function *F = FPtr.get();
		if (F && !Live.count(F)) Dead.push_back(F);
	}

	if (Dead.empty()) return PreservationStatus::all();

	//5.删除Dead函数：先失效对应的FAM缓存，再断开操作数引用，最后删除
	for (Function *F : Dead) {
		FAM.invalidate(*F, PreservationStatus::none());
	}

	// 先统一断开操作数引用，再删除函数，可安全处理互相递归的死函数集合。
	for (Function *F : Dead) {
		F->dropAllReferences();
	}

	for (auto It = Functions.begin(); It != Functions.end(); ) {
		Function *F = It->get();
		if (F && !Live.count(F)) {
			It = Functions.erase(It);
		} else {
			++It;
		}
	}

	return PreservationStatus::none();
}
