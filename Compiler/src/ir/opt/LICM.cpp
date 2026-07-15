#include "ir/opt/LICM.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/MemorySSA.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "ir/opt/DCE.hpp"
#include "ir/opt/LCSSA.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

std::size_t ExitValueKeyHash::operator()(const ExitValueKey &K) const {
	auto A = reinterpret_cast<std::uintptr_t>(K.first);
	auto B = reinterpret_cast<std::uintptr_t>(K.second);
	return A ^ (B + 0x9e3779b97f4a7c15ULL + (A << 6) + (A >> 2));
}

// 在同一基本块中，即使候选指令的块支配了所有 exit/latch，
// 它前面仍可能有不返回的 call 或会 fault 的内存/除法指令。
// 把候选移到 preheader 会让它在这些指令之前执行，所以这种
// 情况不能仅凭基本块支配关系宣称 must-execute。
bool LICMPass::hasSafeExecutionPrefix(const Instruction *Candidate) {
	BasicBlock *BB = Candidate ? Candidate->getParent() : nullptr;
	if (!BB) return false;

	for (Instruction *I : *BB) {
		if (I == Candidate) return true;
		if (!I) return false;
		if (dynamic_cast<StoreInst *>(I) ||
			dynamic_cast<CallInst *>(I) ||
			instructionMayFaultOrProducePoison(I)) {
			return false;
		}
	}
	return false;
}

// 无 must-execute 证明时，只允许提前执行明确不会除零的
// div/rem。signed div/rem 的 INT_MIN, -1 在不同 IR/目标
// 语义下可能产生 poison 或 trap，因此保守拒绝 -1。
bool LICMPass::isSpeculatableDivisionOrRemainder(Instruction *I) {
	auto *BI = dynamic_cast<BinaryInst *>(I);
	Value *Dividend = nullptr;
	Value *Divisor = nullptr;
	if (!BI || !BI->getOperands(Dividend, Divisor) || !Divisor) return false;
	(void)Dividend;

	if (!dynamic_cast<ConstantSystem *>(Divisor) || Divisor->isConstZero())
		return false;
	if ((I->instType == Instruction::SDIV ||
		 I->instType == Instruction::SREM) &&
		Value::isConstMinusOne(Divisor)) {
		return false;
	}
	return true;
}

// Sinking a store removes all of its intermediate executions.  That is only
// valid when the destination is known to be a non-faulting object and no
// other instruction in the loop can observe or overwrite the location.  In
// particular, checking only instructions *after* the store is insufficient:
// a load before the store in iteration N+1 observes the store from iteration
// N and creates a loop-carried memory dependence.
//
// This IR has no dereferenceable/inbounds attributes, so keep the
// non-faulting proof deliberately narrow: a direct global/alloca object or a
// structurally in-bounds, all-constant GEP rooted at one.  Casts, parameters
// and symbolic indices are rejected instead of guessing object bounds.
bool LICMPass::isNonFaultingStoreObject(Value *Ptr, Value *StoredValue) {
	if (!Ptr || !StoredValue || !StoredValue->getIRType()) return false;
	auto IsOwnedRoot = [](Value *V) {
		if (auto *Var = dynamic_cast<Variable *>(V)) return Var->isGlobal();
		return dynamic_cast<AllocaInst *>(V) != nullptr;
	};

	Value *Base = Ptr;
	std::vector<Value *> Indices;
	if (auto *GEP = dynamic_cast<GetElementPtrInst *>(Ptr)) {
		if (!GEP->flattenZeroBasedChain(Base, Indices)) return false;
	}
	if (!IsOwnedRoot(Base) || !Base->getIRType()) return false;

	std::shared_ptr<IRType> Current = Base->getIRType();
	for (Value *Index : Indices) {
		auto *CI = dynamic_cast<ConstInt *>(Index);
		if (!CI) return false;
		int64_t I = CI->getVal();

		if (auto PT = std::dynamic_pointer_cast<POINTERType>(Current)) {
			// The root denotes one complete object, not an arbitrary C pointer
			// range.  Only its zero-th object can be proven in-bounds.
			if (I != 0) return false;
			Current = PT->getPointerType();
		} else if (auto AT = std::dynamic_pointer_cast<ARRAYType>(Current)) {
			if (I < 0 || I >= AT->getElementCount()) return false;
			Current = AT->getElementType();
		} else {
			return false;
		}
		if (!Current) return false;
	}

	if (auto PT = std::dynamic_pointer_cast<POINTERType>(Current))
		Current = PT->getPointerType();
	return Current &&
		StoredValue->getIRType()->getSize() <= Current->getSize();
}

// A loop-invariant call may be evaluated lazily on the first iteration and
// cached for later iterations without requiring willreturn/speculatable: the
// first evaluation stays at the original program point.  Reusing the returned
// value is nevertheless only valid when the call is deterministic with
// respect to the loop.  SideEffectAnalysis supplies the memory summary; reject
// unknown/builtin effects, all writes, and any read location that the loop may
// modify.
bool LICMPass::isReusableLoopInvariantCall(
	CallInst *CI,
	const std::vector<Instruction *> &LoopInsts,
	BasicAAResult &AA,
	const LoopInfo &LI,
	FunctionAnalysisPassManager &FAM) {
	if (!CI) return false;
	auto *CalleeFunc = CI->getDirectCallee();
	if (!CalleeFunc || !CalleeFunc->front()) return false;

	const SideEffectInfo &SEI =
		FAM.getResult<SideEffectAnalysis>(*CalleeFunc);
	if (SEI.hasBuiltin) return false;
	for (const auto &KV : SEI.ptrMap)
		if (KV.second.untracked) return false;

	ModRefInfo FMRI = getFunctionModRefInfo(SEI);
	if (!(FMRI == ModRefInfo::NoModRef || FMRI == ModRefInfo::Ref))
		return false;
	if (!SEI.ModSet.empty()) return false;

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

	return true;
}

// Lazily evaluate CI at its original point on the first iteration, then carry
// the value around the backedge and bypass the call on subsequent iterations:
//
//   header:
//     first  = phi [true, preheader], [false, latch]
//     cached = phi [undef, preheader], [value, latch]
//   original-prefix:
//     br first, compute, merge
//   compute: value.first = call ...; br merge
//   merge:
//     value = phi [value.first, compute], [cached, original-prefix]
//     original-suffix
//
// Unlike ordinary preheader hoisting this preserves zero-trip loops, the
// ordering relative to every instruction before CI, and the behaviour of a
// trapping or non-returning first call.  BB must dominate the unique latch so
// every iteration that continues to the next one has produced ValuePhi.
bool LICMPass::cacheLoopInvariantCall(
	Function &F,
	CallInst *CI,
	Loop &L,
	BasicBlock *PreHeader,
	BasicBlock *Latch,
	const DominatorTree &DT) {
	if (!CI || !PreHeader || !Latch) return false;
	// Splitting the call away from its original terminator would invalidate a
	// tail-call marker even if the returned value itself were cacheable.
	if (CI->isTailCall()) return false;
	BasicBlock *BB = CI->getParent();
	BasicBlock *Header = L.getHeader();
	if (!BB || !Header || BB == Header) return false;
	if (!L.contains(BB) || !DT.dominates(BB, Latch)) return false;
	if (!CI->getIRType() || CI->getIRType()->getEnum() == IR_VOID)
		return false;
	if (!CI->hasUses()) return false;

	// A well-formed basic block must retain a terminator in the continuation.
	std::vector<Instruction *> Tail;
	bool SeenCall = false;
	for (Instruction *I : *BB) {
		if (I == CI) {
			SeenCall = true;
			continue;
		}
		if (SeenCall) Tail.push_back(I);
	}
	if (!SeenCall || Tail.empty()) return false;
	Instruction *OldTerminator = Tail.back();
	if (!dynamic_cast<BrInst *>(OldTerminator) &&
		!dynamic_cast<RetInst *>(OldTerminator)) {
		return false;
	}

	std::vector<BasicBlock *> OldSuccessors = BB->getSuccessors();

	auto *ComputeBB = new BasicBlock();
	auto *MergeBB = new BasicBlock();
	ComputeBB->setIdent(F.makeUniqueBlockIdent(
		BB->getIdent() + ".licm.compute"));
	MergeBB->setIdent(F.makeUniqueBlockIdent(
		BB->getIdent() + ".licm.merge"));
	F.pushBlock(ComputeBB);
	F.pushBlock(MergeBB);

	CI->EraseFromParent();
	ComputeBB->push_back(CI);
	for (Instruction *I : Tail) {
		I->EraseFromParent();
		MergeBB->push_back(I);
	}

	// The old terminator now belongs to MergeBB, hence all of its successor
	// PHIs must name MergeBB rather than the prefix block.  Do this before
	// taking snapshots of CI's uses because rebuilding a PHI invalidates its
	// old Use objects.
	std::unordered_set<BasicBlock *> SeenSuccessors;
	for (BasicBlock *Succ : OldSuccessors) {
		if (!Succ || !SeenSuccessors.insert(Succ).second) continue;
		BasicBlock::replaceIncomingPredInSuccPhis(Succ, BB, MergeBB);
	}

	auto OriginalUses = CI->getUses();

	auto *ValuePhi = new PhiInst(CI->getIRType());
	ValuePhi->setIdent(ValuePhi->getIdent() + ".licm.value");
	ValuePhi->addIncoming(CI, ComputeBB);

	BasicBlock *BackedgePred = (Latch == BB) ? MergeBB : Latch;
	auto *FirstTrue = ConstBool::newConstBool(true);
	auto *FirstFalse = ConstBool::newConstBool(false);
	auto *FirstPhi = new PhiInst(FirstTrue->getIRType());
	FirstPhi->setIdent(FirstPhi->getIdent() + ".licm.first");
	FirstPhi->addIncoming(FirstTrue, PreHeader);
	FirstPhi->addIncoming(FirstFalse, BackedgePred);

	auto *CachedPhi = new PhiInst(CI->getIRType());
	CachedPhi->setIdent(CachedPhi->getIdent() + ".licm.cached");
	CachedPhi->addIncoming(
		UndefValue::NewUndefValue(CI->getIRType()), PreHeader);
	CachedPhi->addIncoming(ValuePhi, BackedgePred);

	ValuePhi->addIncoming(CachedPhi, BB);
	MergeBB->push_front(ValuePhi);
	Header->push_front(CachedPhi);
	Header->push_front(FirstPhi);

	for (Use *U : OriginalUses)
		if (U && U->getValue() == CI) U->replaceUseeWith(ValuePhi);

	BB->genCondInst(FirstPhi, ComputeBB, MergeBB);
	ComputeBB->genUnCondInst(MergeBB);
	return true;
}

// 1) 检查所有 user 是否都在循环外
bool LICMPass::areAllUsersOutsideLoop(Instruction *I, const Loop &L) {
	for (Use *U : I->getUses()) {
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
	MemorySSAResult &MSSA,
	bool MemorySSAUsable,
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
		case Instruction::UDIV:
		case Instruction::FDIV:
		case Instruction::SREM:
		case Instruction::UREM:
		case Instruction::FREM:
			if (!isSpeculatableDivisionOrRemainder(I)) return false;
			break;
		case Instruction::FPTOSI:
			// Out-of-range/NaN conversion is not speculatable in this IR.
			// Hoist it only when the original instruction executes on every
			// loop path and no earlier instruction can trap or fail to return.
			if (!AggressiveHoist || I->getParent() != L.getHeader() ||
				!hasSafeExecutionPrefix(I)) return false;
			break;
		case Instruction::SHL:
		case Instruction::LSHR:
		case Instruction::ASHR:
			if (instructionMayFaultOrProducePoison(I) &&
				(!AggressiveHoist || I->getParent() != L.getHeader() ||
				 !hasSafeExecutionPrefix(I))) {
				return false;
			}
			break;
		default:
			break;
	}

	// Keep the sinking transform memory-specific.  Pure invariant SSA
	// expressions are handled by hoisting below; cloning them into multiple
	// exits needs full SSAUpdater support and the old ad-hoc bridge construction
	// could create an exit PHI whose incoming value was defined in the exit
	// block itself.
	if (ForSink && !dynamic_cast<StoreInst *>(I)) return false;

	// 普通 Binary（排除上面的四个危险算术）可移动
	if (dynamic_cast<BinaryInst *>(I)) return true;

	// select only chooses between already-computed SSA values and has no
	// trapping behaviour of its own.  Treating it as immovable used to break
	// otherwise-pure invariant chains such as max(0, trip-count): the compare
	// could move, but the select and all of its quotient/remainder users stayed
	// in an inner-loop preheader and were then recomputed by the parent loop.
	if (dynamic_cast<SelectInst *>(I)) return true;

	// LOAD：循环内任一指令若可能 Mod 该地址，则不可移动
	if (auto *LIst = dynamic_cast<LoadInst *>(I)) {
		// 没有 dereferenceable/nonnull 属性时，load 不是可无条件
		// 推测执行的指令。Sink 会消除原本可能发生的 fault；
		// Hoist 则只在指令已证明 must-execute 时允许。
		if (ForSink) return false;
		if (!AggressiveHoist || !hasSafeExecutionPrefix(I)) return false;
		if (I->getParent() != L.getHeader()) return false;

		Value *Ptr = LIst->getPointerOperand();
		if (!Ptr) return false;

		// A unique reaching memory state outside the loop proves that every
		// dynamic execution of this load (including executions reached through
		// a backedge) sees no in-loop clobber before it.  This is stronger than
		// a flat instruction scan for transparent CFG cycles and remains
		// conservative when paths disagree: the walker then returns a Phi or
		// nullptr and we use the traditional scan below.
		bool MSSAProvesNoLoopClobber = false;
		if (MemorySSAUsable)
		if (MemoryAccess *Clobber = MSSA.getClobberingAccess(
				LIst, MemoryLocation::get(LIst), AA, LI, FAM, 256)) {
			if (Clobber->isLiveOnEntry()) {
				MSSAProvesNoLoopClobber = true;
			} else if (Clobber->isDef()) {
				Instruction *DefI = Clobber->getInstruction();
				BasicBlock *DefBB = DefI ? DefI->getParent() : nullptr;
				MSSAProvesNoLoopClobber = DefBB && !L.contains(DefBB);
			}
		}
		if (MSSAProvesNoLoopClobber) return true;

			bool UseIndexRange = LIst->getIRType() &&
				LIst->getIRType()->getEnum() != IR_FLOAT;
			MemoryLocation LoadLocation = MemoryLocation::get(LIst);
			for (Instruction *J : LoopInsts) {
				if (!J || J == I) continue;
				if (UseIndexRange) {
					if (auto *SI = dynamic_cast<StoreInst *>(J)) {
						Value *StorePtr = SI->getPointerOperand();
						if (!StorePtr) return false;
						if (AA.aliasWithIndexRange(
								LoadLocation, MemoryLocation::get(SI), LI) ==
							AliasInfo::NoAlias)
							continue;
						return false;
					}
				}
				ModRefInfo MRI = getInstModRefInfo(
					LoadLocation, J, AA, LI, FAM);
			if (isModSet(MRI)) return false;
		}
		return true;
	}

    // STORE：Hoist 直接禁止；Sink 需证明“最后一次写”
	if (auto *SI = dynamic_cast<StoreInst *>(I)) {
		if (!ForSink) return false;
		Value *Ptr = SI->getPointerOperand();
		if (!isNonFaultingStoreObject(Ptr, SI->getValueOperand())) return false;
		for (Value *Operand :
			 {SI->getValueOperand(), SI->getPointerOperand()}) {
			auto *Def = dynamic_cast<Instruction *>(Operand);
			if (Def && Def->getParent() && L.contains(Def->getParent()))
				return false;
		}

		BasicBlock *SBB = SI->getParent();
		if (!SBB) return false;

		// Every path which actually exits the loop must have executed this
		// store.  Check exiting blocks rather than merely exit destinations;
		// the latter can have additional predecessors in malformed/non-
		// dedicated CFGs.
		bool SawExitingBlock = false;
		for (BasicBlock *BB : L.getBlocksIncludingSubLoops()) {
			if (!BB || !L.isExitingBlock(BB)) continue;
			SawExitingBlock = true;
			if (!DT.dominates(SBB, BB)) return false;
		}
		if (!SawExitingBlock) return false;
		for (BasicBlock *LatchBB : Latches)
			if (!DT.dominates(SBB, LatchBB)) return false;

		// Reject *every* competing Mod/Ref, including accesses before SI.
		// This is the conservative MemorySSA-equivalent legality condition
		// until the dedicated memory dependence graph is available.
		for (Instruction *J : LoopInsts) {
			if (!J || J == SI) continue;

				ModRefInfo MRI = getInstModRefInfo(
					MemoryLocation::get(SI), J, AA, LI, FAM);
			if (isModSet(MRI) || isRefSet(MRI)) return false;
		}
		return true;
	}

	// CALL：副作用分析 + ModRef + untracked + RefSet 逐点核对
	if (auto *CI = dynamic_cast<CallInst *>(I)) {
		// 目前 IR 没有 willreturn/nounwind/speculatable 属性。即使
		// SideEffectAnalysis 证明 call 不写内存，它仍可能不返回。
		// 因此不做 call sink，hoist 也必须是 must-execute。
		if (ForSink) return false;
		if (!AggressiveHoist || !hasSafeExecutionPrefix(I)) return false;
		if (I->getParent() != L.getHeader()) return false;

		return isReusableLoopInvariantCall(CI, LoopInsts, AA, LI, FAM);
	}

	// Keep the fallback a pure-operation allowlist.  New instruction kinds
	// must opt in with an explicit speculation rule instead of being hoisted
	// by default.
	if (dynamic_cast<GetElementPtrInst *>(I)) return true;
	switch (I->instType) {
		case Instruction::TRUNC:
		case Instruction::ZEXT:
		case Instruction::SEXT:
		case Instruction::FPTOSI:
		case Instruction::SITOFP:
		case Instruction::BITCAST:
			return true;
		default:
			return false;
	}
}

PreservationStatus LICMPass::run(Function &F, FunctionAnalysisPassManager &FAM) {
	auto &DT = FAM.getResult<DominantAnalysis>(F);
	auto &LI = FAM.getResult<LoopAnalysis>(F);
	auto &AA = FAM.getResult<BasicAliasAnalysis>(F);
	auto &MSSA = FAM.getResult<MemorySSAAnalysis>(F);
	// Store sinking clones a MemoryDef and deletes the original instruction.
	// This compact MemorySSA has no incremental updater, so consumers in the
	// remainder of this LICM invocation must stop dereferencing its snapshot
	// after the first successful store sink.
	bool MemorySSAUsable = true;
	bool Changed = false;
	bool CFGChanged = false;
	std::unordered_map<ExitValueKey, PhiInst *, ExitValueKeyHash> ExitPhiCache;

	std::function<bool(Loop *)> processLoop = [&](Loop *L) -> bool {
		if (!L) return false;
		bool LocalChanged = false;

		// 后序处理子循环
		for (Loop *Sub : L->getSubLoops()) {
			LocalChanged |= processLoop(Sub);
			if (CFGChanged) return true;
		}

		// A rotated loop commonly has a unique guard predecessor whose second
		// edge is the zero-trip bypass.  It is a valid insertion point for
		// speculatable pure instructions, but getLoopSimplifyForm deliberately
		// rejects it because it is not an unconditional preheader.  Keep the
		// stronger form for sinking/CFG rewrites and use the weaker unique-entry
		// shape for pure hoisting only.
		bool InLCSSA = LCSSAPass::isLCSSAForm(L);
		BasicBlock *PreHeader = nullptr;
		BasicBlock *Latch = nullptr;
		bool HasLoopSimplifyForm =
			L->getLoopSimplifyForm(PreHeader, Latch);
		if (!HasLoopSimplifyForm) {
			std::vector<BasicBlock *> OutsidePreds;
			std::vector<BasicBlock *> InsidePreds;
			BasicBlock *Header = L->getHeader();
			if (!Header) return LocalChanged;
			for (BasicBlock *Pred : Header->getPredecessors()) {
				if (!Pred) continue;
				if (L->contains(Pred)) InsidePreds.push_back(Pred);
				else OutsidePreds.push_back(Pred);
			}
			if (OutsidePreds.size() != 1 || InsidePreds.size() != 1)
				return LocalChanged;
			PreHeader = OutsidePreds.front();
			Latch = InsidePreds.front();
		}
		if (!PreHeader || !Latch) return LocalChanged;
		const bool CanSink = HasLoopSimplifyForm && InLCSSA;

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
		auto SinkBlocksRPO = CanSink
			? L->getBlocksInRPO()
			: std::vector<BasicBlock *>{};
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
						MSSA,
						MemorySSAUsable,
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

				if (ExitClone.size() != ExitBlocks.size()) {
					for (auto &KV : ExitClone)
						if (KV.second) delete KV.second;
					continue;
				}

				bool RewriteOK = true;
				auto Uses = I->getUses();

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
				MemorySSAUsable = false;
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
				if (!HasLoopSimplifyForm &&
					!dynamic_cast<BinaryInst *>(I) &&
					!dynamic_cast<SelectInst *>(I)) {
					continue;
				}
				auto LoopInsts = collectLiveLoopInsts();

				bool Aggressive = true;
				for (BasicBlock *ExitBB : ExitBlocks)
					if (!DT.dominates(BB, ExitBB)) Aggressive = false;
				for (BasicBlock *LatchBB : Latches)
					if (!DT.dominates(BB, LatchBB)) Aggressive = false;

				bool SafeToPreheader = isSafeToMove(
						I,
						*L,
						DT,
						ExitBlocks,
						Latches,
						LoopInsts,
						AA,
						MSSA,
						MemorySSAUsable,
						LI,
						FAM,
						/*ForSink=*/false,
						/*AggressiveHoist=*/Aggressive);

				if (!SafeToPreheader) {
					// A pure invariant call in a per-iteration must-pass block may
					// still be cached without speculating it into a zero-trip
					// preheader.  Keep its first execution at the exact original
					// point, then bypass it after the first backedge.
					auto *CI = dynamic_cast<CallInst *>(I);
					if (!HasLoopSimplifyForm || !InLCSSA ||
						!CI || BB == L->getHeader() ||
						!DT.dominates(BB, Latch) ||
						!isReusableLoopInvariantCall(
							CI, LoopInsts, AA, LI, FAM) ||
						!isInvariant(I, *L, DT, HoistInvariant)) {
						continue;
					}

					if (cacheLoopInvariantCall(
							F, CI, *L, PreHeader, Latch, DT)) {
						CFGChanged = true;
						LocalChanged = true;
						return true;
					}
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
		bool LocalChanged = processLoop(TopL);
		Changed |= LocalChanged;
		// The first round hoists invariant address/value producers.  A second
		// bounded round lets store sinking consume those now-preheader values
		// without manufacturing invalid exit PHIs for loop-local operands.
		if (LocalChanged && !CFGChanged)
			Changed |= processLoop(TopL);
		if (CFGChanged) break;
	}

	if (!Changed) return PreservationStatus::all();
	if (CFGChanged) return PreservationStatus::none();

	PreservationStatus PA;
	PA.keep<DominantAnalysis>();
	PA.keep<PostDominantAnalysis>();
	PA.keep<LoopAnalysis>();
	PA.discard<BasicAliasAnalysis>();
	PA.discard<MemorySSAAnalysis>();
	PA.discard<SideEffectAnalysis>();
	return PA;
}
