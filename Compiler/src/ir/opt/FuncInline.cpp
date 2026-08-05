#include "ir/opt/FuncInline.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "lib/IRUtils.hpp"
#include <algorithm>
#include <unordered_set>
#include <vector>
FuncInlinePass::FuncInlinePass(bool enableRecursiveInline): EnableRecursiveInline(enableRecursiveInline) {}
//步骤2.2：计算Callee的收益模型
CalleeInlineProfile FuncInlinePass::analyzeCallee(
	Function* Callee, FunctionAnalysisPassManager& FAM) {
	CalleeInlineProfile P;
	if (!Callee) return P;
    //Callee必须有函数体
	P.HasBody = (Callee->front() != nullptr);
	if (!P.HasBody) return P;
    //遍历所有指令，记录总指令数，自递归调用点数，是否调用其他函数
	forEachInstruction(*Callee, [&](Instruction *I) {
		++P.TotalInstCount;
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
	FunctionAnalysisPassManager& FAM,
	CalleeInlineProfile& OutProfile)
{
	if (!CI || !Callee) return false;
    //普通内联函数调用点及指令支持
	constexpr int kCallSitesThreshold = 3;
	constexpr int kInstThreshold = 200;
    //递归函数内联最大指令支持
	constexpr int kRecursiveExpandMaxInst = 100;
	OutProfile = analyzeCallee(Callee, FAM);
	if (!OutProfile.HasBody) return false;
	if (OutProfile.SafeToMemoize) return false;
	auto it = FreqCallee.find(Callee);
    //调用Callee频次
	int Freq = (it == FreqCallee.end()) ? 1 : std::max(1, it->second);
    //指令数
	int InstCount = std::max(0, OutProfile.TotalInstCount - 1);
	const bool IsRecursiveFunc = (OutProfile.SelfRecursiveCallSites > 0);
	const bool IsSelfCall = (Callee == &Caller);
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
	std::vector<CallInst*> OriginalCallSites;
	for (CallInst *CI : collectInstructions<CallInst>(F)) {
		auto* Callee = CI->getDirectCallee();
		if (!Callee) continue;
		FreqCallee[Callee]++;
		OriginalCallSites.push_back(CI);
	}
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
		if (!isInlineSite(F, CI, Callee, FreqCallee, EnableRecursiveInline, FAM, Profile)) {
			continue;
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
