#include "ir/opt/FuncInline.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "lib/IRUtils.hpp"
#include <algorithm>
#include <unordered_set>
#include <vector>
FuncInlinePass::FuncInlinePass(bool enableRecursiveInline, bool lateCleanupMode)
	: EnableRecursiveInline(enableRecursiveInline),
	  LateCleanupMode(lateCleanupMode) {}
//步骤2.2：计算Callee的收益模型
CalleeInlineProfile FuncInlinePass::analyzeCallee(
	Function* Callee, FunctionAnalysisPassManager& FAM) {
	CalleeInlineProfile P;
	if (!Callee) return P;
    //Callee必须有函数体
	P.HasBody = (Callee->front() != nullptr);
	if (!P.HasBody) return P;
    //遍历所有指令，记录总指令数，自递归调用点数，是否调用其他函数
	P.ParameterUseCounts.resize(Callee->getParamCount(), 0);
	for (unsigned Index = 0; Index < Callee->getParamCount(); ++Index) {
		if (Value *Param = Callee->getParam(Index))
			P.ParameterUseCounts[Index] = Param->countInstructionUses();
	}
	for (BasicBlock *BB : *Callee) {
		if (BB) ++P.BasicBlockCount;
	}
	forEachInstruction(*Callee, [&](Instruction *I) {
		++P.TotalInstCount;
		if (dynamic_cast<RetInst *>(I)) ++P.ReturnInstCount;
		auto *CI = dynamic_cast<CallInst *>(I);
		if (!CI) return;
		Function *CF = CI->getDirectCallee();
		if (!CF) {
			P.HasOtherCalls = true;
			return;
		}
		if (CF == Callee) {
			++P.SelfRecursiveCallSites;
		} else {
			P.HasOtherCalls = true;
		}
	});
	const SideEffectInfo& SEI = FAM.getResult<SideEffectAnalysis>(*Callee);
	//无NoModRef，并且没有Builtin函数，这个条件是用来判断记忆化的
	P.SideEffectFreeLocalOnly = (!SEI.hasBuiltin && getFunctionModRefInfo(SEI) == ModRefInfo::NoModRef);
	/*
    没有其他函数调用，没有副作用，并且自递归调用点数大于等于2时，标记为可记忆化，不内联
    形如fib(n) = fib(n - 1) + fib(n - 2)
    */
	P.SafeToMemoize = (!P.HasOtherCalls && P.SideEffectFreeLocalOnly && P.SelfRecursiveCallSites >= 2);
	return P;
}
//步骤2.1：判断Callee是否符合收益模型
bool FuncInlinePass::isInlineSite(
	Function& Caller,
	CallInst* CI,
	Function* Callee,
	const std::unordered_map<Function*, int>& FreqCallee,
	bool EnableRecursiveInline,
	bool LateCleanupMode,
	bool CalleeIsRecursive,
	FunctionAnalysisPassManager& FAM,
	CalleeInlineProfile& OutProfile)
{
	if (!CI || !Callee) return false;
    //普通内联函数调用点及指令支持
	constexpr int kCallSitesThreshold = 3;
	constexpr int kInstThreshold = 200;
    //递归函数内联最大指令支持
	constexpr int kRecursiveExpandMaxInst = 100;
	//晚期模型只承担低增长机会，避免重新复制已经优化过的大函数体
	constexpr int kLateSingleUseMaxInst = 96;
	constexpr int kLateMultiUseMaxInst = 16;
	constexpr int kLateModuleGrowthBudget = 48;
	constexpr int kLateMaxDirectCallSites = 8;
	OutProfile = analyzeCallee(Callee, FAM);
	if (!OutProfile.HasBody) return false;
	if (OutProfile.SafeToMemoize) return false;
	auto it = FreqCallee.find(Callee);
    //调用Callee频次
	int Freq = (it == FreqCallee.end()) ? 1 : std::max(1, it->second);
    //指令数
	int InstCount = std::max(0, OutProfile.TotalInstCount - 1);
	const bool IsRecursiveFunc = CalleeIsRecursive ||
		(OutProfile.SelfRecursiveCallSites > 0);
	const bool IsSelfCall = (Callee == &Caller);
	if (LateCleanupMode) {
		//晚期固定点不展开任何递归环，保证重复运行的代码规模单调受控。
		if (IsRecursiveFunc || Callee == &Caller) return false;
		if (!Callee->hasOnlyDirectCallUses()) return false;
		int DirectCallSites = static_cast<int>(Callee->getDirectCallSites().size());
		if (DirectCallSites <= 0 || DirectCallSites > kLateMaxDirectCallSites)
			return false;
		int BodyInstCount = std::max(
			0, OutProfile.TotalInstCount - OutProfile.ReturnInstCount);
		//单调用点内联后旧函数体会被删除，模块代码量不会增长。
		if (DirectCallSites == 1)
			return BodyInstCount <= kLateSingleUseMaxInst;
		//多调用点只接受很小且全模块增长受限的函数体。
		if (BodyInstCount > kLateMultiUseMaxInst) return false;
		int ModuleGrowth = std::max(0, BodyInstCount - 1) * DirectCallSites;
		return ModuleGrowth <= kLateModuleGrowthBudget;
	}
	if (IsRecursiveFunc) {
		//递归函数只允许在自身调用点按更严格的总展开预算展开。
		if (!IsSelfCall) return false;
		if (!EnableRecursiveInline) return false;
		if (InstCount * Freq > kRecursiveExpandMaxInst) return false;
	} else {
		//普通函数在调用点较少时直接放行，否则限制“体积 × 调用次数”。
		if (Freq >= kCallSitesThreshold && InstCount * Freq > kInstThreshold) return false;
	}
	return true;
}
PreservationStatus FuncInlinePass::run(Function &F, FunctionAnalysisPassManager &FAM) {
	bool Changed = false;
	//1.收集当前函数的Call指令，并记录Callee的调用频次
	std::unordered_map<Function*, int> FreqCallee;
	std::unordered_map<Function*, bool> RecursiveCallees;
	std::unordered_map<CallInst*, int> SitePriority;
	std::unordered_map<CallInst*, int> SiteOrder;
	FunctionCallGraph CallGraph;
	if (Module *M = Module::getActiveModule()) CallGraph = M->buildDirectCallGraph();
	LoopInfo *LI = LateCleanupMode ? &FAM.getResult<LoopAnalysis>(F) : nullptr;
	std::vector<CallInst*> OriginalCallSites;
	for (CallInst *CI : collectInstructions<CallInst>(F)) {
		auto* Callee = CI->getDirectCallee();
		if (!Callee) continue;
		FreqCallee[Callee] = std::max(
			1, static_cast<int>(Callee->getDirectCallSites().size()));
		if (!RecursiveCallees.count(Callee)) {
			RecursiveCallees[Callee] = !CallGraph.empty() &&
				Module::isFunctionRecursive(Callee, CallGraph);
		}
		if (LateCleanupMode) {
			Loop *CallLoop = LI ? LI->getLoopFor(CI->getParent()) : nullptr;
			int Priority = CallLoop ? static_cast<int>(CallLoop->getDepth()) * 16 : 0;
			CalleeInlineProfile Profile = analyzeCallee(Callee, FAM);
			for (unsigned Index = 0; Index < CI->getArgCount() &&
				 Index < Profile.ParameterUseCounts.size(); ++Index) {
				if (!dynamic_cast<ConstantSystem *>(CI->getArg(Index))) continue;
				Priority += std::min(Profile.ParameterUseCounts[Index], 8);
			}
			SitePriority[CI] = Priority;
		}
		SiteOrder[CI] = static_cast<int>(OriginalCallSites.size());
		OriginalCallSites.push_back(CI);
	}
	if (LateCleanupMode) {
		//先按循环深度和可常量化使用数排序，修改CFG后不再读取旧LoopInfo。
		std::sort(OriginalCallSites.begin(), OriginalCallSites.end(),
			[&SitePriority, &SiteOrder](CallInst *LHS, CallInst *RHS) {
				if (SitePriority[LHS] != SitePriority[RHS])
					return SitePriority[LHS] > SitePriority[RHS];
				return SiteOrder[LHS] < SiteOrder[RHS];
			});
	}
	int LateInlinedSites = 0;
	int LateClonedInsts = 0;
	int LateCodeGrowth = 0;
    //2.遍历刚才收集的每个Call指令
	for (CallInst* CI : OriginalCallSites) {
		if (!CI) continue;
        //获得Call指令所在的基本块CallBB
		BasicBlock* CallBB = CI->getParent();
		if (!CallBB) continue;
        //获得被调函数Callee
		auto* Callee = CI->getDirectCallee();
		if (!Callee) continue;
		CalleeInlineProfile Profile;
//2.1调用收益模型
/*
有函数体
递归函数：调用函数频次×单次函数指令数<=100
非递归函数：调用函数频次<3 || 调用函数频次(>=3)×单次函数指令数<=200
*/
		if (!isInlineSite(F, CI, Callee, FreqCallee, EnableRecursiveInline,
			LateCleanupMode, RecursiveCallees[Callee], FAM, Profile)) {
			continue;
		}
		if (LateCleanupMode) {
			constexpr int kLatePerCallerSiteBudget = 4;
			constexpr int kLatePerCallerCloneBudget = 96;
			constexpr int kLatePerCallerGrowthBudget = 48;
			int BodyInstCount = std::max(
				0, Profile.TotalInstCount - Profile.ReturnInstCount);
			int SiteGrowth = Callee->getDirectCallSites().size() == 1 ? 0 :
				std::max(0, BodyInstCount - 1);
			if (LateInlinedSites >= kLatePerCallerSiteBudget ||
				LateClonedInsts + BodyInstCount > kLatePerCallerCloneBudget ||
				LateCodeGrowth + SiteGrowth > kLatePerCallerGrowthBudget) {
				continue;
			}
			++LateInlinedSites;
			LateClonedInsts += BodyInstCount;
			LateCodeGrowth += SiteGrowth;
		}
		//3.1 建立形参到实参的初始映射，并一次性克隆被调函数的完整基本块区域
		ValueToValueMap InitialMap;
		std::vector<RetInst*> ClonedRets;
		auto CalleeOrigBlocks = snapshotPointers(*Callee);
		CalleeOrigBlocks.erase(
			std::remove(CalleeOrigBlocks.begin(), CalleeOrigBlocks.end(), nullptr),
			CalleeOrigBlocks.end());
		for (unsigned i = 0; i < Callee->getParamCount(); ++i) {
			Value* Param = Callee->getParam(i);
			Value* Arg = CI->getArg(i);
			if (!Param || !Arg) {
				InitialMap.clear();
				break;
			}
			InitialMap[Param] = Arg;
		}
		if (InitialMap.size() != Callee->getParamCount()) continue;
		CloneBlockRegionOptions CloneOptions;
		CloneOptions.ClearTailCallFlags = true;
		auto ClonedRegion = cloneBasicBlockRegion(
			F, CalleeOrigBlocks, InitialMap, CloneOptions);
		if (!ClonedRegion) continue;
		for (const auto& Pair : ClonedRegion->InstPairs) {
			if (auto* RI = dynamic_cast<RetInst*>(Pair.second)) {
				ClonedRets.push_back(RI);
			}
		}
		BasicBlock* ClonedEntry =
			ClonedRegion->getClonedBlock(Callee->front());
		if (!ClonedEntry) continue;
		//3.2 将克隆入口中的 alloca 移到调用者入口
		if (BasicBlock* CallerEntry = F.front()) {
			auto MoveAllocas = collectInstructions<AllocaInst>(*ClonedEntry);
			for (AllocaInst* AI : MoveAllocas) {
				AI->eraseFromParent();
				Instruction* EntryFirst = CallerEntry->front();
				if (!EntryFirst) {
					CallerEntry->pushBack(AI);
				} else {
					auto It = CallerEntry->begin();
					It.insertBefore(AI);
				}
			}
		}
		//3.3在Call指令之后切分基本块
		BasicBlock *A2 = CallBB->splitBlockAfter(CI);
		if (!A2) continue;
		bool CallHasInstUsers = CI->countInstructionUses() > 0;
		std::vector<std::pair<BasicBlock*, Value*>> RetEdges;
		RetEdges.reserve(ClonedRets.size());
		for (RetInst* RI : ClonedRets) {
			if (!RI) continue;
			auto* RB = RI->getParent();
			Value* RV = RI->getReturnValue();
			RetEdges.push_back({RB, RV});
		}
		//3.4 单返回值直接替换调用结果；多返回路径在 A2 头部用 PHI 合并
		if (CallHasInstUsers && CI->getIRType() && !CI->getIRType()->isVoid()) {
			if (RetEdges.size() == 1 && RetEdges[0].second) {
				CI->replaceAllUsesWith(RetEdges[0].second);
			} else {
				auto* RetPhi = new PhiInst(CI->getIRType());
				A2->pushFront(RetPhi);
				for (auto& E : RetEdges) {
					Value* InV = E.second ? E.second : UndefValue::NewUndefValue(CI->getIRType());
					RetPhi->addIncoming(InV, E.first);
				}
				CI->replaceAllUsesWith(RetPhi);
			}
		}
		//4.1 把克隆体的每条 ret 改写为到 A2 的无条件边
		for (RetInst* RI : ClonedRets) {
			if (!RI) continue;
			auto* RB = RI->getParent();
			if (!RB) continue;
			auto It = RB->begin();
			while (*It != RI) ++It;
			It.insertBefore(new BrInst(A2));
			delete RI;
		}
		//4.2把原 call 改成 br ClonedEntry
		{
			auto It = CallBB->begin();
			while (*It != CI) ++It;
			It.insertBefore(new BrInst(ClonedEntry));
			delete CI;
		}
		Changed = true;
	}
	if (!Changed) return PreservationStatus::all();
	PreservationStatus PA;
	PA.discard<DominantAnalysis>();
	PA.discard<PostDominantAnalysis>();
	PA.discard<LoopAnalysis>();
	PA.discard<BasicAliasAnalysis>();
	PA.discard<SideEffectAnalysis>();
	return PA;
}
