#include "ir/opt/LICM.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/MemorySSA.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "ir/opt/DCE.hpp"
#include "ir/opt/LCSSA.hpp"
#include "ir/opt/Mem2Reg.hpp"
#include "lib/CFG.hpp"
#include "lib/IRUtils.hpp"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>
//构造完整LICM或仅移动纯地址的晚期LICM。
LICMPass::LICMPass(LICMMode Mode) : Mode(Mode) {}
//计算出口基本块和SSA值组合键的哈希值
std::size_t ExitValueKeyHash::operator()(const ExitValueKey &K) const {
	auto A = reinterpret_cast<std::uintptr_t>(K.first);
	auto B = reinterpret_cast<std::uintptr_t>(K.second);
	return A ^ (B + 0x9e3779b97f4a7c15ULL + (A << 6) + (A >> 2));
}
//判断候选之前的块内前缀是否可安全提前执行
bool LICMPass::hasSafeExecutionPrefix(const Instruction *Candidate) {
	//2.1 从块首到候选之间不得有写入、调用或可能陷阱/poison 的操作；这证明把原本
	//必经块中的候选提前执行不会越过更早的可观察或异常行为。
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
//判断preheader是否已有同址内存访问证明Load地址可解引用
bool LICMPass::hasPreheaderDereferenceProof(
	LoadInst *Load, const Loop &L, BasicAAResult &AA,
	const LoopInfo &LI) {
	//2.2 顺序扫描 preheader，寻找尺寸覆盖且 MustAlias 的既有 load/store 作为地址
	//已可解引用证明；任一调用会清除该证明，避免跨未知控制点推测 load。
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
//证明内存位置的完整地址区间始终落在已知栈或全局对象内
bool LICMPass::isMemoryLocationProvablyInBounds(
	const MemoryLocation &Location, Instruction *Access,
	BasicAAResult &AA, SCEVResult &SE,
	RangeInfo &RI, const DominatorTree &DT) {
	//2.3 对象边界证明由BasicAA统一发布，LICM只选择真实访存程序点作为上下文。
	return AA.isLocationProvablyInBoundsAt(
		Location, Access, SE, RI, DT);
}
//判断除法或余数指令能否在不引入异常的情况下推测执行
bool LICMPass::isSpeculatableDivisionOrRemainder(Instruction *I) {
	//2.4 除数必须是非零常量；有符号除余还排除 -1，避免最小整数溢出情形。
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
//判断Store目标是否为可在循环出口安全写入的在界对象
bool LICMPass::isNonFaultingStoreObject(Value *Ptr, Value *StoredValue) {
	//3.1 下沉 store 仅接受全局/alloca 根上的全常量在界 GEP，并检查最终对象尺寸
	//覆盖写入值，保证在出口执行不会新增无效地址访问。
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
//判断只读调用结果是否在整个循环中保持可复用
bool LICMPass::isReusableLoopInvariantCall(
	CallInst *CI,
	const std::vector<Instruction *> &LoopInsts,
	BasicAAResult &AA,
	const LoopInfo &LI,
	FunctionAnalysisPassManager &FAM) {
	//2.4 可复用调用的 callee 副作用至多为已跟踪 Ref 且没有 Mod；
	//运行库及其包装调用由统一副作用摘要排除。
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
//判断指令的全部IR用户是否都位于给定循环之外
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
//判断指令全部操作数是否来自循环外或已证明的不变量定义
bool LICMPass::isInvariant(
	Instruction *I,
	const Loop &L,
	const DominatorTree &DT,
	const std::unordered_set<Instruction *> &InvariantSet) {
	//1.1 每个操作数必须来自循环外，或已在本轮不变量集合中且其定义支配当前指令。
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
//把非必执行循环不变调用改写为首轮计算后续复用的缓存CFG
bool LICMPass::cacheLoopInvariantCall(
	Function &F,
	CallInst *CI,
	Loop &L,
	BasicBlock *PreHeader,
	BasicBlock *Latch,
	const DominatorTree &DT,
	const std::vector<Instruction *> &LoopInsts,
	BasicAAResult &AA,
	const LoopInfo &LI,
	FunctionAnalysisPassManager &FAM,
	const std::unordered_set<Instruction *> &KnownInvariant) {
	//4.1 对不是每次必执行但结果循环不变的调用，构造“首轮计算、后续复用”CFG；
	//候选必须支配latch、非tail、非void且有用户。
	if (!CI || !PreHeader || !Latch) return false;
	if (CI->isTailCall()) return false;
	BasicBlock *BB = CI->getParent();
	BasicBlock *Header = L.getHeader();
	if (!BB || !Header || BB == Header) return false;
	if (!L.contains(BB) || !DT.dominates(BB, Latch)) return false;
	if (!CI->getIRType() || CI->getIRType()->isVoid())
		return false;
	if (!CI->hasUses()) return false;
	//4.2 从首个纯调用开始扩展连续单使用者链。链上每个节点都必须由循环外值或
	//已证明节点组成；嵌套调用继续复用统一副作用摘要，普通节点仅接受无内存纯值。
	//这样晚期内联拆开的包装计算仍作为一个整体缓存，不会只缓存依赖链首项。
	constexpr unsigned kMaxReusableChainLength = 32;
	std::vector<Instruction *> ComputeChain{CI};
	std::unordered_set<Instruction *> ChainInvariant = KnownInvariant;
	ChainInvariant.insert(CI);
	Instruction *Root = CI;
	while (ComputeChain.size() < kMaxReusableChainLength) {
		Instruction *SingleUser = nullptr;
		if (Root->countInstructionUses(&SingleUser) != 1 || !SingleUser)
			break;
		if (Root->getNext() != SingleUser || SingleUser->getParent() != BB ||
			!SingleUser->getIRType() || SingleUser->getIRType()->isVoid() ||
			!SingleUser->hasUses()) {
			break;
		}
		if (!isInvariant(SingleUser, L, DT, ChainInvariant)) break;
		//调用仍在原路径首次到达时执行，不会被推测到零次循环；对普通纯值再额外
		//拒绝可能陷阱或产生poison的节点，避免缓存链借助异常输入或未定义行为获利。
		if (!dynamic_cast<CallInst *>(SingleUser) &&
			instructionMayFaultOrProducePoison(SingleUser)) {
			break;
		}
		bool Reusable = false;
		if (auto *NextCall = dynamic_cast<CallInst *>(SingleUser)) {
			Reusable = !NextCall->isTailCall() &&
				isReusableLoopInvariantCall(
					NextCall, LoopInsts, AA, LI, FAM);
		} else if (dynamic_cast<BinaryInst *>(SingleUser) ||
			dynamic_cast<SelectInst *>(SingleUser) ||
			dynamic_cast<GetElementPtrInst *>(SingleUser)) {
			Reusable = true;
		} else {
			switch (SingleUser->instType) {
				case Instruction::TRUNC:
				case Instruction::ZEXT:
				case Instruction::SEXT:
				case Instruction::FPTOSI:
				case Instruction::SITOFP:
				case Instruction::BITCAST:
					Reusable = true;
					break;
				default:
					break;
			}
		}
		if (!Reusable) break;
		ComputeChain.push_back(SingleUser);
		ChainInvariant.insert(SingleUser);
		Root = SingleUser;
	}
	std::vector<Instruction *> Tail;
	bool SeenRoot = false;
	for (Instruction *I : collectInstructions(*BB)) {
		if (I == Root) {
			SeenRoot = true;
			continue;
		}
		if (SeenRoot) Tail.push_back(I);
	}
	if (!SeenRoot || Tail.empty()) return false;
	Instruction *OldTerminator = Tail.back();
	if (!dynamic_cast<BrInst *>(OldTerminator) &&
		!dynamic_cast<RetInst *>(OldTerminator)) {
		return false;
	}
	std::vector<BasicBlock *> OldSuccessors = BB->getSuccessors();
	//4.3 将链后的块尾拆到merge，完整不变量链移入compute；先重写旧后继PHI
	//的前驱，保持原终结边从新的merge块发出。
    auto *ComputeBB = new BasicBlock();
	auto *MergeBB = new BasicBlock();
	ComputeBB->setIdent(F.makeUniqueBlockIdent(
		BB->getIdent() + ".licm.compute"));
	MergeBB->setIdent(F.makeUniqueBlockIdent(
		BB->getIdent() + ".licm.merge"));
	F.pushBlock(ComputeBB);
	F.pushBlock(MergeBB);
	for (Instruction *I : ComputeChain) {
		I->eraseFromParent();
		ComputeBB->pushBack(I);
	}
	for (Instruction *I : Tail) {
		I->eraseFromParent();
		MergeBB->pushBack(I);
	}
	std::unordered_set<BasicBlock *> SeenSuccessors;
	for (BasicBlock *Succ : OldSuccessors) {
		if (!Succ || !SeenSuccessors.insert(Succ).second) continue;
		BasicBlock::replaceIncomingPredInSuccPhis(Succ, BB, MergeBB);
	}
	auto OriginalUses = Root->getUses();
	//4.4 header的first/cached PHI记录是否首轮及缓存值，merge的value PHI在
	//首轮取调用结果、以后取缓存；最后把原调用用户统一改到 value PHI。
	auto *ValuePhi = new PhiInst(Root->getIRType());
	ValuePhi->setIdent(ValuePhi->getIdent() + ".licm.value");
	ValuePhi->addIncoming(Root, ComputeBB);
	BasicBlock *BackedgePred = (Latch == BB) ? MergeBB : Latch;
	auto *FirstTrue = ConstBool::newConstBool(true);
	auto *FirstFalse = ConstBool::newConstBool(false);
	auto *FirstPhi = new PhiInst(FirstTrue->getIRType());
	FirstPhi->setIdent(FirstPhi->getIdent() + ".licm.first");
	FirstPhi->addIncoming(FirstTrue, PreHeader);
	FirstPhi->addIncoming(FirstFalse, BackedgePred);
	auto *CachedPhi = new PhiInst(Root->getIRType());
	CachedPhi->setIdent(CachedPhi->getIdent() + ".licm.cached");
	CachedPhi->addIncoming(
		UndefValue::NewUndefValue(Root->getIRType()), PreHeader);
	CachedPhi->addIncoming(ValuePhi, BackedgePred);
	ValuePhi->addIncoming(CachedPhi, BB);
	MergeBB->pushFront(ValuePhi);
	Header->pushFront(CachedPhi);
	Header->pushFront(FirstPhi);
	for (Use *U : OriginalUses)
		if (U && U->getValue() == Root) U->replaceUseeWith(ValuePhi);
	BB->genCondInst(FirstPhi, ComputeBB, MergeBB);
	ComputeBB->genUnCondInst(MergeBB);
	return true;
}
//判断值能否在循环预头终结指令之前直接使用
bool LICMPass::isAvailableAtPreheader(
	Value *V, BasicBlock *PreHeader, const DominatorTree &DT) {
	//4.5 常量、参数和全局值天然可用；跨块指令必须支配预头，同块指令还必须
	//真实位于终结指令之前，避免仅凭块自支配误用尚未定义的地址。
	if (!V || !PreHeader || !PreHeader->getTerminator()) return false;
	if (dynamic_cast<ConstantSystem *>(V) || V->isParam() || V->isGlobal() ||
		dynamic_cast<Function *>(V) || dynamic_cast<BuildInFunction *>(V)) {
		return true;
	}
	auto *I = dynamic_cast<Instruction *>(V);
	if (!I || !I->getParent()) return false;
	if (I->getParent() != PreHeader)
		return DT.dominates(I->getParent(), PreHeader);
	for (Instruction *Current : *PreHeader) {
		if (Current == I) return true;
		if (Current == PreHeader->getTerminator()) return false;
	}
	return false;
}
//为一个规范循环选择严格有收益的精确内存位置提升计划
bool LICMPass::planLoopMemoryPromotion(
	Function &F, Loop *L, BasicAAResult &AA, const LoopInfo &LI,
	const DominatorTree &DT, FunctionAnalysisPassManager &FAM,
	LoopMemoryPromotionPlan &Plan) {
	Plan = LoopMemoryPromotionPlan{};
	//4.6 第一版只处理叶子循环、唯一回边和唯一专用出口；这种形态使临时槽的
	//入口值与退出写回一一对应，也避免把子循环中的动态访问错误计入本层收益。
	if (!L || !L->getHeader() || !L->getSubLoops().empty() ||
		!LCSSAPass::isLCSSAForm(L)) {
		return false;
	}
	BasicBlock *PreHeader = nullptr;
	BasicBlock *Latch = nullptr;
	if (!L->getLoopSimplifyForm(PreHeader, Latch) ||
		!PreHeader || !Latch || !PreHeader->getTerminator()) {
		return false;
	}
	auto ExitBlocks = L->getExitBlocks();
	if (ExitBlocks.size() != 1 || !ExitBlocks.front() ||
		!ExitBlocks.front()->getTerminator()) {
		return false;
	}
	for (BasicBlock *Pred : ExitBlocks.front()->getPredecessors())
		if (!Pred || !L->contains(Pred)) return false;
	//4.7 收益下界只使用已证明的常量次数，或天然至少执行一次的 latch 退出循环；
	//未知零次循环不会因测试集常见长循环而被猜测为有收益。
	auto &SE = FAM.getResult<SCEVAnalysis>(F);
	auto Trip = SE.analyzeLoopTripCount(L);
	if (!Trip || !Trip->Valid || !Trip->ExitBlock ||
		Trip->ExitBlock != ExitBlocks.front()) {
		return false;
	}
	std::int64_t MinimumTripCount = 0;
	if (Trip->ConstantTripCount && Trip->ProvenNoWrap &&
		*Trip->ConstantTripCount > 0) {
		MinimumTripCount = *Trip->ConstantTripCount;
	} else if (Trip->ExitingBlock == Latch &&
		SE.isKnownFiniteLoop(L)) {
		MinimumTripCount = 1;
	}
	if (MinimumTripCount <= 0) return false;
	//4.8 前测循环的body不按纯CFG支配header退出边；只有唯一退出条件和同一
	//SCEV结果证明常量正次数时，才把“至少先经过一次latch”纳入必执行证明。
	std::vector<BasicBlock *> ExitingBlocks;
	for (BasicBlock *BB : L->getBlocksIncludingSubLoops())
		if (BB && L->isExitingBlock(BB)) ExitingBlocks.push_back(BB);
	const bool HasPositiveHeaderTrip =
		ExitingBlocks.size() == 1 &&
		ExitingBlocks.front() == L->getHeader() &&
		Trip->ExitingBlock == L->getHeader() &&
		Trip->ConstantTripCount && *Trip->ConstantTripCount > 0;
	auto AccessExecutesBeforeExitEdge =
		[&](Instruction *Access, BasicBlock *ExitPred) {
			if (!Access || !Access->getParent() || !ExitPred) return false;
			if (DT.dominates(Access->getParent(), ExitPred)) return true;
			return HasPositiveHeaderTrip &&
				ExitPred == L->getHeader() &&
				DT.dominates(Access->getParent(), Latch);
		};
	std::vector<Instruction *> LoopInstructions;
	for (BasicBlock *BB : L->getBlocksInFunctionOrder()) {
		if (!BB) return false;
		for (Instruction *I : *BB) {
			if (!I) return false;
			//任何调用都可能形成I/O、计时或未知顺序屏障；即使别名摘要声称NoModRef，
			//内存状态提升也不跨越它，防止延迟写回改变可观察时序。
			if (dynamic_cast<CallInst *>(I)) return false;
			//循环内return没有CFG后继，不会出现在普通ExitBlocks中；若允许它，延迟
			//写回会沿该路径丢失。当前先保守拒绝，避免为单一热点复制函数出口规范化。
			if (dynamic_cast<RetInst *>(I)) return false;
			LoopInstructions.push_back(I);
		}
	}
	LoopMemoryPromotionPlan Best;
	for (Instruction *SeedI : LoopInstructions) {
		auto *Seed = dynamic_cast<LoadInst *>(SeedI);
		if (!Seed || !Seed->getParent() ||
			!DT.dominates(Seed->getParent(), Latch)) {
			continue;
		}
		//4.9 初始load会被移到预头，因此还必须证明原程序在每条真实退出路径上
		//都会先执行该load；仅支配latch无法排除提前退出。与此同时沿用普通
		//LICM的安全前缀/已有解引用证明，避免改变潜在故障相对顺序。
		bool DominatesEveryExitEdge = true;
		for (BasicBlock *Exit : ExitBlocks) {
			for (BasicBlock *Pred : Exit->getPredecessors()) {
				if (!Pred || !L->contains(Pred) ||
					!AccessExecutesBeforeExitEdge(Seed, Pred)) {
					DominatesEveryExitEdge = false;
					break;
				}
			}
			if (!DominatesEveryExitEdge) break;
		}
		const bool HasSafeInitialRead =
			(Seed->getParent() == L->getHeader() &&
			 hasSafeExecutionPrefix(Seed)) ||
			hasPreheaderDereferenceProof(Seed, *L, AA, LI);
		if (!DominatesEveryExitEdge || !HasSafeInitialRead) continue;
		Value *Pointer = Seed->getPointerOperand();
		auto ValueType = Seed->getIRType();
		MemoryLocation Location = MemoryLocation::get(Seed);
		if (!Pointer || !ValueType || ValueType->isVoid() ||
			ValueType->isArray() || !Location.Size ||
			!isAvailableAtPreheader(Pointer, PreHeader, DT)) {
			continue;
		}
		auto &RI = FAM.getResult<RangeAnalysis>(F);
		const bool CandidateInBounds =
			isMemoryLocationProvablyInBounds(
				Location, Seed, AA, SE, RI, DT);
		std::vector<LoadInst *> Loads;
		std::vector<StoreInst *> Stores;
		bool Valid = true;
		for (Instruction *I : LoopInstructions) {
			MemoryLocation Other;
			if (auto *Load = dynamic_cast<LoadInst *>(I)) {
				Other = MemoryLocation::get(Load);
				AliasInfo Alias = AA.aliasWithIndexRange(Location, Other, LI);
				//4.10 仅当两次访问都在已知对象内时才允许跨越NoAlias读取；
				//BasicAA因动态GEP退化为MayAlias时，再以唯一对象互斥事实补强，
				//不依据测试名、输入值或未定义越界行为发布结论。
				const bool OtherInBounds =
					isMemoryLocationProvablyInBounds(
						Other, Load, AA, SE, RI, DT);
				if (Alias == AliasInfo::MayAlias && CandidateInBounds &&
					OtherInBounds && AA.haveKnownDistinctUnderlyingObjects(
						Location.Pointer, Other.Pointer)) {
					Alias = AliasInfo::NoAlias;
				}
				if (Alias == AliasInfo::NoAlias) {
					if (!CandidateInBounds || !OtherInBounds) {
						Valid = false;
						break;
					}
					continue;
				}
				if (Alias != AliasInfo::MustAlias || !Other.Size ||
					*Other.Size != *Location.Size ||
					Load->getIRType() != ValueType ||
					!DT.dominates(Load->getParent(), Latch)) {
					Valid = false;
					break;
				}
				Loads.push_back(Load);
				continue;
			}
			if (auto *Store = dynamic_cast<StoreInst *>(I)) {
				Other = MemoryLocation::get(Store);
				AliasInfo Alias = AA.aliasWithIndexRange(Location, Other, LI);
				//其他位置的写入也必须同时满足不别名和完整对象内证明；未知
				//指针仍作为顺序边界，避免延迟写回改变故障前的可观察内存状态。
				const bool OtherInBounds =
					isMemoryLocationProvablyInBounds(
						Other, Store, AA, SE, RI, DT);
				if (Alias == AliasInfo::MayAlias && CandidateInBounds &&
					OtherInBounds && AA.haveKnownDistinctUnderlyingObjects(
						Location.Pointer, Other.Pointer)) {
					Alias = AliasInfo::NoAlias;
				}
				if (Alias == AliasInfo::NoAlias) {
					if (!CandidateInBounds || !OtherInBounds) {
						Valid = false;
						break;
					}
					continue;
				}
				Value *Stored = Store->getValueOperand();
				if (Alias != AliasInfo::MustAlias || !Other.Size ||
					*Other.Size != *Location.Size || !Stored ||
					Stored->getIRType() != ValueType ||
					!DT.dominates(Store->getParent(), Latch)) {
					Valid = false;
					break;
				}
				Stores.push_back(Store);
				continue;
			}
			//除候选同址访存外，动态除余、非法转换和不受约束移位等
			//可能故障或产生poison的计算也不能被写回移动跨越。
			if (instructionMayFaultOrProducePoison(I)) {
				Valid = false;
				break;
			}
		}
		if (!Valid || Loads.empty() || Stores.empty()) continue;
		//4.11 出口写回只能替代原本必然发生的写入。每条真实退出边都必须
		//已由至少一条候选store支配，不能在提前退出路径凭空增加一次写操作。
		bool StoreBeforeEveryExitEdge = true;
		for (BasicBlock *Exit : ExitBlocks) {
			for (BasicBlock *Pred : Exit->getPredecessors()) {
				if (!Pred || !L->contains(Pred)) {
					StoreBeforeEveryExitEdge = false;
					break;
				}
				bool HasDominatingStore = false;
				for (StoreInst *Store : Stores) {
					if (Store && Store->getParent() &&
						AccessExecutesBeforeExitEdge(Store, Pred)) {
						HasDominatingStore = true;
						break;
					}
				}
				if (!HasDominatingStore) {
					StoreBeforeEveryExitEdge = false;
					break;
				}
			}
			if (!StoreBeforeEveryExitEdge) break;
		}
		if (!StoreBeforeEveryExitEdge) continue;
		//4.12 每个旧load/store按现有循环成本模型计2；新方案每次迭代保守计一份
		//循环携带值成本，循环外固定支付一次load和一次store。只接受严格正下界。
		const std::int64_t AccessCount = static_cast<std::int64_t>(
			Loads.size() + Stores.size());
		const __int128 OldCost = static_cast<__int128>(MinimumTripCount) *
			AccessCount * 2;
		const __int128 NewCost = static_cast<__int128>(MinimumTripCount) + 4;
		const __int128 Net = OldCost - NewCost;
		if (Net <= 0 || Net > std::numeric_limits<std::int64_t>::max())
			continue;
		if (Best.L && Net <= Best.EstimatedNetBenefit) continue;
		Best.L = L;
		Best.Pointer = Pointer;
		Best.ValueType = ValueType;
		Best.PreHeader = PreHeader;
		Best.Latch = Latch;
		Best.ExitBlocks = ExitBlocks;
		Best.Loads = std::move(Loads);
		Best.Stores = std::move(Stores);
		Best.MandatoryAccesses = static_cast<unsigned>(AccessCount);
		Best.EstimatedNetBenefit = static_cast<std::int64_t>(Net);
	}
	if (!Best.L) return false;
	Plan = std::move(Best);
	return true;
}
//为条件更新位置选择保留真实Store的循环携带读取缓存计划
bool LICMPass::planStoreSynchronizedLoadCache(
	Function &F, Loop *L, BasicAAResult &AA, const LoopInfo &LI,
	const DominatorTree &DT, FunctionAnalysisPassManager &FAM,
	LoopMemoryPromotionPlan &Plan) {
	Plan = LoopMemoryPromotionPlan{};
	//4.13 首版只处理嵌套的叶子规范循环。preheader必须无条件仅通往header，
	//且原Seed在每次header执行时必经；因此初值load与原Seed具有相同动态执行域。
	if (!L || !L->getHeader() || !L->getSubLoops().empty() ||
		L->getDepth() < 2 || !LCSSAPass::isLCSSAForm(L)) {
		return false;
	}
	BasicBlock *PreHeader = nullptr;
	BasicBlock *Latch = nullptr;
	if (!L->getLoopSimplifyForm(PreHeader, Latch) ||
		!PreHeader || !Latch || !PreHeader->getTerminator()) {
		return false;
	}
	auto *PreHeaderBranch = dynamic_cast<BrInst *>(PreHeader->getTerminator());
	auto PreHeaderSuccessors = PreHeader->getSuccessors();
	if (!PreHeaderBranch || PreHeaderBranch->isConditional() ||
		PreHeaderSuccessors.size() != 1 ||
		PreHeaderSuccessors.front() != L->getHeader()) {
		return false;
	}
	auto ExitBlocks = L->getExitBlocks();
	if (ExitBlocks.size() != 1 || !ExitBlocks.front() ||
		!ExitBlocks.front()->getTerminator()) {
		return false;
	}
	for (BasicBlock *Pred : ExitBlocks.front()->getPredecessors())
		if (!Pred || !L->contains(Pred)) return false;
	auto LoopBlocks = L->getBlocksInFunctionOrder();
	constexpr std::size_t MaxCacheLoopBlocks = 8;
	if (LoopBlocks.empty() || LoopBlocks.size() > MaxCacheLoopBlocks)
		return false;
	//已知最多执行一次时缓存没有循环复用价值；未知次数只在嵌套、低压力
	//形态下保守尝试，最终仍由对应热点的机器码和实测收益验收。
	auto &SE = FAM.getResult<SCEVAnalysis>(F);
	auto KnownTripCount = SE.getSmallConstantTripCount(L);
	if (KnownTripCount && *KnownTripCount <= 1) return false;
	//新增一个循环携带标量会延长活跃区间；用header现有PHI数量限制通用
	//寄存器压力，而不是依据测试规模或输入次数猜测收益。
	constexpr unsigned MaxExistingHeaderPhis = 4;
	unsigned HeaderPhiCount = 0;
	Instruction *FirstNonPhi = nullptr;
	for (Instruction *I : *L->getHeader()) {
		if (dynamic_cast<PhiInst *>(I)) {
			++HeaderPhiCount;
			continue;
		}
		FirstNonPhi = I;
		break;
	}
	if (!FirstNonPhi || HeaderPhiCount > MaxExistingHeaderPhis) return false;
	std::vector<Instruction *> LoopInstructions;
	for (BasicBlock *BB : LoopBlocks) {
		if (!BB) return false;
		for (Instruction *I : *BB) {
			if (!I || dynamic_cast<CallInst *>(I) ||
				dynamic_cast<RetInst *>(I) ||
				dynamic_cast<AllocaInst *>(I)) {
				return false;
			}
			LoopInstructions.push_back(I);
		}
	}
	LoopMemoryPromotionPlan Best;
	for (Instruction *SeedI : LoopInstructions) {
		auto *Seed = dynamic_cast<LoadInst *>(SeedI);
		//初值load必须正是header的第一条非PHI指令。它只跨越进入桥的无条件
		//分支和header PHI，保持第一次潜在故障及所有可观察操作的先后关系。
		if (!Seed || Seed != FirstNonPhi || !Seed->hasUses() ||
			Seed->getParent() != L->getHeader() ||
			!hasSafeExecutionPrefix(Seed)) {
			continue;
		}
		Value *Pointer = Seed->getPointerOperand();
		auto ValueType = Seed->getIRType();
		MemoryLocation Location = MemoryLocation::get(Seed);
		if (!Pointer || valueMayDependOnUndef(Pointer) || !ValueType ||
			!ValueType->isScalar() || ValueType->getSize() == 0 ||
			ValueType->getSize() > 8 || !Location.Size ||
			!isAvailableAtPreheader(Pointer, PreHeader, DT)) {
			continue;
		}
		std::vector<LoadInst *> Loads;
		std::vector<StoreInst *> Stores;
		bool Valid = true;
		for (Instruction *I : LoopInstructions) {
			if (auto *Load = dynamic_cast<LoadInst *>(I)) {
				MemoryLocation Other = MemoryLocation::get(Load);
				AliasInfo Alias = AA.aliasWithIndexRange(Location, Other, LI);
				//只把完整同址同型load接入缓存。其他只读访问保持原位；即使
				//BasicAA只能给出MayAlias，它仍观察未改动的真实Store序列。
				if (Alias != AliasInfo::MustAlias) continue;
				if (!Other.Size || *Other.Size != *Location.Size ||
					Load->getIRType() != ValueType ||
					valueMayDependOnUndef(Load->getPointerOperand()) ||
					!DT.dominates(L->getHeader(), Load->getParent()) ||
					(Load->getParent() == L->getHeader() &&
					 Load != Seed && !Seed->comesBefore(Load))) {
					Valid = false;
					break;
				}
				Loads.push_back(Load);
				continue;
			}
			if (auto *Store = dynamic_cast<StoreInst *>(I)) {
				MemoryLocation Other = MemoryLocation::get(Store);
				Value *Stored = Store->getValueOperand();
				//首版要求循环内每一条真实写都完整更新候选位置。任何无关写、
				//部分覆盖或MayAlias写都拒绝，保证不存在绕过私有缓存的修改。
				if (AA.aliasWithIndexRange(Location, Other, LI) !=
						AliasInfo::MustAlias ||
					!Other.Size || *Other.Size != *Location.Size || !Stored ||
					Stored->getIRType() != ValueType ||
					valueMayDependOnUndef(Store->getPointerOperand()) ||
					valueMayDependOnUndef(Stored)) {
					Valid = false;
					break;
				}
				Stores.push_back(Store);
			}
		}
		constexpr std::size_t MaxCachedLoads = 16;
		constexpr std::size_t MaxMirroredStores = 8;
		if (!Valid || Loads.empty() || Stores.empty() ||
			Loads.size() > MaxCachedLoads ||
			Stores.size() > MaxMirroredStores) {
			continue;
		}
		//初值load由首轮原load平移而来。执行N次时真实读取由N次降为1次，
		//N为0或1均不增加访存；嵌套与PHI预算负责约束未知次数下的收益风险。
		const std::int64_t Benefit = static_cast<std::int64_t>(Loads.size());
		if (Best.L && Benefit <= Best.EstimatedNetBenefit) continue;
		Best.L = L;
		Best.Pointer = Pointer;
		Best.ValueType = ValueType;
		Best.PreHeader = PreHeader;
		Best.Latch = Latch;
		Best.ExitBlocks = ExitBlocks;
		Best.Loads = std::move(Loads);
		Best.Stores = std::move(Stores);
		Best.PreserveOriginalStores = true;
		Best.MandatoryAccesses = static_cast<unsigned>(Best.Loads.size());
		Best.EstimatedNetBenefit = Benefit;
	}
	if (!Best.L) return false;
	Plan = std::move(Best);
	return true;
}
//通过临时标量槽复用Mem2Reg，按计划同步真实Store或在出口写回
bool LICMPass::applyLoopMemoryPromotion(
	Function &F, LoopMemoryPromotionPlan &Plan,
	FunctionAnalysisPassManager &FAM) {
	//4.14 提交前重新验证全部节点和挂接点；临时alloca只接受直接load/store使用，
	//随后由既有Mem2Reg统一放置PHI和重命名，避免在LICM中复制一套不完整SSA算法。
	BasicBlock *Entry = F.front();
	if (!Entry || !Entry->getTerminator() || !Plan.L || !Plan.Pointer ||
		!Plan.ValueType || !Plan.PreHeader || !Plan.PreHeader->getTerminator() ||
		Plan.ExitBlocks.empty() || Plan.Loads.empty() || Plan.Stores.empty() ||
		Plan.EstimatedNetBenefit <= 0) {
		return false;
	}
	for (LoadInst *Load : Plan.Loads)
		if (!Load || !Load->getParent() ||
			Load->getPointerOperand() == nullptr) return false;
	for (StoreInst *Store : Plan.Stores)
		if (!Store || !Store->getParent() ||
			Store->getPointerOperand() == nullptr) return false;
	for (BasicBlock *Exit : Plan.ExitBlocks)
		if (!Exit || !Exit->getTerminator()) return false;
	auto *Slot = new AllocaInst(Plan.ValueType);
	Slot->setIdent(Slot->getIdent() + ".licm.promoted");
	//入口块理论上没有PHI，仍按通用IR约束把alloca放到PHI之后。
	Entry->insertAfterPhiNodes(Slot);
	auto *InitialLoad = new LoadInst(Plan.Pointer);
	InitialLoad->setIdent(InitialLoad->getIdent() + ".licm.init");
	Plan.PreHeader->insertBeforeTerminator(InitialLoad);
	auto *InitialStore = new StoreInst(InitialLoad, Slot);
	Plan.PreHeader->insertBeforeTerminator(InitialStore);
	for (LoadInst *Load : Plan.Loads)
		Load->replaceOperand(0, Slot);
	if (Plan.PreserveOriginalStores) {
		//4.15 真实Store保持原地址和原控制位置；先完成可能故障的真实写，再把
		//同值写入私有槽。Mem2Reg随后把镜像写折成条件SSA更新，不新增内存操作。
		for (StoreInst *Store : Plan.Stores) {
			auto *Mirror = new StoreInst(Store->getValueOperand(), Slot);
			Instruction::insertAfter(Store, Mirror);
		}
	} else {
		for (StoreInst *Store : Plan.Stores)
			Store->replaceOperand(1, Slot);
		for (BasicBlock *Exit : Plan.ExitBlocks) {
			auto *ExitLoad = new LoadInst(Slot);
			ExitLoad->setIdent(ExitLoad->getIdent() + ".licm.exit");
			Exit->insertAfterPhiNodes(ExitLoad);
			auto *WriteBack = new StoreInst(ExitLoad, Plan.Pointer);
			Instruction::insertAfter(ExitLoad, WriteBack);
		}
	}
	//4.16 CFG未改变，现有支配树仍可供Mem2Reg构造裁剪SSA；调用后LICM立即停止，
	//由外层PassManager按none状态重建AA、MemorySSA、SCEV和范围分析。
	PromotePass Promoter;
	PassResult Result = Promoter.run(F, FAM);
	//临时槽已满足Mem2Reg的直接同类型访问约束；无论其内部是否还有额外可清理
	//对象，本函数在提交桥接指令后都必须如实报告IR发生改变。
	(void)Result;
	return true;
}
//综合验证候选能否安全提升到preheader或下沉到全部出口
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
	//2. 先按 opcode 排除控制、PHI、alloca 和不可安全推测的除余/转换/移位；
	//普通下沉当前只支持 store，普通提升再按指令类别继续验证。
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
	//2.5 load 只能提升：必须原本在每轮必执行的安全 header 前缀，或 preheader 已
	//解引用同一地址；优先复用 MemorySSA 的稀疏访问图检查整个循环，旧图失效后
	//再退回逐指令 AA，证明没有任何迭代会改写该位置。
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
		MemoryLocation LoadLocation = MemoryLocation::get(LIst);
		if (MemorySSAUsable) {
			return !MSSA.hasInterferingAccessInLoop(
				LoadLocation, L, LIst, false, AA, LI, FAM);
		}
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
	//3.2 store 只能下沉：地址和值均循环不变，写入点支配所有退出和 latch，且循环内
	//不存在对该位置的其他 Mod/Ref；优先通过 MemorySSA 稀疏节点检查，旧图失效后
	//再逐指令查询；配合对象证明保证每个出口写一次等价。
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
		MemoryLocation StoreLocation = MemoryLocation::get(SI);
		if (MemorySSAUsable) {
			return !MSSA.hasInterferingAccessInLoop(
				StoreLocation, L, SI, true, AA, LI, FAM);
		}
		for (Instruction *J : LoopInsts) {
			if (!J || J == SI) continue;
			ModRefInfo MRI = getInstModRefInfo(
				StoreLocation, J, AA, LI, FAM);
			if (isModSet(MRI) || isRefSet(MRI)) return false;
		}
		return true;
	}
	//2.6 调用只允许从 header 安全前缀直接提升；非必执行调用由缓存 CFG 路径处理。
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
//按由内到外的循环顺序运行LICM并返回分析保留状态
PreservationStatus LICMPass::run(Function &F, FunctionAnalysisPassManager &FAM) {
	//1. 获取支配、循环、AA 与 MemorySSA；一旦移动内存指令，旧 MemorySSA 不再用于
	//后续证明，避免以陈旧内存版本继续优化。
	const bool AddressOnly = Mode == LICMMode::AddressOnly;
	auto &DT = FAM.getResult<DominantAnalysis>(F);
	auto &LI = FAM.getResult<LoopAnalysis>(F);
	SCEVResult *AddressSE = AddressOnly
		? &FAM.getResult<SCEVAnalysis>(F)
		: nullptr;
	auto &AA = FAM.getResult<BasicAliasAnalysis>(F);
	auto &MSSA = FAM.getResult<MemorySSAAnalysis>(F);
	bool MemorySSAUsable = true;
	bool Changed = false;
	bool CFGChanged = false;
	bool MemoryPromotionChanged = false;
	std::unordered_map<ExitValueKey, PhiInst *, ExitValueKeyHash> ExitPhiCache;
	//1.2 递归按内到外处理循环。每个循环确认 LCSSA/LoopSimplify 形态；缺少标准
	//preheader/latch 时只允许最保守的纯标量提升，store 下沉和调用缓存均关闭。
	std::function<bool(Loop *)> processLoop = [&](Loop *L) -> bool {
		if (!L) return false;
		bool LocalChanged = false;
		for (Loop *Sub : L->getSubLoops()) {
			LocalChanged |= processLoop(Sub);
			if (CFGChanged || MemoryPromotionChanged) return true;
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
		//晚期纯地址模式只接受规范循环，并关闭store下沉、调用缓存和内存提升，
		//避免为消费最终GEP形态而扩大整套LICM的行为范围。
		if (AddressOnly && !HasLoopSimplifyForm) return LocalChanged;
		if (AddressOnly) {
			//纯地址模式只处理已证明至少执行两次且归纳不回绕的循环，确保
			//把每迭代必经GEP移到预头会严格减少动态地址计算，而不是猜测热度。
			auto TripCount = AddressSE
				? AddressSE->getSmallConstantTripCount(L)
				: std::nullopt;
			if (!TripCount || *TripCount < 2) return LocalChanged;
		}
		const bool CanSink = !AddressOnly && HasLoopSimplifyForm && InLCSSA;
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
		//3. 下沉阶段按循环 RPO 逆序、块内逆序处理，使用户先移动；只处理所有用户均
		//在循环外的安全 store，并维护已确认不变量集合。
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
				bool CrossesOrderingBarrier = false;
				for (BasicBlock *ExitBB : ExitBlocks) {
					Instruction *InsertBefore = ExitBB ? ExitBB->front() : nullptr;
					while (InsertBefore &&
						   dynamic_cast<PhiInst *>(InsertBefore)) {
						InsertBefore = InsertBefore->getNext();
					}
					if (!InsertBefore || hasOrderingBarrierOnPath(
							F, I, InsertBefore, FAM)) {
						CrossesOrderingBarrier = true;
						break;
					}
				}
				if (CrossesOrderingBarrier) continue;
				//3.3 在每个出口 PHI 区后克隆 store；若操作数仍来自循环，为该出口创建并
				//缓存 LCSSA bridge PHI。必须覆盖所有出口才能提交原指令删除。
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
		//4. 提升阶段按 RPO 正序处理，使操作数先进入 preheader；计算候选是否支配
		//全部出口/latch，以决定可否使用激进的必执行推测规则。
		std::unordered_set<Instruction *> HoistInvariant;
		auto HoistBlocksRPO = L->getBlocksInRPO();
		for (BasicBlock *BB : HoistBlocksRPO) {
			if (!BB) continue;
			auto Insts = collectInstructions(*BB);
			for (Instruction *I : Insts) {
				if (!I || I->getParent() != BB) continue;
				if (AddressOnly &&
					!dynamic_cast<GetElementPtrInst *>(I)) {
					continue;
				}
				if (AddressOnly) {
					//候选原块必须支配全部回边，保证它在每次完成的迭代中都会
					//执行；无用户地址和条件地址不会因静态次数被误判为收益。
					bool ExecutesEveryIteration = I->hasUses() && !Latches.empty();
					for (BasicBlock *LatchBB : Latches) {
						if (!LatchBB || !DT.dominates(BB, LatchBB)) {
							ExecutesEveryIteration = false;
							break;
						}
					}
					if (!ExecutesEveryIteration) continue;
				}
				if (!HasLoopSimplifyForm &&
					!dynamic_cast<BinaryInst *>(I) &&
					!dynamic_cast<SelectInst *>(I)) {
					continue;
				}
				auto LoopInsts = collectLiveLoopInsts();
				Instruction *PreHeaderTerminator = PreHeader->getTerminator();
				if (!PreHeaderTerminator) continue;
				if (dynamic_cast<LoadInst *>(I) &&
					hasOrderingBarrierOnPath(
						F, PreHeader, PreHeaderTerminator->getPrev(), I, FAM)) {
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
				//4.1 非必执行的不变量调用不能直接提前；在完整 LoopSimplify+LCSSA 形态下，
				//尝试改写为首轮按需计算、后续迭代复用的缓存 CFG。
				if (!SafeToPreheader) {
					if (AddressOnly) continue;
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
							F, CI, *L, PreHeader, Latch, DT,
							LoopInsts, AA, LI, FAM, HoistInvariant)) {
						//调用节点已经被移入新块，MemoryAccess中的Block和定义链随即陈旧。
						if (MSSA.getMemoryAccess(CI))
							MemorySSAUsable = false;
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
				//4.4 普通候选从原块摘下并插到 preheader 终结指令前，记录为后续候选不变量。
				const bool MovesMemoryAccess =
					MemorySSAUsable && MSSA.getMemoryAccess(I);
				I->eraseFromParent();
				TermIt.insertBefore(I);
				//MemorySSA 不做增量更新；移动 load/只读 call 后禁止后续候选读取旧图。
				if (MovesMemoryAccess) MemorySSAUsable = false;
				HoistInvariant.insert(I);
				LocalChanged = true;
			}
		}
		//4.5 普通提升完成后再检查同址load-update-store；此时其循环不变GEP已可能
		//进入preheader。一次只提交一个位置，Mem2Reg改写SSA后立即停止使用旧分析。
		if (!AddressOnly && HasLoopSimplifyForm && InLCSSA) {
			LoopMemoryPromotionPlan Promotion;
			bool HasPromotion = planLoopMemoryPromotion(
				F, L, AA, LI, DT, FAM, Promotion);
			if (!HasPromotion) {
				HasPromotion = planStoreSynchronizedLoadCache(
					F, L, AA, LI, DT, FAM, Promotion);
			}
			if (HasPromotion &&
				applyLoopMemoryPromotion(F, Promotion, FAM)) {
				MemoryPromotionChanged = true;
				LocalChanged = true;
				return true;
			}
		}
		return LocalChanged;
	};
	//5. 每个顶层循环在未改 CFG 时最多再跑一轮，消费首轮移动暴露的不变量；
	//调用缓存改 CFG 后所有结构分析立即陈旧，因此停止整个遍历。
	for (Loop *TopL : LI.getTopLevelLoops()) {
		bool LocalChanged = processLoop(TopL);
		Changed |= LocalChanged;
		if (LocalChanged && !CFGChanged && !MemoryPromotionChanged)
			Changed |= processLoop(TopL);
		if (CFGChanged || MemoryPromotionChanged) break;
	}
	if (!Changed) return PreservationStatus::all();
	if (CFGChanged || MemoryPromotionChanged)
		return PreservationStatus::none();
	//6. 纯移动保留 CFG 分析但失效 AA/MemorySSA/副作用；缓存 CFG 改写则不保留分析。
	PreservationStatus PA;
	PA.keep<DominantAnalysis>();
	PA.keep<PostDominantAnalysis>();
	PA.keep<LoopAnalysis>();
	PA.discard<BasicAliasAnalysis>();
	PA.discard<MemorySSAAnalysis>();
	PA.discard<SideEffectAnalysis>();
	return PA;
}
