#include "ir/opt/FuncInline.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include <algorithm>
#include <unordered_set>
#include <vector>


static CalleeInlineProfile analyzeCallee(Function* Callee, FunctionAnalysisPassManager& FAM) {
	CalleeInlineProfile P;
	if (!Callee) return P;

	P.HasBody = (Callee->front() != nullptr);
	if (!P.HasBody) return P;

	for (auto* BB : *Callee) {
		if (!BB) continue;
		for (auto* I : *BB) {
			if (!I) continue;
			++P.TotalInstCount;

			if (I->instType != Instruction::CALL) continue;
			auto& UL = I->getUserList();
			if (UL.empty() || !UL[0]) {
				P.HasOtherCalls = true;
				continue;
			}

			Value* CV = UL[0]->getValue();
			auto* CF = dynamic_cast<Function*>(CV);
			if (!CF) {
				P.HasOtherCalls = true;
				continue;
			}

			if (CF == Callee) {
				++P.SelfRecursiveCallSites;
			} else {
				P.HasOtherCalls = true;
			}
		}
	}

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
static bool isInlineSite(
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

	for (auto* BB : F) {
		if (!BB) continue;
		for (auto* I : *BB) {
			if (!I || I->instType != Instruction::CALL) continue;
			auto* CI = dynamic_cast<CallInst*>(I);
			if (!CI) continue;

			auto& UL = CI->getUserList();
			if (UL.empty() || !UL[0]) continue;
			auto* Callee = dynamic_cast<Function*>(UL[0]->getValue());
			if (!Callee) continue;

			FreqCallee[Callee]++;
			OriginalCallSites.push_back(CI);
		}
	}

	for (CallInst* CI : OriginalCallSites) {
		if (!CI) continue;
		BasicBlock* CallBB = CI->getParent();
		if (!CallBB) continue;

		auto& CallUL = CI->getUserList();
		if (CallUL.empty() || !CallUL[0]) continue;
		auto* Callee = dynamic_cast<Function*>(CallUL[0]->getValue());
		if (!Callee) continue;

		CalleeInlineProfile Profile;
		if (!isInlineSite(F, CI, Callee, FreqCallee, EnableRecursiveInline, FAM, Profile)) {
			continue;
		}

		std::unordered_map<Value*, Value*> VMap;
		std::unordered_map<BasicBlock*, BasicBlock*> BBMap;
		std::vector<RetInst*> ClonedRets;
		std::vector<std::pair<Instruction*, Instruction*>> ClonedInstPairs;
		std::vector<std::pair<PhiInst*, PhiInst*>> ClonedPhiPairs;
		std::vector<BasicBlock*> CalleeOrigBlocks;
		for (auto* OldBB : *Callee) {
			if (OldBB) CalleeOrigBlocks.push_back(OldBB);
		}

		auto& Params = Callee->getParams();
		for (size_t i = 0; i < Params.size(); ++i) {
			Value* Param = Params[i].get();
			Value* Arg = (i + 1 < CallUL.size() && CallUL[i + 1])
				? CallUL[i + 1]->getValue()
				: nullptr;
			VMap[Param] = Arg;
		}

		// 先克隆所有基本块，建立 oldBB -> newBB 映射。
		for (auto* OldBB : CalleeOrigBlocks) {
			if (!OldBB) continue;
			auto* NewBB = new BasicBlock();
			F.pushBlock(NewBB);
			VMap[OldBB] = NewBB;
			BBMap[OldBB] = NewBB;
		}

		// 再克隆指令并填充 oldInst -> newInst 映射。
		for (auto* OldBB : CalleeOrigBlocks) {
			if (!OldBB) continue;
			auto* NewBB = BBMap[OldBB];
			for (auto* OldI : *OldBB) {
				if (!OldI) continue;
				Instruction* NewI = OldI->clone(VMap);
				if (!NewI) continue;

				if (auto* NewCall = dynamic_cast<CallInst*>(NewI)) {
					// clone 后直接清理 tail 标记，避免错误传播。
					NewCall->setTailCall(false);
				}

				NewBB->push_back(NewI);
				VMap[OldI] = NewI;
				ClonedInstPairs.push_back({OldI, NewI});

				if (auto* OldPhi = dynamic_cast<PhiInst*>(OldI)) {
					auto* NewPhi = dynamic_cast<PhiInst*>(NewI);
					if (NewPhi) ClonedPhiPairs.push_back({OldPhi, NewPhi});
				}

				if (auto* RI = dynamic_cast<RetInst*>(NewI)) {
					ClonedRets.push_back(RI);
				}
			}
		}

		// 全量二次修复所有 clone 指令操作数。
		// 仅修 PHI/Br 不够：当 OldI 的某个操作数是“稍后才克隆”的值时，第一次 clone 会残留旧作用域引用。
		for (auto& Pair : ClonedInstPairs) {
			auto* OldI = Pair.first;
			auto* NewI = Pair.second;
			if (!OldI || !NewI) continue;

			auto& OUL = OldI->getUserList();
			auto& NUL = NewI->getUserList();
			size_t N = std::min(OUL.size(), NUL.size());
			for (size_t i = 0; i < N; ++i) {
				if (!OUL[i] || !NUL[i]) continue;
				Value* OldOp = OUL[i]->getValue();
				auto It = VMap.find(OldOp);
				if (It != VMap.end() && It->second) {
					NUL[i]->replaceUseeWith(It->second);
				}
			}
		}

		// 二次修复 clone 后 PHI 的 incoming，避免前向引用残留 old Callee 值。
		for (auto& Pair : ClonedPhiPairs) {
			auto* OldPhi = Pair.first;
			auto* NewPhi = Pair.second;
			if (!OldPhi || !NewPhi) continue;

			for (unsigned i = 0; i < OldPhi->getNumIncomingValues(); ++i) {
				Value* OldInV = OldPhi->getIncomingValue(i);
				BasicBlock* OldInB = OldPhi->getIncomingBlock(i);

				auto Vit = VMap.find(OldInV);
				if (Vit != VMap.end()) {
					NewPhi->setIncomingValue(i, Vit->second);
				}

				auto Bit = BBMap.find(OldInB);
				if (Bit != BBMap.end()) {
					NewPhi->getUserList()[i * 2 + 1]->replaceUseeWith(Bit->second);
				}
			}
		}

		// 显式修复 clone 图里的终结指令目标：只能跳到新 clone 的基本块。
		for (auto* OldBB : CalleeOrigBlocks) {
			if (!OldBB) continue;
			auto* NewBB = BBMap[OldBB];
			if (!NewBB) continue;

			auto* TI = NewBB->back();
			auto* BI = dynamic_cast<BrInst*>(TI);
			if (!BI) continue;

			auto& BUL = BI->getUserList();
			if (BUL.size() == 1 && BUL[0]) {
				auto* Dst = dynamic_cast<BasicBlock*>(BUL[0]->getValue());
				auto it = BBMap.find(Dst);
				if (it != BBMap.end() && it->second != Dst) {
					BUL[0]->replaceUseeWith(it->second);
				}
			} else if (BUL.size() == 3) {
				for (int idx : {1, 2}) {
					if (!BUL[idx]) continue;
					auto* Dst = dynamic_cast<BasicBlock*>(BUL[idx]->getValue());
					auto it = BBMap.find(Dst);
					if (it != BBMap.end() && it->second != Dst) {
						BUL[idx]->replaceUseeWith(it->second);
					}
				}
			}
		}

		BasicBlock* ClonedEntry = BBMap[Callee->front()];
		if (!ClonedEntry) continue;

		// 将 clone 入口块里的 alloca 移到 Caller 入口块最前面。
		if (BasicBlock* CallerEntry = F.front()) {
			std::vector<AllocaInst*> MoveAllocas;
			for (auto* I : *ClonedEntry) {
				if (auto* AI = dynamic_cast<AllocaInst*>(I)) {
					MoveAllocas.push_back(AI);
				}
			}

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
		auto* A2 = new BasicBlock();
		F.pushBlock(A2);

		{
			Instruction* Cur = CI->getNext();
			while (Cur) {
				Instruction* Next = Cur->getNext();
				Cur->EraseFromParent();
				A2->push_back(Cur);
				Cur = Next;
			}
		}

		// 原块 successor 的 PHI incoming 现在应从 A2 进入（原 terminator 已搬到 A2）。
		for (BasicBlock* Succ : A2->getSuccessors()) {
			if (!Succ) continue;
			for (auto* Inst : *Succ) {
				auto* Phi = dynamic_cast<PhiInst*>(Inst);
				if (!Phi) break;
				for (unsigned i = 0; i < Phi->getNumIncomingValues(); ++i) {
					if (Phi->getIncomingBlock(i) == CallBB) {
						Phi->getUserList()[i * 2 + 1]->replaceUseeWith(A2);
					}
				}
			}
		}

		bool CallHasInstUsers = false;
		for (auto It = CI->getUseList().begin(); It != CI->getUseList().end(); ++It) {
			Use* U = *It;
			if (!U || U->getValue() != CI) continue;
			if (dynamic_cast<Instruction*>(U->getUser())) {
				CallHasInstUsers = true;
				break;
			}
		}

		std::vector<std::pair<BasicBlock*, Value*>> RetEdges;
		RetEdges.reserve(ClonedRets.size());
		for (RetInst* RI : ClonedRets) {
			if (!RI) continue;
			auto* RB = RI->getParent();
			Value* RV = nullptr;
			auto& RUL = RI->getUserList();
			if (!RUL.empty() && RUL[0]) RV = RUL[0]->getValue();
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
