#include "ir/opt/CFGSimpify.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"
#include <algorithm>
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

// Return whether A and B denote the same boolean condition.  Negated is true
// when A == !B.  Keep this deliberately small and purely syntactic: it is used
// by CFG simplification, where a false positive would select the wrong edge.
bool CFGSimplifyPass::matchBooleanCondition(
	Value *A, Value *B, bool &Negated) {
	Negated = false;
	if (!A || !B || !IRType::isBool(A->getIRType()) ||
		!IRType::isBool(B->getIRType()) || Value::isUndef(A) ||
		Value::isUndef(B)) {
		return false;
	}

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

	bool NotA = false;
	bool NotB = false;
	A = stripNot(A, NotA);
	B = stripNot(B, NotB);
	if (A == B) {
		Negated = NotA != NotB;
		return true;
	}

	// Integer comparisons with identical operands are also safe to correlate.
	// Do not infer logical complements for the floating unordered predicates.
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

// Map one arm of Controller's branch to the predecessor through which that
// arm reaches Merge.  A short unique-predecessor trampoline chain is allowed;
// this covers the common diamond and "direct edge + trampoline" shapes.
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

// A controller accepted by resolveArmIncomingPred can be at most four unique-
// predecessor trampoline edges above an incoming block.  Walking those local
// ancestors avoids scanning every block (and rescanning every predecessor)
// for each boolean PHI in a large function.
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

//消除死块
bool CFGSimplifyPass::removeDeadBlocks(Function &F) {
	bool Changed = false;
	BasicBlock *Entry = F.front();

	std::vector<BasicBlock *> Blocks;
	for (BasicBlock *BB : F) Blocks.push_back(BB);
    //排除入口块之后，检查前驱块：没有/唯一的前驱是自己，满足条件删除
	for (BasicBlock *BB : Blocks) {
		if (!BB || BB == Entry) continue;

		auto Preds = BB->getPredecessors();
		bool NoPred = Preds.empty();
		bool OnlySelfPred = (Preds.size() == 1 && Preds[0] == BB);
		if (!NoPred && !OnlySelfPred) continue;

		auto Succs = BB->getSuccessors();
		for (BasicBlock *Succ : Succs) {
			int EdgeCnt = BB->countEdgesToSucc(Succ);
            //如果该基本块到后继的边删除了：调用下面这个函数就更新掉后继中PHI来自该基本块的输入值
			if (EdgeCnt > 0) BasicBlock::removeEdgeFromSuccPhis(Succ, BB, EdgeCnt);
		}
        //删除基本块连同里面的所有指令
		BB->eraseBlockSafely();
		Changed = true;
	}

	return Changed;
}
//终结指令常量折叠
bool CFGSimplifyPass::foldTerminatorBranches(Function &F) {
	bool Changed = false;

	std::vector<BasicBlock *> Blocks;
	for (BasicBlock *BB : F) Blocks.push_back(BB);

	for (BasicBlock *BB : Blocks) {
		BrInst *Br = BB->getBranchTerminator();
		if (!BasicBlock::isCondBr(Br)) continue;

		Value *Cond = Br->getCondition();
		auto *TrueBB = Br->getTrueSuccessor();
		auto *FalseBB = Br->getFalseSuccessor();
		if (!TrueBB || !FalseBB) continue;

		if (TrueBB == FalseBB) {
			// cond 分支两条边都到同一个块：删掉一条多余边
			BasicBlock::removeEdgeFromSuccPhis(TrueBB, BB, 1);
			BasicBlock::replaceCondBrWithUncond(BB, TrueBB);
			Changed = true;
			continue;
		}

		bool CondVal = false;
		if (!Cond || !Cond->tryGetBoolConst(CondVal)) continue;

		BasicBlock *Taken = CondVal ? TrueBB : FalseBB;
		BasicBlock *Untaken = CondVal ? FalseBB : TrueBB;

		BasicBlock::removeEdgeFromSuccPhis(Untaken, BB, 1);
		BasicBlock::replaceCondBrWithUncond(BB, Taken);
		Changed = true;
	}

	return Changed;
}
//消除重复PHI节点
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

// Canonicalize integer 0/1 PHIs into boolean SSA.  For an ordinary two-arm
// diamond, remove the PHI entirely and reuse the controlling condition.  For
// larger short-circuit graphs, keep an i1 PHI plus one zext; this preserves the
// exact CFG semantics while exposing boolean folding to later passes.  An
// existing i1 PHI is only changed by a trivial same-constant fold or the proven
// diamond replacement; it never gets another PHI or a redundant zext wrapper.
bool CFGSimplifyPass::normalizeBooleanPhis(Function &F) {
	bool Changed = false;
	std::vector<BasicBlock *> Blocks;
	for (BasicBlock *BB : F) Blocks.push_back(BB);
	// This routine only replaces instructions, so the predecessor cache remains
	// valid for its whole invocation.
	const PredecessorMap Preds = buildPredecessorMap(F);

	for (BasicBlock *BB : Blocks) {
		if (!BB) continue;
		auto Phis = BB->collectPhiNodes();
		for (PhiInst *Phi : Phis) {
			const bool IsBoolPhi =
				Phi && IRType::isBool(Phi->getIRType());
			if (!Phi || Phi->getParent() != BB ||
				!IRType::isInt(Phi->getIRType()) ||
				(!IsBoolPhi && Phi->getIRType()->getSize() != 4)) {
				continue;
			}

			std::vector<bool> Bits;
			Bits.reserve(Phi->getNumIncomingValues());
			bool AllBooleanConstants = Phi->getNumIncomingValues() >= 2;
			for (unsigned I = 0; I < Phi->getNumIncomingValues(); ++I) {
				bool Bit = false;
				Value *Incoming = Phi->getIncomingValue(I);
				if (auto *CB = dynamic_cast<ConstBool *>(Incoming)) {
					Bit = CB->getVal();
				} else if (auto *CI = dynamic_cast<ConstInt *>(Incoming)) {
					if (CI->getVal() != 0 && CI->getVal() != 1) {
						AllBooleanConstants = false;
						break;
					}
					Bit = CI->getVal() == 1;
				} else {
					AllBooleanConstants = false;
					break;
				}
				Bits.push_back(Bit);
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
			// The function entry has an implicit incoming edge which is absent from
			// getPredecessors().  Never derive its current value from a loop backedge.
			if (BB != F.front() && Phi->getNumIncomingValues() == 2 &&
				Bits[0] != Bits[1]) {
				BasicBlock *P0 = Phi->getIncomingBlock(0);
				BasicBlock *P1 = Phi->getIncomingBlock(1);
				if (!P0 || !P1 || P0 == P1) {
					// Duplicate predecessor entries are edge-order-sensitive in this IR;
					// a block identity alone cannot tell which value is the true arm.
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
				BoolPhi = new PhiInst(INTType::NewBool());
				for (unsigned I = 0; I < Phi->getNumIncomingValues(); ++I) {
					BoolPhi->addIncoming(ConstBool::newConstBool(Bits[I]),
									 Phi->getIncomingBlock(I));
				}
				Phi->replaceWith(BoolPhi);
				BooleanValue = BoolPhi;
			}

			auto *Ext = new ZextInst(Phi->getIRType());
			Ext->addUse(BooleanValue);
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
			if (!BoolPhi) Phi->EraseFromParent();
			delete Phi;
			Changed = true;
		}
	}

	return Changed;
}

// Correlate a block's condition with the condition on every incoming edge.
// When all predecessors imply the same truth value, fold the branch.  A
// trivial block then becomes an unconditional trampoline and the existing
// simplifier performs the actual jump threading.
bool CFGSimplifyPass::foldKnownBranchConditions(Function &F) {
	bool Changed = false;
	std::vector<BasicBlock *> Blocks;
	for (BasicBlock *BB : F) Blocks.push_back(BB);

	for (BasicBlock *BB : Blocks) {
		// The function entry also has an implicit predecessor.  Looking only at
		// explicit CFG predecessors there would let a loop backedge incorrectly
		// determine the entry block's condition on its first/current execution.
		if (!BB || BB == F.front()) continue;
		auto *Br = BB->getBranchTerminator();
		if (!BasicBlock::isCondBr(Br)) continue;
		Value *Cond = Br->getCondition();
		if (!Cond) continue;

		auto Preds = BB->getPredecessors();
		if (Preds.empty()) continue;
		bool HaveKnown = false;
		bool KnownValue = false;
		bool AllKnownAndEqual = true;

		for (BasicBlock *Pred : Preds) {
			// An edge from the block to itself describes the previous iteration's
			// condition, not the condition recomputed in the current iteration.
			if (Pred == BB) {
				AllKnownAndEqual = false;
				break;
			}
			auto *PBr = Pred ? Pred->getBranchTerminator() : nullptr;
			if (!BasicBlock::isCondBr(PBr)) {
				AllKnownAndEqual = false;
				break;
			}

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
			if (!HaveKnown) {
				HaveKnown = true;
				KnownValue = ThisValue;
			} else if (KnownValue != ThisValue) {
				AllKnownAndEqual = false;
				break;
			}
		}

		if (!HaveKnown || !AllKnownAndEqual) continue;
		BasicBlock *Taken = KnownValue ? Br->getTrueSuccessor()
								   : Br->getFalseSuccessor();
		BasicBlock *Untaken = KnownValue ? Br->getFalseSuccessor()
									 : Br->getTrueSuccessor();
		if (!Taken || !Untaken) continue;
		if (Taken != Untaken)
			BasicBlock::removeEdgeFromSuccPhis(Untaken, BB, 1);
		if (BasicBlock::replaceCondBrWithUncond(BB, Taken)) Changed = true;
	}

	return Changed;
}

// Thread every incoming edge through a branch whose condition is a constant
// PHI on that edge:
//
//   P0 --\                 P0 ----> T
//         B: c = phi       P1 ----> F
//   P1 --/   br c
//
// B must contain only PHIs and the branch.  Unlike cloning-based jump
// threading this never duplicates work.  Successor PHIs are expanded with the
// edge-specific values from B before the block is removed.
bool CFGSimplifyPass::threadBranchOnPhi(Function &F, BasicBlock *BB) {
	if (!BB || BB == F.front()) return false;
	auto *Br = BB->getBranchTerminator();
	if (!BasicBlock::isCondBr(Br)) return false;

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

	for (Instruction *I : *BB) {
		if (I == Br || I == CondNot || dynamic_cast<PhiInst *>(I)) continue;
		return false;
	}
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

	// Every PHI in B must have one unambiguous value per incoming block.  Its
	// uses may only be the branch or a successor PHI on B's outgoing edge.
	auto BBPhis = BB->collectPhiNodes();
	for (PhiInst *Phi : BBPhis) {
		if (!Phi || Phi->getNumIncomingValues() != Preds.size()) return false;
		std::unordered_set<BasicBlock *> IncomingPreds;
		for (unsigned I = 0; I < Phi->getNumIncomingValues(); ++I) {
			BasicBlock *Pred = Phi->getIncomingBlock(I);
			if (!Pred || !UniquePreds.count(Pred) ||
				!IncomingPreds.insert(Pred).second) {
				return false;
			}
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

	// A predecessor that already reaches the chosen destination would create
	// duplicate predecessor entries.  The IR cannot attach distinct PHI values
	// to those edges, so reject that shape whenever the destination has PHIs.
	for (BasicBlock *Pred : Preds) {
		BasicBlock *Dest = Destination[Pred];
		if (!Dest->collectPhiNodes().empty() && Pred->countEdgesToSucc(Dest) != 0)
			return false;
	}

	// Build all successor-PHI rewrites before mutating the CFG.  An incoming
	// value defined by a PHI in B is translated to that PHI's value on Pred->B.
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
//合并基本块
bool CFGSimplifyPass::mergeBlocks(Function &F) {
	bool Changed = false;
	BasicBlock *Entry = F.front();

	std::vector<BasicBlock *> Blocks;
	for (BasicBlock *BB : F) Blocks.push_back(BB);

	for (BasicBlock *BB : Blocks) {
		if (!BB || BB == Entry) continue;

		auto Preds = BB->getPredecessors();
		if (Preds.size() != 1) continue;

		BasicBlock *Pred = Preds[0];
		if (!Pred || Pred == BB) continue;

		auto PredSuccs = Pred->getSuccessors();
		if (PredSuccs.size() != 1 || PredSuccs[0] != BB) continue;

		bool HasSelfRefPhi = false;
		for (PhiInst *Phi : BB->collectPhiNodes()) {
			if (!Phi) continue;
			for (unsigned i = 0; i < Phi->getNumIncomingValues(); ++i) {
				if (Phi->getIncomingValue(i) == Phi) {
					HasSelfRefPhi = true;
					break;
				}
			}
			if (HasSelfRefPhi) break;
		}
		if (HasSelfRefPhi) continue;

		auto Succs = BB->getSuccessors();

		// 1) 处理当前块中的 PHI（唯一前驱场景）
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
			I->EraseFromParent();
			Pred->push_back(I);
		}

		BB->EraseFromParent();
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

	// If Succ has PHIs, overlapping predecessors would create two incoming
	// edges from the same block whose edge-specific values cannot be represented
	// unambiguously by this IR.  Without PHIs, redirecting is always safe and a
	// subsequent fold turns the resulting two-way-to-one-way branch into an
	// unconditional jump.
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
		std::vector<BasicBlock *> Blocks;
		for (BasicBlock *BB : F) Blocks.push_back(BB);
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

PreservationStatus CFGSimplifyPass::run(Function &F, FunctionAnalysisPassManager &) {
	bool ChangedAny = false;

	while (true) {
		bool Changed = false;

		Changed |= removeDeadBlocks(F);
		Changed |= foldTerminatorBranches(F);
		Changed |= foldKnownBranchConditions(F);
		Changed |= eliminateDuplicatePhis(F);
		// Thread while the edge-specific constants are still represented by a
		// PHI.  normalizeBooleanPhis may replace a two-arm PHI with its remote
		// controller condition, which is useful algebraically but would lose the
		// local edge facts needed by this zero-duplication transform.
		{
			std::vector<BasicBlock *> Blocks;
			for (BasicBlock *BB : F) Blocks.push_back(BB);
			for (BasicBlock *BB : Blocks) {
				if (BB) Changed |= threadBranchOnPhi(F, BB);
			}
		}
		Changed |= normalizeBooleanPhis(F);
		Changed |= mergeBlocks(F);
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
