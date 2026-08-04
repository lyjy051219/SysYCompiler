#include "ir/opt/LoopExitValueSimplify.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/analysis/SCEVExpander.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "ir/opt/DCE.hpp"
#include "ir/opt/LCSSA.hpp"
#include "lib/IRUtils.hpp"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <unordered_set>
#include <vector>
//判断单条循环指令能否在删除整个有限循环时无观察地丢弃。
bool LoopExitValueSimplifyPass::isSafePureInstruction(Instruction *I) {
	//1.1 控制流与PHI本身无副作用；访存、调用、分配、返回和未知指令一律拒绝。
	if (!I) return false;
	if (dynamic_cast<PhiInst *>(I) || dynamic_cast<BrInst *>(I)) return true;
	if (dynamic_cast<LoadInst *>(I) || dynamic_cast<StoreInst *>(I) ||
		dynamic_cast<CallInst *>(I) || dynamic_cast<AllocaInst *>(I) ||
		dynamic_cast<RetInst *>(I) ||
		dynamic_cast<GetElementPtrInst *>(I)) {
		return false;
	}
	//1.2 即使结果最终无用户，也不删除可能陷阱、产生poison或语义未知的计算。
	return !dceInstructionMustBePreserved(I) &&
		!instructionMayFaultOrProducePoison(I);
}
//检查循环全部指令均无副作用、故障、poison和未知语义。
bool LoopExitValueSimplifyPass::hasNoObservableInstruction(Loop *L) {
	//1.3 第一版只接受不含子循环的叶子循环，逐条按通用指令语义完成白名单验证。
	if (!L || !L->getSubLoops().empty()) return false;
	for (BasicBlock *BB : L->getBlocksInFunctionOrder()) {
		if (!BB) return false;
		for (Instruction *I : *BB)
			if (!isSafePureInstruction(I)) return false;
	}
	return true;
}
//估算删除循环能够消除的通用静态IR成本。
unsigned LoopExitValueSimplifyPass::estimateRemovableLoopCost(Loop *L) {
	//4.1 成本仅由opcode类别决定，不读取名字、常量取值、输入模式或指令邻接模式。
	if (!L) return 0;
	unsigned Cost = 0;
	for (BasicBlock *BB : L->getBlocksInFunctionOrder()) {
		if (!BB) continue;
		for (Instruction *I : *BB) {
			if (!I || dynamic_cast<PhiInst *>(I)) continue;
			Cost += estimateInstructionCost(I);
		}
	}
	return Cost;
}
//估算单条可删除指令的目标无关通用成本。
unsigned LoopExitValueSimplifyPass::estimateInstructionCost(Instruction *I) {
	//成本按运算类别而不是局部排列计数，避免形成依赖测试代码形状的指纹。
	if (!I || dynamic_cast<PhiInst *>(I)) return 0;
	switch (I->instType) {
	case Instruction::MUL:
	case Instruction::FMUL:
	case Instruction::FADD:
	case Instruction::FSUB:
	case Instruction::FCMP:
		return 2;
	case Instruction::SITOFP:
		return 3;
	case Instruction::SDIV:
	case Instruction::UDIV:
	case Instruction::FDIV:
	case Instruction::SREM:
	case Instruction::UREM:
	case Instruction::FREM:
		return 8;
	default:
		return 1;
	}
}
//结合SCEV、Range与支配信息证明候选循环次数可靠且有限。
bool LoopExitValueSimplifyPass::hasReliableTripCount(
    SCEVResult &SE, const LoopTripCountInfo &Trip,
    RangeInfo &RI, DominatorTree &DT) {
	//1.4 常量零次循环交给既有LoopDeletion；正次数必须证明归纳更新不回绕。
	if (!Trip.Valid || !Trip.TripCount || !Trip.BackedgeTakenCount ||
		Trip.ComparePhase == LoopExitComparePhase::Unknown) {
		return false;
	}
	if (Trip.ConstantTripCount) {
		return *Trip.ConstantTripCount > 0 && Trip.ProvenNoWrap;
	}
	//1.5 运行期次数在预头结合Range和支配关系证明方向、边界与更新均不回绕。
	return Trip.IsRuntime &&
		SE.proveRuntimeNoWrapAtPreheader(Trip, RI, DT);
}
//根据退出块位于Header或Latch选择N或N-1迭代表达式。
const SCEVExpr *LoopExitValueSimplifyPass::getExitIteration(
    const LoopTripCountInfo &Trip) {
	//2.Latch退出发生在最后一次执行迭代N-1；Header退出观察完成N次后的Header值。
	if (!Trip.Header || !Trip.Latch || !Trip.ExitingBlock ||
		Trip.Header == Trip.Latch) {
		return nullptr;
	}
	if (Trip.ExitingBlock == Trip.Latch)
		return Trip.BackedgeTakenCount;
	if (Trip.ExitingBlock == Trip.Header)
		return Trip.TripCount;
	return nullptr;
}
//为单个循环建立全量live-out综合与收益计划。
bool LoopExitValueSimplifyPass::planCandidate(
    Loop *L, SCEVResult &SE, RangeInfo &RI, DominatorTree &DT,
    LoopExitValueCandidate &Candidate) {
	Candidate = LoopExitValueCandidate{};
	//1.要求LoopSimplify、LCSSA、单专用出口和完全无可观察行为，保证综合后可删环。
	if (!L || !L->getHeader() || !LCSSAPass::isLCSSAForm(L) ||
		!hasNoObservableInstruction(L)) {
		return false;
	}
	BasicBlock *PreHeader = nullptr;
	BasicBlock *Latch = nullptr;
	if (!L->getLoopSimplifyForm(PreHeader, Latch) ||
		!PreHeader || !Latch) {
		return false;
	}
	auto Trip = SE.analyzeLoopTripCount(L);
	if (!Trip || !Trip->ExitBlock || !Trip->ExitingBlock ||
		!hasReliableTripCount(SE, *Trip, RI, DT)) {
		return false;
	}
	auto ExitPreds = Trip->ExitBlock->getPredecessors();
	if (ExitPreds.size() != 1 || ExitPreds.front() != Trip->ExitingBlock ||
		Trip->ExitingBlock->countEdgesToSucc(Trip->ExitBlock) != 1) {
		return false;
	}
	//运行期次数只综合完整纯循环中的单一模递推live-out，并保留原循环作为
	//小次数慢路径；其他循环值不能从闭式快路径获得时一律拒绝。
	if (Trip->IsRuntime) {
		auto ExitPhis = Trip->ExitBlock->collectPhiNodes();
		if (ExitPhis.size() != 1) return false;
		PhiInst *Phi = ExitPhis.front();
		if (!Phi || Phi->getNumIncomingValues() != 1 ||
			Phi->getIncomingBlock(0) != Trip->ExitingBlock) {
			return false;
		}
		Value *OnExit = Phi->getIncomingValue(0);
		if (!OnExit || valueMayDependOnUndef(OnExit) ||
			!IRType::hasSameShape(OnExit->getIRType(), Phi->getIRType())) {
			return false;
		}
		return planModuloLiveOut(
			L, Phi, OnExit, *Trip, SE, RI, DT, Candidate);
	}
	if (!Trip->ConstantTripCount) return false;
	const SCEVExpr *ExitIteration = getExitIteration(*Trip);
	if (!ExitIteration) return false;
	//3.逐个验证退出PHI；任一循环live-out无法综合就拒绝整个循环，避免遗留外部use。
	std::vector<LoopExitValuePlan> Plans;
	unsigned TotalCost = 0;
	SCEVExpander Preflight(L, PreHeader, DT, MaxExpansionCost);
	for (PhiInst *Phi : Trip->ExitBlock->collectPhiNodes()) {
		if (!Phi || Phi->getNumIncomingValues() != 1 ||
			Phi->getIncomingBlock(0) != Trip->ExitingBlock) {
			return false;
		}
		Value *OnExit = Phi->getIncomingValue(0);
		auto *Definition = dynamic_cast<Instruction *>(OnExit);
		if (!Definition || !Definition->getParent() ||
			!L->contains(Definition->getParent())) {
			continue;
		}
		if (!IRType::hasIntegerBitWidth(Phi->getIRType(), 32) ||
			!IRType::hasSameShape(OnExit->getIRType(), Phi->getIRType()) ||
			valueMayDependOnUndef(OnExit)) {
			return false;
		}
		const SCEVExpr *AtExit = SE.getSCEVAtIteration(
			SE.getSCEV(OnExit), ExitIteration, L);
		int Cost = Preflight.estimateCost(AtExit);
		//3.1 物化仅接受现有Expander支持的循环不变算术；i32加减乘按原IR回绕语义
		//求值，因此即使中间值越界也与逐次递推保持相同位模式，不利用未定义结果。
		if (!AtExit || AtExit->mentionsLoop(L) || Cost < 0 ||
			Cost > static_cast<int>(MaxExpansionCost) ||
			TotalCost > MaxExpansionCost - static_cast<unsigned>(Cost)) {
			return false;
		}
		LoopExitValuePlan Plan;
		Plan.Phi = Phi;
		Plan.OriginalValue = OnExit;
		Plan.ExitExpression = AtExit;
		Plan.ExpansionCost = Cost;
		Plans.push_back(Plan);
		TotalCost += static_cast<unsigned>(Cost);
	}
	if (Plans.empty()) return false;
	//4.统一收益模型要求预头新增成本不超过删除整环的静态成本，并施加通用上限。
	unsigned RemovableCost = estimateRemovableLoopCost(L);
	if (RemovableCost == 0 || TotalCost >= RemovableCost) return false;
	Candidate.L = L;
	Candidate.PreHeader = PreHeader;
	Candidate.Plans = std::move(Plans);
	Candidate.TotalExpansionCost = TotalCost;
	Candidate.RemovableLoopCost = RemovableCost;
	return true;
}
//识别并综合正常量步长、正常量模数的有界模递推。
bool LoopExitValueSimplifyPass::planModuloLiveOut(
    Loop *L, PhiInst *ExitPhi, Value *OnExit,
    const LoopTripCountInfo &Trip, SCEVResult &SE, RangeInfo &RI,
    DominatorTree &DT, LoopExitValueCandidate &Candidate) {
	//3.2 仅接受 r.next=(r+step)%mod 的直接回边形式；step和mod必须是
	//编译期正常量，次数可以是已经证明有限且不回绕的运行期值。
	if (!L || !ExitPhi || !OnExit || !Trip.PreHeader || !Trip.Latch ||
		!Trip.TripCount ||
		(Trip.ConstantTripCount && *Trip.ConstantTripCount < 2) ||
		(!Trip.ConstantTripCount && !Trip.IsRuntime) ||
		!IRType::hasIntegerBitWidth(ExitPhi->getIRType(), 32)) {
		return false;
	}
	PhiInst *Recurrence = nullptr;
	Instruction *Update = dynamic_cast<Instruction *>(OnExit);
	if (auto *Phi = dynamic_cast<PhiInst *>(OnExit)) {
		Recurrence = Phi;
		Update = dynamic_cast<Instruction *>(
			Phi->getIncomingFromPred(Trip.Latch));
	}
	if (!Update || !Update->getParent() || !L->contains(Update->getParent()))
		return false;
	if (Trip.IsRuntime) {
		//运行期主循环可能已经由通用展开形成“首次srem+若干条件校正”的
		//等价更新链。这里按完整递推语义递归识别，不依赖展开因子或具体常量。
		std::function<bool(Value *, std::int64_t &, unsigned)>
			GetExactConstant = [&](Value *V, std::int64_t &Result,
				unsigned Depth) {
			if (!V || Depth > 4) return false;
			if (auto *CI = dynamic_cast<ConstInt *>(V)) {
				Result = CI->getVal();
				return true;
			}
			auto *BI = dynamic_cast<BinaryInst *>(V);
			if (!BI) return false;
			std::int64_t Left = 0;
			std::int64_t Right = 0;
			if (!GetExactConstant(BI->getOperand(0), Left, Depth + 1) ||
				!GetExactConstant(BI->getOperand(1), Right, Depth + 1)) {
				return false;
			}
			const __int128 WideLeft = Left;
			const __int128 WideRight = Right;
			__int128 WideResult = 0;
			if (BI->getOp() == BinaryInst::ADD)
				WideResult = WideLeft + WideRight;
			else if (BI->getOp() == BinaryInst::SUB)
				WideResult = WideLeft - WideRight;
			else
				return false;
			//只折叠未发生i32回绕的常量链；出现回绕时保守拒绝，避免把
			//编译期辅助表达式误解释成数学整数偏移。
			if (WideResult < IntRange::I32_MIN ||
				WideResult > IntRange::I32_MAX) {
				return false;
			}
			Result = static_cast<std::int64_t>(WideResult);
			return true;
		};
		auto ExtractOffset = [&](Value *V, Value *&Base,
			std::int64_t &Offset) {
			auto *BI = dynamic_cast<BinaryInst *>(V);
			if (!BI) return false;
			Value *A = BI->getOperand(0);
			Value *B = BI->getOperand(1);
			std::int64_t Constant = 0;
			if (BI->getOp() == BinaryInst::ADD &&
				GetExactConstant(B, Constant, 0)) {
				Base = A;
				Offset = Constant;
				return true;
			}
			if (BI->getOp() == BinaryInst::ADD &&
				GetExactConstant(A, Constant, 0)) {
				Base = B;
				Offset = Constant;
				return true;
			}
			if (BI->getOp() == BinaryInst::SUB &&
				GetExactConstant(B, Constant, 0)) {
				Base = A;
				Offset = -Constant;
				return true;
			}
			return false;
		};
		PhiInst *ChainPhi = nullptr;
		std::int64_t ChainStep = 0;
		std::int64_t ChainModulus = 0;
		unsigned UpdatesPerIteration = 0;
		std::unordered_set<Value *> ActiveUpdates;
		std::vector<Instruction *> UpdateNodes;
		std::function<bool(Value *, unsigned)> ParseUpdate =
			[&](Value *V, unsigned Depth) {
			if (!V || Depth >= 8 || !ActiveUpdates.insert(V).second) {
				return false;
			}
			auto *BI = dynamic_cast<BinaryInst *>(V);
			if (BI && BI->getOp() == BinaryInst::MOD) {
				std::int64_t Divisor = 0;
				Value *Base = nullptr;
				std::int64_t Offset = 0;
				if (!GetExactConstant(BI->getOperand(1), Divisor, 0) ||
					!ExtractOffset(BI->getOperand(0), Base, Offset)) {
					return false;
				}
				auto *Phi = dynamic_cast<PhiInst *>(Base);
				if (!Phi || Divisor < 2 || Divisor > IntRange::I32_MAX ||
					Offset <= 0 || Offset > IntRange::I32_MAX ||
					static_cast<__int128>(Divisor - 1) + Offset >
						IntRange::I32_MAX) {
					return false;
				}
				ChainPhi = Phi;
				ChainStep = Offset;
				ChainModulus = Divisor;
				UpdatesPerIteration = 1;
				UpdateNodes.push_back(
					dynamic_cast<Instruction *>(BI->getOperand(0)));
				UpdateNodes.push_back(BI);
				return true;
			}
			auto *Select = dynamic_cast<SelectInst *>(V);
			if (!Select) return false;
			auto *Compare = dynamic_cast<BinaryInst *>(
				Select->getCondition());
			Value *SumValue = Select->getFalseValue();
			Value *CorrectedValue = Select->getTrueValue();
			std::int64_t Divisor = 0;
			Value *Previous = nullptr;
			std::int64_t LocalStep = 0;
			if (!Compare || Compare->getOp() != BinaryInst::GE ||
				Compare->getOperand(0) != SumValue ||
				!GetExactConstant(Compare->getOperand(1), Divisor, 0) ||
				!ExtractOffset(SumValue, Previous, LocalStep) ||
				LocalStep <= 0) {
				return false;
			}
			Value *CorrectedBase = nullptr;
			std::int64_t CorrectedOffset = 0;
			if (!ExtractOffset(
					CorrectedValue, CorrectedBase, CorrectedOffset) ||
				CorrectedBase != Previous ||
				static_cast<__int128>(CorrectedOffset) !=
					static_cast<__int128>(LocalStep) - Divisor) {
				return false;
			}
			if (!ParseUpdate(Previous, Depth + 1) ||
				LocalStep != ChainStep ||
				Divisor != ChainModulus || ChainStep >= ChainModulus) {
				return false;
			}
			++UpdatesPerIteration;
			UpdateNodes.push_back(dynamic_cast<Instruction *>(SumValue));
			UpdateNodes.push_back(Compare);
			UpdateNodes.push_back(
				dynamic_cast<Instruction *>(CorrectedValue));
			UpdateNodes.push_back(Select);
			return true;
		};
		if (!ParseUpdate(Update, 0) || !ChainPhi ||
			ChainPhi->getParent() != L->getHeader() ||
			ChainPhi->getIncomingFromPred(Trip.Latch) != Update ||
			ChainPhi->getNumIncomingValues() != 2 ||
			ChainPhi->countIncomingFromPred(Trip.PreHeader) != 1 ||
			ChainPhi->countIncomingFromPred(Trip.Latch) != 1) {
			return false;
		}
		if ((Trip.ExitingBlock == Trip.Header && OnExit != ChainPhi) ||
			(Trip.ExitingBlock == Trip.Latch && OnExit != Update)) {
			return false;
		}
		Value *Start = ChainPhi->getIncomingFromPred(Trip.PreHeader);
		BrInst *OldBranch = Trip.PreHeader->getBranchTerminator();
		if (!Start || Value::isUndef(Start) ||
			!hasNoObservableInstruction(L) || !OldBranch ||
			OldBranch->isConditional() || OldBranch->getSuccessorCount() != 1 ||
			OldBranch->getSuccessor(0) != Trip.Header ||
			Trip.ExitBlock->collectPhiNodes().size() != 1 ||
			Trip.TripCount->mentionsLoop(L) ||
			!IRType::hasIntegerBitWidth(Trip.TripCount->Ty, 32)) {
			return false;
		}
		for (Instruction *Node : UpdateNodes) {
			if (!Node || !Node->getParent() ||
				!L->contains(Node->getParent()) ||
				!DT.dominates(Node->getParent(), Trip.Latch)) {
				return false;
			}
		}
		//闭式只在总次数具有有限非负上界时生成。该范围来自通用SCEV与
		//Range事实，不读取输入值；它同时保证后续全部闭式算术留在i32内，
		//避免把后端尚未稳定支持的宽整数余数引入目标程序。
		auto TripRange = SE.evaluateRangeAt(
			Trip.TripCount, Trip.PreHeader, RI, DT);
		if (!TripRange || TripRange->isBottom() || TripRange->isTop() ||
			TripRange->min < 0) {
			return false;
		}
		const __int128 MaxUpdateCount =
			static_cast<__int128>(TripRange->max) *
			UpdatesPerIteration;
		if (MaxUpdateCount <= 0) return false;
		const __int128 MaxClosedMagnitude =
			(MaxUpdateCount - 1) * ChainStep + ChainModulus - 1;
		if (MaxClosedMagnitude > IntRange::I32_MAX)
			return false;
		SCEVExpander Preflight(
			L, Trip.PreHeader, DT, MaxExpansionCost);
		int TripExpansionCost =
			Preflight.estimateCost(Trip.TripCount);
		Value *DirectTripCount = nullptr;
		bool DirectTripNeedsClamp = false;
		//SCEV若因边界内部含移位等节点而无法重新物化，则退回到规范
		//“iv从0开始、每轮加一、iv<bound”的现有边界SSA值。
		if (TripExpansionCost < 0 && Trip.IndVar &&
			Trip.ComparePhase == LoopExitComparePhase::HeaderPhi &&
			Trip.ExitingBlock == Trip.Header) {
			Value *IVStart = Trip.IndVar->getIncomingFromPred(Trip.PreHeader);
			auto *StartConstant = dynamic_cast<ConstInt *>(IVStart);
			BinaryInst::Operation Predicate = Trip.ContinueOnTrue
				? Trip.ExitCmp->getOp()
				: BinaryInst::negatePredicate(Trip.ExitCmp->getOp());
			Value *LHS = Trip.ExitCmp->getOperand(0);
			Value *RHS = Trip.ExitCmp->getOperand(1);
			if (RHS == Trip.IndVar) {
				std::swap(LHS, RHS);
				Predicate = BinaryInst::reversePredicate(Predicate);
			}
			bool BoundAvailable = RHS &&
				IRType::hasIntegerBitWidth(RHS->getIRType(), 32);
			if (auto *BoundI = dynamic_cast<Instruction *>(RHS)) {
				if (!BoundI->getParent()) {
					BoundAvailable = false;
				} else if (BoundI->getParent() == Trip.PreHeader) {
					BoundAvailable = Instruction::comesBefore(
						BoundI, Trip.PreHeader->getTerminator());
				} else {
					BoundAvailable = DT.dominates(
						BoundI->getParent(), Trip.PreHeader);
				}
			}
			if (StartConstant && StartConstant->getVal() == 0 &&
				LHS == Trip.IndVar && Predicate == BinaryInst::L &&
				BoundAvailable) {
				DirectTripCount = RHS;
				IntRange BoundRange = RI.query(RHS, Trip.PreHeader, DT);
				DirectTripNeedsClamp = BoundRange.isBottom() ||
					BoundRange.min < 0;
				TripExpansionCost = DirectTripNeedsClamp ? 2 : 0;
			}
		}
		constexpr unsigned ClosedFormInstructionCount = 12;
		if (TripExpansionCost < 0 ||
			static_cast<unsigned>(TripExpansionCost) >
				MaxExpansionCost - ClosedFormInstructionCount) {
			return false;
		}
		const unsigned LoopCost = estimateRemovableLoopCost(L);
		//闭式路径按通用运算类别保守计入两次余数、两次乘法、入口门禁
		//和控制流成本，不以任何具体模数或用例分布调低阈值。
		const unsigned ClosedFormCost =
			28u + static_cast<unsigned>(TripExpansionCost);
		const unsigned PerIterationSaving = std::max(1u, LoopCost);
		const std::int64_t FastTripThreshold = std::max<std::int64_t>(
			2, static_cast<std::int64_t>(
				(ClosedFormCost + PerIterationSaving - 1) /
				PerIterationSaving) + 1);
		//若静态上界连收益阈值都达不到，运行时门禁必为假；此时不插入
		//永远不可达的闭式路径，避免仅增加比较和控制流而没有动态收益。
		if (TripRange->max < FastTripThreshold)
			return false;
		LoopExitValuePlan Plan;
		Plan.Phi = ExitPhi;
		Plan.OriginalValue = OnExit;
		Plan.ExpansionCost = TripExpansionCost +
			static_cast<int>(ClosedFormInstructionCount);
		Plan.ModuloBase = Start;
		Plan.ModuloDivisor = ChainModulus;
		Plan.ModuloTripCount = DirectTripCount ? nullptr : Trip.TripCount;
		Plan.ModuloTripCountValue = DirectTripCount;
		Plan.ModuloTripCountNeedsClamp = DirectTripNeedsClamp;
		Plan.ModuloStep = ChainStep;
		Plan.ModuloUpdatesPerIteration = UpdatesPerIteration;
		Plan.ModuloFastTripThreshold = FastTripThreshold;
		Candidate.L = L;
		Candidate.PreHeader = Trip.PreHeader;
		Candidate.Plans.push_back(Plan);
		Candidate.TotalExpansionCost =
			static_cast<unsigned>(Plan.ExpansionCost);
		Candidate.RemovableLoopCost = LoopCost;
		return LoopCost != 0;
	}
	BinaryInst *Sum = nullptr;
	BinaryInst *Compare = nullptr;
	Instruction *Corrected = nullptr;
	SelectInst *CorrectionSelect = nullptr;
	std::int64_t Modulus = 0;
	if (auto *Remainder = dynamic_cast<BinaryInst *>(Update);
		Remainder && Remainder->getOp() == BinaryInst::MOD) {
		Sum = dynamic_cast<BinaryInst *>(Remainder->getOperand(0));
		auto *Divisor = dynamic_cast<ConstInt *>(Remainder->getOperand(1));
		if (!Divisor) return false;
		Modulus = Divisor->getVal();
	} else if (auto *Select = dynamic_cast<SelectInst *>(Update)) {
		CorrectionSelect = Select;
		Compare = dynamic_cast<BinaryInst *>(Select->getCondition());
		Sum = dynamic_cast<BinaryInst *>(Select->getFalseValue());
		Corrected = dynamic_cast<Instruction *>(Select->getTrueValue());
		auto *Divisor = Compare && Compare->getOp() == BinaryInst::GE &&
			Compare->getOperand(0) == Sum
			? dynamic_cast<ConstInt *>(Compare->getOperand(1)) : nullptr;
		if (!Divisor || !Corrected) return false;
		Modulus = Divisor->getVal();
	} else {
		return false;
	}
	if (!Sum || Sum->getOp() != BinaryInst::ADD || !Sum->getParent() ||
		!L->contains(Sum->getParent())) {
		return false;
	}
	//把phi与常量构成的短加减链规范为phi+offset，用同一语义同时验证原始srem
	//和范围化简后形成的单次条件校正。
	std::function<bool(Value *, PhiInst *&, std::int64_t &, unsigned)>
		DecomposeAffine = [&](Value *V, PhiInst *&Phi,
			std::int64_t &Offset, unsigned Depth) {
			if (!V || Depth > 3) return false;
			if (auto *P = dynamic_cast<PhiInst *>(V)) {
				Phi = P;
				Offset = 0;
				return true;
			}
			auto *BI = dynamic_cast<BinaryInst *>(V);
			if (!BI) return false;
			Value *A = BI->getOperand(0);
			Value *B = BI->getOperand(1);
			auto *AC = dynamic_cast<ConstInt *>(A);
			auto *BC = dynamic_cast<ConstInt *>(B);
			Value *Nested = nullptr;
			std::int64_t Delta = 0;
			if (BI->getOp() == BinaryInst::ADD && BC) {
				Nested = A;
				Delta = BC->getVal();
			} else if (BI->getOp() == BinaryInst::ADD && AC) {
				Nested = B;
				Delta = AC->getVal();
			} else if (BI->getOp() == BinaryInst::SUB && BC) {
				Nested = A;
				Delta = -BC->getVal();
			} else {
				return false;
			}
			std::int64_t BaseOffset = 0;
			if (!DecomposeAffine(Nested, Phi, BaseOffset, Depth + 1))
				return false;
			const __int128 Combined = static_cast<__int128>(BaseOffset) + Delta;
			if (Combined < std::numeric_limits<std::int64_t>::min() ||
				Combined > std::numeric_limits<std::int64_t>::max()) {
				return false;
			}
			Offset = static_cast<std::int64_t>(Combined);
			return true;
		};
	PhiInst *SumPhi = nullptr;
	std::int64_t Step = 0;
	if (!DecomposeAffine(Sum, SumPhi, Step, 0)) return false;
	if (Recurrence && Recurrence != SumPhi) return false;
	Recurrence = SumPhi;
	if (CorrectionSelect) {
		//单次条件校正只有在每轮增量小于模数时才与取模递推等价；否则一次减法
		//可能仍留下不小于模数的值，不能套用闭式模运算。
		if (Step >= Modulus) return false;
		PhiInst *CorrectedPhi = nullptr;
		std::int64_t CorrectedOffset = 0;
		if (!DecomposeAffine(Corrected, CorrectedPhi, CorrectedOffset, 0) ||
			CorrectedPhi != Recurrence ||
			static_cast<__int128>(CorrectedOffset) !=
				static_cast<__int128>(Step) - Modulus) {
			return false;
		}
	}
	if (!Recurrence || Recurrence->getParent() != L->getHeader() ||
		Recurrence->getIncomingFromPred(Trip.Latch) != Update ||
		!IRType::hasIntegerBitWidth(Recurrence->getIRType(), 32) ||
		Update->getIRType() != Recurrence->getIRType() ||
		Sum->getIRType() != Recurrence->getIRType()) {
		return false;
	}
	//Header退出观察更新后的PHI，Latch退出观察本轮更新；其他相位不能套用N次公式。
	if ((Trip.ExitingBlock == Trip.Header && OnExit != Recurrence) ||
		(Trip.ExitingBlock == Trip.Latch && OnExit != Update)) {
		return false;
	}
	Value *Start = Recurrence->getIncomingFromPred(Trip.PreHeader);
	if (Recurrence->getNumIncomingValues() != 2 ||
		Recurrence->countIncomingFromPred(Trip.PreHeader) != 1 ||
		Recurrence->countIncomingFromPred(Trip.Latch) != 1 ||
		!Start || Value::isUndef(Start) || Modulus < 2 ||
		Modulus > IntRange::I32_MAX || Step < 0 ||
		Step > IntRange::I32_MAX ||
		static_cast<__int128>(Modulus - 1) + Step > IntRange::I32_MAX) {
		return false;
	}
	IntRange StartRange = RI.query(Start, Trip.PreHeader, DT);
	const bool HasCanonicalStartRange =
		!StartRange.isBottom() && !StartRange.isTop() &&
		StartRange.min >= 0 && StartRange.max < Modulus;
	//单次条件校正只在标准余数区间内等价；直接srem允许任意i32初值，
	//运行期闭式会单独保留第一轮i32回绕和有符号余数语义。
	if (CorrectionSelect && !HasCanonicalStartRange) return false;
	//递推必须只服务于自身回边和当前live-out；若还有循环内用户，闭式替换不会
	//删除逐次srem，因而不满足严格正收益。
	auto UsersAre = [](Instruction *I,
		const std::vector<Instruction *> &Allowed) {
		if (!I) return false;
		for (Instruction *UserI : I->getInstructionUsers()) {
			if (std::find(Allowed.begin(), Allowed.end(), UserI) == Allowed.end())
				return false;
		}
		return true;
	};
	std::vector<Instruction *> RecurrenceUsers{Sum, ExitPhi};
	std::vector<Instruction *> SumUsers{Update};
	unsigned RemovedUpdateCost = estimateInstructionCost(Sum) +
		estimateInstructionCost(Update);
	if (CorrectionSelect) {
		RecurrenceUsers.push_back(Corrected);
		SumUsers = {Compare, CorrectionSelect, Corrected};
		RemovedUpdateCost += estimateInstructionCost(Compare) +
			estimateInstructionCost(Corrected);
		if (!UsersAre(Compare, {CorrectionSelect}) ||
			!UsersAre(Corrected, {CorrectionSelect})) {
			return false;
		}
	}
	if (!UsersAre(Sum, SumUsers) ||
		!UsersAre(Update, {Recurrence, ExitPhi}) ||
		!UsersAre(Recurrence, RecurrenceUsers) ||
		!DT.dominates(Sum->getParent(), Trip.Latch) ||
		!DT.dominates(Update->getParent(), Trip.Latch)) {
		return false;
	}
	if (!HasCanonicalStartRange) return false;
	const std::int64_t Iterations = *Trip.ConstantTripCount;
	const std::int64_t Contribution = static_cast<std::int64_t>(
		(static_cast<__int128>(Iterations % Modulus) *
		 (Step % Modulus)) % Modulus);
	const __int128 SmallestSum =
		static_cast<__int128>(StartRange.min) + Contribution;
	const __int128 LargestSum =
		static_cast<__int128>(StartRange.max) + Contribution;
	if (LargestSum > IntRange::I32_MAX) return false;
	std::int64_t Addend = Contribution;
	std::int64_t FinalDivisor = 0;
	if (SmallestSum >= Modulus) {
		Addend -= Modulus;
	} else if (LargestSum >= Modulus) {
		FinalDivisor = Modulus;
	}
	const int ExpansionCost = (Addend != 0 ? 1 : 0) +
		(FinalDivisor != 0 ? 1 : 0);
	const std::uint64_t SavedCost = static_cast<std::uint64_t>(Iterations - 1) *
		RemovedUpdateCost;
	if (SavedCost <= static_cast<unsigned>(ExpansionCost)) return false;
	LoopExitValuePlan Plan;
	Plan.Phi = ExitPhi;
	Plan.OriginalValue = OnExit;
	Plan.ExpansionCost = ExpansionCost;
	Plan.ModuloBase = Start;
	Plan.ModuloAddend = Addend;
	Plan.ModuloDivisor = FinalDivisor;
	Candidate.L = L;
	Candidate.PreHeader = Trip.PreHeader;
	Candidate.Plans.push_back(Plan);
	Candidate.TotalExpansionCost = static_cast<unsigned>(ExpansionCost);
	Candidate.RemovableLoopCost = RemovedUpdateCost;
	return true;
}
//为含副作用循环中独立的纯递推live-out建立保留循环的综合计划。
bool LoopExitValueSimplifyPass::planPreservedLoopCandidate(
    Loop *L, SCEVResult &SE, RangeInfo &RI, DominatorTree &DT,
    FunctionAnalysisPassManager &FAM,
    LoopExitValueCandidate &Candidate) {
	Candidate = LoopExitValueCandidate{};
	//1.只处理规范叶子循环和单一专用出口；循环本身的store/call等副作用继续保留。
	if (!L || !L->getHeader() || !L->getSubLoops().empty() ||
		!LCSSAPass::isLCSSAForm(L)) {
		return false;
	}
	BasicBlock *PreHeader = nullptr;
	BasicBlock *Latch = nullptr;
	if (!L->getLoopSimplifyForm(PreHeader, Latch) || !PreHeader || !Latch)
		return false;
	auto Trip = SE.analyzeLoopTripCount(L);
	if (!Trip || !Trip->ExitBlock || !Trip->ExitingBlock ||
		!hasReliableTripCount(SE, *Trip, RI, DT) ||
		!Trip->ConstantTripCount || *Trip->ConstantTripCount < 2) {
		return false;
	}
	auto ExitPreds = Trip->ExitBlock->getPredecessors();
	if (ExitPreds.size() != 1 || ExitPreds.front() != Trip->ExitingBlock ||
		Trip->ExitingBlock->countEdgesToSucc(Trip->ExitBlock) != 1) {
		return false;
	}
	//2.不跨越计时或其他顺序屏障提前物化表达式，保持运行库可观察时序。
	for (BasicBlock *BB : L->getBlocksInFunctionOrder()) {
		if (!BB) return false;
		for (Instruction *I : *BB) {
			auto *CI = dynamic_cast<CallInst *>(I);
			if (CI && getCallEffectSummary(CI, FAM).HasOrderingBarrier)
				return false;
		}
	}
	const SCEVExpr *ExitIteration = getExitIteration(*Trip);
	if (!ExitIteration) return false;
	//3.逐个尝试独立live-out；一次只提交一个，修改后由PassManager重建SCEV与支配树。
	for (PhiInst *Phi : Trip->ExitBlock->collectPhiNodes()) {
		if (!Phi || Phi->getNumIncomingValues() != 1 ||
			Phi->getIncomingBlock(0) != Trip->ExitingBlock ||
			!IRType::hasIntegerBitWidth(Phi->getIRType(), 32)) {
			continue;
		}
		Value *OnExit = Phi->getIncomingValue(0);
		auto *Definition = dynamic_cast<Instruction *>(OnExit);
		if (!Definition || !Definition->getParent() ||
			!L->contains(Definition->getParent()) ||
			!IRType::hasSameShape(OnExit->getIRType(), Phi->getIRType()) ||
			valueMayDependOnUndef(OnExit)) {
			continue;
		}
		if (planModuloLiveOut(L, Phi, OnExit, *Trip, SE, RI, DT, Candidate))
			return true;
		const SCEVExpr *AtExit = SE.getSCEVAtIteration(
			SE.getSCEV(OnExit), ExitIteration, L);
		SCEVExpander Preflight(L, PreHeader, DT, MaxExpansionCost);
		int ExpansionCost = Preflight.estimateCost(AtExit);
		if (!AtExit || AtExit->mentionsLoop(L) || ExpansionCost < 0 ||
			ExpansionCost > static_cast<int>(MaxExpansionCost)) {
			continue;
		}
		//3.1 从出口值反向收集纯计算闭包，再删除具有闭包外用户的节点，得到DCE
		//真正能够清除的最大自包含递推子图。
		std::unordered_set<Instruction *> Removable;
		std::vector<Instruction *> WorkList{Definition};
		while (!WorkList.empty()) {
			Instruction *Current = WorkList.back();
			WorkList.pop_back();
			if (!Current || !Current->getParent() ||
				!L->contains(Current->getParent()) ||
				dynamic_cast<BrInst *>(Current) ||
				!isSafePureInstruction(Current) ||
				!Removable.insert(Current).second) {
				continue;
			}
			for (std::size_t Index = 0; Index < Current->getNumOperands(); ++Index) {
				auto *OperandI = dynamic_cast<Instruction *>(Current->getOperand(Index));
				if (OperandI && OperandI->getParent() &&
					L->contains(OperandI->getParent())) {
					WorkList.push_back(OperandI);
				}
			}
		}
		bool Pruned = true;
		while (Pruned) {
			Pruned = false;
			for (auto It = Removable.begin(); It != Removable.end();) {
				Instruction *Current = *It;
				bool HasOutsideUser = false;
				for (Instruction *UserI : Current->getInstructionUsers()) {
					if (UserI == Phi || Removable.count(UserI)) continue;
					HasOutsideUser = true;
					break;
				}
				if (!HasOutsideUser) {
					++It;
					continue;
				}
				It = Removable.erase(It);
				Pruned = true;
			}
		}
		if (!Removable.count(Definition)) continue;
		unsigned RemovableCost = 0;
		bool MustExecute = true;
		for (Instruction *I : Removable) {
			if (!dynamic_cast<PhiInst *>(I) &&
				!DT.dominates(I->getParent(), Latch)) {
				MustExecute = false;
				break;
			}
			RemovableCost += estimateInstructionCost(I);
		}
		if (!MustExecute || RemovableCost == 0) continue;
		std::uint64_t Executions = static_cast<std::uint64_t>(
			*Trip->ConstantTripCount - 1);
		std::uint64_t SavedCost = Executions * RemovableCost;
		if (SavedCost <= static_cast<unsigned>(ExpansionCost)) continue;
		LoopExitValuePlan Plan;
		Plan.Phi = Phi;
		Plan.OriginalValue = OnExit;
		Plan.ExitExpression = AtExit;
		Plan.ExpansionCost = ExpansionCost;
		Candidate.L = L;
		Candidate.PreHeader = PreHeader;
		Candidate.Plans.push_back(Plan);
		Candidate.TotalExpansionCost = static_cast<unsigned>(ExpansionCost);
		Candidate.RemovableLoopCost = RemovableCost;
		return true;
	}
	return false;
}
//在预头物化表达式并替换已规划的退出PHI。
bool LoopExitValueSimplifyPass::applyCandidate(
    LoopExitValueCandidate &Candidate, DominatorTree &DT) {
	//3—4.全部表达式已经预检；先统一物化，再直接提交PHI替换，不建立额外回滚框架。
	if (!Candidate.L || !Candidate.PreHeader || Candidate.Plans.empty())
		return false;
	//运行期模递推建立“闭式快路径/原循环慢路径”版本。小次数、零次和一次
	//仍执行原循环，既精确保留退出相位，也避免闭式固定成本造成负优化。
	if (Candidate.Plans.size() == 1 &&
		(Candidate.Plans.front().ModuloTripCount ||
		 Candidate.Plans.front().ModuloTripCountValue)) {
		LoopExitValuePlan &Plan = Candidate.Plans.front();
		BasicBlock *Header = Candidate.L->getHeader();
		BasicBlock *Exit = Plan.Phi ? Plan.Phi->getParent() : nullptr;
		Function *F = Candidate.PreHeader->getParent();
		BrInst *OldBranch = Candidate.PreHeader->getBranchTerminator();
		if (!Header || !Exit || !F || !Plan.ModuloBase ||
			Plan.ModuloDivisor < 2 || Plan.ModuloStep <= 0 ||
			Plan.ModuloUpdatesPerIteration <= 0 ||
			Plan.ModuloFastTripThreshold < 2 || !OldBranch ||
			OldBranch->isConditional() || OldBranch->getSuccessorCount() != 1 ||
			OldBranch->getSuccessor(0) != Header) {
			return false;
		}
		SCEVExpander Expander(
			Candidate.L, Candidate.PreHeader, DT, MaxExpansionCost);
		Value *TripCount = Plan.ModuloTripCountValue
			? Plan.ModuloTripCountValue
			: Expander.expand(Plan.ModuloTripCount);
		if (TripCount && Plan.ModuloTripCountNeedsClamp) {
			Value *Zero = Value::makeIntegerLike(TripCount, 0);
			auto *Positive = new BinaryInst(
				TripCount, BinaryInst::G, Zero);
			if (!Candidate.PreHeader->insertBeforeTerminator(Positive)) {
				delete Positive;
				return false;
			}
			auto *Clamped = new SelectInst(Positive, TripCount, Zero);
			if (!Candidate.PreHeader->insertBeforeTerminator(Clamped)) {
				delete Clamped;
				return false;
			}
			TripCount = Clamped;
		}
		if (!TripCount ||
			!IRType::hasIntegerBitWidth(TripCount->getIRType(), 32)) {
			return false;
		}
		Value *Threshold = Value::makeIntegerLike(
			TripCount, Plan.ModuloFastTripThreshold);
		auto *FastEnough = new BinaryInst(
			TripCount, BinaryInst::GE, Threshold);
		if (!Candidate.PreHeader->insertBeforeTerminator(FastEnough)) {
			delete FastEnough;
			return false;
		}
		//第一轮加法保留原i32回绕和有符号余数语义；计划阶段已经用
		//SCEV/Range证明剩余更新次数、贡献和闭式和都落在i32范围内，
		//因此后续运算无需宽化，也不会依赖目标后端的宽整数余数实现。
		Value *Step32 = Value::makeIntegerLike(
			Plan.ModuloBase, Plan.ModuloStep);
		Value *Modulus32 = Value::makeIntegerLike(
			Plan.ModuloBase, Plan.ModuloDivisor);
		Value *One32 = Value::makeIntegerLike(TripCount, 1);
		Value *Updates32 = Value::makeIntegerLike(
			TripCount, Plan.ModuloUpdatesPerIteration);
		if (!Step32 || !Modulus32 || !One32 || !Updates32) {
			delete FastEnough;
			return false;
		}
		auto *FirstSum = new BinaryInst(
			Plan.ModuloBase, BinaryInst::ADD, Step32);
		auto *FirstRemainder = new BinaryInst(
			FirstSum, BinaryInst::MOD, Modulus32);
		auto *CoveredUpdates = new BinaryInst(
			TripCount, BinaryInst::MUL, Updates32);
		auto *Remaining = new BinaryInst(
			CoveredUpdates, BinaryInst::SUB, One32);
		auto *Contribution = new BinaryInst(
			Remaining, BinaryInst::MUL, Step32);
		auto *ClosedSum = new BinaryInst(
			FirstRemainder, BinaryInst::ADD, Contribution);
		auto *ClosedRemainder = new BinaryInst(
			ClosedSum, BinaryInst::MOD, Modulus32);
		auto *FastBlock = new BasicBlock();
		FastBlock->setIdent(F->makeUniqueBlockIdent(
			Candidate.PreHeader->getIdent() + ".mod.closed"));
		F->pushBlock(FastBlock);
		for (Instruction *I : {
				static_cast<Instruction *>(FirstSum),
				static_cast<Instruction *>(FirstRemainder),
				static_cast<Instruction *>(CoveredUpdates),
				static_cast<Instruction *>(Remaining),
				static_cast<Instruction *>(Contribution),
				static_cast<Instruction *>(ClosedSum),
				static_cast<Instruction *>(ClosedRemainder)}) {
			FastBlock->pushBack(I);
		}
		FastBlock->genUnCondInst(Exit);
		Plan.Phi->addIncoming(ClosedRemainder, FastBlock);
		delete OldBranch;
		Candidate.PreHeader->genCondInst(
			FastEnough, FastBlock, Header);
		return true;
	}
	SCEVExpander Expander(
		Candidate.L, Candidate.PreHeader, DT, MaxExpansionCost);
	std::vector<Value *> Replacements;
	Replacements.reserve(Candidate.Plans.size());
	for (const LoopExitValuePlan &Plan : Candidate.Plans) {
		Value *Replacement = nullptr;
		if (Plan.ModuloBase) {
			Replacement = Plan.ModuloBase;
			if (Plan.ModuloAddend != 0) {
				Value *Addend = Value::makeIntegerLike(
					Plan.ModuloBase, Plan.ModuloAddend);
				auto *Add = new BinaryInst(
					Replacement, BinaryInst::ADD, Addend);
				Candidate.PreHeader->insertBeforeTerminator(Add);
				Replacement = Add;
			}
			if (Plan.ModuloDivisor != 0) {
				Value *Divisor = Value::makeIntegerLike(
					Plan.ModuloBase, Plan.ModuloDivisor);
				auto *Mod = new BinaryInst(
					Replacement, BinaryInst::MOD, Divisor);
				Candidate.PreHeader->insertBeforeTerminator(Mod);
				Replacement = Mod;
			}
		} else {
			Replacement = Expander.expand(Plan.ExitExpression);
		}
		if (!Replacement || !IRType::hasSameShape(
				Replacement->getIRType(), Plan.Phi->getIRType())) {
			return false;
		}
		Replacements.push_back(Replacement);
	}
	for (std::size_t Index = 0; Index < Candidate.Plans.size(); ++Index) {
		PhiInst *Phi = Candidate.Plans[Index].Phi;
		Phi->replaceAllUsesWith(Replacements[Index]);
		delete Phi;
	}
	return true;
}
//把可证明的无副作用循环出口递推值提前综合到循环预头。
PreservationStatus LoopExitValueSimplifyPass::run(
    Function &F, FunctionAnalysisPassManager &FAM) {
	//1—4.按内到外顺序一次提交一个循环，改写后让PassManager重建全部值流分析。
	auto &LI = FAM.getResult<LoopAnalysis>(F);
	auto &SE = FAM.getResult<SCEVAnalysis>(F);
	auto &RI = FAM.getResult<RangeAnalysis>(F);
	auto &DT = FAM.getResult<DominantAnalysis>(F);
	for (Loop *L : LI.getLoopsInPostOrder(true)) {
		LoopExitValueCandidate Candidate;
		if (!planCandidate(L, SE, RI, DT, Candidate) &&
			!planPreservedLoopCandidate(L, SE, RI, DT, FAM, Candidate)) {
			continue;
		}
		if (!applyCandidate(Candidate, DT)) continue;
		return PreservationStatus::none();
	}
	return PreservationStatus::all();
}
