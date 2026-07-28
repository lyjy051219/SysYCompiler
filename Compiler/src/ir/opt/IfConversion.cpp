#include "ir/opt/IfConversion.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "ir/opt/DCE.hpp"
#include "lib/IRUtils.hpp"

#include <vector>

bool IfConversionPass::isSafeToSpeculativelyExecute(const Instruction *I) {
	// 2.1 白名单只接纳无副作用且不会新增陷阱/毒值的指令；移位还需常量位数合法。
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

		default:
			return false;
	}
}

int IfConversionPass::instructionCost(const Instruction *I) {
	// 2.2 用简化代价模型限制被无条件执行的工作量，浮点与转换操作提高权重。
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

bool IfConversionPass::usersAreSafeForHoist(BasicBlock *BB, BasicBlock *Succ) {
	// 2.3 分支内定义只能在本分支继续使用，或作为汇合块 PHI 的 incoming；
	// 这样把指令上提到控制块后不会产生跨区域的 SSA 可见性变化。
	if (!BB || !Succ) return false;
	for (Instruction *I : *BB) {
		if (!I || dynamic_cast<BrInst *>(I)) continue;
		for (Use *U : I->getUses()) {
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
	// 1. 从候选臂反向识别 triangle/diamond：控制块条件跳转，两臂至多各一块，
	// 且最终汇合块的前驱必须与该形态精确一致。
	if (!TrueBB) return false;

	auto Preds = TrueBB->getPredecessors();
	auto Succs = TrueBB->getSuccessors();
	if (Preds.size() != 1 || Succs.size() != 1) return false;

	BasicBlock *Pred = Preds[0];
	BasicBlock *Succ = Succs[0];
	if (!Pred || !Succ) return false;

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

	if ((Dest0 == TrueBB && Dest1 == Succ) || (Dest1 == TrueBB && Dest0 == Succ)) {
		CondTrueToTrueBB = (Dest0 == TrueBB);
		HasElse = false;
	} else {

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

	if (HasElse) {
		if (!hasExactPreds(Succ, TrueBB, ElseBB)) return false;
	} else {
		if (!hasExactPreds(Succ, TrueBB, Pred)) return false;
	}

	// 2. 验证两臂可安全推测执行、没有内部 PHI，且所有外部用户均由汇合 PHI 承接。
	if (!checkBlockConvertible(TrueBB)) return false;
	if (HasElse && !checkBlockConvertible(ElseBB)) return false;

	if (!TrueBB->collectPhiNodes().empty()) return false;
	if (HasElse && !ElseBB->collectPhiNodes().empty()) return false;

	if (!usersAreSafeForHoist(TrueBB, Succ)) return false;
	if (HasElse && !usersAreSafeForHoist(ElseBB, Succ)) return false;

	// 3. 汇合块必须确有二元 PHI，并且每个 incoming 都来自预期两条边且不在
	// 汇合块内部定义；这些 PHI 将逐一转换成 select。
	auto SuccPhis = Succ->collectPhiNodes();
	if (SuccPhis.empty()) return false;

	BasicBlock *OtherArmBB = HasElse ? ElseBB : Pred;
	for (PhiInst *Phi : SuccPhis) {
		if (!Phi || Phi->getNumIncomingValues() != 2 ||
			!Phi->getIRType() || Phi->getIRType()->isVoid())
			return false;
		Value *TrueIncoming = Phi->getIncomingFromPred(TrueBB);
		Value *OtherIncoming = Phi->getIncomingFromPred(OtherArmBB);
		if (!TrueIncoming || !OtherIncoming) return false;

		auto IsDefinedInMerge = [Succ](Value *V) {
			auto *I = dynamic_cast<Instruction *>(V);
			return I && I->getParent() == Succ;
		};
		if (TrueIncoming == Phi || OtherIncoming == Phi ||
			IsDefinedInMerge(TrueIncoming) || IsDefinedInMerge(OtherIncoming))
			return false;
	}

	// 4. 最后应用代码尺寸与加权成本预算，避免消除分支却增加过多无条件计算。
	int InstCnt = countCandidateInsts(TrueBB) + (HasElse ? countCandidateInsts(ElseBB) : 0);
	if (InstCnt > 4) return false;

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
	// 5. 先完成整套模式与合法性检查，之后的 CFG/SSA 改写不再允许部分失败。
	IfConvMatch M;
	if (!matchPattern(TrueBB, M)) return false;

	std::vector<Instruction *> ToMove;
	for (Instruction *I : collectInstructions(*M.TrueBB)) {
		if (!I || dynamic_cast<BrInst *>(I)) continue;
		ToMove.push_back(I);
	}
	if (M.HasElse) {
		for (Instruction *I : collectInstructions(*M.ElseBB)) {
			if (!I || dynamic_cast<BrInst *>(I)) continue;
			ToMove.push_back(I);
		}
	}

	auto Phis = M.Succ->collectPhiNodes();

	// 5.1 删除条件分支，把两臂的可推测指令按原有臂内顺序移到控制块。
	delete M.PredBr;

	for (Instruction *I : ToMove) {
		I->eraseFromParent();
		M.Pred->pushBack(I);
	}

	// 5.2 根据条件边方向构造 select 替换汇合 PHI，随后建立直达汇合块的边。
	for (PhiInst *Phi : Phis) {
		if (!Phi) continue;

		Value *VFromTrueBB = Phi->getIncomingFromPred(M.TrueBB);
		BasicBlock *OtherArmBB = M.HasElse ? M.ElseBB : M.Pred;
		Value *VFromOtherArm = Phi->getIncomingFromPred(OtherArmBB);

		Value *SelTrueV = M.CondTrueToTrueBB ? VFromTrueBB : VFromOtherArm;
		Value *SelFalseV = M.CondTrueToTrueBB ? VFromOtherArm : VFromTrueBB;

		Value *Sel = M.Pred->genSelectInst(M.Cond, SelTrueV, SelFalseV);
		Phi->replaceAllUsesWith(Sel);
		delete Phi;
	}

	M.Pred->genUnCondInst(M.Succ);

	// 5.3 SSA 用户已重连，安全删除失去入口的臂块及其旧终结指令。
	M.TrueBB->eraseBlockSafely();
	if (M.HasElse) {
		M.ElseBB->eraseBlockSafely();
	}

	return true;
}

PreservationStatus IfConversionPass::run(Function &F, FunctionAnalysisPassManager &FAM) {
	(void)FAM;

	// 1—5. 每轮只转换一个候选并重新快照 CFG，避免一次改写让后续块指针或前驱信息陈旧。
	bool ChangedAny = false;
	while (true) {
		bool Changed = false;

		auto Blocks = snapshotPointers(F);

		for (BasicBlock *BB : Blocks) {
			if (!BB) continue;
			if (tryConvertOne(F, BB)) {
				Changed = true;
				ChangedAny = true;
				break;
			}
		}

		if (!Changed) break;
	}

	if (!ChangedAny) return PreservationStatus::all();

	// 6. 分支、基本块和数据流均已变化，失效所有依赖 CFG/内存可达性的函数分析。
	PreservationStatus PA;
	PA.discard<DominantAnalysis>();
	PA.discard<PostDominantAnalysis>();
	PA.discard<LoopAnalysis>();
	PA.discard<BasicAliasAnalysis>();
	PA.discard<SideEffectAnalysis>();
	return PA;
}
