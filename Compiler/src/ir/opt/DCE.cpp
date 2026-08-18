#include "ir/opt/DCE.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "lib/IRUtils.hpp"
#include <functional>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

//递归证明函数自身CFG和全部直接用户函数调用均必然有限返回。
bool DCEPass::isGuaranteedReturningFunction(
	Function &F,
	std::unordered_map<Function *, ReturnProofState> &Memo) {
	auto It = Memo.find(&F);
	if (It != Memo.end())
		return It->second == ReturnProofState::Proven;
	Memo[&F] = ReturnProofState::Visiting;
	if (!hasAcyclicReturningCFG(F)) {
		Memo[&F] = ReturnProofState::Failed;
		return false;
	}
	for (BasicBlock *BB : F) {
		for (Instruction *I : *BB) {
			auto *CI = dynamic_cast<CallInst *>(I);
			if (!CI) continue;
			//所有BuildInFunction均来自受支持的运行时声明；其可观察性由
			//调用者在返回性证明之外单独通过ModRef/SideEffect门禁处理。
			if (CI->getBuiltinCallee()) continue;
			Function *Callee = CI->getDirectCallee();
			if (!Callee ||
				!isGuaranteedReturningFunction(*Callee, Memo)) {
				Memo[&F] = ReturnProofState::Failed;
				return false;
			}
		}
	}
	Memo[&F] = ReturnProofState::Proven;
	return true;
}

//证明函数入口可达CFG无环且每条路径均以ret结束。
bool hasAcyclicReturningCFG(Function &F) {
	//1.1 三色 DFS 要求入口可达的每条路径都以 ret 结束且不存在回边；这同时
	//证明被删除的纯调用不会因无限循环而具有可观察的“不返回”行为。
	auto *Entry = F.front();
	if (!Entry) return false;
	std::unordered_map<BasicBlock *, unsigned> Color;
	std::function<bool(BasicBlock *)> Visit = [&](BasicBlock *BB) -> bool {
		if (!BB) return false;
		unsigned &C = Color[BB];
		if (C == 1) return false;
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
//递归证明函数及其调用闭包无可观察副作用且必然有限返回。
bool DCEPass::isGuaranteedRemovableFunction(
	Function &F, FunctionAnalysisPassManager &FAM,
	std::unordered_map<Function *, ReturnProofState> &Memo) {
	//1.2 递归证明普通函数调用可删除：函数必须 NoModRef、CFG 有限返回，且函数体
	//不含未知语义指令、显式 store 或不可删除 callee。Visiting 状态打断递归环。
	auto It = Memo.find(&F);
	if (It != Memo.end()) {
		if (It->second == ReturnProofState::Proven) return true;
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
			if (dynamic_cast<StoreInst *>(I)) {
				Memo[&F] = ReturnProofState::Failed;
				return false;
			}
			if (dceInstructionMustBePreserved(I)) {
				Memo[&F] = ReturnProofState::Failed;
				return false;
			}
			auto *CI = dynamic_cast<CallInst *>(I);
			if (!CI) continue;
			if (CI->getBuiltinCallee()) {
				// 内置调用统一按调用效果摘要判断；无程序内存访问并不等于
				// 没有 I/O、计时或其他外部可观察行为。
				CallEffectSummary Effects = getCallEffectSummary(CI, FAM);
				if (Effects.HasUnknownMemory ||
					Effects.HasObservableEffect ||
					Effects.HasOrderingBarrier ||
					Effects.MayCrossObjectBounds ||
					!Effects.ProgramMemory.empty()) {
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
//检查移位指令是否使用位宽范围内的已定义常量移位量。
bool hasDefinedConstantShiftAmount(const Instruction *I) {
	//2.1 只有常量移位量落在 [0, bitwidth) 才保证不产生 poison。
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
	const int BitWidth = Ty->isBool() ? 1 : LHS->integerBitWidth(0);
	int32_t Amount = 0;
	return BitWidth > 0 && RHS->tryGetIntConst(Amount) && Amount >= 0 &&
		Amount < BitWidth;
}
//有界反向检查值是否可能依赖undef或缺失操作数。
bool valueMayDependOnUndef(
	const Value *V, std::size_t MaxVisitedValues) {
	//2.2 沿指令操作数反向做有界闭包；遇到 undef、缺失操作数或预算耗尽都按
	//“可能依赖 undef”处理，供其他 pass 的合法性检查复用。
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
			if (dynamic_cast<Instruction *>(Operand))
				WorkList.push_back(Operand);
		}
	}
	return false;
}
//用已知指令白名单判断指令是否具有未知或不可丢弃语义。
bool dceInstructionMustBePreserved(Instruction *I) {
	//2.3 用已知指令种类白名单隔离未来扩展：未识别 opcode 默认具有不可丢弃语义。
	if (!I) return true;
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
//判断无用户指令在执行时是否仍可能陷阱或产生poison。
bool instructionMayFaultOrProducePoison(const Instruction *I) {
	//2.4 标记即使结果无用户也可能陷阱或产生 poison 的操作，供推测执行类 pass 复用。
	if (!I) return true;
	if (dynamic_cast<const LoadInst *>(I)) return true;
	switch (I->instType) {
		case Instruction::SDIV:
		case Instruction::SREM:
		case Instruction::UDIV:
		case Instruction::UREM:
		{
			//2.4.1 常量非零除数已排除除零；有符号-1还需排除INT_MIN溢出。
			auto *BI = dynamic_cast<const BinaryInst *>(I);
			Value *LHS = nullptr;
			Value *RHS = nullptr;
			int32_t Divisor = 0;
			if (!BI || !BI->getOperands(LHS, RHS) ||
				!RHS || !RHS->tryGetIntConst(Divisor) || Divisor == 0) {
				return true;
			}
			if ((I->instType == Instruction::SDIV ||
				 I->instType == Instruction::SREM) && Divisor == -1) {
				int32_t Dividend = 0;
				return !LHS || !LHS->tryGetIntConst(Dividend) ||
					Dividend == std::numeric_limits<int32_t>::min();
			}
			return false;
		}
		case Instruction::FDIV:
		case Instruction::FREM:
			//IEEE浮点除余以Inf/NaN表达零除数，不产生整数式陷阱或poison。
			return false;
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

//有界反向检查值是否可能依赖 undef 或 poison；普通 load 的潜在故障由
//DCE 单独保留，但它的结果本身不因此被当作 poison。
bool valueMayDependOnUndefOrPoison(
	const Value *V, std::size_t MaxVisitedValues) {
	if (valueMayDependOnUndef(V, MaxVisitedValues)) return true;
	if (!V || MaxVisitedValues == 0) return true;
	std::vector<const Value *> WorkList{V};
	std::unordered_set<const Value *> Seen;
	while (!WorkList.empty()) {
		const Value *Current = WorkList.back();
		WorkList.pop_back();
		if (!Current || !Seen.insert(Current).second) continue;
		if (Seen.size() > MaxVisitedValues) return true;
		auto *I = dynamic_cast<const Instruction *>(Current);
		if (!I) continue;
		// inbounds GEP 越出底层对象时产生 poison。这里把它作为值依赖风险，
		// 但不改变 DCE 对无用户地址计算的既有删除策略。
		if (auto *GEP = dynamic_cast<const GetElementPtrInst *>(I);
			GEP && GEP->isInBounds())
			return true;
		if (!dynamic_cast<const LoadInst *>(I) &&
			instructionMayFaultOrProducePoison(I))
			return true;
		for (std::size_t Index = 0; Index < I->getNumOperands(); ++Index) {
			Value *Operand = I->getOperand(Index);
			if (!Operand) return true;
			if (dynamic_cast<Instruction *>(Operand))
				WorkList.push_back(Operand);
		}
	}
	return false;
}
//判断调用是否具有可观察副作用或无法证明必然返回。
bool dceCallHasSideEffect(CallInst* CI, FunctionAnalysisPassManager& FAM) {
	//1.3 调用删除按 builtin/普通函数分流：内置调用统一查询完整效果摘要，
	//普通函数必须通过完整的“无副作用且必返回”递归证明。
	if (!CI) return true;
	Value* Callee = CI->getCallee();
	if (!Callee) return true;
	if (dynamic_cast<BuildInFunction*>(Callee)) {
		CallEffectSummary Effects = getCallEffectSummary(CI, FAM);
		return Effects.HasUnknownMemory ||
			Effects.HasObservableEffect ||
			Effects.HasOrderingBarrier ||
			Effects.MayCrossObjectBounds ||
			!Effects.ProgramMemory.empty();
	}
	if (auto* F = dynamic_cast<Function*>(Callee)) {
		std::unordered_map<Function *, DCEPass::ReturnProofState> Memo;
		return !DCEPass::isGuaranteedRemovableFunction(*F, FAM, Memo);
	}
	return true;
}

//递归证明一次调用及其直接调用闭包必然有限返回。
bool dceCallIsGuaranteedToReturn(CallInst *CI) {
	if (!CI) return false;
	if (CI->getBuiltinCallee()) return true;
	Function *Callee = CI->getDirectCallee();
	if (!Callee) return false;
	std::unordered_map<Function *, DCEPass::ReturnProofState> Memo;
	return DCEPass::isGuaranteedReturningFunction(*Callee, Memo);
}

//判断指令是否没有用户且不包含任何必须保留的语义。
bool DCEPass::isDeletableInstruction(
	Instruction* I, FunctionAnalysisPassManager& FAM) {
	//2.候选必须没有指令用户，且不是终结指令、store、未知指令或有可观察行为的 call。
	if (!I) return false;
	if (I->countInstructionUses() > 0) return false;
	if (I->instType == Instruction::RET || I->instType == Instruction::BR) return false;
	if (I->instType == Instruction::STORE) {
		return false;
	}
	if (dceInstructionMustBePreserved(I)) return false;
	//竞赛模式不利用除零、非法转换或动态越界移位等未定义行为删除原计算。
	if (instructionMayFaultOrProducePoison(I)) return false;
	if (auto* CI = dynamic_cast<CallInst*>(I)) {
		if (dceCallHasSideEffect(CI, FAM)) return false;
	}
	return true;
}
//以FIFO工作表沿def-use链删除函数中的死指令闭包。
PassResult DCEPass::run(Function &F, FunctionAnalysisPassManager &FAM) {
	//1.建立 FIFO 去重工作表和已删除集合；删除任一指令后，其操作数定义可能成为
	//新死代码，因此沿 def-use 反向传播直到局部闭包清空。
	bool Changed = false;
	UniqueWorkList<
		Instruction *, WorkListOrder::FIFO> WorkQ;
	std::unordered_set<Instruction*> Erased;
	auto enqueue = [&](Instruction* I) {
		if (!I) return;
		if (Erased.count(I)) return;
		WorkQ.push(I);
	};
	//2.删除前先快照所有指令型操作数并断开 use，再判断哪些定义刚刚失去最后用户；
	//这样不会在 delete 后访问当前指令持有的 Use。
	auto tryErase = [&](Instruction* I) -> bool {
		if (!I) return false;
		if (Erased.count(I)) return false;
		if (!isDeletableInstruction(I, FAM)) return false;
		std::vector<Instruction *> OperandDefs;
		OperandDefs.reserve(I->getNumOperands());
		forEachOperand(
			I, [&](size_t, Use *U, Value *Op) {
				if (!U || !Op || Op == I) return;
				if (auto *DefI = dynamic_cast<Instruction *>(Op))
					OperandDefs.push_back(DefI);
			});
		I->clearOperands();
		for (Instruction *DefI : OperandDefs) {
			if (Erased.count(DefI)) continue;
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
	//3.对函数做一次稳定快照；每个种子删除后立即排空级联工作表，再继续下一个种子。
	auto AllInsts = collectInstructions(F);
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
	//4.仅删除非 CFG 指令，保留结构分析；值图和调用集合变化使 AA/副作用摘要失效。
	PreservationStatus PA;
	PA.keep<DominantAnalysis>();
	PA.keep<PostDominantAnalysis>();
	PA.keep<LoopAnalysis>();
	PA.discard<BasicAliasAnalysis>();
	PA.discard<SideEffectAnalysis>();
	return PassResult::changed(std::move(PA));
}
