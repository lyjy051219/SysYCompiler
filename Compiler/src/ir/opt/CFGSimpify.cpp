#include "ir/opt/CFGSimpify.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "lib/IRUtils.hpp"
#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

CFGSimplifyPass::PredecessorMap
CFGSimplifyPass::buildPredecessorMap(Function &F) {
	PredecessorMap Result;
	for (BasicBlock *BB : F) {
		if (BB) Result.try_emplace(BB);
	}
	for (BasicBlock *From : F) {
		if (!From) continue;
		std::unordered_set<BasicBlock *> Seen;
		for (BasicBlock *Succ : From->getSuccessors()) {
			if (!Succ || !Seen.insert(Succ).second) continue;
			Result[Succ].push_back(From);
		}
	}
	return Result;
}

const std::vector<BasicBlock *> &CFGSimplifyPass::predecessorsOf(
	BasicBlock *BB, const PredecessorMap &Preds) {
	static const std::vector<BasicBlock *> Empty;
	auto It = Preds.find(BB);
	return It == Preds.end() ? Empty : It->second;
}

//第三个优化：折叠已知分支条件的接口：判断该块和前驱块条件分支指令是否相同或相反
//如果均不存在那么是推导不出的
//A：当前块的条件分支指令的条件值，B：前驱块的条件分支指令的条件值
bool CFGSimplifyPass::matchBooleanCondition(
	Value *A, Value *B, bool &Negated) {
	Negated = false;
	if (!A || !B || !IRType::isBool(A->getIRType()) ||
		!IRType::isBool(B->getIRType()) || Value::isUndef(A) ||
		Value::isUndef(B)) {
		return false;
	}

    //处理A，B如果是XOR的情况，剥离掉XOR指令
	auto stripNot = [](Value *V, bool &Parity) -> Value * {
		Parity = false;
		while (auto *BI = dynamic_cast<BinaryInst *>(V)) {
			Value *L = nullptr;
			Value *R = nullptr;
			if (BI->getOp() != BinaryInst::XOR ||
				!BinaryInst::getOperands(BI, L, R)) {
				break;
			}
			if (Value::isConstOne(R) && L && IRType::isBool(L->getIRType())) {
				Parity = !Parity;
				V = L;
				continue;
			}
			if (Value::isConstOne(L) && R && IRType::isBool(R->getIRType())) {
				Parity = !Parity;
				V = R;
				continue;
			}
			break;
		}
		return V;
	};
    //然后判断A和B是否相同或相反
	bool NotA = false;
	bool NotB = false;
	A = stripNot(A, NotA);
	B = stripNot(B, NotB);
	if (A == B) {
		Negated = NotA != NotB;
		return true;
	}

    //如果A和B都是比较指令，且操作数相同，则判断它们的比较操作码是否相同或相反
	auto *CA = dynamic_cast<BinaryInst *>(A);
	auto *CB = dynamic_cast<BinaryInst *>(B);
	Value *AL = nullptr;
	Value *AR = nullptr;
	Value *BL = nullptr;
	Value *BR = nullptr;
	if (!CA || !CB || !BinaryInst::isCompareOp(CA->getOp()) ||
		!BinaryInst::isCompareOp(CB->getOp()) ||
		!BinaryInst::getOperands(CA, AL, AR) ||
		!BinaryInst::getOperands(CB, BL, BR) || !AL || !AR || !BL || !BR ||
		Value::isUndef(AL) || Value::isUndef(AR) || Value::isUndef(BL) ||
		Value::isUndef(BR) ||
		IRType::isFloat(AL->getIRType()) || IRType::isFloat(BL->getIRType())) {
		return false;
	}

	auto AOp = CA->getOp();
	auto BOp = CB->getOp();
	if (AL == BR && AR == BL) {
		BOp = BinaryInst::reversePredicate(BOp);
		std::swap(BL, BR);
	}
	if (AL != BL || AR != BR) return false;

	bool PredNegated = false;
	if (AOp == BOp) {
		PredNegated = false;
	} else if (AOp == BinaryInst::negatePredicate(BOp)) {
		PredNegated = true;
	} else {
		return false;
	}

	Negated = PredNegated ^ NotA ^ NotB;
	return true;
}

BasicBlock *CFGSimplifyPass::resolveArmIncomingPred(
	BasicBlock *Controller, BasicBlock *Arm, BasicBlock *Merge,
	const PredecessorMap &Preds) {
	if (!Controller || !Arm || !Merge) return nullptr;
	if (Arm == Merge) return Controller;

	BasicBlock *Cur = Arm;
	BasicBlock *Previous = Controller;
	for (unsigned Depth = 0; Depth < 4; ++Depth) {
		const auto &CurPreds = predecessorsOf(Cur, Preds);
		if (CurPreds.size() != 1 || CurPreds.front() != Previous) {
			return nullptr;
		}
		auto *Br = Cur->getBranchTerminator();
		if (!BasicBlock::isUncondBr(Br)) return nullptr;
		BasicBlock *Next = Br->getSuccessor(0);
		if (Next == Merge) return Cur;
		if (!Next || Next == Cur || Next == Controller) return nullptr;
		Previous = Cur;
		Cur = Next;
	}
	return nullptr;
}

std::vector<BasicBlock *> CFGSimplifyPass::collectControllerCandidates(
	BasicBlock *IncomingPred, const PredecessorMap &Preds) {
	std::vector<BasicBlock *> Result;
	BasicBlock *Cur = IncomingPred;
	for (unsigned Depth = 0; Cur && Depth <= 4; ++Depth) {
		Result.push_back(Cur);
		const auto &CurPreds = predecessorsOf(Cur, Preds);
		if (CurPreds.size() != 1 || CurPreds.front() == Cur) break;
		Cur = CurPreds.front();
	}
	return Result;
}

//入边特化只克隆不访问内存、不调用函数且不可观察的标量计算。
bool CFGSimplifyPass::isEdgeSpecializableInstruction(
	Instruction *I, BrInst *Branch) {
	if (!I) return false;
	if (auto *Br = dynamic_cast<BrInst *>(I)) return Br == Branch;
	if (auto *Phi = dynamic_cast<PhiInst *>(I))
		return IRType::isScalar(Phi->getIRType());

	if (auto *BI = dynamic_cast<BinaryInst *>(I)) {
		Value *LHS = nullptr;
		Value *RHS = nullptr;
		return IRType::isScalar(BI->getIRType()) &&
			BinaryInst::getOperands(BI, LHS, RHS) && LHS && RHS &&
			IRType::isScalar(LHS->getIRType()) &&
			IRType::isScalar(RHS->getIRType());
	}

	if (auto *Select = dynamic_cast<SelectInst *>(I)) {
		Value *Condition = Select->getCondition();
		Value *TrueValue = Select->getTrueValue();
		Value *FalseValue = Select->getFalseValue();
		return IRType::isScalar(Select->getIRType()) && Condition &&
			IRType::isBool(Condition->getIRType()) && TrueValue && FalseValue &&
			IRType::isScalar(TrueValue->getIRType()) &&
			IRType::isScalar(FalseValue->getIRType());
	}

	switch (I->instType) {
		case Instruction::TRUNC:
		case Instruction::ZEXT:
		case Instruction::SEXT:
		case Instruction::FPTOSI:
		case Instruction::SITOFP: {
			Value *Operand = I->getUnaryOperand();
			return Operand && IRType::isScalar(Operand->getIRType()) &&
				IRType::isScalar(I->getIRType());
		}
		default:
			return false;
	}
}

//指定入边上的PHI输入相当于一组局部常量环境；此解释器只消费
//IR已定义的常量语义，不根据指令排列或常量的具体数值做特判。
Value *CFGSimplifyPass::evaluateValueOnIncomingEdge(
	Value *V, BasicBlock *Block,
	const std::unordered_map<PhiInst *, Value *> &PhiInputs,
	unsigned Depth, std::unordered_map<Value *, Value *> &Memo,
	std::unordered_set<Value *> &Visiting,
	unsigned &FoldedInstructionCount, bool &UsedIncomingPhi) {
	constexpr unsigned MaxEvaluationDepth = 6;
	if (!V || Value::isUndef(V) || Depth > MaxEvaluationDepth) return nullptr;
	if (V->isConst()) return V;
	auto MemoIt = Memo.find(V);
	if (MemoIt != Memo.end()) return MemoIt->second;
	if (!Visiting.insert(V).second) return nullptr;

	auto Finish = [&](Value *Result) -> Value * {
		Visiting.erase(V);
		Memo[V] = Result;
		return Result;
	};

	auto *I = dynamic_cast<Instruction *>(V);
	if (!I || I->getParent() != Block)
		return Finish(nullptr);

	if (auto *Phi = dynamic_cast<PhiInst *>(I)) {
		auto It = PhiInputs.find(Phi);
		if (It == PhiInputs.end() || !It->second ||
			Value::isUndef(It->second) || !It->second->isConst()) {
			return Finish(nullptr);
		}
		UsedIncomingPhi = true;
		return Finish(It->second);
	}

	if (auto *BI = dynamic_cast<BinaryInst *>(I)) {
		Value *LHS = nullptr;
		Value *RHS = nullptr;
		if (!BinaryInst::getOperands(BI, LHS, RHS)) return Finish(nullptr);
		Value *FoldedLHS = evaluateValueOnIncomingEdge(
			LHS, Block, PhiInputs, Depth + 1, Memo, Visiting,
			FoldedInstructionCount, UsedIncomingPhi);
		Value *FoldedRHS = evaluateValueOnIncomingEdge(
			RHS, Block, PhiInputs, Depth + 1, Memo, Visiting,
			FoldedInstructionCount, UsedIncomingPhi);
		if (!FoldedLHS || !FoldedRHS) return Finish(nullptr);
		Value *Folded = BinaryInst::foldConstants(
			BI->getOp(), FoldedLHS, FoldedRHS, true);
		if (!Folded || Value::isUndef(Folded)) return Finish(nullptr);
		++FoldedInstructionCount;
		return Finish(Folded);
	}

	if (auto *Select = dynamic_cast<SelectInst *>(I)) {
		Value *FoldedCondition = evaluateValueOnIncomingEdge(
			Select->getCondition(), Block, PhiInputs, Depth + 1, Memo,
			Visiting, FoldedInstructionCount, UsedIncomingPhi);
		bool ConditionValue = false;
		if (!FoldedCondition ||
			!FoldedCondition->tryGetBoolConst(ConditionValue)) {
			return Finish(nullptr);
		}
		Value *Selected = ConditionValue ? Select->getTrueValue()
			: Select->getFalseValue();
		Value *Folded = evaluateValueOnIncomingEdge(
			Selected, Block, PhiInputs, Depth + 1, Memo, Visiting,
			FoldedInstructionCount, UsedIncomingPhi);
		if (!Folded || Value::isUndef(Folded) || !Folded->isConst())
			return Finish(nullptr);
		++FoldedInstructionCount;
		return Finish(Folded);
	}

	switch (I->instType) {
		case Instruction::TRUNC:
		case Instruction::ZEXT:
		case Instruction::SEXT:
		case Instruction::FPTOSI:
		case Instruction::SITOFP: {
			Value *Operand = I->getUnaryOperand();
			Value *FoldedOperand = evaluateValueOnIncomingEdge(
				Operand, Block, PhiInputs, Depth + 1, Memo, Visiting,
				FoldedInstructionCount, UsedIncomingPhi);
			if (!FoldedOperand) return Finish(nullptr);
			Value *Folded = Instruction::foldConstantCast(
				I->instType, FoldedOperand, I->getIRType());
			if (!Folded || Value::isUndef(Folded)) return Finish(nullptr);
			++FoldedInstructionCount;
			return Finish(Folded);
		}
		default:
			return Finish(nullptr);
	}
}

//只记录“当前值唯一用户就是期望用户”的依赖链，因此删掉克隆分支
//后可以从链顶向操作数安全删除，不会破坏后继PHI或其他计算。
void CFGSimplifyPass::collectDeadConditionChain(
	Value *V, Instruction *ExpectedUser, BasicBlock *Block,
	std::unordered_set<Instruction *> &Visited,
	std::vector<Instruction *> &Result) {
	auto *I = dynamic_cast<Instruction *>(V);
	if (!I || I->getParent() != Block || dynamic_cast<PhiInst *>(I) ||
		dynamic_cast<BrInst *>(I) ||
		!isEdgeSpecializableInstruction(I, nullptr)) {
		return;
	}
	Instruction *OnlyUser = nullptr;
	if (I->countInstructionUses(&OnlyUser) != 1 || OnlyUser != ExpectedUser ||
		!Visited.insert(I).second) {
		return;
	}
	Result.push_back(I);
	for (std::size_t Index = 0; Index < I->getNumOperands(); ++Index) {
		collectDeadConditionChain(
			I->getOperand(Index), I, Block, Visited, Result);
	}
}

//建计划阶段同时校验块形、SSA外部use、所有PHI输入和收益；任一项
//不完整就放弃整个候选，不会产生部分CFG修改。
bool CFGSimplifyPass::buildEdgeSpecializationPlan(
	Function &F, BasicBlock *Block,
	const std::unordered_set<BasicBlock *> &LoopBlocks,
	EdgeSpecializationPlan &Plan) {
	Plan = EdgeSpecializationPlan{};
	if (!Block || Block == F.front() || LoopBlocks.count(Block)) return false;
	auto *Branch = Block->getBranchTerminator();
	if (!BasicBlock::isCondBr(Branch)) return false;
	BasicBlock *TrueBlock = Branch->getTrueSuccessor();
	BasicBlock *FalseBlock = Branch->getFalseSuccessor();
	if (!TrueBlock || !FalseBlock || TrueBlock == FalseBlock ||
		TrueBlock == Block || FalseBlock == Block) {
		return false;
	}
	Value *Condition = Branch->getCondition();
	bool ConstantCondition = false;
	if (!Condition || Value::isUndef(Condition) ||
		Condition->tryGetBoolConst(ConstantCondition)) {
		return false;
	}

	auto Predecessors = Block->getPredecessors();
	if (Predecessors.size() < 2 || Predecessors.size() > 4) return false;
	std::unordered_set<BasicBlock *> UniquePredecessors;
	for (BasicBlock *Pred : Predecessors) {
		if (!Pred || Pred == Block || !UniquePredecessors.insert(Pred).second ||
			Pred->countEdgesToSucc(Block) != 1) {
			return false;
		}
	}

	unsigned PhiCount = 0;
	unsigned NonPhiInstructionCount = 0;
	for (Instruction *I : *Block) {
		if (!isEdgeSpecializableInstruction(I, Branch)) return false;
		if (dynamic_cast<PhiInst *>(I))
			++PhiCount;
		else
			++NonPhiInstructionCount;
	}
	if (PhiCount > 8 || NonPhiInstructionCount == 0 ||
		NonPhiInstructionCount > 6) {
		return false;
	}

	auto Phis = Block->collectPhiNodes();
	if (Phis.size() != PhiCount) return false;
	for (PhiInst *Phi : Phis) {
		if (!Phi || Phi->getNumIncomingValues() != Predecessors.size())
			return false;
		for (BasicBlock *Pred : Predecessors) {
			Value *Incoming = Phi->getIncomingFromPred(Pred);
			if (Phi->countIncomingFromPred(Pred) != 1 || !Incoming ||
				!Incoming->getIRType() || !Phi->getIRType() ||
				!Incoming->getIRType()->hasSameShape(*Phi->getIRType())) {
				return false;
			}
			if (auto *IncomingI = dynamic_cast<Instruction *>(Incoming)) {
				if (IncomingI->getParent() == Block) return false;
			}
		}
	}

	//SSA定义离开本块时必须经过直接后继PHI，这样才能为克隆边构造对应输入。
	for (Instruction *I : *Block) {
		if (!I || I == Branch) continue;
		for (Use *U : I->getUses()) {
			if (!U || U->getValue() != I) continue;
			auto *UserI = dynamic_cast<Instruction *>(U->getUser());
			if (!UserI) return false;
			if (UserI->getParent() == Block) continue;
			auto *SuccessorPhi = dynamic_cast<PhiInst *>(UserI);
			if (!SuccessorPhi ||
				(SuccessorPhi->getParent() != TrueBlock &&
				 SuccessorPhi->getParent() != FalseBlock) ||
				SuccessorPhi->countIncomingFromPred(Block) != 1 ||
				SuccessorPhi->getIncomingFromPred(Block) != I) {
				return false;
			}
			unsigned SameUserUses = 0;
			for (Use *OtherUse : I->getUses()) {
				if (OtherUse && OtherUse->getValue() == I &&
					OtherUse->getUser() == SuccessorPhi) {
					++SameUserUses;
				}
			}
			if (SameUserUses != 1) return false;
		}
	}

	std::unordered_set<Instruction *> DeadVisited;
	std::vector<Instruction *> DeadConditionChain;
	collectDeadConditionChain(
		Condition, Branch, Block, DeadVisited, DeadConditionChain);
	if (DeadConditionChain.empty()) return false;

	//只在两入边汇合块上做特化：移走一条边后原PHI会全部消失。
	//克隆中保留下来的指令数不得超过被删除的PHI数，从而静态码量
	//不增长；被删除的条件链则保证特化边上的动态指令严格减少。
	unsigned SurvivingCloneInstructionCount =
		NonPhiInstructionCount -
		static_cast<unsigned>(DeadConditionChain.size());
	if (Predecessors.size() != 2 ||
		SurvivingCloneInstructionCount > PhiCount) {
		return false;
	}

	unsigned BestBenefit = 0;
	for (BasicBlock *Pred : Predecessors) {
		if (!Pred || Pred == TrueBlock || Pred == FalseBlock) continue;
		std::unordered_map<PhiInst *, Value *> PhiInputMap;
		std::vector<EdgeSpecializationPhiInput> CandidatePhiInputs;
		bool InputsValid = true;
		for (PhiInst *Phi : Phis) {
			Value *Incoming = Phi->getIncomingFromPred(Pred);
			if (!Incoming || !PhiInputMap.emplace(Phi, Incoming).second) {
				InputsValid = false;
				break;
			}
			CandidatePhiInputs.push_back({Phi, Incoming});
		}
		if (!InputsValid) continue;

		std::unordered_map<Value *, Value *> Memo;
		std::unordered_set<Value *> Visiting;
		unsigned FoldedInstructionCount = 0;
		bool UsedIncomingPhi = false;
		Value *FoldedCondition = evaluateValueOnIncomingEdge(
			Condition, Block, PhiInputMap, 0, Memo, Visiting,
			FoldedInstructionCount, UsedIncomingPhi);
		bool ConditionValue = false;
		if (!FoldedCondition ||
			!FoldedCondition->tryGetBoolConst(ConditionValue) ||
			FoldedInstructionCount == 0 || !UsedIncomingPhi) {
			continue;
		}

		BasicBlock *Taken = ConditionValue ? TrueBlock : FalseBlock;
		if (!Taken || Taken == Pred) continue;
		//收益只按该边上实际消失的纯计算计数，不使用操作码排列、
		//常量取值或测试名称等特征。
		unsigned Benefit =
			static_cast<unsigned>(DeadConditionChain.size());
		if (Benefit <= BestBenefit) continue;

		std::vector<EdgeSpecializationSuccessorInput> SuccessorInputs;
		bool SuccessorInputsValid = true;
		for (PhiInst *Phi : Taken->collectPhiNodes()) {
			if (!Phi || Phi->countIncomingFromPred(Block) != 1) {
				SuccessorInputsValid = false;
				break;
			}
			Value *Incoming = Phi->getIncomingFromPred(Block);
			if (!Incoming || !Incoming->getIRType() || !Phi->getIRType() ||
				!Incoming->getIRType()->hasSameShape(*Phi->getIRType())) {
				SuccessorInputsValid = false;
				break;
			}
			SuccessorInputs.push_back({Phi, Incoming});
		}
		if (!SuccessorInputsValid) continue;

		BestBenefit = Benefit;
		Plan.Block = Block;
		Plan.Predecessor = Pred;
		Plan.Taken = Taken;
		Plan.Branch = Branch;
		Plan.CloneInstructionCount =
			PhiCount + NonPhiInstructionCount;
		Plan.PhiInputs = std::move(CandidatePhiInputs);
		Plan.SuccessorInputs = std::move(SuccessorInputs);
		Plan.DeadConditionChain = DeadConditionChain;
	}
	return Plan.Block != nullptr;
}

//提交前再校验所有指针和边；克隆完成后所需值一次性翻译，之后的
//操作都是无失败的CFG/PHI基础操作，避免留下半提交状态。
bool CFGSimplifyPass::commitEdgeSpecialization(
	Function &F, const EdgeSpecializationPlan &Plan) {
	if (!Plan.Block || !Plan.Predecessor || !Plan.Taken || !Plan.Branch ||
		Plan.Branch->getParent() != Plan.Block ||
		Plan.Block->getBranchTerminator() != Plan.Branch ||
		Plan.Predecessor->countEdgesToSucc(Plan.Block) != 1) {
		return false;
	}
	for (const EdgeSpecializationPhiInput &Input : Plan.PhiInputs) {
		if (!Input.Phi || Input.Phi->getParent() != Plan.Block ||
			Input.Phi->countIncomingFromPred(Plan.Predecessor) != 1 ||
			Input.Phi->getIncomingFromPred(Plan.Predecessor) != Input.Incoming) {
			return false;
		}
	}
	for (const EdgeSpecializationSuccessorInput &Input : Plan.SuccessorInputs) {
		if (!Input.Phi || Input.Phi->getParent() != Plan.Taken ||
			Input.Phi->countIncomingFromPred(Plan.Block) != 1 ||
			Input.Phi->getIncomingFromPred(Plan.Block) != Input.Incoming) {
			return false;
		}
	}

	std::string CloneName = F.makeUniqueBlockIdent(
		Plan.Block->getIdent() + ".edge.spec");
	CloneBlockRegionOptions Options;
	Options.MakeBlockIdent = [CloneName](const BasicBlock *, std::size_t) {
		return CloneName;
	};
	auto Region = cloneBasicBlockRegion(F, {Plan.Block}, {}, Options);
	if (!Region) return false;
	BasicBlock *CloneBlock = Region->getClonedBlock(Plan.Block);
	auto *CloneBranch = dynamic_cast<BrInst *>(
		Region->getClonedInstruction(Plan.Branch));
	if (!CloneBlock || !CloneBranch ||
		CloneBlock->getBranchTerminator() != CloneBranch) {
		if (CloneBlock) CloneBlock->eraseBlockSafely();
		return false;
	}

	std::vector<std::pair<PhiInst *, Value *>> ClonePhiInputs;
	for (const EdgeSpecializationPhiInput &Input : Plan.PhiInputs) {
		auto *ClonePhi = dynamic_cast<PhiInst *>(
			Region->getClonedInstruction(Input.Phi));
		if (!ClonePhi || !Input.Incoming || !ClonePhi->getIRType() ||
			!Input.Incoming->getIRType() ||
			!Input.Incoming->getIRType()->hasSameShape(
				*ClonePhi->getIRType())) {
			CloneBlock->eraseBlockSafely();
			return false;
		}
		ClonePhiInputs.push_back({ClonePhi, Input.Incoming});
	}

	auto TranslateValue = [&](Value *Source) -> Value * {
		if (!Source) return nullptr;
		for (const EdgeSpecializationPhiInput &Input : Plan.PhiInputs) {
			if (Source == Input.Phi) return Input.Incoming;
		}
		if (auto *I = dynamic_cast<Instruction *>(Source)) {
			if (I->getParent() == Plan.Block)
				return Region->getClonedInstruction(I);
		}
		return Source;
	};

	std::vector<std::pair<PhiInst *, Value *>> TranslatedSuccessorInputs;
	for (const EdgeSpecializationSuccessorInput &Input :
		 Plan.SuccessorInputs) {
		Value *Translated = TranslateValue(Input.Incoming);
		if (!Translated || !Translated->getIRType() ||
			!Input.Phi->getIRType() ||
			!Translated->getIRType()->hasSameShape(*Input.Phi->getIRType())) {
			CloneBlock->eraseBlockSafely();
			return false;
		}
		TranslatedSuccessorInputs.push_back({Input.Phi, Translated});
	}

	std::vector<Instruction *> ClonedDeadConditionChain;
	for (Instruction *I : Plan.DeadConditionChain) {
		Instruction *CloneI = Region->getClonedInstruction(I);
		if (!CloneI) {
			CloneBlock->eraseBlockSafely();
			return false;
		}
		ClonedDeadConditionChain.push_back(CloneI);
	}

	//重定向是提交点；在此之前出现任何不一致都只需删除孤立克隆块。
	if (!BasicBlock::redirectBranchTarget(
			Plan.Predecessor, Plan.Block, CloneBlock)) {
		CloneBlock->eraseBlockSafely();
		return false;
	}

	for (const auto &Input : ClonePhiInputs) {
		Input.first->replaceAllUsesWith(Input.second);
		delete Input.first;
	}
	if (!BasicBlock::replaceCondBrWithUncond(CloneBlock, Plan.Taken))
		return false;
	for (const auto &Input : TranslatedSuccessorInputs)
		Input.first->addIncoming(Input.second, CloneBlock);
	for (const EdgeSpecializationPhiInput &Input : Plan.PhiInputs)
		PhiInst::removeIncomingByPred(
			Input.Phi, Plan.Predecessor, 1);

	//先删条件链顶再删操作数；计划阶段已证明每一层都只有上一层一个use。
	for (Instruction *I : ClonedDeadConditionChain) {
		if (I && I->getParent() == CloneBlock && !I->hasUses()) delete I;
	}
	return true;
}

//入边特化每个源块最多选一条边，并以全函数块数/指令数预算限制码量。
bool CFGSimplifyPass::specializeBranchesOnIncomingEdges(
	Function &F, const std::unordered_set<BasicBlock *> &LoopBlocks) {
	constexpr unsigned MaxSpecializedBlocks = 4;
	constexpr unsigned MaxClonedInstructions = 24;
	unsigned SpecializedBlocks = 0;
	unsigned ClonedInstructions = 0;
	bool Changed = false;
	auto Blocks = snapshotPointers(F);
	for (BasicBlock *Block : Blocks) {
		if (!Block || SpecializedBlocks >= MaxSpecializedBlocks) break;
		EdgeSpecializationPlan Plan;
		if (!buildEdgeSpecializationPlan(F, Block, LoopBlocks, Plan)) continue;
		if (Plan.CloneInstructionCount == 0 ||
			ClonedInstructions + Plan.CloneInstructionCount >
				MaxClonedInstructions) {
			continue;
		}
		if (!commitEdgeSpecialization(F, Plan)) continue;
		++SpecializedBlocks;
		ClonedInstructions += Plan.CloneInstructionCount;
		Changed = true;
	}
	return Changed;
}

//1.消除死块
bool CFGSimplifyPass::removeDeadBlocks(Function &F) {
	bool Changed = false;
	BasicBlock *Entry = F.front();

    //将函数中的基本块复制到一个临时的vector中，避免在删除过程中迭代器失效
	auto Blocks = snapshotPointers(F);
    //排除入口块之后，检查前驱块：没有/唯一的前驱是自己，满足条件删除
	for (BasicBlock *BB : Blocks) {
		if (!BB || BB == Entry) continue;

		auto Preds = BB->getPredecessors();
		bool NoPred = Preds.empty();
		bool OnlySelfPred = (Preds.size() == 1 && Preds[0] == BB);
		if (!NoPred && !OnlySelfPred) continue;

		auto Succs = BB->getSuccessors();
		for (BasicBlock *Succ : Succs) {
            //删除前先维护后继块的PHI
			int EdgeCnt = BB->countEdgesToSucc(Succ);
			if (EdgeCnt > 0) BasicBlock::removeEdgeFromSuccPhis(Succ, BB, EdgeCnt);
		}
        //删除基本块连同里面的所有指令
		BB->eraseBlockSafely();
		Changed = true;
	}

	return Changed;
}
//2.终结指令常量折叠
bool CFGSimplifyPass::foldTerminatorBranches(Function &F) {
	bool Changed = false;

	auto Blocks = snapshotPointers(F);

	for (BasicBlock *BB : Blocks) {
		BrInst *Br = BB->getBranchTerminator();
		if (!BasicBlock::isCondBr(Br)) continue;

		Value *Cond = Br->getCondition();
		auto *TrueBB = Br->getTrueSuccessor();
		auto *FalseBB = Br->getFalseSuccessor();
		if (!TrueBB || !FalseBB) continue;

        //2.1如果条件分支的两个后继块相同，则删除一条多余的边，改为无条件分支
		if (TrueBB == FalseBB) {
			// cond 分支两条边都到同一个块：删掉一条多余边
			BasicBlock::removeEdgeFromSuccPhis(TrueBB, BB, 1);
			BasicBlock::replaceCondBrWithUncond(BB, TrueBB);
			Changed = true;
			continue;
		}

		bool CondVal = false;
        //2.2如果条件分支的条件是常量，则根据常量值选择一条边，删除另一条边，改为无条件分支
		if (!Cond || !Cond->tryGetBoolConst(CondVal)) continue;

		BasicBlock *Taken = CondVal ? TrueBB : FalseBB;
		BasicBlock *Untaken = CondVal ? FalseBB : TrueBB;

		BasicBlock::removeEdgeFromSuccPhis(Untaken, BB, 1);
		BasicBlock::replaceCondBrWithUncond(BB, Taken);
		Changed = true;
	}

	return Changed;
}
//4.消除重复PHI节点
bool CFGSimplifyPass::eliminateDuplicatePhis(Function &F) {
	bool Changed = false;

	for (BasicBlock *BB : F) {
		if (!BB) continue;
		bool LocalChanged = true;
		while (LocalChanged) {
			LocalChanged = false;

			auto Phis = BB->collectPhiNodes();
			std::unordered_set<PhiInst *> Removed;

			for (size_t i = 0; i < Phis.size(); ++i) {
				PhiInst *Base = Phis[i];
				if (!Base || Removed.count(Base)) continue;

				for (size_t j = i + 1; j < Phis.size(); ++j) {
					PhiInst *Cand = Phis[j];
					if (!Cand || Removed.count(Cand)) continue;

					if (!PhiInst::areEquivalent(Base, Cand)) continue;

					Cand->replaceAllUsesWith(Base);
					Removed.insert(Cand);
					LocalChanged = true;
					Changed = true;
					break;
				}

				if (LocalChanged) break;
			}

			for (PhiInst *P : Removed) {
				if (P) delete P;
			}
		}
	}

	return Changed;
}

//6.规范化布尔PHI节点
bool CFGSimplifyPass::normalizeBooleanPhis(Function &F) {
	bool Changed = false;
	auto Blocks = snapshotPointers(F);
	const PredecessorMap Preds = buildPredecessorMap(F);

	for (BasicBlock *BB : Blocks) {
		if (!BB) continue;
		auto Phis = BB->collectPhiNodes();
		for (PhiInst *Phi : Phis) {
			const bool IsBoolPhi =
				Phi && IRType::isBool(Phi->getIRType());
			if (!Phi || Phi->getParent() != BB ||
				(!IsBoolPhi &&
				 !IRType::hasIntegerBitWidth(Phi->getIRType(), 32))) {
				continue;
			}

			std::vector<bool> Bits;
			Bits.reserve(Phi->getNumIncomingValues());
			bool AllBooleanConstants = Phi->getNumIncomingValues() >= 2;
			if (!forEachPhiIncoming(
					Phi, [&](unsigned, Value *Incoming, BasicBlock *) {
				bool Bit = false;
				if (auto *CB = dynamic_cast<ConstBool *>(Incoming)) {
					Bit = CB->getVal();
				} else if (auto *CI = dynamic_cast<ConstInt *>(Incoming)) {
					if (CI->getVal() != 0 && CI->getVal() != 1) {
						return false;
					}
					Bit = CI->getVal() == 1;
				} else {
					return false;
				}
				Bits.push_back(Bit);
				return true;
			})) {
				AllBooleanConstants = false;
			}
			if (!AllBooleanConstants) continue;

			if (std::all_of(Bits.begin() + 1, Bits.end(),
							 [First = Bits.front()](bool Bit) {
								 return Bit == First;
							 })) {
				Value *C = Value::makeIntegerForType(
					Bits.front() ? 1 : 0, Phi->getIRType());
				if (!C) continue;
				Phi->replaceAllUsesWith(C);
				delete Phi;
				Changed = true;
				continue;
			}

			Value *BooleanValue = nullptr;
			Instruction *InsertedNot = nullptr;

			// Recover a bounded, single-entry short-circuit decision tree.  A
			// multi-way constant PHI is often just the materialized result of
			// `a && b` / `a || b`; rebuilding that boolean expression in the
			// common controller removes the PHI without changing the CFG.  Child
			// conditions are cloned only when they consist of speculatable binary
			// operations whose operands can also be made available in the root.
			if (BB != F.front() && Phi->getNumIncomingValues() >= 3) {
				std::unordered_map<BasicBlock *, bool> IncomingBits;
				bool UniqueIncomingPreds = true;
				for (unsigned I = 0; I < Phi->getNumIncomingValues(); ++I) {
					BasicBlock *Pred = Phi->getIncomingBlock(I);
					if (!Pred || !IncomingBits.emplace(Pred, Bits[I]).second) {
						UniqueIncomingPreds = false;
						break;
					}
				}

				BasicBlock *Root = nullptr;
				if (UniqueIncomingPreds) {
					auto FirstCandidates = collectControllerCandidates(
						Phi->getIncomingBlock(0), Preds);
					for (BasicBlock *Candidate : FirstCandidates) {
						if (!Candidate || Candidate == BB ||
							!BasicBlock::isCondBr(
								Candidate->getBranchTerminator())) {
							continue;
						}

						bool CommonToAll = true;
						for (unsigned I = 1;
							 I < Phi->getNumIncomingValues(); ++I) {
							auto Candidates = collectControllerCandidates(
								Phi->getIncomingBlock(I), Preds);
							if (std::find(Candidates.begin(), Candidates.end(),
										  Candidate) == Candidates.end()) {
								CommonToAll = false;
								break;
							}
						}
						if (CommonToAll) {
							Root = Candidate;
							break;
						}
					}
				}

				if (Root) {
					BrInst *RootBr = Root->getBranchTerminator();
					constexpr unsigned MaxRegionDepth = 6;
					constexpr unsigned MaxNewInstructions = 10;
					unsigned NewInstructionCount = 0;
					bool Failed = false;
					std::vector<Instruction *> Created;
					std::unordered_map<Value *, Value *> Materialized;
					std::unordered_set<BasicBlock *> ActiveBlocks;
					std::unordered_set<BasicBlock *> ConsumedIncomingPreds;

					auto insertAtRoot = [&](Instruction *I) -> Value * {
						if (!I || Failed ||
							NewInstructionCount >= MaxNewInstructions ||
							!Instruction::insertBefore(RootBr, I)) {
							delete I;
							Failed = true;
							return nullptr;
						}
						++NewInstructionCount;
						Created.push_back(I);
						return I;
					};

					auto isSpeculatableBinary = [](BinaryInst::Operation Op) {
						switch (Op) {
							case BinaryInst::ADD:
							case BinaryInst::SUB:
							case BinaryInst::MUL:
							case BinaryInst::AND:
							case BinaryInst::OR:
							case BinaryInst::XOR:
							case BinaryInst::E:
							case BinaryInst::NE:
							case BinaryInst::L:
							case BinaryInst::LE:
							case BinaryInst::G:
							case BinaryInst::GE:
								return true;
							default:
								return false;
						}
					};

					std::function<Value *(Value *)> materializeAtRoot;
					materializeAtRoot = [&](Value *V) -> Value * {
						if (!V || Failed || Value::isUndef(V)) return nullptr;
						if (V->isConst() || V->isParam() || V->isGlobal()) return V;
						auto *I = dynamic_cast<Instruction *>(V);
						if (!I) return nullptr;
						if (I->getParent() == Root) return I;
						auto Cached = Materialized.find(V);
						if (Cached != Materialized.end()) return Cached->second;

						auto *BI = dynamic_cast<BinaryInst *>(I);
						Value *L = nullptr;
						Value *R = nullptr;
						if (!BI || !ActiveBlocks.count(I->getParent()) ||
							!isSpeculatableBinary(BI->getOp()) ||
							!BinaryInst::getOperands(BI, L, R)) {
							return nullptr;
						}

						Value *NewL = materializeAtRoot(L);
						Value *NewR = materializeAtRoot(R);
						if (!NewL || !NewR) return nullptr;
						auto *Clone = new BinaryInst(NewL, BI->getOp(), NewR);
						Value *Inserted = insertAtRoot(Clone);
						if (Inserted) Materialized[V] = Inserted;
						return Inserted;
					};

					auto makeNot = [&](Value *V) -> Value * {
						if (!V || !IRType::isBool(V->getIRType())) return nullptr;
						return insertAtRoot(BinaryInst::createBooleanNot(V));
					};

					auto combine = [&](Value *Cond, Value *TrueV,
								   Value *FalseV) -> Value * {
						if (!Cond || !TrueV || !FalseV) return nullptr;
						bool T = false;
						bool FBit = false;
						const bool TC = TrueV->tryGetBoolConst(T);
						const bool FC = FalseV->tryGetBoolConst(FBit);
						if (TC && FC) {
							if (T == FBit) return TrueV;
							return T ? Cond : makeNot(Cond);
						}
						if (TC) {
							if (T)
								return insertAtRoot(new BinaryInst(
									Cond, BinaryInst::OR, FalseV));
							Value *Not = makeNot(Cond);
							return Not ? insertAtRoot(new BinaryInst(
								Not, BinaryInst::AND, FalseV)) : nullptr;
						}
						if (FC) {
							if (!FBit)
								return insertAtRoot(new BinaryInst(
									Cond, BinaryInst::AND, TrueV));
							Value *Not = makeNot(Cond);
							return Not ? insertAtRoot(new BinaryInst(
								Not, BinaryInst::OR, TrueV)) : nullptr;
						}
						return insertAtRoot(new SelectInst(Cond, TrueV, FalseV));
					};

					std::function<Value *(BasicBlock *, unsigned)> buildNode;
					std::function<Value *(BasicBlock *, BasicBlock *, unsigned)>
						buildArm;

					buildArm = [&](BasicBlock *Controller, BasicBlock *Arm,
								   unsigned Depth) -> Value * {
						if (!Controller || !Arm || Depth > MaxRegionDepth)
							return nullptr;
						if (Arm == BB) {
							auto It = IncomingBits.find(Controller);
							if (It == IncomingBits.end() ||
								!ConsumedIncomingPreds.insert(Controller).second)
								return nullptr;
							return ConstBool::newConstBool(It->second);
						}

						BasicBlock *Cur = Arm;
						BasicBlock *Previous = Controller;
						for (unsigned D = Depth; D <= MaxRegionDepth; ++D) {
							const auto &CurPreds = predecessorsOf(Cur, Preds);
							if (CurPreds.size() != 1 || CurPreds.front() != Previous)
								return nullptr;
							BrInst *Br = Cur->getBranchTerminator();
							if (BasicBlock::isCondBr(Br)) return buildNode(Cur, D);
							if (!BasicBlock::isUncondBr(Br)) return nullptr;
							if (!ActiveBlocks.insert(Cur).second) return nullptr;
							for (Instruction *I : *Cur) {
								if (I != Br) return nullptr;
							}

							BasicBlock *Next = Br->getSuccessor(0);
							if (Next == BB) {
								auto It = IncomingBits.find(Cur);
								if (It == IncomingBits.end() ||
									!ConsumedIncomingPreds.insert(Cur).second)
									return nullptr;
								return ConstBool::newConstBool(It->second);
							}
							if (!Next || Next == Cur || Next == Controller) return nullptr;
							Previous = Cur;
							Cur = Next;
						}
						return nullptr;
					};

					buildNode = [&](BasicBlock *Node, unsigned Depth) -> Value * {
						if (!Node || Depth > MaxRegionDepth ||
							!ActiveBlocks.insert(Node).second) {
							return nullptr;
						}
						BrInst *Br = Node->getBranchTerminator();
						if (!BasicBlock::isCondBr(Br) ||
							Br->getTrueSuccessor() == Br->getFalseSuccessor()) {
							return nullptr;
						}
						Value *Cond = materializeAtRoot(Br->getCondition());
						Value *TrueV = buildArm(
							Node, Br->getTrueSuccessor(), Depth + 1);
						Value *FalseV = buildArm(
							Node, Br->getFalseSuccessor(), Depth + 1);
						return combine(Cond, TrueV, FalseV);
					};

					Value *Candidate = buildNode(Root, 0);
					if (!Failed && Candidate &&
						ConsumedIncomingPreds.size() == IncomingBits.size()) {
						BooleanValue = Candidate;
					} else {
						for (auto It = Created.rbegin(); It != Created.rend(); ++It)
							delete *It;
					}
				}
			}

			if (BB != F.front() && Phi->getNumIncomingValues() == 2 &&
				Bits[0] != Bits[1]) {
				BasicBlock *P0 = Phi->getIncomingBlock(0);
				BasicBlock *P1 = Phi->getIncomingBlock(1);
				if (!P0 || !P1 || P0 == P1) {
					P0 = nullptr;
				}
				auto P0Candidates = collectControllerCandidates(P0, Preds);
				auto P1Candidates = collectControllerCandidates(P1, Preds);
				std::unordered_set<BasicBlock *> P0CandidateSet(
					P0Candidates.begin(), P0Candidates.end());
				for (BasicBlock *Controller : P1Candidates) {
					if (!Controller || Controller == BB ||
						!P0CandidateSet.count(Controller)) {
						continue;
					}
					auto *Br = Controller ? Controller->getBranchTerminator() : nullptr;
					if (!BasicBlock::isCondBr(Br)) continue;
					BasicBlock *TP = resolveArmIncomingPred(
						Controller, Br->getTrueSuccessor(), BB, Preds);
					BasicBlock *FP = resolveArmIncomingPred(
						Controller, Br->getFalseSuccessor(), BB, Preds);
					if (!TP || !FP) continue;

					bool TrueBit = false;
					bool FalseBit = false;
					if (TP == P0 && FP == P1) {
						TrueBit = Bits[0];
						FalseBit = Bits[1];
					} else if (TP == P1 && FP == P0) {
						TrueBit = Bits[1];
						FalseBit = Bits[0];
					} else {
						continue;
					}

					Value *Cond = Br->getCondition();
					if (!Cond || Value::isUndef(Cond) ||
						!IRType::isBool(Cond->getIRType())) {
						continue;
					}
					if (TrueBit && !FalseBit) {
						BooleanValue = Cond;
					} else if (!TrueBit && FalseBit) {
						auto *Not = BinaryInst::createBooleanNot(Cond);
						if (!BB->insertAfterPhiNodes(Not)) {
							delete Not;
							continue;
						}
						InsertedNot = Not;
						BooleanValue = Not;
					}
					if (BooleanValue) break;
				}
			}

			if (IsBoolPhi) {
				if (!BooleanValue) continue;
				Phi->replaceAllUsesWith(BooleanValue);
				delete Phi;
				Changed = true;
				continue;
			}

			PhiInst *BoolPhi = nullptr;
			if (!BooleanValue) {
				BoolPhi = new PhiInst(INTType::getBoolTy());
				forEachPhiIncoming(
					Phi, [&](unsigned I, Value *, BasicBlock *IncomingBlock) {
					BoolPhi->addIncoming(ConstBool::newConstBool(Bits[I]),
									 IncomingBlock);
				});
				Phi->replaceWith(BoolPhi);
				BooleanValue = BoolPhi;
			}

			auto *Ext = new ZextInst(
				BooleanValue,
				std::dynamic_pointer_cast<INTType>(Phi->getIRType()));
			bool Inserted = InsertedNot
				? Instruction::insertAfter(InsertedNot, Ext)
				: BB->insertAfterPhiNodes(Ext);
			if (!Inserted) {
				delete Ext;
				if (BoolPhi) {
					BoolPhi->replaceWith(Phi);
					delete BoolPhi;
				}
				if (InsertedNot && !InsertedNot->hasUses())
					delete InsertedNot;
				continue;
			}

			Phi->replaceAllUsesWith(Ext);
			if (!BoolPhi) Phi->eraseFromParent();
			delete Phi;
			Changed = true;
		}
	}

	return Changed;
}

//3.折叠已知分支条件
bool CFGSimplifyPass::foldKnownBranchConditions(Function &F) {
	bool Changed = false;
	auto Blocks = snapshotPointers(F);

	for (BasicBlock *BB : Blocks) {
        //3.1筛选基本块：拥有含变量条件Cond的条件分支，且至少有一个前驱块
		if (!BB || BB == F.front()) continue;
		auto *Br = BB->getBranchTerminator();
		if (!BasicBlock::isCondBr(Br)) continue;
		Value *Cond = Br->getCondition();
		if (!Cond) continue;
		auto Preds = BB->getPredecessors();
		if (Preds.empty()) continue;

        //已经从至少一个前驱推导出当前条件值
		bool HaveKnown = false;
        //保存所有前驱共同证明的条件值
		bool KnownValue = false;
        //每个前驱都能推导出条件值，所有前驱推导出的条件值完全一致
		bool AllKnownAndEqual = true;

		for (BasicBlock *Pred : Preds) {
            //前驱不能包含自己
			if (Pred == BB) {
				AllKnownAndEqual = false;
				break;
			}

            //前驱必须是条件分支
			auto *PBr = Pred ? Pred->getBranchTerminator() : nullptr;
			if (!BasicBlock::isCondBr(PBr)) {
				AllKnownAndEqual = false;
				break;
			}

            //判断通过前驱的哪条分支进入当前块
			bool EdgeValue = false;
			if (PBr->getTrueSuccessor() == BB &&
				PBr->getFalseSuccessor() != BB) {
				EdgeValue = true;
			} else if (PBr->getFalseSuccessor() == BB &&
					   PBr->getTrueSuccessor() != BB) {
				EdgeValue = false;
			} else {
				AllKnownAndEqual = false;
				break;
			}

			bool Negated = false;
			if (!matchBooleanCondition(Cond, PBr->getCondition(), Negated)) {
				AllKnownAndEqual = false;
				break;
			}
			bool ThisValue = EdgeValue ^ Negated;
            /*
            EdgeValue = PredCond 的值
            Negated   = BBCond 是否等于 !PredCond
            EdgeValue  Negated	ThisValue	      含义
            false	   false	false	    条件相同，前驱条件为假
            true	   false	true	    条件相同，前驱条件为真
            false	   true	    true	    当前条件是前驱条件的非
            true	   true	    false	    当前条件是前驱条件的非
            */

            //汇总所有前驱的推导结果
			if (!HaveKnown) {
				HaveKnown = true;
				KnownValue = ThisValue;
			} else if (KnownValue != ThisValue) {
				AllKnownAndEqual = false;
				break;
			}
		}

        //确定保留和删除的后继
		if (!HaveKnown || !AllKnownAndEqual) continue;
		BasicBlock *Taken = KnownValue ? Br->getTrueSuccessor()
								   : Br->getFalseSuccessor();
		BasicBlock *Untaken = KnownValue ? Br->getFalseSuccessor()
									 : Br->getTrueSuccessor();
		if (!Taken || !Untaken) continue;
        //进行删除并维护PHI
		if (Taken != Untaken)
			BasicBlock::removeEdgeFromSuccPhis(Untaken, BB, 1);
		if (BasicBlock::replaceCondBrWithUncond(BB, Taken)) Changed = true;
	}

	return Changed;
}

//5.穿透布尔PHI节点的条件分支
/*
优化前：
   P0 --\
         B: c = phi
   P1 --/   br c

  B:
  %c = phi i1 [ true, %P0 ], [ false, %P1 ]
  br i1 %c, label %T, label %F
优化后：
  P0 ----> T
  P1 ----> F
*/
bool CFGSimplifyPass::threadBranchOnPhi(Function &F, BasicBlock *BB) {
	if (!BB || BB == F.front()) return false;
	auto *Br = BB->getBranchTerminator();
	if (!BasicBlock::isCondBr(Br)) return false;

    //1.识别分支条件PHI
    /*
    两种形式：
    %cond = phi i1 [ true, %P0 ], [ false, %P1 ]
    br i1 %cond, label %T, label %F

    %cond = phi i1 [ true, %P0 ], [ false, %P1 ]
    %not = xor i1 %cond, true
    br i1 %not, label %T, label %F
    */
	PhiInst *CondPhi = dynamic_cast<PhiInst *>(Br->getCondition());
	BinaryInst *CondNot = nullptr;
	bool Negated = false;
	if (!CondPhi) {
		auto *MaybeNot = dynamic_cast<BinaryInst *>(Br->getCondition());
		Value *L = nullptr;
		Value *R = nullptr;
		if (!MaybeNot || MaybeNot->getOp() != BinaryInst::XOR ||
			!BinaryInst::getOperands(MaybeNot, L, R)) {
			return false;
		}
		if (Value::isConstOne(R))
			CondPhi = dynamic_cast<PhiInst *>(L);
		else if (Value::isConstOne(L))
			CondPhi = dynamic_cast<PhiInst *>(R);
		if (!CondPhi) return false;
		CondNot = MaybeNot;
		Negated = true;
	}
	if (!CondPhi || CondPhi->getParent() != BB) return false;

    //要求当前块除了分支指令、条件非指令和PHI指令之外没有其他指令
	for (Instruction *I : *BB) {
		if (I == Br || I == CondNot || dynamic_cast<PhiInst *>(I)) continue;
		return false;
	}
    //Cond只能被分支使用
	if (CondNot) {
		for (Use *U : CondNot->getUses()) {
			if (!U || U->getValue() != CondNot) continue;
			if (U->getUser() != Br) return false;
		}
	}

	BasicBlock *TrueBB = Br->getTrueSuccessor();
	BasicBlock *FalseBB = Br->getFalseSuccessor();
	if (!TrueBB || !FalseBB || TrueBB == FalseBB || TrueBB == BB ||
		FalseBB == BB) {
		return false;
	}

    //要求条件 PHI 的 incoming 数量 = BB 的不同前驱数量
	auto Preds = BB->getPredecessors();
	if (Preds.empty() || CondPhi->getNumIncomingValues() != Preds.size())
		return false;

	std::unordered_set<BasicBlock *> UniquePreds;
	std::unordered_map<BasicBlock *, BasicBlock *> Destination;
	for (BasicBlock *Pred : Preds) {
		if (!Pred || !UniquePreds.insert(Pred).second ||
			Pred->countEdgesToSucc(BB) != 1) {
			return false;
		}
		Value *Incoming = CondPhi->getIncomingFromPred(Pred);
		bool Bit = false;
		if (!Incoming || !Incoming->tryGetBoolConst(Bit)) return false;
		Bit ^= Negated;
		Destination[Pred] = Bit ? TrueBB : FalseBB;
	}

	auto BBPhis = BB->collectPhiNodes();
	for (PhiInst *Phi : BBPhis) {
		if (!Phi || Phi->getNumIncomingValues() != Preds.size()) return false;
		std::unordered_set<BasicBlock *> IncomingPreds;
		if (!forEachPhiIncoming(
				Phi, [&](unsigned, Value *, BasicBlock *Pred) {
					return Pred && UniquePreds.count(Pred) &&
						IncomingPreds.insert(Pred).second;
				})) {
			return false;
		}

		for (Use *U : Phi->getUses()) {
			if (!U || U->getValue() != Phi) continue;
			auto *UserI = dynamic_cast<Instruction *>(U->getUser());
			if (Phi == CondPhi &&
				((!CondNot && UserI == Br) || (CondNot && UserI == CondNot)))
				continue;
			auto *SuccPhi = dynamic_cast<PhiInst *>(UserI);
			if (!SuccPhi ||
				(SuccPhi->getParent() != TrueBB &&
				 SuccPhi->getParent() != FalseBB) ||
				SuccPhi->getIncomingFromPred(BB) != Phi) {
				return false;
			}
		}
	}

	for (BasicBlock *Pred : Preds) {
		BasicBlock *Dest = Destination[Pred];
		if (!Dest->collectPhiNodes().empty() && Pred->countEdgesToSucc(Dest) != 0)
			return false;
	}

		std::vector<BranchPhiRewrite> Rewrites;
	for (BasicBlock *Dest : {TrueBB, FalseBB}) {
		for (PhiInst *Phi : Dest->collectPhiNodes()) {
			if (!Phi) return false;
			auto Old = Phi->collectIncomingEdges();
			unsigned FromBBCount = 0;
				BranchPhiRewrite Rewrite;
			Rewrite.Phi = Phi;
			for (const auto &Edge : Old) {
				if (Edge.Pred != BB) {
					Rewrite.Incoming.push_back(Edge);
					continue;
				}
				++FromBBCount;
				for (BasicBlock *Pred : Preds) {
					if (Destination[Pred] != Dest) continue;
					Value *Translated = Edge.V;
					if (auto *FromPhi = dynamic_cast<PhiInst *>(Translated)) {
						if (FromPhi->getParent() == BB)
							Translated = FromPhi->getIncomingFromPred(Pred);
					}
					if (!Translated ||
						(dynamic_cast<Instruction *>(Translated) &&
						 dynamic_cast<Instruction *>(Translated)->getParent() == BB)) {
						return false;
					}
					Rewrite.Incoming.push_back({Translated, Pred});
				}
			}
			if (FromBBCount != 1) return false;
			Rewrites.push_back(std::move(Rewrite));
		}
	}

	for (auto &Rewrite : Rewrites)
		PhiInst::rebuildFromIncoming(Rewrite.Phi, Rewrite.Incoming);

	for (BasicBlock *Pred : Preds) {
		if (!BasicBlock::redirectBranchTarget(Pred, BB, Destination[Pred]))
			return false;
	}

	BB->eraseBlockSafely();
	return true;
}
//7.合并基本块
bool CFGSimplifyPass::mergeBlocks(Function &F) {
	bool Changed = false;
	BasicBlock *Entry = F.front();
	auto Blocks = snapshotPointers(F);

	for (BasicBlock *BB : Blocks) {
		if (!BB || BB == Entry) continue;

        //该块只有一个前驱，该前驱只有一个后继
		auto Preds = BB->getPredecessors();
		if (Preds.size() != 1) continue;
		BasicBlock *Pred = Preds[0];
		if (!Pred || Pred == BB) continue;
		auto PredSuccs = Pred->getSuccessors();
		if (PredSuccs.size() != 1 || PredSuccs[0] != BB) continue;

        //检查是否有自引用的 PHI 节点，如果有则跳过合并
		bool HasSelfRefPhi = false;
		for (PhiInst *Phi : BB->collectPhiNodes()) {
			if (!Phi) continue;
			HasSelfRefPhi = !forEachPhiIncoming(
				Phi, [Phi](unsigned, Value *Incoming, BasicBlock *) {
					return Incoming != Phi;
				});
			if (HasSelfRefPhi) break;
		}
		if (HasSelfRefPhi) continue;

		auto Succs = BB->getSuccessors();

		// 1)该块中的PHI一定是单incoming的，所以可以直接把PHI的incoming替换为前驱块的值，然后删除PHI
		for (PhiInst *Phi : BB->collectPhiNodes()) {
			if (!Phi) continue;
			Value *InV = Phi->getIncomingFromPred(Pred);
			if (!InV) InV = UndefValue::NewUndefValue(Phi->getIRType());
			Phi->replaceAllUsesWith(InV);
			delete Phi;
			Changed = true;
		}

		// 2) 后继 PHI incoming block: BB -> Pred
		for (BasicBlock *Succ : Succs) {
			BasicBlock::replaceIncomingPredInSuccPhis(Succ, BB, Pred);
		}

		// 3) 把 BB 的指令整体搬到 Pred，并删掉 Pred 原终结指令
		BrInst *PredBr = Pred->getBranchTerminator();
		if (!PredBr) continue;
		delete PredBr;

		std::vector<Instruction *> ToMove;
		for (Instruction *I : *BB) {
			ToMove.push_back(I);
		}
		for (Instruction *I : ToMove) {
			I->eraseFromParent();
			Pred->pushBack(I);
		}

		BB->eraseFromParent();
		Changed = true;
	}

	return Changed;
}

bool CFGSimplifyPass::simplifyUncondBranch(Function &F, BasicBlock *BB) {
	if (!BB || BB == F.front()) return false;

	BrInst *Br = BB->getBranchTerminator();
	if (!BasicBlock::isUncondBr(Br)) return false;
	if (!BB->onlyPhiAndTerminator()) return false;

	auto *Succ = Br->getSuccessor(0);
	if (!Succ || Succ == BB) return false;

	auto Preds = BB->getPredecessors();
	if (Preds.empty()) return false;

	auto SuccPhis = Succ->collectPhiNodes();
	std::unordered_set<BasicBlock *> SuccPredSet;
	for (BasicBlock *P : Succ->getPredecessors()) {
		if (P) SuccPredSet.insert(P);
	}
	for (BasicBlock *P : Preds) {
		if (!P) return false;
		if (!SuccPhis.empty() && SuccPredSet.count(P)) return false;
	}

	// BB 中 PHI 必须只被 Succ 的 PHI 使用
	auto BBPhis = BB->collectPhiNodes();
	for (PhiInst *Phi : BBPhis) {
		if (!Phi) continue;
		for (Use *U : Phi->getUses()) {
			if (!U || U->getValue() != Phi) continue;
			auto *UI = dynamic_cast<Instruction *>(U->getUser());
			if (!UI || UI->getParent() != Succ) return false;
			if (!dynamic_cast<PhiInst *>(UI)) return false;
		}
	}

	// 改写 Succ 的 PHI：把来自 BB 的 incoming 展开为来自 Preds 的 incoming
	for (PhiInst *SP : SuccPhis) {
		if (!SP) continue;

		auto In = SP->collectIncomingEdges();
		bool Touched = false;
		std::vector<PhiInst::IncomingEdge> NewIn;

		for (const auto &E : In) {
			if (E.Pred != BB) {
				NewIn.push_back(E);
				continue;
			}

			Touched = true;
			for (BasicBlock *P : Preds) {
				Value *V = E.V;
				if (auto *BBPhi = dynamic_cast<PhiInst *>(V)) {
					if (BBPhi->getParent() == BB) {
						Value *FromP = BBPhi->getIncomingFromPred(P);
						V = FromP ? FromP : UndefValue::NewUndefValue(BBPhi->getIRType());
					}
				}
				NewIn.push_back({V, P});
			}
		}

		if (Touched) {
			PhiInst::rebuildFromIncoming(SP, NewIn);
		}
	}

	// 重定向 Pred -> BB 到 Pred -> Succ
	bool Rewired = false;
	for (BasicBlock *P : Preds) {
		Rewired |= BasicBlock::redirectBranchTarget(P, BB, Succ);
	}
	if (!Rewired) return false;

	BB->eraseBlockSafely();
	return true;
}

bool CFGSimplifyPass::simplifyBranches(Function &F) {
	bool Changed = false;

	{
		auto Blocks = snapshotPointers(F);
		for (BasicBlock *BB : Blocks) {
			if (!BB) continue;
			if (threadBranchOnPhi(F, BB)) {
				Changed = true;
				continue;
			}
			Changed |= simplifyUncondBranch(F, BB);
		}
	}

	return Changed;
}

PreservationStatus CFGSimplifyPass::run(
	Function &F, FunctionAnalysisPassManager &FAM) {
	//入边特化只读取修改前的循环归属快照；第一次克隆后LoopInfo即失效，
	//后续固定点阶段不再查询该分析。
	auto &LI = FAM.getResult<LoopAnalysis>(F);
	std::unordered_set<BasicBlock *> LoopBlocks;
	for (const auto &Entry : LI.BBToLoop) {
		if (Entry.first && Entry.second) LoopBlocks.insert(Entry.first);
	}
	bool ChangedAny = specializeBranchesOnIncomingEdges(F, LoopBlocks);

	while (true) {
        //必须迭代到不动点，因为这些子优化会互相创造优化机会
		bool Changed = false;
        //1.消除死块
		Changed |= removeDeadBlocks(F);
        //2.终结指令常量折叠
		Changed |= foldTerminatorBranches(F);
        //3.折叠已知分支条件
		Changed |= foldKnownBranchConditions(F);
        //4.消除重复PHI节点
		Changed |= eliminateDuplicatePhis(F);
		{
			auto Blocks = snapshotPointers(F);
			for (BasicBlock *BB : Blocks) {
				if (BB) Changed |= threadBranchOnPhi(F, BB);
			}
		}
		//6.规范化布尔PHI。
		Changed |= normalizeBooleanPhis(F);
		//7.合并入边特化产生的空跳板并继续合并普通基本块。
		Changed |= mergeBlocks(F);
		//8.简化条件分支
		Changed |= simplifyBranches(F);

		ChangedAny |= Changed;
		if (!Changed) break;
	}

	if (!ChangedAny) return PreservationStatus::all();
	PreservationStatus PA;
	PA.discard<DominantAnalysis>();
	PA.discard<PostDominantAnalysis>();
	PA.discard<LoopAnalysis>();
	PA.discard<BasicAliasAnalysis>();
	PA.discard<SideEffectAnalysis>();
	return PA;
}
