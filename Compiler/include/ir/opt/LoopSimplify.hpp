#pragma once

#include "ir/analysis/LoopInfo.hpp"
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include <unordered_set>
#include <vector>
// 逐步把自然循环规范成唯一预头、唯一回边锁存块和专用出口：
// 1. 按后序选择循环，并优先合并循环外入口；
// 2. 将同时拥有循环内外前驱的出口拆成专用出口；
// 3. 合并多个回边来源，最后清理退化的头部 PHI；
// 4. 每次调用至多提交一次 CFG 改写，下一轮用重建后的 LoopInfo 继续收敛。
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
