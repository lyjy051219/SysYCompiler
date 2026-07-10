#include "ir/opt/LICM.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "ir/opt/LCSSA.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>


// 1) 检查所有 user 是否都在循环外
bool LICMPass::areAllUsersOutsideLoop(Instruction *I, const Loop &L) {
	for (auto It = I->getUseList().begin(); It != I->getUseList().end(); ++It) {
		Use *U = *It;
		if (!U || U->getValue() != I) continue;
		auto *UI = dynamic_cast<Instruction *>(U->getUser());
		if (!UI) continue;
		BasicBlock *UBB = UI->getParent();
		if (UBB && L.contains(UBB)) return false;
	}
	return true;
}

// 2)循环不变量
bool LICMPass::isInvariant(
	Instruction *I,
	const Loop &L,
	const DominatorTree &DT,
	const std::unordered_set<Instruction *> &InvariantSet) {
	if (!I || !I->getParent()) return false;
	for (size_t OperandIndex = 0;
		 OperandIndex < I->getNumOperands(); ++OperandIndex) {
		Value *V = I->getOperand(OperandIndex);
		if (!V) return false;

		if (dynamic_cast<ConstantSystem *>(V) ||
			dynamic_cast<Function *>(V) ||
			dynamic_cast<BuildInFunction *>(V) ||
			dynamic_cast<BasicBlock *>(V) ||
			dynamic_cast<Variable *>(V)) {
			continue;
		}

		auto *DefI = dynamic_cast<Instruction *>(V);
		if (!DefI || !DefI->getParent()) return false;
		if (!L.contains(DefI->getParent())) continue;

		if (InvariantSet.count(DefI) == 0) return false;
		if (!DT.dominates(DefI->getParent(), I->getParent())) return false;
	}
	return true;
}

// 3) 指令能否安全移动（基于别名/副作用 + 指令类别规则）
bool LICMPass::isSafeToMove(
	Instruction *I,
	const Loop &L,
	const DominatorTree &DT,
	const std::vector<BasicBlock *> &ExitBlocks,
	const std::vector<BasicBlock *> &Latches,
	const std::vector<Instruction *> &LoopInsts,
	BasicAAResult &AA,
	const LoopInfo &LI,
	FunctionAnalysisPassManager &FAM,
	bool ForSink,
	bool AggressiveHoist) {
	if (!I) return false;
	if (!I->getParent()) return false;

	// 绝对不能移动
	switch (I->instType) {
		case Instruction::RET:
		case Instruction::BR:
		case Instruction::PHI:
		case Instruction::ALLOCA:
			return false;
		case Instruction::SDIV:
		case Instruction::FDIV:
		case Instruction::SREM:
		case Instruction::FREM: {
			auto *BI = dynamic_cast<BinaryInst *>(I);
			Value *Dividend = nullptr;
			Value *Divisor = nullptr;
			if (!BI || !BI->getOperands(Dividend, Divisor)) return false;
			if (!Divisor) return false;
			if (!dynamic_cast<ConstantSystem *>(Divisor)) return false;
			if (Divisor->isConstZero()) return false;
			break;
		}
		default:
			break;
	}

	// 普通 Binary（排除上面的四个危险算术）可移动
	if (dynamic_cast<BinaryInst *>(I)) return true;

	// LOAD：循环内任一指令若可能 Mod 该地址，则不可移动
	if (auto *LIst = dynamic_cast<LoadInst *>(I)) {
		Value *Ptr = LIst->getPointerOperand();
		if (!Ptr) return false;
		bool UseIndexRange = LIst->getIRType() &&
			LIst->getIRType()->getEnum() != IR_FLOAT;
		for (Instruction *J : LoopInsts) {
			if (!J || J == I) continue;
			if (UseIndexRange) {
				if (auto *SI = dynamic_cast<StoreInst *>(J)) {
					Value *StorePtr = SI->getPointerOperand();
					if (!StorePtr) return false;
					if (AA.aliasWithIndexRange(Ptr, StorePtr, LI) == AliasInfo::NoAlias)
						continue;
					return false;
				}
			}
			ModRefInfo MRI = getInstModRefInfo(Ptr, J, AA, LI, FAM);
			if (isModSet(MRI)) return false;
		}
		return true;
	}

    // STORE：Hoist 直接禁止；Sink 需证明“最后一次写”
	if (auto *SI = dynamic_cast<StoreInst *>(I)) {
		if (!ForSink && AggressiveHoist) return false;
		Value *Ptr = SI->getPointerOperand();
		if (!Ptr) return false;

		BasicBlock *SBB = SI->getParent();
		if (!SBB) return false;

		for (BasicBlock *ExitBB : ExitBlocks)
			if (!DT.dominates(SBB, ExitBB)) return false;
		for (BasicBlock *LatchBB : Latches)
			if (!DT.dominates(SBB, LatchBB)) return false;

		for (Instruction *J : LoopInsts) {
			if (!J || J == SI) continue;

			ModRefInfo MRI = getInstModRefInfo(Ptr, J, AA, LI, FAM);
			if (!isModSet(MRI) && !isRefSet(MRI)) continue;

			BasicBlock *JBB = J->getParent();
			if (!JBB) return false;

			if (JBB == SBB) {
				std::vector<Instruction *> Seq;
				for (auto *K : *SBB) Seq.push_back(K);
				bool seenSI = false;
				for (Instruction *K : Seq) {
					if (K == SI) {
						seenSI = true;
						continue;
					}
					if (!seenSI) continue;
					if (K == J) return false;
				}
			} else if (DT.dominates(SBB, JBB)) {
				return false;
			}
		}
		return true;
	}

	// CALL：副作用分析 + ModRef + untracked + RefSet 逐点核对
	if (auto *CI = dynamic_cast<CallInst *>(I)) {
		auto *CalleeFunc = CI->getDirectCallee();
		if (!CalleeFunc) return false;

		const SideEffectInfo &SEI = FAM.getResult<SideEffectAnalysis>(*CalleeFunc);
		if (SEI.hasBuiltin) return false;
		for (const auto &KV : SEI.ptrMap)
			if (KV.second.untracked) return false;

		ModRefInfo FMRI = getFunctionModRefInfo(SEI);
		if (!(FMRI == ModRefInfo::NoModRef || FMRI == ModRefInfo::Ref)) return false;
		if (!SEI.ModSet.empty()) return false;

		if (!SEI.RefSet.empty()) {
			std::vector<Value *> ReadPtrs;

			for (Value *RV : SEI.RefSet) {
				auto *Var = dynamic_cast<Variable *>(RV);
				if (!Var) continue;
				if (Var->isGlobal()) {
					ReadPtrs.push_back(RV);
					continue;
				}
				if (!Var->isParam()) continue;

				int ParamIndex = CalleeFunc->getParamIndex(RV);
				if (ParamIndex < 0) continue;
				Value *Arg = CI->getArg(static_cast<unsigned>(ParamIndex));
				if (!Arg || !Arg->isPointerValue()) continue;
				ReadPtrs.push_back(Arg);
			}

			for (Value *Ptr : ReadPtrs) {
				for (Instruction *J : LoopInsts) {
					if (!J || J == CI) continue;
					ModRefInfo MRI = getInstModRefInfo(Ptr, J, AA, LI, FAM);
					if (isModSet(MRI)) return false;
				}
			}
		}

		return true;
	}

	return true;
}

PreservationStatus LICMPass::run(Function &F, FunctionAnalysisPassManager &FAM) {
	auto &DT = FAM.getResult<DominantAnalysis>(F);
	auto &LI = FAM.getResult<LoopAnalysis>(F);
	auto &AA = FAM.getResult<BasicAliasAnalysis>(F);
	bool Changed = false;
	std::unordered_map<ExitValueKey, PhiInst *, ExitValueKeyHash> ExitPhiCache;

	std::function<bool(Loop *)> processLoop = [&](Loop *L) -> bool {
		if (!L) return false;
		bool LocalChanged = false;

		// 后序处理子循环
		for (Loop *Sub : L->getSubLoops()) {
			LocalChanged |= processLoop(Sub);
		}

		// 前置检查
		if (!LCSSAPass::isLCSSAForm(L)) return LocalChanged;
		BasicBlock *PreHeader = nullptr;
		BasicBlock *Latch = nullptr;
		if (!L->getLoopSimplifyForm(PreHeader, Latch)) return LocalChanged;
		if (!PreHeader || !Latch) return LocalChanged;

		auto ExitBlocks = L->getExitBlocks();
		if (ExitBlocks.empty()) return LocalChanged;
		auto Latches = L->getBackedgeSrcs();

		auto collectLiveLoopInsts = [&]() {
			std::vector<Instruction *> Insts;
			for (BasicBlock *BB : L->getBlocksIncludingSubLoops()) {
				if (!BB) continue;
				for (auto *II : *BB) Insts.push_back(II);
			}
			return Insts;
		};

		// Sink：后序块 + 逆序指令
		std::unordered_set<Instruction *> SinkInvariant;
		auto SinkBlocksRPO = L->getBlocksInRPO();
		for (auto BIt = SinkBlocksRPO.rbegin(); BIt != SinkBlocksRPO.rend(); ++BIt) {
			BasicBlock *BB = *BIt;
			if (!BB) continue;

			std::vector<Instruction *> Insts;
			for (auto *I : *BB) Insts.push_back(I);

			for (auto IIt = Insts.rbegin(); IIt != Insts.rend(); ++IIt) {
				Instruction *I = *IIt;
				if (!I || I->getParent() != BB) continue;
				auto LoopInsts = collectLiveLoopInsts();

				if (!isSafeToMove(
						I,
						*L,
						DT,
						ExitBlocks,
						Latches,
						LoopInsts,
						AA,
						LI,
						FAM,
						/*ForSink=*/true,
						/*AggressiveHoist=*/false)) {
					continue;
				}

				if (!areAllUsersOutsideLoop(I, *L)) {
					if (isInvariant(I, *L, DT, SinkInvariant)) SinkInvariant.insert(I);
					continue;
				}

				if (!isInvariant(I, *L, DT, SinkInvariant)) continue;

				BasicBlock *DefBB = I->getParent();
				std::unordered_map<BasicBlock *, Instruction *> ExitClone;

				for (BasicBlock *ExitBB : ExitBlocks) {
					if (!ExitBB || !DT.dominates(DefBB, ExitBB)) continue;
					Instruction *Cloned = I->clone({});
					if (!Cloned) {
						continue;
					}

					for (size_t OperandIndex = 0;
						 OperandIndex < Cloned->getNumOperands(); ++OperandIndex) {
						Use *U = Cloned->getOperandUse(OperandIndex);
						Value *OldOp = Cloned->getOperand(OperandIndex);
						if (!OldOp) continue;

						auto *DefOpI = dynamic_cast<Instruction *>(OldOp);
						if (!DefOpI || !DefOpI->getParent() || !L->contains(DefOpI->getParent())) continue;

						ExitValueKey Key{ExitBB, OldOp};
						PhiInst *Bridge = nullptr;
						auto It = ExitPhiCache.find(Key);
						if (It != ExitPhiCache.end()) {
							Bridge = It->second;
						} else {
							Bridge = new PhiInst(OldOp->getIRType());
							Bridge->setIdent(Bridge->getIdent() + ".lcssa");
							for (BasicBlock *Pred : ExitBB->getPredecessors()) {
								if (!Pred || !L->contains(Pred)) continue;
								Value *Incoming = DT.dominates(DefOpI->getParent(), Pred)
													  ? OldOp
													  : UndefValue::NewUndefValue(OldOp->getIRType());
								Bridge->addIncoming(Incoming, Pred);
							}
							if (Bridge->getNumIncomingValues() == 0) {
								delete Bridge;
								Bridge = nullptr;
							} else {
								ExitBB->push_front(Bridge);
								ExitPhiCache[Key] = Bridge;
							}
						}

						if (Bridge) U->replaceUseeWith(Bridge);
					}

					auto InsIt = ExitBB->begin();
					while (InsIt != ExitBB->end() && dynamic_cast<PhiInst *>(*InsIt)) ++InsIt;
					if (InsIt == ExitBB->end()) ExitBB->push_back(Cloned);
					else InsIt.insertBefore(Cloned);
					ExitClone[ExitBB] = Cloned;
				}

				if (ExitClone.empty()) continue;

				bool RewriteOK = true;
				std::vector<Use *> Uses;
				for (auto Uit = I->getUseList().begin(); Uit != I->getUseList().end(); ++Uit) {
					Use *U = *Uit;
					if (U && U->getValue() == I) Uses.push_back(U);
				}

				for (Use *U : Uses) {
					auto *UI = dynamic_cast<Instruction *>(U ? U->getUser() : nullptr);
					if (!UI || !UI->getParent()) continue;
					BasicBlock *UseBB = UI->getParent();

					Instruction *Rep = nullptr;
					int hit = 0;
					for (auto &KV : ExitClone) {
						if (DT.dominates(KV.first, UseBB)) {
							Rep = KV.second;
							++hit;
						}
					}
					if (hit != 1 || !Rep) {
						RewriteOK = false;
						break;
					}
					U->replaceUseeWith(Rep);
				}

				if (!RewriteOK) continue;

				I->EraseFromParent();
				delete I;
				LocalChanged = true;
			}
		}

		// Hoist：RPO 块 + 正序指令
		std::unordered_set<Instruction *> HoistInvariant;
		auto HoistBlocksRPO = L->getBlocksInRPO();
		for (BasicBlock *BB : HoistBlocksRPO) {
			if (!BB) continue;
			std::vector<Instruction *> Insts;
			for (auto *I : *BB) Insts.push_back(I);

			for (Instruction *I : Insts) {
				if (!I || I->getParent() != BB) continue;
				auto LoopInsts = collectLiveLoopInsts();

				bool Aggressive = true;
				for (BasicBlock *ExitBB : ExitBlocks)
					if (!DT.dominates(BB, ExitBB)) Aggressive = false;
				for (BasicBlock *LatchBB : Latches)
					if (!DT.dominates(BB, LatchBB)) Aggressive = false;

				if (!isSafeToMove(
						I,
						*L,
						DT,
						ExitBlocks,
						Latches,
						LoopInsts,
						AA,
						LI,
						FAM,
						/*ForSink=*/false,
						/*AggressiveHoist=*/Aggressive)) {
					continue;
				}

				if (!isInvariant(I, *L, DT, HoistInvariant)) continue;

				auto TermIt = PreHeader->begin();
				while (TermIt != PreHeader->end() && *TermIt != PreHeader->back()) ++TermIt;
				if (TermIt == PreHeader->end()) continue;

				I->EraseFromParent();
				TermIt.insertBefore(I);
				HoistInvariant.insert(I);
				LocalChanged = true;
			}
		}

		return LocalChanged;
	};

	for (Loop *TopL : LI.getTopLevelLoops()) {
		Changed |= processLoop(TopL);
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
