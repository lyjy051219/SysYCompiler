#pragma once

#include "ir/analysis/LoopInfo.hpp"
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include <unordered_set>
#include <vector>

//LoopSimplifyPass：
//依赖Dominant和LoopInfo两个AnalysisPass
//- 目的：确保Loop满足规范形式
//- 唯一的 PreHeader
//- 唯一的 Back Edge
//- Dedicated Exits （ 所有的 Exit Block 只能被循环内部的基本块指向）

class LoopSimplifyPass : public PassDescriptor<LoopSimplifyPass> {
public:
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);
    static bool rewritePhisForMergedPreds(BasicBlock *Target,
										  const std::unordered_set<BasicBlock *> &FromPreds,
										  BasicBlock *MergeBlock);

private:
	bool insertPreHeader(Function &F, Loop &L);
	bool insertDedicatedExits(Function &F, Loop &L);
	bool insertLatch(Function &F, Loop &L);
	bool simplifyHeaderPhis(Loop &L);
	bool runOnLoop(Function &F, Loop &L, bool &cfgChanged);

};
