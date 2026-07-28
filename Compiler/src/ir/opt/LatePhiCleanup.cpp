#include "ir/opt/LatePhiCleanup.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "lib/IRUtils.hpp"
#include <unordered_set>
#include <vector>

Value *LatePhiCleanupPass::safeReplacement(PhiInst *Phi, const DominatorTree &DT) {
	// 1.1 汇总 PHI 的有效 incoming：忽略自引用和 undef，但要求其余值全部相同。
	if (!Phi) return nullptr;

	const unsigned N = Phi->getNumIncomingValues();
	if (N == 0) {
		return UndefValue::NewUndefValue(Phi->getIRType());
	}

	Value *Common = nullptr;
	bool SawUndef = false;
	std::unordered_set<BasicBlock *> Preds;

	bool Compatible = forEachPhiIncoming(
		Phi, [&](unsigned, Value *V, BasicBlock *Pred) {
			if (Pred) Preds.insert(Pred);
			if (!V || V == Phi) return true;
			if (V->isUndef()) {
				SawUndef = true;
				return true;
			}
			if (!Common) {
				Common = V;
				return true;
			}
			return Common == V;
		});
	if (!Compatible) return nullptr;

	if (!Common) return nullptr;

	// 1.2 唯一 CFG 前驱天然保证 incoming 可用；其他形态必须由支配关系证明替换值
	// 在 PHI 位置已经定义，不能因为忽略了 undef 就引入 use-before-def。
	BasicBlock *BB = Phi->getParent();
	const bool SoleCFGPred =
		BB && Preds.size() == 1 && BB->getPredecessors().size() == 1 &&
		BB->getPredecessors().front() == *Preds.begin();

	if (!SoleCFGPred && !DT.dominatesValue(Common, Phi)) return nullptr;

	if (SawUndef && !DT.dominatesValue(Common, Phi) && !SoleCFGPred) return nullptr;
	return Common;
}

bool LatePhiCleanupPass::cleanupFunction(Function &F, DominatorTree &DT) {
	// 2. 反复快照并清理 PHI。一次替换会让上游 PHI 失去用户或退化，因此迭代到不动点。
	bool Changed = false;
	bool LocalChanged = true;

	while (LocalChanged) {
		LocalChanged = false;
		std::vector<PhiInst *> Phis;
		for (BasicBlock *BB : F) {
			if (!BB) continue;
			for (PhiInst *Phi : BB->collectPhiNodes()) {
				if (Phi) Phis.push_back(Phi);
			}
		}

		for (PhiInst *Phi : Phis) {
			if (!Phi || !Phi->getParent()) continue;

			// 2.1 无用户 PHI 直接删除；退化 PHI 仅在 safeReplacement 完成支配验证后折叠。
			if (!Phi->hasUses()) {
				delete Phi;
				Changed = true;
				LocalChanged = true;
				continue;
			}

			Value *Replacement = safeReplacement(Phi, DT);
			if (!Replacement || Replacement == Phi) continue;

			Phi->replaceAllUsesWith(Replacement);
			delete Phi;
			Changed = true;
			LocalChanged = true;
		}
	}

	return Changed;
}

PreservationStatus LatePhiCleanupPass::run(Function &F,
                                           FunctionAnalysisPassManager &FAM) {
	// 1. 获取支配树，供所有候选 PHI 共用同一份可用性证明。
	auto &DT = FAM.getResult<DominantAnalysis>(F);
	// 2. 将删除与折叠交替执行到局部不动点。
	bool Changed = cleanupFunction(F, DT);
	if (!Changed) return PreservationStatus::all();

	// 3. 该 pass 不改 CFG，因此保留结构分析；值流变化使别名与副作用摘要失效。
	PreservationStatus PA;
	PA.keep<DominantAnalysis>();
	PA.keep<PostDominantAnalysis>();
	PA.keep<LoopAnalysis>();
	PA.discard<BasicAliasAnalysis>();
	PA.discard<SideEffectAnalysis>();
	return PA;
}
