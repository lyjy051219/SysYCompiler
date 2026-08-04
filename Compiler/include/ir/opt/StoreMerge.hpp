#pragma once

#include "ir/opt/NewPassManager.hpp"

#include <cstddef>
#include <vector>

class BasicBlock;
class StoreInst;
class Value;

//StoreMergeCandidate记录一个汇合块及其所有前驱末尾能够合并的store。
struct StoreMergeCandidate {
	//接收合并后唯一store的汇合块。
	BasicBlock *Join = nullptr;
	//按照汇合块前驱顺序保存各路径末尾的store。
	std::vector<StoreInst *> Stores;
};

//流程：1.匹配所有前驱末尾的同类型store；2.以PHI分别合流地址和值；
//3.删除路径store并在汇合块开头生成唯一store；4.迭代合并公共store后缀。
class StoreMergePass : public PassDescriptor<StoreMergePass> {
public:
	//合并控制流汇合点各前驱末尾语义等价位置上的store。
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
	//限制单个汇合点参与合流的前驱数量，约束病态CFG的编译期开销。
	static constexpr std::size_t MaxMergePredecessors = 16;
	//返回前驱无条件跳向汇合块时紧邻分支的store。
	static StoreInst *getTailStore(BasicBlock *Pred, BasicBlock *Join);
	//验证候选store的值与指针类型能够由同一组PHI合流。
	static bool haveCompatibleTypes(const std::vector<StoreInst *> &Stores);
	//匹配一个至少具有两个不同前驱的通用store汇合候选。
	static bool matchCandidate(BasicBlock *Join,
	                           StoreMergeCandidate &Candidate);
	//为不完全相同的一组路径值建立汇合PHI，否则直接复用公共值。
	static Value *mergeIncomingValues(
		BasicBlock *Join, const std::vector<BasicBlock *> &Preds,
		const std::vector<Value *> &Values);
	//提交单个候选的SSA与内存改写。
	static bool applyCandidate(const StoreMergeCandidate &Candidate);
};
