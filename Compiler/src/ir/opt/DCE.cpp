#include "ir/opt/DCE.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"

#include <queue>
#include <unordered_set>
#include <vector>


bool dceCallHasSideEffect(CallInst* CI, FunctionAnalysisPassManager& FAM) {
	if (!CI) return true;
	auto& UL = CI->getUserList();
	if (UL.empty() || !UL[0]) return true;

	Value* Callee = UL[0]->getValue();
	if (!Callee) return true;

	if (auto* BIF = dynamic_cast<BuildInFunction*>(Callee)) {
		const std::string& N = BIF->getIdent();
		// I/O builtins have observable side effects even if they are memory-readonly.
		if (N == "putint" || N == "putch" || N == "putfloat" ||
			N == "putarray" || N == "putfarray" || N == "putf") {
			return true;
		}

		FuncAttr Attr = classifyBuiltin(BIF);
		if (Attr == FuncAttr::NoModRef || Attr == FuncAttr::MemReadOnly) return false;
		return true;
	}

	if (auto* F = dynamic_cast<Function*>(Callee)) {
		const SideEffectInfo& SEI = FAM.getResult<SideEffectAnalysis>(*F);
		ModRefInfo MRI = getFunctionModRefInfo(SEI);
		return isModSet(MRI);
	}

	return true;
}

static bool isDeletableInst(Instruction* I, FunctionAnalysisPassManager& FAM) {
	if (!I) return false;

	// 1) 仍被其他指令使用：不能删
	for (auto It = I->getUseList().begin(); It != I->getUseList().end(); ++It) {
		Use* U = *It;
		if (!U || U->getValue() != I) continue;
		if (dynamic_cast<Instruction*>(U->getUser())) return false;
	}

	// 2) 终结指令：不能删
	if (I->instType == Instruction::RET || I->instType == Instruction::BR) return false;

	// 3) 有副作用指令：不能删
	if (I->instType == Instruction::STORE) {
		return false;
	}

	if (auto* CI = dynamic_cast<CallInst*>(I)) {
		if (dceCallHasSideEffect(CI, FAM)) return false;
	}

	return true;
}

PreservationStatus DCEPass::run(Function &F, FunctionAnalysisPassManager &FAM) {
	bool Changed = false;

	std::queue<Instruction*> WorkQ;
	std::unordered_set<Instruction*> InWorkQ;
	std::unordered_set<Instruction*> Erased;

	auto enqueue = [&](Instruction* I) {
		if (!I) return;
		if (Erased.count(I)) return;
		if (InWorkQ.insert(I).second) WorkQ.push(I);
	};

	auto tryErase = [&](Instruction* I) -> bool {
		if (!I) return false;
		if (Erased.count(I)) return false;
		if (!isDeletableInst(I, FAM)) return false;

		// 遍历并断开 I 的所有操作数，再尝试把可能新变死的定义指令加入队列
		auto& Operands = I->getUserList();
		for (auto& UsePtr : Operands) {
			Use* U = UsePtr.get();
			if (!U) continue;

			Value* Op = U->getValue();
			if (!Op) continue;
			if (Op == I) continue; // 自引用直接跳过

			Instruction* DefI = dynamic_cast<Instruction*>(Op);

			// 将当前操作数 use 关系切断（改为 undef），便于后续判断 DefI 是否变死
			if (Op->getIRType() && Op->getIRType()->getEnum() != IR_VOID) {
				U->replaceUseeWith(UndefValue::NewUndefValue(Op->getIRType()));
			}

			if (!DefI) continue;
			if (Erased.count(DefI)) continue;

			// 该操作数若已无指令使用，并且本身可删，则入队
			bool DefIHasInstUser = false;
			for (auto It = DefI->getUseList().begin(); It != DefI->getUseList().end(); ++It) {
				Use* DU = *It;
				if (!DU || DU->getValue() != DefI) continue;
				if (dynamic_cast<Instruction*>(DU->getUser())) {
					DefIHasInstUser = true;
					break;
				}
			}

			if (!DefIHasInstUser && isDeletableInst(DefI, FAM)) {
				enqueue(DefI);
			}
		}

		delete I;
		Erased.insert(I);
		Changed = true;
		return true;
	};

	// 先拍平当前函数所有指令，避免边遍历边删除导致迭代器失效
	std::vector<Instruction*> AllInsts;
	for (auto BBIt = F.begin(); BBIt != F.end(); ++BBIt) {
		BasicBlock* BB = *BBIt;
		for (auto IIt = BB->begin(); IIt != BB->end(); ++IIt) {
			AllInsts.push_back(*IIt);
		}
	}

	// 按“种子 + 工作队列”方式做级联 DCE
	for (Instruction* I : AllInsts) {
		if (!I) continue;
		if (Erased.count(I)) continue;
		if (InWorkQ.count(I)) continue;

		tryErase(I);

		while (!WorkQ.empty()) {
			Instruction* Cur = WorkQ.front();
			WorkQ.pop();
			InWorkQ.erase(Cur);
			tryErase(Cur);
		}
	}

	if (!Changed) return PreservationStatus::all();

	PreservationStatus PA;
	PA.keep<DominantAnalysis>();
	PA.keep<PostDominantAnalysis>();
	PA.keep<LoopAnalysis>();
	PA.discard<BasicAliasAnalysis>();
	PA.discard<SideEffectAnalysis>();
	return PA;
}
