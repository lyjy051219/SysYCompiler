#include "ir/opt/LICM.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/MemorySSA.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "ir/opt/DCE.hpp"
#include "ir/opt/LCSSA.hpp"
#include "lib/CFG.hpp"
#include "lib/IRUtils.hpp"

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

bool LICMPass::hasSafeExecutionPrefix(const Instruction *Candidate) {
	// 2.1 从块首到候选之间不得有写入、调用或可能陷阱/poison 的操作；这证明把原本
	// 必经块中的候选提前执行不会越过更早的可观察或异常行为。
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

bool LICMPass::hasPreheaderDereferenceProof(
	LoadInst *Load, const Loop &L, BasicAAResult &AA,
	const LoopInfo &LI) {
	// 2.2 顺序扫描 preheader，寻找尺寸覆盖且 MustAlias 的既有 load/store 作为地址
	// 已可解引用证明；任一调用会清除该证明，避免跨未知控制点推测 load。
	if (!Load) return false;
	BasicBlock *PreHeader = nullptr;
	BasicBlock *Latch = nullptr;
	if (!L.getLoopSimplifyForm(PreHeader, Latch) || !PreHeader || !Latch)
		return false;

	const MemoryLocation Candidate = MemoryLocation::get(Load);
	bool Proven = false;
	for (Instruction *I : *PreHeader) {
		if (!I) return false;
		if (dynamic_cast<BrInst *>(I) || dynamic_cast<RetInst *>(I)) break;
		if (dynamic_cast<CallInst *>(I)) {

			Proven = false;
			continue;
		}

		MemoryLocation Existing;
		if (auto *ExistingLoad = dynamic_cast<LoadInst *>(I))
			Existing = MemoryLocation::get(ExistingLoad);
		else if (auto *ExistingStore = dynamic_cast<StoreInst *>(I))
			Existing = MemoryLocation::get(ExistingStore);
		else
			continue;

		if (Candidate.Size && Existing.Size &&
			*Existing.Size >= *Candidate.Size &&
			AA.alias(Candidate, Existing, LI) == AliasInfo::MustAlias)
			Proven = true;
	}
	return Proven;
}

bool LICMPass::isSpeculatableDivisionOrRemainder(Instruction *I) {
	// 2.3 除数必须是非零常量；有符号除余还排除 -1，避免最小整数溢出情形。
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

bool LICMPass::isNonFaultingStoreObject(Value *Ptr, Value *StoredValue) {
	// 3.1 下沉 store 仅接受全局/alloca 根上的全常量在界 GEP，并检查最终对象尺寸
	// 覆盖写入值，保证在出口执行不会新增无效地址访问。
	if (!Ptr || !StoredValue || !StoredValue->getIRType()) return false;
	auto IsOwnedRoot = [](Value *V) {
		if (V && V->isGlobal()) return true;
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

bool LICMPass::isReusableLoopInvariantCall(
	CallInst *CI,
	const std::vector<Instruction *> &LoopInsts,
	BasicAAResult &AA,
	const LoopInfo &LI,
	FunctionAnalysisPassManager &FAM) {
	// 2.4 可复用调用本身必须无计时行为、callee 副作用至多为已跟踪 Ref 且没有
	// Mod；是否跨越 timer 由最终插入位置的 CFG 路径检查判断，不封锁整个循环。
	if (!CI || CI->mayReachTimingCall()) return false;
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
		if (!RV) continue;
		if (RV->isGlobal()) {
			ReadPtrs.push_back(RV);
			continue;
		}
		if (!RV->isParam()) continue;

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

bool LICMPass::cacheLoopInvariantCall(
	Function &F,
	CallInst *CI,
	Loop &L,
	BasicBlock *PreHeader,
	BasicBlock *Latch,
	const DominatorTree &DT) {
	// 4.1 对不是每次必执行但结果循环不变的调用，构造“首轮计算、后续复用”CFG；
	// 候选必须支配 latch、非 tail、非 void 且有用户。
	if (!CI || !PreHeader || !Latch) return false;

	if (CI->isTailCall()) return false;
	BasicBlock *BB = CI->getParent();
	BasicBlock *Header = L.getHeader();
	if (!BB || !Header || BB == Header) return false;
	if (!L.contains(BB) || !DT.dominates(BB, Latch)) return false;
	if (!CI->getIRType() || CI->getIRType()->isVoid())
		return false;
	if (!CI->hasUses()) return false;

	std::vector<Instruction *> Tail;
	bool SeenCall = false;
	for (Instruction *I : collectInstructions(*BB)) {
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

	// 4.2 将调用后的块尾拆到 merge，调用移入 compute；先重写旧后继 PHI 的前驱，
	// 保持原终结边从新的 merge 块发出。
    auto *ComputeBB = new BasicBlock();
	auto *MergeBB = new BasicBlock();
	ComputeBB->setIdent(F.makeUniqueBlockIdent(
		BB->getIdent() + ".licm.compute"));
	MergeBB->setIdent(F.makeUniqueBlockIdent(
		BB->getIdent() + ".licm.merge"));
	F.pushBlock(ComputeBB);
	F.pushBlock(MergeBB);

	CI->eraseFromParent();
	ComputeBB->pushBack(CI);
	for (Instruction *I : Tail) {
		I->eraseFromParent();
		MergeBB->pushBack(I);
	}

	std::unordered_set<BasicBlock *> SeenSuccessors;
	for (BasicBlock *Succ : OldSuccessors) {
		if (!Succ || !SeenSuccessors.insert(Succ).second) continue;
		BasicBlock::replaceIncomingPredInSuccPhis(Succ, BB, MergeBB);
	}

	auto OriginalUses = CI->getUses();

	// 4.3 header 的 first/cached PHI 记录是否首轮及缓存值，merge 的 value PHI 在
	// 首轮取调用结果、以后取缓存；最后把原调用用户统一改到 value PHI。
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
	MergeBB->pushFront(ValuePhi);
	Header->pushFront(CachedPhi);
	Header->pushFront(FirstPhi);

	for (Use *U : OriginalUses)
		if (U && U->getValue() == CI) U->replaceUseeWith(ValuePhi);

	BB->genCondInst(FirstPhi, ComputeBB, MergeBB);
	ComputeBB->genUnCondInst(MergeBB);
	return true;
}

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

bool LICMPass::isInvariant(
	Instruction *I,
	const Loop &L,
	const DominatorTree &DT,
	const std::unordered_set<Instruction *> &InvariantSet) {
	// 1.1 每个操作数必须来自循环外，或已在本轮不变量集合中且其定义支配当前指令。
	if (!I || !I->getParent()) return false;
	return forEachOperand(
		I, [&](size_t, Use *, Value *V) {
		if (!V) return false;

		if (dynamic_cast<ConstantSystem *>(V) ||
			dynamic_cast<Function *>(V) ||
			dynamic_cast<BuildInFunction *>(V) ||
			dynamic_cast<BasicBlock *>(V) ||
			V->isParam() || V->isGlobal()) {
			return true;
		}

		auto *DefI = dynamic_cast<Instruction *>(V);
		if (!DefI || !DefI->getParent()) return false;
		if (!L.contains(DefI->getParent())) return true;

		if (InvariantSet.count(DefI) == 0) return false;
		if (!DT.dominates(DefI->getParent(), I->getParent())) return false;
		return true;
	});
}

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
	// 2. 先按 opcode 排除控制、PHI、alloca 和不可安全推测的除余/转换/移位；
	// 普通下沉当前只支持 store，普通提升再按指令类别继续验证。
	if (!I) return false;
	if (!I->getParent()) return false;

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

	if (ForSink && !dynamic_cast<StoreInst *>(I)) return false;

	if (dynamic_cast<BinaryInst *>(I)) return true;

	if (dynamic_cast<SelectInst *>(I)) return true;

	// 2.5 load 只能提升：必须原本在每轮必执行的安全 header 前缀，或 preheader 已
	// 解引用同一地址；再由 MemorySSA 或逐指令 AA 证明循环内无 clobber。
	if (auto *LIst = dynamic_cast<LoadInst *>(I)) {

		if (ForSink) return false;
		const bool HeaderMustExecute =
			AggressiveHoist && I->getParent() == L.getHeader() &&
			hasSafeExecutionPrefix(I);
		const bool PreheaderAlreadyDereferenced =
			hasPreheaderDereferenceProof(LIst, L, AA, LI);
		Value *Ptr = LIst->getPointerOperand();
		if (!Ptr) return false;
		if (!HeaderMustExecute && !PreheaderAlreadyDereferenced) return false;

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

			MemoryLocation LoadLocation = MemoryLocation::get(LIst);
			for (Instruction *J : LoopInsts) {
				if (!J || J == I) continue;
				if (auto *SI = dynamic_cast<StoreInst *>(J)) {
					Value *StorePtr = SI->getPointerOperand();
					if (!StorePtr) return false;

					AliasInfo Alias = AA.aliasWithIndexRange(
						LoadLocation, MemoryLocation::get(SI), LI);
					if (Alias == AliasInfo::NoAlias)
						continue;
					return false;
				}
				ModRefInfo MRI = getInstModRefInfo(
					LoadLocation, J, AA, LI, FAM);
			if (isModSet(MRI)) return false;
		}
		return true;
	}

	// 3.2 store 只能下沉：地址和值均循环不变，写入点支配所有退出和 latch，且循环内
	// 不存在对该位置的其他 Mod/Ref；配合对象证明保证每个出口写一次等价。
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

		bool SawExitingBlock = false;
		for (BasicBlock *BB : L.getBlocksIncludingSubLoops()) {
			if (!BB || !L.isExitingBlock(BB)) continue;
			SawExitingBlock = true;
			if (!DT.dominates(SBB, BB)) return false;
		}
		if (!SawExitingBlock) return false;
		for (BasicBlock *LatchBB : Latches)
			if (!DT.dominates(SBB, LatchBB)) return false;

		for (Instruction *J : LoopInsts) {
			if (!J || J == SI) continue;

				ModRefInfo MRI = getInstModRefInfo(
					MemoryLocation::get(SI), J, AA, LI, FAM);
			if (isModSet(MRI) || isRefSet(MRI)) return false;
		}
		return true;
	}

	// 2.6 调用只允许从 header 安全前缀直接提升；非必执行调用由缓存 CFG 路径处理。
	if (auto *CI = dynamic_cast<CallInst *>(I)) {

		if (ForSink) return false;
		if (!AggressiveHoist || !hasSafeExecutionPrefix(I)) return false;
		if (I->getParent() != L.getHeader()) return false;

		return isReusableLoopInvariantCall(CI, LoopInsts, AA, LI, FAM);
	}

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
	// 1. 获取支配、循环、AA 与 MemorySSA；一旦移动内存指令，旧 MemorySSA 不再用于
	// 后续证明，避免以陈旧内存版本继续优化。
	auto &DT = FAM.getResult<DominantAnalysis>(F);
	auto &LI = FAM.getResult<LoopAnalysis>(F);
	auto &AA = FAM.getResult<BasicAliasAnalysis>(F);
	auto &MSSA = FAM.getResult<MemorySSAAnalysis>(F);

	bool MemorySSAUsable = true;
	bool Changed = false;
	bool CFGChanged = false;
	std::unordered_map<ExitValueKey, PhiInst *, ExitValueKeyHash> ExitPhiCache;

	// 1.2 递归按内到外处理循环。每个循环确认 LCSSA/LoopSimplify 形态；缺少标准
	// preheader/latch 时只允许最保守的纯标量提升，store 下沉和调用缓存均关闭。
	std::function<bool(Loop *)> processLoop = [&](Loop *L) -> bool {
		if (!L) return false;
		bool LocalChanged = false;

		for (Loop *Sub : L->getSubLoops()) {
			LocalChanged |= processLoop(Sub);
			if (CFGChanged) return true;
		}

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
				auto BlockInsts = collectInstructions(*BB);
				Insts.insert(Insts.end(), BlockInsts.begin(), BlockInsts.end());
			}
			return Insts;
		};

		// 3. 下沉阶段按循环 RPO 逆序、块内逆序处理，使用户先移动；只处理所有用户均
		// 在循环外的安全 store，并维护已确认不变量集合。
		std::unordered_set<Instruction *> SinkInvariant;
		auto SinkBlocksRPO = CanSink
			? L->getBlocksInRPO()
			: std::vector<BasicBlock *>{};
		for (auto BIt = SinkBlocksRPO.rbegin(); BIt != SinkBlocksRPO.rend(); ++BIt) {
			BasicBlock *BB = *BIt;
			if (!BB) continue;

			auto Insts = collectInstructions(*BB);

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
						 true,
						 false)) {
					continue;
				}

				if (!areAllUsersOutsideLoop(I, *L)) {
					if (isInvariant(I, *L, DT, SinkInvariant)) SinkInvariant.insert(I);
					continue;
				}

				if (!isInvariant(I, *L, DT, SinkInvariant)) continue;

				BasicBlock *DefBB = I->getParent();
				bool CrossesTiming = false;
				for (BasicBlock *ExitBB : ExitBlocks) {
					Instruction *InsertBefore = ExitBB ? ExitBB->front() : nullptr;
					while (InsertBefore &&
						   dynamic_cast<PhiInst *>(InsertBefore)) {
						InsertBefore = InsertBefore->getNext();
					}
					if (!InsertBefore ||
						F.hasTimingBarrierOnPath(I, InsertBefore)) {
						CrossesTiming = true;
						break;
					}
				}
				if (CrossesTiming) continue;
				// 3.3 在每个出口 PHI 区后克隆 store；若操作数仍来自循环，为该出口创建并
				// 缓存 LCSSA bridge PHI。必须覆盖所有出口才能提交原指令删除。
				std::unordered_map<BasicBlock *, Instruction *> ExitClone;

				for (BasicBlock *ExitBB : ExitBlocks) {
					if (!ExitBB || !DT.dominates(DefBB, ExitBB)) continue;
					Instruction *Cloned = I->clone({});
					if (!Cloned) {
						continue;
					}

					forEachOperand(
						Cloned, [&](size_t, Use *U, Value *OldOp) {
						if (!OldOp) return;

						auto *DefOpI = dynamic_cast<Instruction *>(OldOp);
						if (!DefOpI || !DefOpI->getParent() ||
							!L->contains(DefOpI->getParent())) return;

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
								ExitBB->pushFront(Bridge);
								ExitPhiCache[Key] = Bridge;
							}
						}

						if (Bridge) U->replaceUseeWith(Bridge);
					});

					auto InsIt = ExitBB->begin();
					while (InsIt != ExitBB->end() && dynamic_cast<PhiInst *>(*InsIt)) ++InsIt;
					if (InsIt == ExitBB->end()) ExitBB->pushBack(Cloned);
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

				I->eraseFromParent();
				MemorySSAUsable = false;
				delete I;
				LocalChanged = true;
			}
		}

		// 4. 提升阶段按 RPO 正序处理，使操作数先进入 preheader；计算候选是否支配
		// 全部出口/latch，以决定可否使用激进的必执行推测规则。
		std::unordered_set<Instruction *> HoistInvariant;
		auto HoistBlocksRPO = L->getBlocksInRPO();
		for (BasicBlock *BB : HoistBlocksRPO) {
			if (!BB) continue;
			auto Insts = collectInstructions(*BB);

			for (Instruction *I : Insts) {
				if (!I || I->getParent() != BB) continue;
				if (!HasLoopSimplifyForm &&
					!dynamic_cast<BinaryInst *>(I) &&
					!dynamic_cast<SelectInst *>(I)) {
					continue;
				}
				auto LoopInsts = collectLiveLoopInsts();
				Instruction *PreHeaderTerminator =
					PreHeader->getTerminator();
				if (!PreHeaderTerminator ||
					F.hasTimingBarrierOnPath(
						PreHeader, PreHeaderTerminator->getPrev(), I)) {
					continue;
				}

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
						 false,
						 Aggressive);

				// 4.1 非必执行的不变量调用不能直接提前；在完整 LoopSimplify+LCSSA 形态下，
				// 尝试改写为首轮按需计算、后续迭代复用的缓存 CFG。
				if (!SafeToPreheader) {

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

				// 4.4 普通候选从原块摘下并插到 preheader 终结指令前，记录为后续候选不变量。
				I->eraseFromParent();
				TermIt.insertBefore(I);
				HoistInvariant.insert(I);
				LocalChanged = true;
			}
		}

		return LocalChanged;
	};

	// 5. 每个顶层循环在未改 CFG 时最多再跑一轮，消费首轮移动暴露的不变量；
	// 调用缓存改 CFG 后所有结构分析立即陈旧，因此停止整个遍历。
	for (Loop *TopL : LI.getTopLevelLoops()) {
		bool LocalChanged = processLoop(TopL);
		Changed |= LocalChanged;

		if (LocalChanged && !CFGChanged)
			Changed |= processLoop(TopL);
		if (CFGChanged) break;
	}

	if (!Changed) return PreservationStatus::all();
	if (CFGChanged) return PreservationStatus::none();

	// 6. 纯移动保留 CFG 分析但失效 AA/MemorySSA/副作用；缓存 CFG 改写则不保留分析。
	PreservationStatus PA;
	PA.keep<DominantAnalysis>();
	PA.keep<PostDominantAnalysis>();
	PA.keep<LoopAnalysis>();
	PA.discard<BasicAliasAnalysis>();
	PA.discard<MemorySSAAnalysis>();
	PA.discard<SideEffectAnalysis>();
	return PA;
}
