#include "ir/opt/DAE.hpp"
#include "lib/IRUtils.hpp"

#include <algorithm>

bool DAEPass::tryConstantSpecialization(
	Function *F, const std::vector<CallInst *> &CallSites) {
	if (!F) return false;
	if (F->getParamCount() == 0) return false;
	if (CallSites.empty()) return false;

	bool Changed = false;

	for (unsigned i = 0; i < F->getParamCount(); ++i) {
		Value *P = F->getParam(i);
		if (!P) continue;

		bool AllSameConst = true;
		Value *TheConst = nullptr;

		for (CallInst *CI : CallSites) {
			if (!CI) {
				AllSameConst = false;
				break;
			}

				Value *Arg = CI->getArg(static_cast<unsigned>(i));
				if (!Arg) {
					AllSameConst = false;
					break;
				}
			if (!Arg->isConst()) {
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

bool DAEPass::tryRedundantArgMerge(
	Function *F, const std::vector<CallInst *> &CallSites) {
	if (!F) return false;
	if (F->getParamCount() < 2) return false;
	if (CallSites.empty()) return false;

	bool Changed = false;

	for (unsigned i = 0; i < F->getParamCount(); ++i) {
		Value *Pi = F->getParam(i);
		if (!Pi) continue;

		for (unsigned j = i + 1; j < F->getParamCount(); ++j) {
			Value *Pj = F->getParam(j);
			if (!Pj) continue;

			bool AlwaysEqual = true;
			for (CallInst *CI : CallSites) {
				if (!CI) {
					AlwaysEqual = false;
					break;
				}
				Value *IArg = CI->getArg(static_cast<unsigned>(i));
				Value *JArg = CI->getArg(static_cast<unsigned>(j));
				if (!IArg || !JArg) {
					AlwaysEqual = false;
					break;
				}
				if (IArg != JArg) {
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
			const int Idx = Owner->getParamIndex(Param);
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

	ScopedActiveMark VisitingMark(Visiting, Param);
	if (!VisitingMark.inserted()) {
		Memo[Param] = ParamLiveness::Dead;
		return false;
	}
	Memo[Param] = ParamLiveness::InProgress;

	bool Live = false;
	if (Param->hasUses()) {
		for (Use *U : Param->getUses()) {

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

			int ArgIdx = CI->getArgIndex(U);
			if (ArgIdx < 0) {
				Live = true;
				break;
			}

			Function *Callee = CI->getDirectCallee();
			if (!Callee || Callee->hasReservedSignature()) {
				Live = true;
				break;
			}

			auto &CalleeParams = Callee->getParams();
			const size_t CalleeParamIdx = static_cast<size_t>(ArgIdx);
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

	std::vector<CallInst *> CallSites = OldF->getDirectCallSites();

	for (CallInst *OldCI : CallSites) {
		if (!OldCI) continue;

		// 倒序删除，避免实参索引偏移。
		for (size_t k = DeadParamIdx.size(); k > 0; --k) {
			const size_t ParamIdx = DeadParamIdx[k - 1];
			OldCI->eraseArg(static_cast<unsigned>(ParamIdx));
		}
	}

	// 原地收缩函数形参表（倒序删除）
	for (size_t k = DeadParamIdx.size(); k > 0; --k) {
		const size_t ParamIdx = DeadParamIdx[k - 1];
		OldF->eraseParam(ParamIdx);
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
		if (!F || F->hasReservedSignature()) continue;

		std::vector<CallInst *> CallSites = F->getDirectCallSites();
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
		if (!F || F->hasReservedSignature()) continue;
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
		if (!OldF || OldF->hasReservedSignature()) continue;
		const std::vector<bool> &LiveMask = Entry.second;
		if (rewriteFunctionWithDeadArgs(M, OldF, LiveMask)) {
			Changed = true;
		}
	}

	return Changed ? PreservationStatus::none() : PreservationStatus::all();
}
