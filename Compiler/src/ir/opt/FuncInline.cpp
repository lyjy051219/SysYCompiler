#include "ir/opt/FuncInline.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "lib/IRUtils.hpp"
#include "lib/TraverseTools.hpp"
#include <algorithm>
#include <unordered_set>
#include <vector>

FuncInlinePass::FuncInlinePass(bool enableRecursiveInline)
	: EnableRecursiveInline(enableRecursiveInline) {}

CalleeInlineProfile FuncInlinePass::analyzeCallee(
	Function* Callee, FunctionAnalysisPassManager& FAM) {
	CalleeInlineProfile P;
	if (!Callee) return P;

	P.HasBody = (Callee->front() != nullptr);
	if (!P.HasBody) return P;

	TraverseTools::forEachInstruction(*Callee, [&](Instruction *I) {
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
	// 要求 STORE/LOAD 的基地址只能是局部变量，等价于“对外部内存无 Mod/Ref”。
	P.SideEffectFreeLocalOnly = (!SEI.hasBuiltin && getFunctionModRefInfo(SEI) == ModRefInfo::NoModRef);

	// 对齐 isSafeToMemoize 语义：
	// 1) 只有自递归调用（无其它调用）
	// 2) 仅访问局部内存
	// 3) 至少两个自递归调用点（否则无重叠子问题收益）
	P.SafeToMemoize = (!P.HasOtherCalls && P.SideEffectFreeLocalOnly && P.SelfRecursiveCallSites >= 2);
	return P;
}

// 外层内联条件判定：仅保留在辅助函数中，内联主流程尽量集中在 run 里。
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
		// 递归函数只能做自调用内联。
		if (!IsSelfCall) return false;
		if (!EnableRecursiveInline) return false;
		// 对齐外部策略：按“函数规模 * 调用点数量”控制递归展开成本。
		if (InstCount * Freq > kRecursiveExpandMaxInst) return false;
	} else {
		// 对齐外部策略：调用点少时直接放行；否则再做总规模门槛。
		if (Freq >= kCallSitesThreshold && InstCount * Freq > kInstThreshold) return false;
	}

	return true;
}

PreservationStatus FuncInlinePass::run(Function &F, FunctionAnalysisPassManager &FAM) {
	bool Changed = false;

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
		if (!isInlineSite(F, CI, Callee, FreqCallee, EnableRecursiveInline, FAM, Profile)) {
			continue;
		}

		ValueToValueMap InitialMap;
		std::vector<RetInst*> ClonedRets;
		auto CalleeOrigBlocks = TraverseTools::snapshotPointers(*Callee);
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

		// 将 clone 入口块里的 alloca 移到 Caller 入口块最前面。
		if (BasicBlock* CallerEntry = F.front()) {
			auto MoveAllocas = collectInstructions<AllocaInst>(*ClonedEntry);

			for (AllocaInst* AI : MoveAllocas) {
				AI->EraseFromParent();
				Instruction* EntryFirst = CallerEntry->front();
				if (!EntryFirst) {
					CallerEntry->push_back(AI);
				} else {
					auto It = CallerEntry->begin();
					It.insertBefore(AI);
				}
			}
		}

		// CFG 重构：把 Call 后续指令切到 A2，A1(原块) 跳到 clone 入口。
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

		if (CallHasInstUsers && CI->getIRType() && CI->getIRType()->getEnum() != IR_VOID) {
			if (RetEdges.size() == 1 && RetEdges[0].second) {
				CI->replaceAllUsesWith(RetEdges[0].second);
			} else {
				auto* RetPhi = new PhiInst(CI->getIRType());
				A2->push_front(RetPhi);
				for (auto& E : RetEdges) {
					Value* InV = E.second ? E.second : UndefValue::NewUndefValue(CI->getIRType());
					RetPhi->addIncoming(InV, E.first);
				}
				CI->replaceAllUsesWith(RetPhi);
			}
		}

		// 将所有 clone Ret 改成跳转到 A2。
		for (RetInst* RI : ClonedRets) {
			if (!RI) continue;
			auto* RB = RI->getParent();
			if (!RB) continue;

			auto It = RB->begin();
			while (*It != RI) ++It;
			It.insertBefore(new BrInst(A2));
			delete RI;
		}

		// A1 末尾改为跳转到 clone 入口，并移除原 Call。
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
