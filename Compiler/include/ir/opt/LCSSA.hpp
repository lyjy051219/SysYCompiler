#pragma once

#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include "lib/IRUtils.hpp"
#include <unordered_map>
#include <unordered_set>
#include <vector>

// LCSSA
/*
依赖Dominant和LoopInfo
标准SSA：每个变量只被赋值一次，且使用该变量的地方必须被其定义所支配
LCSSA附加规则：如果一个变量是在循环内部定义的，但它在循环外部被使用了，那么这个跨越循环边界的数据流，
必须通过在循环退出块（Exit Block）中的 PHI 节点来传递。
*/
class LCSSAPass : public PassDescriptor<LCSSAPass> {
public:
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);
	static bool isLCSSAForm(Loop *L);

private:
	struct ExternalUse {
		Use *U = nullptr;
		Instruction *UserInst = nullptr;
		PhiInst *UserPhi = nullptr;
		BasicBlock *UseBlock = nullptr; // Conceptual use location (phi incoming block or user block)
	};

	using ExitPhiMap = std::unordered_map<BasicBlock *, PhiInst *>;
	using InstructionWorkList =
		UniqueWorkList<Instruction *>;

	static bool replaceUseWithValue(Use *U, Value *V);
    Value *getValueForBlock(BasicBlock *BB,
							Instruction *InstDef,
                            const Loop &L,
                            const DominatorTree &DT,
                            std::unordered_map<BasicBlock *, Value *> &MapValue,
							InstructionWorkList &WorkList,
                            bool &Changed);

	bool runLCSSA(Instruction *I,
							Function &F,
							const LoopInfo &LI,
							const DominatorTree &DT,
							const std::unordered_set<BasicBlock *> &Reachable,
							InstructionWorkList &WorkList);
};
