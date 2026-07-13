#include "ir/opt/IfConversion.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "ir/opt/DCE.hpp"

#include <vector>

// If-conversion executes both arm computations before the select.  Use an
// allow-list rather than a side-effect deny-list so a newly-added instruction
// cannot accidentally become speculatable by default.
bool IfConversionPass::isSafeToSpeculativelyExecute(const Instruction *I) {
	if (!I) return false;
	switch (I->instType) {
		case Instruction::ADD:
		case Instruction::FADD:
		case Instruction::SUB:
		case Instruction::FSUB:
		case Instruction::MUL:
		case Instruction::FMUL:
		case Instruction::AND:
		case Instruction::OR:
		case Instruction::XOR:
		case Instruction::ICMP:
		case Instruction::FCMP:
			return true;
		case Instruction::SHL:
		case Instruction::LSHR:
		case Instruction::ASHR:
			return hasDefinedConstantShiftAmount(I);
		case Instruction::GEP:
		case Instruction::SELECT:
		case Instruction::TRUNC:
		case Instruction::ZEXT:
		case Instruction::SEXT:
		case Instruction::SITOFP:
		case Instruction::BITCAST:
			return true;
		// Loads/calls/stores can fault, diverge or expose memory effects.  All
		// integer/FP divisions and remainders are also excluded: besides their
		// latency, integer division may observe a zero divisor on the untaken
		// arm.  FPTOSI is excluded because NaN/out-of-range inputs do not have a
		// defined integer result; SITOFP remains safe for every i32.  Unknown/new
		// opcodes remain non-speculatable by default.
		default:
			return false;
	}
}

int IfConversionPass::instructionCost(const Instruction *I) {
	if (!I) return 1000;
	switch (I->instType) {
		case Instruction::MUL:
		case Instruction::FMUL:
		case Instruction::FADD:
		case Instruction::FSUB:
		case Instruction::FCMP:
			return 2;
		case Instruction::SITOFP:
			return 3;
		default:
			return 1;
	}
}

int IfConversionPass::countCandidateInsts(BasicBlock *BB) {
	if (!BB) return 0;
	int Cnt = 0;
	for (Instruction *I : *BB) {
		if (!I) continue;
		if (dynamic_cast<BrInst *>(I)) continue;
		++Cnt;
	}
	return Cnt;
}

int IfConversionPass::candidateCost(BasicBlock *BB) {
	if (!BB) return 0;
	int Cost = 0;
	for (Instruction *I : *BB) {
		if (!I || dynamic_cast<BrInst *>(I)) continue;
		Cost += instructionCost(I);
	}
	return Cost;
}

bool IfConversionPass::checkBlockConvertible(BasicBlock *BB) {
	if (!BB) return false;
	for (Instruction *I : *BB) {
		if (!I) continue;
		if (dynamic_cast<BrInst *>(I)) continue;
		if (!isSafeToSpeculativelyExecute(I)) return false;
	}
	return true;
}

// 仅允许候选块中的指令被本块内部或 Succ 的 PHI 使用，
// 防止将分支内部计算无约束上提，造成性能/语义风险。
bool IfConversionPass::usersAreSafeForHoist(BasicBlock *BB, BasicBlock *Succ) {
	if (!BB || !Succ) return false;
	for (Instruction *I : *BB) {
		if (!I || dynamic_cast<BrInst *>(I)) continue;
		for (auto It = I->getUseList().begin(); It != I->getUseList().end(); ++It) {
			Use *U = *It;
			if (!U || U->getValue() != I) continue;
			auto *UI = dynamic_cast<Instruction *>(U->getUser());
			if (!UI) continue;
			if (UI->getParent() == BB) continue;
			if (UI->getParent() == Succ && dynamic_cast<PhiInst *>(UI)) continue;
			return false;
		}
	}
	return true;
}

bool IfConversionPass::hasExactPreds(BasicBlock *BB, BasicBlock *A, BasicBlock *B) {
	if (!BB || !A || !B) return false;
	auto Preds = BB->getPredecessors();
	if (Preds.size() != 2) return false;
	bool SeenA = false;
	bool SeenB = false;
	for (BasicBlock *P : Preds) {
		if (P == A) SeenA = true;
		else if (P == B) SeenB = true;
		else return false;
	}
	return SeenA && SeenB;
}

bool IfConversionPass::matchPattern(BasicBlock *TrueBB, IfConvMatch &M) {
	if (!TrueBB) return false;

	auto Preds = TrueBB->getPredecessors();
	auto Succs = TrueBB->getSuccessors();
	if (Preds.size() != 1 || Succs.size() != 1) return false;

	BasicBlock *Pred = Preds[0];
	BasicBlock *Succ = Succs[0];
	if (!Pred || !Succ) return false;
	// A convertible triangle/diamond is an acyclic local region.  Allowing
	// Succ == Pred would misclassify a loop backedge as the merge edge; moving
	// the arm into Pred can then create a self-referential select and destroy
	// the loop CFG.  The optional else block is checked against the same three
	// blocks below, so this makes every region block pairwise distinct.
	if (Pred == TrueBB || Succ == TrueBB || Succ == Pred) return false;

	BrInst *PredBr = Pred->getBranchTerminator();
	if (!PredBr || !BasicBlock::isCondBr(PredBr)) return false;

	Value *Cond = PredBr->getCondition();
	auto *Dest0 = PredBr->getTrueSuccessor();
	auto *Dest1 = PredBr->getFalseSuccessor();
	bool ConstCond = false;
	if (!Cond || !IRType::isBool(Cond->getIRType()) || !Dest0 || !Dest1 ||
		Cond->tryGetBoolConst(ConstCond))
		return false;

	BasicBlock *ElseBB = nullptr;
	bool HasElse = false;
	bool CondTrueToTrueBB = false;

	// 情况 1: 一个分支到 True，另一个直接到 Succ（无 else）。
	if ((Dest0 == TrueBB && Dest1 == Succ) || (Dest1 == TrueBB && Dest0 == Succ)) {
		CondTrueToTrueBB = (Dest0 == TrueBB);
		HasElse = false;
	} else {
		// 情况 2: 一个分支到 True，另一个到 Else；Else 必须是单前驱单后继并指向同一个 Succ。
		if (Dest0 == TrueBB && Dest1 != Succ) {
			ElseBB = Dest1;
			HasElse = true;
			CondTrueToTrueBB = true;
		} else if (Dest1 == TrueBB && Dest0 != Succ) {
			ElseBB = Dest0;
			HasElse = true;
			CondTrueToTrueBB = false;
		} else {
			return false;
		}

		if (!ElseBB || ElseBB == TrueBB || ElseBB == Pred || ElseBB == Succ) return false;
		auto ElsePreds = ElseBB->getPredecessors();
		auto ElseSuccs = ElseBB->getSuccessors();
		if (ElsePreds.size() != 1 || ElsePreds[0] != Pred) return false;
		if (ElseSuccs.size() != 1 || ElseSuccs[0] != Succ) return false;
	}

	BrInst *TrueBr = TrueBB->getBranchTerminator();
	if (!TrueBr || !BasicBlock::isUncondBr(TrueBr)) return false;
	if (TrueBr->getSuccessor(0) != Succ) return false;

	if (HasElse) {
		BrInst *ElseBr = ElseBB->getBranchTerminator();
		if (!ElseBr || !BasicBlock::isUncondBr(ElseBr)) return false;
		if (ElseBr->getSuccessor(0) != Succ) return false;
	}

	// 仅处理标准三角/菱形：Succ 不能有额外前驱（例如循环回边）。
	if (HasElse) {
		if (!hasExactPreds(Succ, TrueBB, ElseBB)) return false;
	} else {
		if (!hasExactPreds(Succ, TrueBB, Pred)) return false;
	}

	if (!checkBlockConvertible(TrueBB)) return false;
	if (HasElse && !checkBlockConvertible(ElseBB)) return false;

	// 按算法前提：候选块单前驱，规范 IR 下不应含 PHI。
	// 这里作为结构化前置条件显式约束，避免后续搬移出现非法位置。
	if (!TrueBB->collectPhiNodes().empty()) return false;
	if (HasElse && !ElseBB->collectPhiNodes().empty()) return false;

	if (!usersAreSafeForHoist(TrueBB, Succ)) return false;
	if (HasElse && !usersAreSafeForHoist(ElseBB, Succ)) return false;

	// 没有 PHI 的汇合块不做 IfConversion：收益低，且常引入无谓投机执行。
	auto SuccPhis = Succ->collectPhiNodes();
	if (SuccPhis.empty()) return false;

	// 不为结构不完整的 PHI 凭空制造 undef。精确两前驱的
	// diamond/triangle 中，每个 PHI 都必须每条边恰好一个 incoming。
	BasicBlock *OtherArmBB = HasElse ? ElseBB : Pred;
	for (PhiInst *Phi : SuccPhis) {
		if (!Phi || Phi->getNumIncomingValues() != 2 ||
			!Phi->getIRType() || Phi->getIRType()->getEnum() == IR_VOID)
			return false;
		Value *TrueIncoming = Phi->getIncomingFromPred(TrueBB);
		Value *OtherIncoming = Phi->getIncomingFromPred(OtherArmBB);
		if (!TrueIncoming || !OtherIncoming) return false;
		// A loop-carried PHI may refer to itself (or another PHI in the same
		// header).  Replacing it with a select inserted in Pred would then make
		// the select self-referential or fail to dominate the old header uses.
		// Such values cannot occur in an acyclic if-region and are rejected.
		auto IsDefinedInMerge = [Succ](Value *V) {
			auto *I = dynamic_cast<Instruction *>(V);
			return I && I->getParent() == Succ;
		};
		if (TrueIncoming == Phi || OtherIncoming == Phi ||
			IsDefinedInMerge(TrueIncoming) || IsDefinedInMerge(OtherIncoming))
			return false;
	}

	int InstCnt = countCandidateInsts(TrueBB) + (HasElse ? countCandidateInsts(ElseBB) : 0);
	if (InstCnt > 4) return false;
	// PHI 原本是边上拷贝，转成 select 后是真实指令。将 select
	// 也纳入成本，防止“空分支 + 大量 PHI”反而显著增加代码。
	int WeightedCost = candidateCost(TrueBB) +
		(HasElse ? candidateCost(ElseBB) : 0) +
		static_cast<int>(SuccPhis.size());
	if (SuccPhis.size() > 3 || WeightedCost > 6) return false;

	M.Pred = Pred;
	M.TrueBB = TrueBB;
	M.ElseBB = ElseBB;
	M.Succ = Succ;
	M.PredBr = PredBr;
	M.Cond = Cond;
	M.HasElse = HasElse;
	M.CondTrueToTrueBB = CondTrueToTrueBB;
	return true;
}

bool IfConversionPass::tryConvertOne(Function &, BasicBlock *TrueBB) {
	IfConvMatch M;
	if (!matchPattern(TrueBB, M)) return false;

	std::vector<Instruction *> ToMove;
	for (Instruction *I : *M.TrueBB) {
		if (!I || dynamic_cast<BrInst *>(I)) continue;
		ToMove.push_back(I);
	}
	if (M.HasElse) {
		for (Instruction *I : *M.ElseBB) {
			if (!I || dynamic_cast<BrInst *>(I)) continue;
			ToMove.push_back(I);
		}
	}

	auto Phis = M.Succ->collectPhiNodes();

	// 删除 Pred 原条件分支后，按顺序拼接搬移指令和 select，最后补无条件跳转。
	delete M.PredBr;

	for (Instruction *I : ToMove) {
		I->EraseFromParent();
		M.Pred->push_back(I);
	}

	for (PhiInst *Phi : Phis) {
		if (!Phi) continue;

		Value *VFromTrueBB = Phi->getIncomingFromPred(M.TrueBB);
		BasicBlock *OtherArmBB = M.HasElse ? M.ElseBB : M.Pred;
		Value *VFromOtherArm = Phi->getIncomingFromPred(OtherArmBB);
		// matchPattern proved both incoming values exist; unlike the old path,
		// malformed PHIs are rejected before any CFG mutation instead of being
		// repaired with undef.

		Value *SelTrueV = M.CondTrueToTrueBB ? VFromTrueBB : VFromOtherArm;
		Value *SelFalseV = M.CondTrueToTrueBB ? VFromOtherArm : VFromTrueBB;

		Value *Sel = M.Pred->genSelectInst(M.Cond, SelTrueV, SelFalseV);
		Phi->replaceAllUsesWith(Sel);
		delete Phi;
	}

	M.Pred->genUnCondInst(M.Succ);

	M.TrueBB->eraseBlockSafely();
	if (M.HasElse) {
		M.ElseBB->eraseBlockSafely();
	}

	return true;
}

PreservationStatus IfConversionPass::run(Function &F, FunctionAnalysisPassManager &FAM) {
	(void)FAM;

	bool ChangedAny = false;
	while (true) {
		bool Changed = false;

		std::vector<BasicBlock *> Blocks;
		for (BasicBlock *BB : F) {
			if (BB) Blocks.push_back(BB);
		}

		for (BasicBlock *BB : Blocks) {
			if (tryConvertOne(F, BB)) {
				Changed = true;
				ChangedAny = true;
				break;
			}
		}

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
