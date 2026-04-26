#include "ir/opt/IfConversion.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"

#include <vector>

namespace {

struct IfConvMatch {
	BasicBlock *Pred = nullptr;
	BasicBlock *TrueBB = nullptr;
	BasicBlock *ElseBB = nullptr;
	BasicBlock *Succ = nullptr;
	BrInst *PredBr = nullptr;
	Value *Cond = nullptr;
	bool HasElse = false;
	bool CondTrueToTrueBB = false;
};

static bool isConstCond(Value *V) {
	return dynamic_cast<ConstBool *>(V) ||
		   dynamic_cast<ConstInt *>(V) ||
		   dynamic_cast<ConstFloat *>(V);
}

static bool isForbiddenInst(const Instruction *I) {
	if (!I) return false;
	switch (I->instType) {
		case Instruction::STORE:
		case Instruction::LOAD:
		case Instruction::CALL:
		case Instruction::SDIV:
		case Instruction::UDIV:
		case Instruction::SREM:
		case Instruction::FDIV:
		case Instruction::FREM:
			return true;
		default:
			return false;
	}
}

static int countCandidateInsts(BasicBlock *BB) {
	if (!BB) return 0;
	int Cnt = 0;
	for (Instruction *I : *BB) {
		if (!I) continue;
		if (dynamic_cast<BrInst *>(I)) continue;
		++Cnt;
	}
	return Cnt;
}

static bool checkBlockConvertible(BasicBlock *BB) {
	if (!BB) return false;
	for (Instruction *I : *BB) {
		if (!I) continue;
		if (dynamic_cast<BrInst *>(I)) continue;
		if (isForbiddenInst(I)) return false;
	}
	return true;
}

// 仅允许候选块中的指令被本块内部或 Succ 的 PHI 使用，
// 防止将分支内部计算无约束上提，造成性能/语义风险。
static bool usersAreSafeForHoist(BasicBlock *BB, BasicBlock *Succ) {
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

static bool hasExactPreds(BasicBlock *BB, BasicBlock *A, BasicBlock *B) {
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

static bool matchPattern(BasicBlock *TrueBB, IfConvMatch &M) {
	if (!TrueBB) return false;

	auto Preds = TrueBB->getPredecessors();
	auto Succs = TrueBB->getSuccessors();
	if (Preds.size() != 1 || Succs.size() != 1) return false;

	BasicBlock *Pred = Preds[0];
	BasicBlock *Succ = Succs[0];
	if (!Pred || !Succ) return false;

	BrInst *PredBr = Pred->getBranchTerminator();
	if (!PredBr || !BasicBlock::isCondBr(PredBr)) return false;

	auto &UL = PredBr->getUserList();
	if (UL.size() != 3) return false;
	Value *Cond = UL[0]->getValue();
	auto *Dest0 = dynamic_cast<BasicBlock *>(UL[1]->getValue());
	auto *Dest1 = dynamic_cast<BasicBlock *>(UL[2]->getValue());
	if (!Cond || !Dest0 || !Dest1 || isConstCond(Cond)) return false;

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
	if (TrueBr->getUserList().size() != 1 || TrueBr->getUserList()[0]->getValue() != Succ) return false;

	if (HasElse) {
		BrInst *ElseBr = ElseBB->getBranchTerminator();
		if (!ElseBr || !BasicBlock::isUncondBr(ElseBr)) return false;
		if (ElseBr->getUserList().size() != 1 || ElseBr->getUserList()[0]->getValue() != Succ) return false;
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
	if (Succ->collectPhiNodes().empty()) return false;

	int InstCnt = countCandidateInsts(TrueBB) + (HasElse ? countCandidateInsts(ElseBB) : 0);
	if (InstCnt > 4) return false;

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

} // namespace

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
		if (!VFromTrueBB) VFromTrueBB = UndefValue::NewUndefValue(Phi->getIRType());
		if (!VFromOtherArm) VFromOtherArm = UndefValue::NewUndefValue(Phi->getIRType());

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
