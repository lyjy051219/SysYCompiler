#include "ir/opt/StoreMerge.hpp"
#include "lib/IRUtils.hpp"
#include <vector>
//返回前驱无条件跳向汇合块时紧邻分支的store。
StoreInst *StoreMergePass::getTailStore(BasicBlock *Pred,
                                        BasicBlock *Join) {
	//1.只跨越一条无条件控制流边移动store，避免越过任何可能观察内存的指令。
	if (!Pred || !Join || Pred == Join) return nullptr;
	BrInst *Terminator = Pred->getBranchTerminator();
	if (!BasicBlock::isUncondBr(Terminator) ||
		Terminator->getSuccessor(0) != Join) {
		return nullptr;
	}
	return dynamic_cast<StoreInst *>(Terminator->getPrev());
}
//验证候选store的值与指针类型能够由同一组PHI合流。
bool StoreMergePass::haveCompatibleTypes(
    const std::vector<StoreInst *> &Stores) {
	//2.地址和值允许在不同路径上取不同SSA值，但所有路径的类型形状必须一致。
	if (Stores.size() < 2 || !Stores.front()) return false;
	Value *FirstValue = Stores.front()->getValueOperand();
	Value *FirstPointer = Stores.front()->getPointerOperand();
	if (!FirstValue || !FirstPointer || !FirstValue->getIRType() ||
		!FirstPointer->getIRType() || !FirstPointer->getIRType()->isPointer()) {
		return false;
	}
	for (StoreInst *Store : Stores) {
		Value *StoredValue = Store ? Store->getValueOperand() : nullptr;
		Value *Pointer = Store ? Store->getPointerOperand() : nullptr;
		if (!StoredValue || !Pointer || !StoredValue->getIRType() ||
			!Pointer->getIRType() || !Pointer->getIRType()->isPointer() ||
			!IRType::hasSameShape(StoredValue->getIRType(),
			                      FirstValue->getIRType()) ||
			!IRType::hasSameShape(Pointer->getIRType(),
			                      FirstPointer->getIRType())) {
			return false;
		}
	}
	return true;
}
//匹配一个至少具有两个不同前驱的通用store汇合候选。
bool StoreMergePass::matchCandidate(
    BasicBlock *Join, StoreMergeCandidate &Candidate) {
	//1—2.匹配只依赖CFG边和store类型，不读取基本块、函数或测试用例名称。
	Candidate = StoreMergeCandidate{};
	if (!Join) return false;
	auto Preds = Join->getPredecessors();
	if (Preds.size() < 2 || Preds.size() > MaxMergePredecessors) return false;
	std::vector<StoreInst *> Stores;
	Stores.reserve(Preds.size());
	for (BasicBlock *Pred : Preds) {
		if (!Pred || Pred->countEdgesToSucc(Join) != 1) return false;
		StoreInst *Store = getTailStore(Pred, Join);
		if (!Store) return false;
		Stores.push_back(Store);
	}
	if (!haveCompatibleTypes(Stores)) return false;
	//2.1 旧方案静态成本为每条路径一条store；新方案是一条store加地址/值
	//中实际需要的PHI。只接受IR条数严格下降，不依赖后续DSE碰巧接力。
	bool SameValue = true;
	bool SamePointer = true;
	Value *FirstValue = Stores.front()->getValueOperand();
	Value *FirstPointer = Stores.front()->getPointerOperand();
	for (StoreInst *Store : Stores) {
		SameValue &= Store->getValueOperand() == FirstValue;
		SamePointer &= Store->getPointerOperand() == FirstPointer;
	}
	if (!SameValue &&
		!IRType::isFirstClassValueType(FirstValue->getIRType())) {
		return false;
	}
	const std::size_t NewInstructionCount =
		1 + (SameValue ? 0 : 1) + (SamePointer ? 0 : 1);
	if (Stores.size() <= NewInstructionCount) return false;
	Candidate.Join = Join;
	Candidate.Stores = std::move(Stores);
	return true;
}
//为不完全相同的一组路径值建立汇合PHI，否则直接复用公共值。
Value *StoreMergePass::mergeIncomingValues(
    BasicBlock *Join, const std::vector<BasicBlock *> &Preds,
    const std::vector<Value *> &Values) {
	//3.1 公共SSA值无需新PHI；路径值不同时由标准PHI按控制流边精确选择。
	if (!Join || Preds.size() < 2 || Preds.size() != Values.size() ||
		!Values.front() || !Values.front()->getIRType()) {
		return nullptr;
	}
	bool AllSame = true;
	for (Value *ValueOnPath : Values) {
		if (!ValueOnPath || !ValueOnPath->getIRType() ||
			!IRType::hasSameShape(ValueOnPath->getIRType(),
			                      Values.front()->getIRType())) {
			return nullptr;
		}
		if (ValueOnPath != Values.front()) AllSame = false;
	}
	if (AllSame) return Values.front();
	auto *Phi = new PhiInst(Values.front()->getIRType());
	for (std::size_t Index = 0; Index < Preds.size(); ++Index)
		Phi->addIncoming(Values[Index], Preds[Index]);
	Join->pushFront(Phi);
	return Phi;
}
//提交单个候选的SSA与内存改写。
bool StoreMergePass::applyCandidate(
    const StoreMergeCandidate &Candidate) {
	//3.候选已完整验证，先按稳定前驱顺序收集地址和值，再建立合流节点。
	BasicBlock *Join = Candidate.Join;
	if (!Join || Candidate.Stores.size() < 2) return false;
	auto Preds = Join->getPredecessors();
	if (Preds.size() != Candidate.Stores.size()) return false;
	std::vector<Value *> Values;
	std::vector<Value *> Pointers;
	Values.reserve(Candidate.Stores.size());
	Pointers.reserve(Candidate.Stores.size());
	for (std::size_t Index = 0; Index < Candidate.Stores.size(); ++Index) {
		StoreInst *Store = Candidate.Stores[Index];
		if (!Store || Store->getParent() != Preds[Index] ||
			getTailStore(Preds[Index], Join) != Store) {
			return false;
		}
		Values.push_back(Store->getValueOperand());
		Pointers.push_back(Store->getPointerOperand());
	}
	Value *MergedValue = mergeIncomingValues(Join, Preds, Values);
	Value *MergedPointer = mergeIncomingValues(Join, Preds, Pointers);
	if (!MergedValue || !MergedPointer) return false;
	//3.2 PHI不执行内存访问，因此把store放到全部PHI之后与原来的边尾store等价。
	for (StoreInst *Store : Candidate.Stores) delete Store;
	auto *MergedStore = new StoreInst(MergedValue, MergedPointer);
	if (!Join->insertAfterPhiNodes(MergedStore)) {
		delete MergedStore;
		return false;
	}
	return true;
}
//迭代合并函数中全部控制流汇合点的公共store后缀。
PreservationStatus StoreMergePass::run(
    Function &F, FunctionAnalysisPassManager &FAM) {
	(void)FAM;
	//1—4.每次改写一个候选后重新快照CFG；重复执行可按原顺序合并多条公共store后缀。
	bool ChangedAny = false;
	while (true) {
		bool Changed = false;
		for (BasicBlock *Join : snapshotPointers(F)) {
			StoreMergeCandidate Candidate;
			if (!matchCandidate(Join, Candidate)) continue;
			if (!applyCandidate(Candidate)) continue;
			Changed = true;
			ChangedAny = true;
			break;
		}
		if (!Changed) break;
	}
	//5.控制流未变，但SSA值图与内存定义位置均已变化，保守失效函数分析。
	if (!ChangedAny) return PreservationStatus::all();
	return PreservationStatus::none();
}
