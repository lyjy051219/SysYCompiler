#pragma once

#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include "lib/IRUtils.hpp"
#include <unordered_map>
#include <unordered_set>
#include <vector>
// 将循环内定义、循环外使用的 SSA 值封闭在循环出口：
// 1. 按循环后序寻找支配退出路径且存在使用的内部定义；
// 2. 精确识别普通使用与 PHI 入边的概念使用块，并过滤不可达使用；
// 3. 在出口插入初始 LCSSA PHI，沿支配树为外部汇合点递归构造桥接 PHI；
// 4. 改写所有跨边界 use，并把新 PHI 加入工作队列直至局部收敛。
class LCSSAPass : public PassDescriptor<LCSSAPass> {
public:
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);
	static bool isLCSSAForm(Loop *L);

private:
	struct ExternalUse {
		Use *U = nullptr;
		Instruction *UserInst = nullptr;
		PhiInst *UserPhi = nullptr;
		BasicBlock *UseBlock = nullptr;
	};

	using ExitPhiMap = std::unordered_map<BasicBlock *, PhiInst *>;
	using InstructionWorkList =
		UniqueWorkList<Instruction *>;

	static bool replaceUseWithValue(Use *U, Value *V);
    // 服务步骤 3：MapValue 既是递归缓存，也在新建桥接 PHI 前预置占位，
    // 从而终止 CFG 环上的递归并保持每个块至多一个桥接值。
    Value *getValueForBlock(BasicBlock *BB,
							Instruction *InstDef,
                            const Loop &L,
                            const DominatorTree &DT,
                            std::unordered_map<BasicBlock *, Value *> &MapValue,
							InstructionWorkList &WorkList,
                            bool &Changed);

	// 服务步骤 2—4：处理一个循环内定义，收集跨界 use、建立出口种子值并
	// 递归取得各概念使用块上的替换值。
	bool runLCSSA(Instruction *I,
							Function &F,
							const LoopInfo &LI,
							const DominatorTree &DT,
							const std::unordered_set<BasicBlock *> &Reachable,
							InstructionWorkList &WorkList);
};
