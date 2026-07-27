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
			// Stores are observable even when their destination is local to the
			// callee because the call boundary is the unit being removed.  An
			// ordinary non-volatile load, however, has no observable result once
			// all of its users are dead; an invalid address already makes the
			// source program undefined in this IR.
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

bool dceInstructionMustBePreserved(Instruction *I) {
	if (!I) return true;
	// The current IR has only ordinary (non-volatile, non-atomic) loads and
	// unconstrained arithmetic/conversions.  Their invalid-input behaviour is
	// represented as undefined/poison semantics, not as an observable trap.
	// Consequently an unused load, div/rem, shift, or fptosi can be removed.
	// Stores, control flow and calls are classified separately by the caller.
	// If volatile/atomic/constrained instructions are introduced later, their
	// explicit flags belong in this predicate.  Keep unknown instruction kinds
	// conservative so extending InstType cannot silently weaken DCE.
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
		// I/O builtins have observable side effects even if they are memory-readonly.
		if (BuildInFunction::isOutputName(N)) {
			return true;
		}

		// Readonly is not enough: a read may fault, and an I/O-like readonly
		// builtin is observable.  Only an explicit readnone/no-side-effect
		// contract permits removal.
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
