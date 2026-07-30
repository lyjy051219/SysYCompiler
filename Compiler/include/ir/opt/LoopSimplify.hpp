#pragma once

#include "ir/analysis/LoopInfo.hpp"
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include <unordered_set>
#include <vector>
//逐步把自然循环规范成唯一预头、唯一回边锁存块和专用出口：
//1. 按后序选择循环，并优先合并循环外入口；
//2. 将同时拥有循环内外前驱的出口拆成专用出口；
//3. 合并多个回边来源，最后清理退化的头部 PHI；
//4. 每次调用至多提交一次 CFG 改写，下一轮用重建后的 LoopInfo 继续收敛。
class LoopSimplifyPass : public PassDescriptor<LoopSimplifyPass> {
public:
	//按循环后序提交一次规范化改写并报告应保留的分析结果。
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);
    //把多条前驱边的PHI输入经合并块汇成一条并重建目标块PHI。
    static bool rewritePhisForMergedPreds(BasicBlock *Target,
										  const std::unordered_set<BasicBlock *> &FromPreds,
										  BasicBlock *MergeBlock);

private:
	//为循环的全部外部入口插入或确认唯一预头块。
	bool insertPreHeader(Function &F, Loop &L);
	//拆分同时具有循环内外前驱的退出块以形成专用出口。
	bool insertDedicatedExits(Function &F, Loop &L);
	//合并多个回边来源并为循环建立唯一锁存块。
	bool insertLatch(Function &F, Loop &L);
	//清理循环头中的空、单输入和纯自环退化PHI。
	bool simplifyHeaderPhis(Loop &L);
	//按预头、专用出口、锁存块和PHI顺序规范化单个循环。
	bool runOnLoop(Function &F, Loop &L, bool &cfgChanged);

};
