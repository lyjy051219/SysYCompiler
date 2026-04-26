#include "ir/opt/DAE.hpp"

#include <algorithm>

static int findParamIndex(Function *F, Value *Param) {
	if (!F || !Param) return -1;
	auto &Params = F->getParams();
	for (size_t i = 0; i < Params.size(); ++i) {
		if (Params[i].get() == Param) return static_cast<int>(i);
	}
	return -1;
}

static std::vector<CallInst *> collectDirectCallSites(Function *F) {
	std::vector<CallInst *> CallSites;
	if (!F) return CallSites;

	for (auto UIt = F->getUseList().begin(); UIt != F->getUseList().end(); ++UIt) {
		Use *U = *UIt;
		if (!U) continue;
		auto *CI = dynamic_cast<CallInst *>(U->getUser());
		if (!CI) continue;
		if (CI->getUserIndex(U) != 0) continue;
		CallSites.push_back(CI);
	}

	return CallSites;
}

static bool tryConstantSpecialization(Function *F, const std::vector<CallInst *> &CallSites) {
	if (!F) return false;
	auto &Params = F->getParams();
	if (Params.empty()) return false;
	if (CallSites.empty()) return false;

	bool Changed = false;

	for (size_t i = 0; i < Params.size(); ++i) {
		Value *P = Params[i].get();
		if (!P) continue;

		bool AllSameConst = true;
		Value *TheConst = nullptr;

		for (CallInst *CI : CallSites) {
			if (!CI) {
				AllSameConst = false;
				break;
			}

			auto &Ops = CI->getUserList();
			const size_t OpIdx = i + 1;
			if (OpIdx >= Ops.size() || !Ops[OpIdx]) {
				AllSameConst = false;
				break;
			}

			Value *Arg = Ops[OpIdx]->getValue();
			if (!dynamic_cast<ConstantSystem *>(Arg)) {
				AllSameConst = false;
				break;
			}

			if (!TheConst) {
				TheConst = Arg;
			} else if (TheConst != Arg) {
				AllSameConst = false;
				break;
			}
		}

		if (AllSameConst && TheConst) {
			P->replaceAllUsesWith(TheConst);
			Changed = true;
		}
	}

	return Changed;
}

static bool tryRedundantArgMerge(Function *F, const std::vector<CallInst *> &CallSites) {
	if (!F) return false;
	auto &Params = F->getParams();
	if (Params.size() < 2) return false;
	if (CallSites.empty()) return false;

	bool Changed = false;

	for (size_t i = 0; i < Params.size(); ++i) {
		Value *Pi = Params[i].get();
		if (!Pi) continue;

		for (size_t j = i + 1; j < Params.size(); ++j) {
			Value *Pj = Params[j].get();
			if (!Pj) continue;

			bool AlwaysEqual = true;
			for (CallInst *CI : CallSites) {
				if (!CI) {
					AlwaysEqual = false;
					break;
				}
				auto &Ops = CI->getUserList();
				const size_t IIdx = i + 1;
				const size_t JIdx = j + 1;
				if (IIdx >= Ops.size() || JIdx >= Ops.size() || !Ops[IIdx] || !Ops[JIdx]) {
					AlwaysEqual = false;
					break;
				}
				if (Ops[IIdx]->getValue() != Ops[JIdx]->getValue()) {
					AlwaysEqual = false;
					break;
				}
			}

			if (AlwaysEqual) {
				Pj->replaceAllUsesWith(Pi);
				Changed = true;
			}
		}
	}

	return Changed;
}

bool DAEPass::isExternelFunc(Function *F) {
	if (!F) return true;
	const std::string Name = F->getIdent();
	static const std::unordered_set<std::string> SysyLibNames = {
		"getint", "getch", "getfloat", "getarray", "getfarray",
		"putint", "putch", "putfloat", "putarray", "putfarray", "putf",
		"starttime", "stoptime", "_sysy_starttime", "_sysy_stoptime"
	};
	if (Name == "main") return true;
	if (SysyLibNames.count(Name)) return true;
	if (Name.find("_sysy_") != std::string::npos) return true;
	if (Name.find("memcpy") != std::string::npos) return true;
	if (Name.find("llvm.memcpy") != std::string::npos) return true;
	return false;
}

bool DAEPass::isDirectCallOperand(CallInst *CI, const Use *U) {
	if (!CI || !U) return false;
	return CI->getUserIndex(const_cast<Use *>(U)) >= 0;
}

Function *DAEPass::getDirectCallee(CallInst *CI) {
	if (!CI) return nullptr;
	auto &UL = CI->getUserList();
	if (UL.empty() || !UL[0]) return nullptr;
	return dynamic_cast<Function *>(UL[0]->getValue());
}

const std::vector<bool> *DAEPass::getLiveMask(const ParamMaskMap &Masks, Function *F) {
	auto It = Masks.find(F);
	if (It == Masks.end()) return nullptr;
	return &It->second;
}

bool DAEPass::isParamLive(
	Function *Owner,
	Value *Param,
	std::unordered_map<Value *, ParamLiveness> &Memo,
	std::unordered_set<Value *> &Visiting,
	const ParamMaskMap &ExistingMasks) {
	if (!Owner || !Param) return true;

	if (const std::vector<bool> *KnownMask = getLiveMask(ExistingMasks, Owner)) {
		const int Idx = findParamIndex(Owner, Param);
		if (Idx >= 0 && static_cast<size_t>(Idx) < KnownMask->size()) {
			return (*KnownMask)[static_cast<size_t>(Idx)];
		}
	}

	auto It = Memo.find(Param);
	if (It != Memo.end()) {
		if (It->second == ParamLiveness::Live) return true;
		if (It->second == ParamLiveness::Dead) return false;
		if (It->second == ParamLiveness::InProgress) return false;
	}

	if (!Visiting.insert(Param).second) {
		Memo[Param] = ParamLiveness::Dead;
		return false;
	}
	Memo[Param] = ParamLiveness::InProgress;

	bool Live = false;
	if (Param->getUseList().size != 0) {
		for (auto UIt = Param->getUseList().begin(); UIt != Param->getUseList().end(); ++UIt) {
			Use *U = *UIt;
			if (!U) continue;

			User *UseUser = U->getUser();
			if (!UseUser) {
				Live = true;
				break;
			}

			auto *CI = dynamic_cast<CallInst *>(UseUser);
			if (!CI) {
				Live = true;
				break;
			}

			if (!isDirectCallOperand(CI, U)) {
				Live = true;
				break;
			}

			int OpIdx = CI->getUserIndex(U);
			if (OpIdx <= 0) {
				Live = true;
				break;
			}

			Function *Callee = getDirectCallee(CI);
			if (!Callee || isExternelFunc(Callee)) {
				Live = true;
				break;
			}

			auto &CalleeParams = Callee->getParams();
			const size_t CalleeParamIdx = static_cast<size_t>(OpIdx - 1);
			if (CalleeParamIdx >= CalleeParams.size()) {
				Live = true;
				break;
			}

			Value *CalleeParam = CalleeParams[CalleeParamIdx].get();
			if (!CalleeParam) {
				Live = true;
				break;
			}

			if (isParamLive(Callee, CalleeParam, Memo, Visiting, ExistingMasks)) {
				Live = true;
				break;
			}
		}
	}

	Visiting.erase(Param);
	Memo[Param] = Live ? ParamLiveness::Live : ParamLiveness::Dead;
	return Live;
}

bool DAEPass::rewriteFunctionWithDeadArgs(Module &M,
										  Function *OldF,
										  const std::vector<bool> &LiveMask) {
	(void)M;
	if (!OldF) return false;
	auto &OldParams = OldF->getParams();
	if (OldParams.empty()) return false;

	std::vector<size_t> DeadParamIdx;
	DeadParamIdx.reserve(OldParams.size());
	for (size_t i = 0; i < OldParams.size(); ++i) {
		const bool IsLive = (i < LiveMask.size() ? LiveMask[i] : false);
		if (!IsLive) DeadParamIdx.push_back(i);
	}
	if (DeadParamIdx.empty()) return false;

	// 先把死参在函数体内的使用替换成 undef，避免后续 erase 触发危险析构链
	for (size_t Idx : DeadParamIdx) {
		if (Idx >= OldParams.size() || !OldParams[Idx]) continue;
		Value *OldParam = OldParams[Idx].get();
		OldParam->replaceAllUsesWith(UndefValue::NewUndefValue(OldParam->getIRType()));
	}

	std::vector<CallInst *> CallSites;
	for (auto UIt = OldF->getUseList().begin(); UIt != OldF->getUseList().end(); ++UIt) {
		Use *U = *UIt;
		if (!U) continue;
		auto *CI = dynamic_cast<CallInst *>(U->getUser());
		if (!CI) continue;
		if (!isDirectCallOperand(CI, U)) continue;
		if (CI->getUserIndex(U) != 0) continue;
		CallSites.push_back(CI);
	}

	for (CallInst *OldCI : CallSites) {
		if (!OldCI) continue;

		auto &CallOps = OldCI->getUserList();
		// 倒序删除，避免索引偏移；CallOps[0] 是 callee，从 1 开始是实参
		for (size_t k = DeadParamIdx.size(); k > 0; --k) {
			const size_t ParamIdx = DeadParamIdx[k - 1];
			const size_t OpIdx = ParamIdx + 1;
			if (OpIdx < CallOps.size()) {
				CallOps.erase(CallOps.begin() + static_cast<std::ptrdiff_t>(OpIdx));
			}
		}
	}

	// 原地收缩函数形参表（倒序删除）
	for (size_t k = DeadParamIdx.size(); k > 0; --k) {
		const size_t ParamIdx = DeadParamIdx[k - 1];
		if (ParamIdx < OldParams.size()) {
			OldParams.erase(OldParams.begin() + static_cast<std::ptrdiff_t>(ParamIdx));
		}
	}

	return true;
}

PreservationStatus DAEPass::run(Module &M, ModuleAnalysisPassManager &MAM) {
	(void)MAM;

	bool Changed = false;

	std::vector<Function *> Snapshot;
	for (auto &FPtr : M.getFunctions()) {
		if (!FPtr) continue;
		Snapshot.push_back(FPtr.get());
	}

	// Step 1: 常量特化 + 冗余参数合并
	for (Function *F : Snapshot) {
		if (!F || isExternelFunc(F)) continue;

		std::vector<CallInst *> CallSites = collectDirectCallSites(F);
		if (CallSites.empty()) continue;

		if (tryConstantSpecialization(F, CallSites)) {
			Changed = true;
		}
		if (tryRedundantArgMerge(F, CallSites)) {
			Changed = true;
		}
	}

	ParamMaskMap DeadArgMaskByFunc;

	for (Function *F : Snapshot) {
		if (!F || isExternelFunc(F)) continue;
		auto &Params = F->getParams();
		if (Params.empty()) continue;

		std::unordered_map<Value *, ParamLiveness> Memo;
		std::unordered_set<Value *> Visiting;
		std::vector<bool> LiveMask(Params.size(), true);

		bool HasDeadArg = false;
		for (size_t i = 0; i < Params.size(); ++i) {
			Value *P = Params[i].get();
			const bool IsLive = isParamLive(F, P, Memo, Visiting, DeadArgMaskByFunc);
			LiveMask[i] = IsLive;
			if (!IsLive) HasDeadArg = true;
		}

		if (HasDeadArg) {
			DeadArgMaskByFunc[F] = std::move(LiveMask);
		}
	}

	std::vector<std::pair<Function *, std::vector<bool>>> RewritePlan;
	RewritePlan.reserve(DeadArgMaskByFunc.size());
	for (auto &KV : DeadArgMaskByFunc) {
		RewritePlan.push_back({KV.first, KV.second});
	}

	for (auto &Entry : RewritePlan) {
		Function *OldF = Entry.first;
		if (!OldF || isExternelFunc(OldF)) continue;
		const std::vector<bool> &LiveMask = Entry.second;
		if (rewriteFunctionWithDeadArgs(M, OldF, LiveMask)) {
			Changed = true;
		}
	}

	return Changed ? PreservationStatus::none() : PreservationStatus::all();
}
