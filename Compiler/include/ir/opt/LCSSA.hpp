#pragma once
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include "lib/IRUtils.hpp"
#include <unordered_map>
#include <unordered_set>
#include <vector>
//将循环内定义、循环外使用的 SSA 值封闭在循环出口：
//1. 按循环后序寻找支配退出路径且存在使用的内部定义；
//2. 精确识别普通使用与 PHI 入边的概念使用块，并过滤不可达使用；
//3. 在出口插入初始 LCSSA PHI，沿支配树为外部汇合点递归构造桥接 PHI；
//4. 改写所有跨边界 use，并把新 PHI 加入工作队列直至局部收敛。
class LCSSAPass : public PassDescriptor<LCSSAPass> {
public:
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);
	//检查循环外使用是否都通过位于退出块的合法LCSSA PHI。
	static bool isLCSSAForm(Loop *L);

private:
	//ExternalUse描述一条跨循环边界use及其概念使用位置。
	struct ExternalUse {
		//记录需要改写的底层Use对象。
		Use *U = nullptr;
		//记录拥有该use的指令。
		Instruction *UserInst = nullptr;
		//当用户为PHI时记录该PHI以解释入边使用位置。
		PhiInst *UserPhi = nullptr;
		//记录普通指令所在块或PHI对应入边前驱块。
		BasicBlock *UseBlock = nullptr;
	};

	//ExitPhiMap把循环退出块映射到定义对应的初始LCSSA PHI。
	using ExitPhiMap = std::unordered_map<BasicBlock *, PhiInst *>;
	//InstructionWorkList保存尚未处理且自动去重的内部定义和新桥接PHI。
	using InstructionWorkList =
		UniqueWorkList<Instruction *>;

	//用给定值替换use并返回是否真正发生改写。
	static bool replaceUseWithValue(Use *U, Value *V);
    //服务步骤 3：MapValue 既是递归缓存，也在新建桥接 PHI 前预置占位，
    //从而终止 CFG 环上的递归并保持每个块至多一个桥接值。
    //递归取得定义在指定块入口处可见的边界值并按需创建桥接PHI。
    Value *getValueForBlock(BasicBlock *BB,
							Instruction *InstDef,
                            const Loop &L,
                            const DominatorTree &DT,
                            std::unordered_map<BasicBlock *, Value *> &MapValue,
							InstructionWorkList &WorkList,
                            bool &Changed);

	//服务步骤 2—4：处理一个循环内定义，收集跨界 use、建立出口种子值并
	//递归取得各概念使用块上的替换值。
	//把单条循环内定义的全部外部use改写为经出口PHI传递的值。
	bool runLCSSA(Instruction *I,
							Function &F,
							const LoopInfo &LI,
							const DominatorTree &DT,
							const std::unordered_set<BasicBlock *> &Reachable,
							InstructionWorkList &WorkList);
};
