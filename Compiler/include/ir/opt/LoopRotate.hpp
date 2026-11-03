// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include <unordered_map>
#include <unordered_set>
#include <vector>

//LoopRotate 的整体流程：
//1. 在函数内从外向内尝试循环，每成功旋转一个候选就失效循环/支配分析并重新扫描。
//2. 候选必须是 LoopSimplify + LCSSA 的头部测试循环，具有唯一出口、回边和严格前驱形态。
//3. 先折叠 Body 中可简化的普通 PHI，再把 Header 计算克隆到 PreHeader，并在 Body 为首次/回边值建 PHI。
//4. 按 Header、循环体和循环外使用分别重写 SSA，在出口补齐 LCSSA 合流。
//5. 将首次条件移到 PreHeader，修复专用 preheader/出口，再把旧 Header 合并到 Latch 形成尾部测试。
//6. 未变更时保留全部分析；改写 CFG 后使支配、后支配、循环、别名和副作用分析失效。
class LoopRotatePass : public PassDescriptor<LoopRotatePass> {
public:
	//在函数内反复把合法的头部测试循环旋转为尾部测试形态。
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);
 
private:
	//2-5. 完成单循环的预检、Header 克隆、SSA 转发和 CFG 旋转。
	bool runLoopRotate(
		Function &F, Loop &L, const DominatorTree &DT,
		const LoopInfo &LI, FunctionAnalysisPassManager &FAM);

	//识别带有约定标记且不应被普通PHI折叠的LCSSA节点。
	static bool isLCSSAPhi(PhiInst *Phi);
	//使用给定值映射克隆Header指令并返回游离副本。
	static Instruction *cloneInstructionWithMap(
		Instruction *I,
		const std::unordered_map<Value *, Value *> &VMap);
};
