#include "ir/opt/Local2Global.hpp"

#include "lib/CFG.hpp"
#include "lib/TypeSystem.hpp"

#include <memory>
#include <utility>
#include <vector>

PreservationStatus Local2GlobalPass::run(Module &M, ModuleAnalysisPassManager &MAM) {
	(void)MAM;

	std::vector<std::pair<AllocaInst *, std::shared_ptr<IRType>>> WorkList;

	for (auto &FuncPtr : M.getFunctions()) {
		Function *F = FuncPtr.get();
		if (!F) continue;

		for (auto BBIt = F->begin(); BBIt != F->end(); ++BBIt) {
			BasicBlock *BB = *BBIt;
			if (!BB) continue;

			for (auto IIt = BB->begin(); IIt != BB->end(); ++IIt) {
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
