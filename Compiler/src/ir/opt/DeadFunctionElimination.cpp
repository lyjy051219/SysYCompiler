#include "ir/opt/DeadFunctionElimination.hpp"

#include <unordered_set>
#include <vector>

PreservationStatus DeadFunctionEliminationPass::run(
	Module &M,
	ModuleAnalysisPassManager &MAM)
{
	(void)MAM;

	auto &Functions = M.getFunctions();
	if (Functions.empty()) return PreservationStatus::all();

	std::unordered_set<Function *> InModule;
	InModule.reserve(Functions.size() * 2 + 1);
	for (auto &FPtr : Functions) {
		if (FPtr) InModule.insert(FPtr.get());
	}

	std::unordered_set<Function *> Live;
	Live.reserve(Functions.size() * 2 + 1);
	std::vector<Function *> WorkList;
	WorkList.reserve(Functions.size());

	auto markLive = [&](Function *F) {
		if (!F || !InModule.count(F)) return;
		if (Live.insert(F).second) {
			WorkList.push_back(F);
		}
	};

	// ---------- Mark roots ----------
	// main、运行库/外部 ABI 名称、以及不在任何函数体内的函数引用都作为根。
	for (auto &FPtr : Functions) {
		Function *F = FPtr.get();
		if (!F) continue;
			if (F->hasReservedSignature()) {
			markLive(F);
		}
	}

	for (auto &FPtr : Functions) {
		Function *F = FPtr.get();
		if (!F) continue;

		for (auto UIt = F->getUseList().begin(); UIt != F->getUseList().end(); ++UIt) {
			Use *U = *UIt;
			if (!U) continue;

			auto *I = dynamic_cast<Instruction *>(U->getUser());
			if (!I) {
				markLive(F);
				continue;
			}

			// 如果函数值被挂在无父函数的游离指令上，保守当作外部可见。
			if (!I->getParentFunction()) {
				markLive(F);
			}
		}
	}

	// ---------- Mark transitive callees / escaped functions ----------
	// 每个 live 函数最多扫描一次；扫描时只看 CFG 可达块，复杂度为 O(可达 IR 大小)。
	std::unordered_set<BasicBlock *> ReachableBlocks;
	while (!WorkList.empty()) {
		Function *F = WorkList.back();
		WorkList.pop_back();
		if (!F) continue;

		F->collectReachableBlocks(ReachableBlocks);

		for (BasicBlock *BB : *F) {
			if (!BB || !ReachableBlocks.count(BB)) continue;

			for (Instruction *I : *BB) {
				if (!I) continue;

				for (size_t OperandIndex = 0;
					 OperandIndex < I->getNumOperands(); ++OperandIndex) {
					auto *Target = dynamic_cast<Function *>(
						I->getOperand(OperandIndex));
					if (!Target || !InModule.count(Target)) continue;

					// 直接调用会让 callee 可达；函数值作为普通操作数出现时，
					// 说明地址在 live 代码里逃逸，也必须保守保留。
					markLive(Target);
				}
			}
		}
	}

	std::unordered_set<Function *> Dead;
	Dead.reserve(Functions.size());
	for (auto &FPtr : Functions) {
		Function *F = FPtr.get();
		if (!F) continue;
		if (!Live.count(F)) {
			Dead.insert(F);
		}
	}

	if (Dead.empty()) return PreservationStatus::all();

	// ---------- Sweep ----------
	// DFE 不修改任何存活函数的 CFG，因此不能清空整个 FAM。只在函数对象
	// 仍有效时移除待删函数的缓存，既避免悬空 Function* key，也保留存活
	// 函数的分析结果，防止无关分析被重建并扰动后续流水线。
	for (Function *F : Dead) {
		FAM.invalidate(*F, PreservationStatus::none());
	}

	// 先统一断引用，再统一 erase。这样删除互相递归的死函数 SCC 也是线性的，
	// 不会在析构某个 Function 时沿 use-list 反复触发递归删除。
	for (Function *F : Dead) {
		F->dropAllReferences();
	}

	for (auto It = Functions.begin(); It != Functions.end(); ) {
		Function *F = It->get();
		if (F && Dead.count(F)) {
			It = Functions.erase(It);
		} else {
			++It;
		}
	}

	return PreservationStatus::none();
}
