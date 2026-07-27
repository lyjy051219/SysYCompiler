#include "ir/opt/DCE.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "lib/IRUtils.hpp"

#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

bool hasAcyclicReturningCFG(Function &F) {
	auto *Entry = F.front();
	if (!Entry) return false;
	std::unordered_map<BasicBlock *, unsigned> Color;

	std::function<bool(BasicBlock *)> Visit = [&](BasicBlock *BB) -> bool {
		if (!BB) return false;
		unsigned &C = Color[BB];
		if (C == 1) return false; // reachable cycle may fail to terminate
		if (C == 2) return true;
		C = 1;
		Instruction *Term = BB->getTerminator();
		if (!Term) return false;
		if (dynamic_cast<RetInst *>(Term)) {
			C = 2;
			return true;
		}
		auto *Br = dynamic_cast<BrInst *>(Term);
		if (!Br || Br->getSuccessorCount() == 0) return false;
		for (unsigned I = 0; I < Br->getSuccessorCount(); ++I)
			if (!Visit(Br->getSuccessor(I))) return false;
		C = 2;
		return true;
	};

	return Visit(Entry);
}

bool DCEPass::isGuaranteedRemovableFunction(
	Function &F, FunctionAnalysisPassManager &FAM,
	std::unordered_map<Function *, ReturnProofState> &Memo) {
	auto It = Memo.find(&F);
	if (It != Memo.end()) {
		if (It->second == ReturnProofState::Proven) return true;
		// A recursive call-graph edge is not a will-return proof.
		return false;
	}
	Memo[&F] = ReturnProofState::Visiting;

	const SideEffectInfo &SEI = FAM.getResult<SideEffectAnalysis>(F);
	if (!isNoModRef(getFunctionModRefInfo(SEI)) ||
		!hasAcyclicReturningCFG(F)) {
		Memo[&F] = ReturnProofState::Failed;
		return false;
	}

	for (BasicBlock *BB : F) {
		for (Instruction *I : *BB) {
			// 以整个调用为删除单位时，store 始终可观察。普通非 volatile
			// load 和纯算术若结果无用，则沿用本 IR 的常规 undef/poison
			// 语义，不因为它们可能接收非法输入而额外保留。
			if (dynamic_cast<StoreInst *>(I)) {
				Memo[&F] = ReturnProofState::Failed;
				return false;
			}

			// Keep this scan in lockstep with ordinary/aggressive DCE for future
			// instruction kinds that carry an explicit observable-execution flag
			// (for example a future volatile/atomic operation).
			if (dceInstructionMustBePreserved(I)) {
				Memo[&F] = ReturnProofState::Failed;
				return false;
			}

			auto *CI = dynamic_cast<CallInst *>(I);
			if (!CI) continue;
			if (auto *BIF = CI->getBuiltinCallee()) {
				if (classifyBuiltin(BIF) != FuncAttr::NoModRef) {
					Memo[&F] = ReturnProofState::Failed;
					return false;
				}
				continue;
			}
			Function *Callee = CI->getDirectCallee();
			if (!Callee || !isGuaranteedRemovableFunction(*Callee, FAM, Memo)) {
				Memo[&F] = ReturnProofState::Failed;
				return false;
			}
		}
	}

	Memo[&F] = ReturnProofState::Proven;
	return true;
}

bool hasDefinedConstantShiftAmount(const Instruction *I) {
	if (!I) return false;
	switch (I->instType) {
		case Instruction::SHL:
		case Instruction::LSHR:
		case Instruction::ASHR:
			break;
		default:
			return false;
	}

	auto *BI = dynamic_cast<const BinaryInst *>(I);
	Value *LHS = nullptr;
	Value *RHS = nullptr;
	if (!BI || !BI->getOperands(LHS, RHS) || !LHS || !RHS)
		return false;

	auto Ty = LHS->getIRType();
	if (!Ty || !Ty->isInt()) return false;
	// INTType represents i1 with a byte of storage, so handle it explicitly;
	// all other integer types use their declared storage width.
	const int BitWidth = Ty->isBool() ? 1 : LHS->integerBitWidth(0);
	int32_t Amount = 0;
	return BitWidth > 0 && RHS->tryGetIntConst(Amount) && Amount >= 0 &&
		Amount < BitWidth;
}

bool valueMayDependOnUndef(
	const Value *V, std::size_t MaxVisitedValues) {
	if (!V || MaxVisitedValues == 0) return true;

	std::vector<const Value *> WorkList{V};
	std::unordered_set<const Value *> Seen;
	while (!WorkList.empty()) {
		const Value *Current = WorkList.back();
		WorkList.pop_back();
		if (!Current || Value::isUndef(Current)) return true;
		if (!Seen.insert(Current).second) continue;
		if (Seen.size() > MaxVisitedValues) return true;

		auto *I = dynamic_cast<const Instruction *>(Current);
		if (!I) continue;
		for (std::size_t Index = 0;
			 Index < I->getNumOperands(); ++Index) {
			Value *Operand = I->getOperand(Index);
			if (!Operand || Value::isUndef(Operand)) return true;
			// 常量、参数、函数和基本块不可能在自身内部继续携带 undef；
			// 只沿 SSA 指令定义继续遍历。
			if (dynamic_cast<Instruction *>(Operand))
				WorkList.push_back(Operand);
		}
	}
	return false;
}

bool dceInstructionMustBePreserved(Instruction *I) {
	if (!I) return true;
	// 当前 IR 只有普通（非 volatile、非 atomic）load，以及非 constrained
	// 的算术和转换。它们的非法输入由 undef/poison 语义表示，并非可观察
	// trap，所以结果无人使用时可以删除。store、控制流和 call 由调用者
	// 另行判定；未知的新指令仍默认保留。
	switch (I->instType) {
		case Instruction::RET:
		case Instruction::BR:
		case Instruction::ALLOCA:
		case Instruction::STORE:
		case Instruction::LOAD:
		case Instruction::GEP:
		case Instruction::ADD:
		case Instruction::FADD:
		case Instruction::SUB:
		case Instruction::FSUB:
		case Instruction::MUL:
		case Instruction::FMUL:
		case Instruction::SDIV:
		case Instruction::UDIV:
		case Instruction::FDIV:
		case Instruction::SREM:
		case Instruction::UREM:
		case Instruction::FREM:
		case Instruction::AND:
		case Instruction::OR:
		case Instruction::XOR:
		case Instruction::ICMP:
		case Instruction::FCMP:
		case Instruction::SHL:
		case Instruction::LSHR:
		case Instruction::ASHR:
		case Instruction::SELECT:
		case Instruction::PHI:
		case Instruction::CALL:
		case Instruction::TRUNC:
		case Instruction::ZEXT:
		case Instruction::SEXT:
		case Instruction::FPTOSI:
		case Instruction::SITOFP:
		case Instruction::BITCAST:
			return false;
		case Instruction::BinaryUnknown:
		default:
			return true;
	}
}

bool instructionMayFaultOrProducePoison(const Instruction *I) {
	if (!I) return true;
	// SysY 运行时库可能通过数组参数访问任意用户内存，而普通 load 本身
	// 没有携带“地址已验证”属性，因此移动或投机执行时一律保守处理。
	if (dynamic_cast<const LoadInst *>(I)) return true;

	switch (I->instType) {
		case Instruction::SDIV:
		case Instruction::UDIV:
		case Instruction::FDIV:
		case Instruction::SREM:
		case Instruction::UREM:
		case Instruction::FREM:
		case Instruction::FPTOSI:
			return true;
		case Instruction::SHL:
		case Instruction::LSHR:
		case Instruction::ASHR:
			return !hasDefinedConstantShiftAmount(I);
		default:
			return false;
	}
}

bool dceCallHasSideEffect(CallInst* CI, FunctionAnalysisPassManager& FAM) {
	if (!CI) return true;
	Value* Callee = CI->getCallee();
	if (!Callee) return true;

	if (auto* BIF = dynamic_cast<BuildInFunction*>(Callee)) {
		const std::string& N = BIF->getIdent();
		// SysY 输出函数具有可观察副作用，即使内存分类可能是 readonly。
		if (BuildInFunction::isOutputName(N)) {
			return true;
		}

		// readonly 并不足以允许删除；只有明确的 readnone/no-side-effect
		// 契约才表示该内置调用不可观察。
		return classifyBuiltin(BIF) != FuncAttr::NoModRef;
	}

	if (auto* F = dynamic_cast<Function*>(Callee)) {
		std::unordered_map<Function *, DCEPass::ReturnProofState> Memo;
		return !DCEPass::isGuaranteedRemovableFunction(*F, FAM, Memo);
	}

	return true;
}

bool DCEPass::isDeletableInstruction(
	Instruction* I, FunctionAnalysisPassManager& FAM) {
	if (!I) return false;

	// 1) 仍被其他指令使用：不能删
	if (I->countInstructionUses() > 0) return false;

	// 2) 终结指令：不能删
	if (I->instType == Instruction::RET || I->instType == Instruction::BR) return false;

	// 3) 有副作用指令：不能删
	if (I->instType == Instruction::STORE) {
		return false;
	}
	if (dceInstructionMustBePreserved(I)) return false;

	if (auto* CI = dynamic_cast<CallInst*>(I)) {
		if (dceCallHasSideEffect(CI, FAM)) return false;
	}

	return true;
}

PassResult DCEPass::run(Function &F, FunctionAnalysisPassManager &FAM) {
	bool Changed = false;

	UniqueWorkList<
		Instruction *, WorkListOrder::FIFO> WorkQ;
	std::unordered_set<Instruction*> Erased;

	auto enqueue = [&](Instruction* I) {
		if (!I) return;
		if (Erased.count(I)) return;
		WorkQ.push(I);
	};

	auto tryErase = [&](Instruction* I) -> bool {
		if (!I) return false;
		if (Erased.count(I)) return false;
		if (!isDeletableInstruction(I, FAM)) return false;

		// 先记住操作数定义，然后一次性断开 Use。旧实现把
		// 每个操作数临时替换为 undef；对 call callee/PHI block 这类
		// function/label 操作数会制造没有合法 IR 意义的 undef。
		// clearOperands 直接销毁 Use，同时更准确地更新上游 use count。
		std::vector<Instruction *> OperandDefs;
		OperandDefs.reserve(I->getNumOperands());
		forEachOperand(
			I, [&](size_t, Use *U, Value *Op) {
				if (!U || !Op || Op == I) return; // 自引用直接跳过
				if (auto *DefI = dynamic_cast<Instruction *>(Op))
					OperandDefs.push_back(DefI);
			});

		I->clearOperands();
		for (Instruction *DefI : OperandDefs) {
			if (Erased.count(DefI)) continue;

			// 该操作数若已无指令使用，并且本身可删，则入队
			bool DefIHasInstUser = DefI->countInstructionUses() > 0;

			if (!DefIHasInstUser && isDeletableInstruction(DefI, FAM)) {
				enqueue(DefI);
			}
		}

		delete I;
		Erased.insert(I);
		Changed = true;
		return true;
	};

	// 先拍平当前函数所有指令，避免边遍历边删除导致迭代器失效
	auto AllInsts = collectInstructions(F);

	// 按“种子 + 工作队列”方式做级联 DCE
	for (Instruction* I : AllInsts) {
		if (!I) continue;
		if (Erased.count(I)) continue;
		if (WorkQ.contains(I)) continue;

		tryErase(I);

		while (!WorkQ.empty()) {
			Instruction* Cur = WorkQ.pop();
			tryErase(Cur);
		}
	}

	if (!Changed) return PassResult::unchanged();

	PreservationStatus PA;
	PA.keep<DominantAnalysis>();
	PA.keep<PostDominantAnalysis>();
	PA.keep<LoopAnalysis>();
	PA.discard<BasicAliasAnalysis>();
	PA.discard<SideEffectAnalysis>();
	return PassResult::changed(std::move(PA));
}
