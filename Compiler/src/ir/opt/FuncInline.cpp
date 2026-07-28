#include "ir/opt/FuncInline.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "lib/IRUtils.hpp"
#include <algorithm>
#include <unordered_set>
#include <vector>

FuncInlinePass::FuncInlinePass(bool enableRecursiveInline)
	: EnableRecursiveInline(enableRecursiveInline) {}

// 步骤 1.2：扫描被调函数，统计体积和调用形态，并结合副作用分析判断它是否
// 仅访问局部内存以及是否更适合后续递归记忆化，而不是在此处展开。
CalleeInlineProfile FuncInlinePass::analyzeCallee(
	Function* Callee, FunctionAnalysisPassManager& FAM) {
	CalleeInlineProfile P;
	if (!Callee) return P;

	P.HasBody = (Callee->front() != nullptr);
	if (!P.HasBody) return P;

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
	// NoModRef 在当前 IR 中意味着 load/store 的基址均局限于函数内部对象。
	P.SideEffectFreeLocalOnly = (!SEI.hasBuiltin && getFunctionModRefInfo(SEI) == ModRefInfo::NoModRef);

	// 无其他调用、只访问局部内存且有多个自递归分支时，保留调用结构供
	// FunctionMemoization 识别重叠子问题。
	P.SafeToMemoize = (!P.HasOtherCalls && P.SideEffectFreeLocalOnly && P.SelfRecursiveCallSites >= 2);
	return P;
}

// 步骤 2：综合语义边界与代码增长预算筛选单个调用点。返回的 Profile 与
// 本次判定来自同一次分析，可直接供后续改写使用。
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
	// 内联会消除调用边界，计时调用及其可达封装均不得展开。
	if (CI->mayReachTimingCall() || Callee->mayReachTimingCall()) return false;

	// 克隆的 alloca 会被移到调用者入口；若移动路径或调用点所在环路穿过
	// 计时屏障，则拒绝改变其动态执行区间。
	bool CalleeHasAlloca = false;
	for (BasicBlock *BB : *Callee) {
		if (!BB) continue;
		for (Instruction *I : *BB) {
			if (dynamic_cast<AllocaInst *>(I)) {
				CalleeHasAlloca = true;
				break;
			}
		}
		if (CalleeHasAlloca) break;
	}
	if (CalleeHasAlloca && Caller.mayReachTimingCall() && Caller.front() &&
		(Caller.hasTimingBarrierOnPath(Caller.front(), nullptr, CI) ||
		 Caller.hasTimingBarrierOnPath(CI, CI))) {
		return false;
	}
	constexpr int kCallSitesThreshold = 3;
	constexpr int kInstThreshold = 200;
	constexpr int kRecursiveExpandMaxInst = 100;

	OutProfile = analyzeCallee(Callee, FAM);
	if (!OutProfile.HasBody) return false;

	if (OutProfile.SafeToMemoize) return false;

	auto it = FreqCallee.find(Callee);
	int Freq = (it == FreqCallee.end()) ? 1 : std::max(1, it->second);
	int InstCount = std::max(0, OutProfile.TotalInstCount - 1);

	const bool IsRecursiveFunc = (OutProfile.SelfRecursiveCallSites > 0);
	const bool IsSelfCall = (Callee == &Caller);

	if (IsRecursiveFunc) {
		// 递归函数只允许在自身调用点按更严格的总展开预算展开。
		if (!IsSelfCall) return false;
		if (!EnableRecursiveInline) return false;
		if (InstCount * Freq > kRecursiveExpandMaxInst) return false;
	} else {
		// 普通函数在调用点较少时直接放行，否则限制“体积 × 调用次数”。
		if (Freq >= kCallSitesThreshold && InstCount * Freq > kInstThreshold) return false;
	}

	return true;
}

PreservationStatus FuncInlinePass::run(Function &F, FunctionAnalysisPassManager &FAM) {
	bool Changed = false;

	// 1. 快照原始直接调用点并统计每个被调函数的频次；克隆体内新出现的调用
	// 不在本轮继续展开，从而给代码增长设置稳定边界。
	std::unordered_map<Function*, int> FreqCallee;
	std::vector<CallInst*> OriginalCallSites;

	for (CallInst *CI : collectInstructions<CallInst>(F)) {
		auto* Callee = CI->getDirectCallee();
		if (!Callee) continue;

		FreqCallee[Callee]++;
		OriginalCallSites.push_back(CI);
	}

	for (CallInst* CI : OriginalCallSites) {
		if (!CI) continue;
		BasicBlock* CallBB = CI->getParent();
		if (!CallBB) continue;

		auto* Callee = CI->getDirectCallee();
		if (!Callee) continue;

		CalleeInlineProfile Profile;
		// 2. 对每个快照调用点执行完整的语义与收益筛选。
		if (!isInlineSite(F, CI, Callee, FreqCallee, EnableRecursiveInline, FAM, Profile)) {
			continue;
		}

		// 3.1 建立形参到实参的初始映射，并一次性克隆被调函数的完整基本块区域。
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

		// 3.2 将克隆入口中的 alloca 移到调用者入口，保持栈对象由函数入口统一
		// 分配的 IR 约定。
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

		// 3.3 在调用之后切分原块，后续块 A2 将成为所有克隆返回路径的汇合点。
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

		// 3.4 单返回值直接替换调用结果；多返回路径在 A2 头部用 PHI 合并，缺失
		// 返回值的路径以 undef 补齐类型。
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

		// 4.1 把克隆体的每条 ret 改写为到 A2 的无条件边。
		for (RetInst* RI : ClonedRets) {
			if (!RI) continue;
			auto* RB = RI->getParent();
			if (!RB) continue;

			auto It = RB->begin();
			while (*It != RI) ++It;
			It.insertBefore(new BrInst(A2));
			delete RI;
		}

		// 4.2 用到克隆入口的分支替代原 call，完成 CFG 接入。
		{
			auto It = CallBB->begin();
			while (*It != CI) ++It;
			It.insertBefore(new BrInst(ClonedEntry));
			delete CI;
		}

		Changed = true;
	}

	if (!Changed) return PreservationStatus::all();

	// 5. CFG、调用图可见副作用及别名关系均已改变，相关函数分析全部失效。
	PreservationStatus PA;
	PA.discard<DominantAnalysis>();
	PA.discard<PostDominantAnalysis>();
	PA.discard<LoopAnalysis>();
	PA.discard<BasicAliasAnalysis>();
	PA.discard<SideEffectAnalysis>();
	return PA;
}
