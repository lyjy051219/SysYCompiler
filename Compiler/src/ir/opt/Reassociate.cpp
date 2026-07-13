#include "ir/opt/Reassociate.hpp"
#include "ir/opt/DCE.hpp"

#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "lib/TraverseTools.hpp"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using RankMap = std::unordered_map<Value *, uint32_t>;

bool ReassociatePass::isDeadInstruction(
	Instruction *I, FunctionAnalysisPassManager &FAM) {
	if (!I) return false;
	if (I->countInstructionUses() > 0) return false;

	if (I->instType == Instruction::RET || I->instType == Instruction::BR ||
		I->instType == Instruction::PHI || I->instType == Instruction::STORE) {
		return false;
	}

	if (auto *CI = dynamic_cast<CallInst *>(I)) {
		if (dceCallHasSideEffect(CI, FAM)) return false;
	}

	return true;
}

bool ReassociatePass::isAlreadyBuilt(
	Instruction *I, BinaryInst::Operation Op,
	const std::vector<Value *> &Ops) {
	if (!I) return false;
	if (Ops.size() < 2) return false;

	Value *Cur = I;
	for (size_t i = 0; i < Ops.size() - 1; ++i) {
		auto *BI = dynamic_cast<BinaryInst *>(Cur);
		if (!BI || BI->getOp() != Op) return false;

		Value *L = nullptr;
		Value *R = nullptr;
		if (!BI->getOperands(L, R)) return false;

		if (i == Ops.size() - 2) {
			return (L == Ops[i] && R == Ops[i + 1]) || (L == Ops[i + 1] && R == Ops[i]);
		}

		if (L != Ops[i]) return false;
		Cur = R;
	}

	return true;
}

class ReassociateEngine {
public:
	ReassociateEngine(Function &F, FunctionAnalysisPassManager &FAM,
		bool PreserveAddressArithmetic)
		: F(F), FAM(FAM),
		  PreserveAddressArithmetic(PreserveAddressArithmetic) {}

	bool run() {
		initRankMap();

		bool AnyChanged = false;
		bool LocalChanged = false;
		do {
			LocalChanged = false;
			std::vector<BasicBlock *> Roots;
			if (F.front()) Roots.push_back(F.front());
			auto RPO = TraverseTools::buildRPO<BasicBlock>(Roots, F);
			for (BasicBlock *BB : RPO) {
				if (!BB) continue;
				std::vector<Instruction *> Insts;
				for (auto *I : *BB) Insts.push_back(I);
				for (Instruction *I : Insts) {
					if (!I || !I->getParent()) continue;
					if (ReassociatePass::isDeadInstruction(I, FAM)) {
						continue;
					}
					LocalChanged |= optimizeInst(I);
				}
			}
			LocalChanged |= processTodo();

			AnyChanged |= LocalChanged;
		} while (LocalChanged);

		return AnyChanged;
	}

private:
	Function &F;
	FunctionAnalysisPassManager &FAM;
	bool PreserveAddressArithmetic = false;
	RankMap Ranks;
	std::unordered_map<Value *, uint64_t> StableOrder;
	uint64_t NextStableOrder = 0;
	std::unordered_map<BasicBlock *, uint32_t> BlockBase;
	std::unordered_map<BasicBlock *, uint32_t> BlockSeq;
	std::vector<Instruction *> Todo;
	std::unordered_set<Instruction *> InTodo;

	void initRankMap() {
		Ranks.clear();
		StableOrder.clear();
		NextStableOrder = 0;
		BlockBase.clear();
		BlockSeq.clear();
		Todo.clear();
		InTodo.clear();

		uint32_t Rank = 2;
		for (auto &P : F.getParams()) {
			Value *V = P.get();
			if (!V) continue;
			StableOrder[V] = ++NextStableOrder;
			Ranks[V] = ++Rank;
		}

		std::vector<BasicBlock *> Roots;
		if (F.front()) Roots.push_back(F.front());
		auto RPO = TraverseTools::buildRPO<BasicBlock>(Roots, F);
		for (BasicBlock *BB : RPO) {
			if (!BB) continue;
			uint32_t Base = (++Rank) << 16;
			BlockBase[BB] = Base;
			uint32_t Seq = 0;
			for (auto *I : *BB) {
				if (!I) continue;
				StableOrder[I] = ++NextStableOrder;

				bool ShouldSkip = false;
				if (auto *BI = dynamic_cast<BinaryInst *>(I)) {
					BinaryInst::Operation Op = BI->getOp();
					ShouldSkip = I->getIRType() && I->getIRType()->isInt() &&
						BinaryInst::isReassociateOp(Op);
				}

				if (ShouldSkip) continue;
				Ranks[I] = Base + (++Seq);
			}
			BlockSeq[BB] = Seq;
		}
	}

	uint32_t getRank(Value *V) const {
		if (!V) return 0;
		if (V && V->isConst()) return 1;
		// Rebuilding an unrolled reduction from purely textual ranks can bury an
		// earlier divide/remainder at the bottom of a long add tree.  Those
		// operations dominate the latency of the next unrolled lane, so rank their
		// results above ordinary same-block values and attach them near the root of
		// the rebuilt tree.  This changes only operand association for integer
		// associative operators; it does not move or speculate the operation.
		if (auto *I = dynamic_cast<Instruction *>(V)) {
			switch (I->instType) {
				case Instruction::SDIV:
				case Instruction::UDIV:
				case Instruction::SREM:
				case Instruction::UREM:
					return UINT32_MAX;
				default:
					break;
			}
		}
		auto It = Ranks.find(V);
		if (It != Ranks.end()) return It->second;
		return 2;
	}

	bool rankComesBefore(Value *A, Value *B) const {
		uint32_t RA = getRank(A);
		uint32_t RB = getRank(B);
		if (RA != RB) return RA > RB;
		auto AI = StableOrder.find(A);
		auto BI = StableOrder.find(B);
		if (AI != StableOrder.end() && BI != StableOrder.end())
			return AI->second > BI->second;
		if (AI != StableOrder.end()) return true;
		if (BI != StableOrder.end()) return false;
		return false;
	}

	void stampRank(Instruction *I) {
		if (!I) return;
		if (!StableOrder.count(I))
			StableOrder[I] = ++NextStableOrder;
		if (auto *BI = dynamic_cast<BinaryInst *>(I)) {
			if (I->getIRType() && I->getIRType()->isInt() &&
				BinaryInst::isReassociateOp(BI->getOp())) {
				// Keep reassociate nodes at neutral rank to avoid reorder oscillation.
				Ranks.erase(I);
				return;
			}
		}
		BasicBlock *BB = I->getParent();
		if (!BB) return;
		uint32_t Base = 0;
		auto BI = BlockBase.find(BB);
		if (BI != BlockBase.end()) Base = BI->second;
		else {
			Base = 3u << 16;
			BlockBase[BB] = Base;
			BlockSeq[BB] = 0;
		}
		Ranks[I] = Base + (++BlockSeq[BB]);
	}

	void pushTodo(Instruction *I) {
		if (!I || !I->getParent()) return;
		if (InTodo.insert(I).second) Todo.push_back(I);
	}


	bool processTodo() {
		bool Changed = false;
		while (!Todo.empty()) {
			Instruction *I = Todo.back();
			Todo.pop_back();
			InTodo.erase(I);
			if (!I || !I->getParent()) continue;
			if (ReassociatePass::isDeadInstruction(I, FAM)) continue;
			Changed |= optimizeInst(I);
		}
		return Changed;
	}

	bool feedsAddressComputation(Instruction *Root) const {
		if (!Root) return false;
		std::vector<Instruction *> WorkList{Root};
		std::unordered_set<Instruction *> Visited;
		while (!WorkList.empty()) {
			Instruction *I = WorkList.back();
			WorkList.pop_back();
			if (!I || !Visited.insert(I).second) continue;
			for (auto It = I->getUseList().begin(); It != I->getUseList().end();
				 ++It) {
				Use *U = *It;
				if (!U || U->getValue() != I) continue;
				auto *UserI = dynamic_cast<Instruction *>(U->getUser());
				if (!UserI) continue;
				if (dynamic_cast<GetElementPtrInst *>(UserI)) return true;
				// Address indices may be widened before the GEP.  Follow only these
				// lossless/width-changing integer wrappers; arbitrary arithmetic users
				// belong to a different expression and are reconsidered at their root.
				if (UserI->instType == Instruction::ZEXT ||
					UserI->instType == Instruction::SEXT ||
					UserI->instType == Instruction::TRUNC) {
					WorkList.push_back(UserI);
				}
			}
		}
		return false;
	}

	Instruction *createBinaryBefore(Instruction *Pos, BinaryInst::Operation Op, Value *LHS, Value *RHS) {
		if (!Pos || !LHS || !RHS) return nullptr;
		BasicBlock *BB = Pos->getParent();
		if (!BB) return nullptr;

		auto *NI = new BinaryInst(LHS, Op, RHS);
		// O(1) insertion: avoid scanning the whole block on every rebuild step.
		Pos->insertBefore(NI);
		stampRank(NI);
		pushTodo(NI);
		return NI;
	}

	bool flattenMulFactors(Value *V, std::vector<Value *> &Factors) {
		auto *BI = dynamic_cast<BinaryInst *>(V);
		if (!BI || BI->getOp() != BinaryInst::MUL ||
			BI->countInstructionUses() != 1) {
			Factors.push_back(V);
			return false;
		}
		Value *L = nullptr;
		Value *R = nullptr;
		if (!BI->getOperands(L, R)) {
			Factors.push_back(V);
			return false;
		}
		flattenMulFactors(L, Factors);
		flattenMulFactors(R, Factors);
		return true;
	}

	bool buildBalancedProduct(Instruction *Root, std::vector<Value *> Terms, Value *&Out) {
		Out = nullptr;
		if (Terms.empty()) return false;
		if (Terms.size() == 1) {
			Out = Terms[0];
			return true;
		}

		Value *Acc = Terms.back();
		Terms.pop_back();
		while (!Terms.empty()) {
			Value *Next = Terms.back();
			Terms.pop_back();
			Instruction *NI = createBinaryBefore(Root, BinaryInst::MUL, Next, Acc);
			if (!NI) return false;
			Acc = NI;
		}
		Out = Acc;
		return true;
	}

	Value *buildMulPowProduct(Instruction *Root,
			const std::vector<std::pair<Value *, int>> &PowList,
			bool &Changed) {
		if (PowList.empty()) return nullptr;

		std::unordered_map<int, std::vector<Value *>> ByPow;
		for (const auto &KV : PowList) {
			if (KV.second <= 0 || !KV.first) continue;
			ByPow[KV.second].push_back(KV.first);
		}

		std::vector<int> Powers;
		Powers.reserve(ByPow.size());
		for (const auto &KV : ByPow) Powers.push_back(KV.first);
		std::sort(Powers.begin(), Powers.end());

		std::vector<std::pair<Value *, int>> Normalized;
		for (int P : Powers) {
			auto &Bases = ByPow[P];
			if (Bases.size() == 1) {
				Normalized.push_back({Bases[0], P});
				continue;
			}

			Value *Merged = nullptr;
			if (!buildBalancedProduct(Root, Bases, Merged)) return nullptr;
			if (!Merged) return nullptr;
			Normalized.push_back({Merged, P});
			Changed = true;
		}

		std::vector<Value *> OuterProduct;
		std::vector<std::pair<Value *, int>> Halved;
		int MaxHalfPow = 0;

		for (auto &KV : Normalized) {
			Value *Base = KV.first;
			int Pow = KV.second;
			if (!Base || Pow <= 0) continue;
			if (Pow & 1) OuterProduct.push_back(Base);
			Pow >>= 1;
			if (Pow > 0) {
				Halved.push_back({Base, Pow});
				MaxHalfPow = std::max(MaxHalfPow, Pow);
			}
		}

		if (MaxHalfPow > 0) {
			Value *SquareRoot = buildMulPowProduct(Root, Halved, Changed);
			if (!SquareRoot) return nullptr;
			OuterProduct.push_back(SquareRoot);
			OuterProduct.push_back(SquareRoot);
		}

		if (OuterProduct.empty()) return ConstInt::newConstInt(1);

		Value *Out = nullptr;
		if (!buildBalancedProduct(Root, OuterProduct, Out)) return nullptr;
		return Out;
	}

	bool matchMulByMinusOne(Value *V, Value *&X) {
		X = nullptr;
		auto *BI = dynamic_cast<BinaryInst *>(V);
		if (!BI || BI->getOp() != BinaryInst::MUL) return false;
		Value *L = nullptr;
		Value *R = nullptr;
		if (!BI->getOperands(L, R)) return false;
		if (L->isConstMinusOne()) {
			X = R;
			return true;
		}
		if (R->isConstMinusOne()) {
			X = L;
			return true;
		}
		return false;
	}

	bool buildAddFromTerms(Instruction *Root, std::vector<Value *> Terms, Value *&Out) {
		Out = nullptr;
		if (Terms.empty()) return false;
		if (Terms.size() == 1) {
			Out = Terms[0];
			return true;
		}

		Value *Acc = Terms.back();
		Terms.pop_back();
		while (!Terms.empty()) {
			Value *Next = Terms.back();
			Terms.pop_back();
			auto *NI = createBinaryBefore(Root, BinaryInst::ADD, Next, Acc);
			if (!NI) return false;
			Acc = NI;
		}
		Out = Acc;
		return true;
	}

	Value *optExp(Instruction *Root, BinaryInst::Operation Op,
			std::vector<Value *> &Ops, bool &Changed) {
		if (Ops.empty()) return nullptr;

		std::vector<Value *> NonConst;
		std::vector<Value *> Consts;
		NonConst.reserve(Ops.size());
		Consts.reserve(Ops.size());
		for (Value *V : Ops) {
			if (V && V->isConst()) Consts.push_back(V);
			else NonConst.push_back(V);
		}

		Value *TotalConst = nullptr;
		for (auto It = Consts.rbegin(); It != Consts.rend(); ++It) {
			Value *C = *It;
			if (!TotalConst) {
				TotalConst = C;
				continue;
			}
			Value *Folded = BinaryInst::foldConstants(Op, TotalConst, C);
			if (Folded) {
				TotalConst = Folded;
				Changed = true;
			}
		}

		Ops.swap(NonConst);
		if (Ops.empty()) return TotalConst;

		if (TotalConst) {
			if (Op == BinaryInst::MUL && TotalConst->isConstZero()) return TotalConst;
			if (!((Op == BinaryInst::ADD && TotalConst->isConstZero()) ||
				(Op == BinaryInst::MUL && TotalConst->isConstOne()))) {
				Ops.push_back(TotalConst);
			}
		}

		if (Ops.size() == 1) return Ops[0];

		if (Op == BinaryInst::ADD) {
			std::unordered_map<Value *, int> Cnt;
			Cnt.reserve(Ops.size());
			for (Value *V : Ops) ++Cnt[V];

			if (!Cnt.empty()) {
				std::vector<Value *> NewOps;
				std::unordered_set<Value *> Seen;
				NewOps.reserve(Ops.size());
				Seen.reserve(Ops.size());
				for (Value *V : Ops) {
					if (!Seen.insert(V).second) continue;
					int K = Cnt[V];
					if (K <= 1) {
						NewOps.push_back(V);
						continue;
					}

					Instruction *MulI = createBinaryBefore(
						Root, BinaryInst::MUL, ConstInt::newConstInt(K), V);
					if (!MulI) {
						NewOps.push_back(V);
						continue;
					}
					NewOps.push_back(MulI);
					Changed = true;
				}
				Ops.swap(NewOps);
			}

			if (Ops.size() >= 2) {
				// Pairwise cancellation only: remove exactly one X and one (-X) each time.
				// Use buckets to keep linear complexity on very long terms.
				std::vector<char> Removed(Ops.size(), 0);
				bool CanceledAny = false;
				std::unordered_map<Value *, std::vector<size_t>> PosIdx;
				std::unordered_map<Value *, std::vector<size_t>> NegIdx;
				PosIdx.reserve(Ops.size());
				NegIdx.reserve(Ops.size());

				for (size_t i = 0; i < Ops.size(); ++i) {
					Value *X = nullptr;
					if (matchMulByMinusOne(Ops[i], X) && X) {
						NegIdx[X].push_back(i);
					} else {
						PosIdx[Ops[i]].push_back(i);
					}
				}

				for (auto &KV : PosIdx) {
					Value *X = KV.first;
					auto NI = NegIdx.find(X);
					if (NI == NegIdx.end()) continue;

					auto &P = KV.second;
					auto &N = NI->second;
					size_t PairCnt = std::min(P.size(), N.size());
					if (PairCnt == 0) continue;

					for (size_t k = 0; k < PairCnt; ++k) {
						Removed[P[k]] = 1;
						Removed[N[k]] = 1;
					}
					CanceledAny = true;
					Changed = true;
				}

				if (CanceledAny) {
					std::vector<Value *> NewOps;
					NewOps.reserve(Ops.size());
					for (size_t i = 0; i < Ops.size(); ++i) {
						if (!Removed[i]) NewOps.push_back(Ops[i]);
					}
					Ops.swap(NewOps);
					if (Ops.empty()) return Value::makeZeroForType(Root->getIRType());
					if (Ops.size() == 1) return Ops[0];
				}
			}

			if (Ops.size() >= 2) {
				std::unordered_map<Value *, int> FactorFreq;
				std::vector<std::vector<Value *>> TermFactors(Ops.size());
				for (size_t i = 0; i < Ops.size(); ++i) {
					flattenMulFactors(Ops[i], TermFactors[i]);
					std::unordered_set<Value *> Once;
					for (Value *F : TermFactors[i]) {
						if (Once.insert(F).second) ++FactorFreq[F];
					}
				}

				Value *BestFactor = nullptr;
				int BestCount = 1;
				for (auto &KV : FactorFreq) {
					if (KV.second > BestCount ||
						(KV.second == BestCount && BestFactor &&
						 rankComesBefore(KV.first, BestFactor))) {
						BestFactor = KV.first;
						BestCount = KV.second;
					}
				}

				if (BestFactor && BestCount > 1) {
					std::vector<Value *> RemainingTerms;
					std::vector<Value *> OutsideTerms;

					for (size_t i = 0; i < Ops.size(); ++i) {
						auto Factors = TermFactors[i];
						auto Fit = std::find(Factors.begin(), Factors.end(), BestFactor);
						if (Fit == Factors.end()) {
							OutsideTerms.push_back(Ops[i]);
							continue;
						}
						Factors.erase(Fit);
						if (Factors.empty()) {
							RemainingTerms.push_back(ConstInt::newConstInt(1));
						} else if (Factors.size() == 1) {
							RemainingTerms.push_back(Factors[0]);
						} else {
							Value *TermMul = nullptr;
							if (!buildBalancedProduct(Root, Factors, TermMul) || !TermMul) {
								RemainingTerms.push_back(Ops[i]);
							} else {
								RemainingTerms.push_back(TermMul);
							}
						}
					}

					if (RemainingTerms.size() >= 2) {
						Value *InsideAdd = nullptr;
						if (buildAddFromTerms(Root, RemainingTerms, InsideAdd) && InsideAdd) {
							Instruction *Factored = createBinaryBefore(Root, BinaryInst::MUL, BestFactor, InsideAdd);
							if (Factored) {
								std::vector<Value *> NewOps = OutsideTerms;
								NewOps.push_back(Factored);
								Ops.swap(NewOps);
								Changed = true;
							}
						}
					}
				}
			}
		}

		if (Op == BinaryInst::MUL && Ops.size() >= 4) {
			std::unordered_map<Value *, int> PowCnt;
			for (Value *V : Ops) ++PowCnt[V];

			bool HasDup = false;
			for (auto &KV : PowCnt) {
				if (KV.second > 1) {
					HasDup = true;
					break;
				}
			}

			if (HasDup) {
				std::vector<std::pair<Value *, int>> PowList;
				PowList.reserve(PowCnt.size());
				std::unordered_set<Value *> Seen;
				for (Value *V : Ops) {
					if (Seen.insert(V).second)
						PowList.push_back({V, PowCnt[V]});
				}

				bool LocalPowChanged = false;
				Value *PowRebuild = buildMulPowProduct(Root, PowList, LocalPowChanged);
				if (PowRebuild) {
					Ops.clear();
					Ops.push_back(PowRebuild);
					Changed = Changed || LocalPowChanged || true;
				}
			}
		}

		if (Ops.size() == 1) return Ops[0];
		return nullptr;
	}

	bool optimizeInst(Instruction *I) {
        //1.如果不是整数ADD，MUL，AND，OR，XOR，SUB直接返回执行下一条指令
		if (!I || !I->getParent()) return false;

		auto *BI = dynamic_cast<BinaryInst *>(I);
		if (!BI) return false;
		if (!I->getIRType() || !I->getIRType()->isInt()) return false;

		BinaryInst::Operation Op = BI->getOp();
		if (!BinaryInst::isReassociateOp(Op)) return false;
		// Once loop/address canonicalization has started, GEP index expressions
		// are owned by GEPCombine/LSR.  Reassociating them again can replace short
		// pointer recurrences with several simultaneously-live integer IVs and
		// force spills in memory-bound nests.  The early source cleanup keeps the
		// unrestricted mode; repeated late cleanup uses this guarded mode.
		if (PreserveAddressArithmetic && feedsAddressComputation(I)) return false;

		Value *LHS = nullptr;
		Value *RHS = nullptr;
		if (!BI->getOperands(LHS, RHS)) return false;
		if (!IRType::isInt(LHS ? LHS->getIRType() : nullptr) ||
			!IRType::isInt(RHS ? RHS->getIRType() : nullptr)) {
			return false;
		}
        //2.如果本条指令是SUB A-B指令，直接优化成A + B×（-1），将新生成的指令加入TodoList，然后直接返回执行下一条指令
		if (Op == BinaryInst::SUB) {
			Instruction *Neg = createBinaryBefore(
				I, BinaryInst::MUL, RHS,
				Value::makeIntegerForType(-1, RHS->getIRType()));
			if (!Neg) return false;
			Instruction *Add = createBinaryBefore(I, BinaryInst::ADD, LHS, Neg);
			if (!Add) return false;
			I->replaceAllUsesWith(Add);
			pushTodo(I);
			return true;
		}

		if (!BinaryInst::isAssociativeOp(Op)) return false;

		Instruction *OnlyUser = nullptr;
		if (I->countInstructionUses(&OnlyUser) == 1) {
			if (OnlyUser && OnlyUser != I && OnlyUser->getParent() == I->getParent()) {
				auto *UBI = dynamic_cast<BinaryInst *>(OnlyUser);
				if (UBI && UBI->getOp() == Op) {
					return false;
				}
			}
		}

		std::vector<Value *> Ops;
		std::vector<Value *> WorkList;
		std::unordered_set<Value *> Visited;
		Ops.reserve(8);
		WorkList.reserve(8);
		Visited.reserve(16);
		WorkList.push_back(LHS);
		WorkList.push_back(RHS);

		while (!WorkList.empty()) {
			Value *Cur = WorkList.back();
			WorkList.pop_back();
			if (!Visited.insert(Cur).second) {
				Ops.push_back(Cur);
				continue;
			}
			auto *CurBI = dynamic_cast<BinaryInst *>(Cur);
			if (!CurBI || CurBI->getOp() != Op ||
				Cur->countInstructionUses() != 1) {
				Ops.push_back(Cur);
				continue;
			}
			Value *A = nullptr;
			Value *B = nullptr;
			if (!CurBI->getOperands(A, B)) {
				Ops.push_back(Cur);
				continue;
			}
			WorkList.push_back(A);
			WorkList.push_back(B);
		}

		if (Ops.size() <= 1) {
			if (Ops.size() == 1 && Ops[0] != I) {
				I->replaceAllUsesWith(Ops[0]);
				pushTodo(I);
				return true;
			}
			return false;
		}

		// Use the function's deterministic SSA order as the tie-breaker.  Comparing
		// allocation addresses made otherwise identical compilations choose
		// different expression trees under ASLR.  A total SSA order also avoids the
		// rebuild oscillation that a merely stable sort can cause across rounds.
		std::sort(Ops.begin(), Ops.end(), [&](Value *A, Value *B) {
			return rankComesBefore(A, B);
		});

		bool Changed = false;
		while (true) {
			size_t OldSize = Ops.size();
			Value *Folded = optExp(I, Op, Ops, Changed);
			if (Folded && Folded != I) {
				I->replaceAllUsesWith(Folded);
				pushTodo(I);
				return true;
			}
			if (Ops.size() == OldSize) break;
		}

		if (Ops.size() == 1 && Ops[0] != I) {
			I->replaceAllUsesWith(Ops[0]);
			pushTodo(I);
			return true;
		}

		if (Ops.size() < 2) return Changed;

		// Avoid rebuilding when the tree is already in canonical shape.
		if (ReassociatePass::isAlreadyBuilt(I, Op, Ops)) return Changed;

		Value *R = Ops.back(); Ops.pop_back();
		Value *L = Ops.back(); Ops.pop_back();
		Instruction *Result = createBinaryBefore(I, Op, L, R);
		if (!Result) return Changed;

		while (!Ops.empty()) {
			Value *Next = Ops.back();
			Ops.pop_back();
			Instruction *NewInst = createBinaryBefore(I, Op, Next, Result);
			if (!NewInst) break;
			Result = NewInst;
		}

		I->replaceAllUsesWith(Result);
		pushTodo(I);
		return true;
	}
};

PreservationStatus ReassociatePass::run(Function &F, FunctionAnalysisPassManager &FAM) {
	ReassociateEngine Engine(F, FAM, PreserveAddressArithmetic);
	bool Changed = Engine.run();
	if (!Changed) return PreservationStatus::all();

	PreservationStatus PA;

    //只修改指令，不影响CFG级分析结果
	PA.keep<DominantAnalysis>();
	PA.keep<PostDominantAnalysis>();
	PA.keep<LoopAnalysis>();
	PA.discard<BasicAliasAnalysis>();
	PA.discard<SideEffectAnalysis>();
	return PA;
}
