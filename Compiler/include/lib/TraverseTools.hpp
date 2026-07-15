#pragma once

#include "lib/CFG.hpp"
#include "ir/analysis/LoopInfo.hpp"

#include <algorithm>
#include <deque>
#include <functional>
#include <iterator>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

//用于支持中端各种遍历对象和遍历方法的工具库

namespace TraverseTools {

enum class TraversalControl {
	Continue,
	SkipSuccessors,
	Stop,
};

enum class WorkListOrder {
	FIFO,
	LIFO,
};

// 只对“当前仍在队列中的元素”去重；pop 后同一元素可以再次入队。
// 这与 SCCP/Range 中允许重复调度的队列语义不同，不能用于替换它们。
template <typename ValueT, WorkListOrder Order = WorkListOrder::FIFO,
	      typename HashT = std::hash<ValueT>,
	      typename EqualT = std::equal_to<ValueT>>
class UniqueWorkList {
	std::deque<ValueT> Items;
	std::unordered_set<ValueT, HashT, EqualT> Pending;

public:
	bool push(ValueT Value) {
		if (!Pending.insert(Value).second) return false;
		Items.push_back(std::move(Value));
		return true;
	}

	bool empty() const { return Items.empty(); }
	std::size_t size() const { return Items.size(); }
	bool contains(const ValueT &Value) const {
		return Pending.count(Value) != 0;
	}

	ValueT pop() {
		if constexpr (Order == WorkListOrder::FIFO) {
			ValueT Value = std::move(Items.front());
			Items.pop_front();
			Pending.erase(Value);
			return Value;
		} else {
			ValueT Value = std::move(Items.back());
			Items.pop_back();
			Pending.erase(Value);
			return Value;
		}
	}

	void clear() {
		Items.clear();
		Pending.clear();
	}
};

// 递归证明只在“当前递归路径”上标记节点；所有提前返回都会自动撤销标记。
// 它不是全局 visited 集，遇环时应返回什么仍由调用方决定。
template <typename SetT>
class ScopedActiveMark {
	SetT *ActiveSet;
	typename SetT::value_type Value;
	bool Inserted;

public:
	ScopedActiveMark(SetT &Set, typename SetT::value_type Current)
		: ActiveSet(&Set), Value(std::move(Current)),
		  Inserted(ActiveSet->insert(Value).second) {}

	ScopedActiveMark(const ScopedActiveMark &) = delete;
	ScopedActiveMark &operator=(const ScopedActiveMark &) = delete;
	ScopedActiveMark(ScopedActiveMark &&) = delete;
	ScopedActiveMark &operator=(ScopedActiveMark &&) = delete;

	~ScopedActiveMark() {
		if (Inserted) ActiveSet->erase(Value);
	}

	bool inserted() const { return Inserted; }
};

template <typename SetT>
ScopedActiveMark(SetT &, typename SetT::value_type) -> ScopedActiveMark<SetT>;

// Visitor 返回 void 时完整遍历，返回可转换为 bool 的类型时 false 表示提前停止。
template <typename VisitorT, typename... ArgT>
bool invokeTraversalVisitor(VisitorT &Visitor, ArgT &&...Args) {
	static_assert(std::is_invocable_v<VisitorT &, ArgT...>,
	              "Visitor 参数与遍历接口不匹配");
	using ResultT = std::invoke_result_t<VisitorT &, ArgT...>;
	if constexpr (std::is_void_v<ResultT>) {
		std::invoke(Visitor, std::forward<ArgT>(Args)...);
		return true;
	} else {
		static_assert(std::is_convertible_v<ResultT, bool>,
		              "Visitor 必须返回 void 或可转换为 bool 的类型");
		return static_cast<bool>(
			std::invoke(Visitor, std::forward<ArgT>(Args)...));
	}
}

template <typename VisitorT, typename... ArgT>
TraversalControl invokeTraversalControlVisitor(VisitorT &Visitor,
	                                            ArgT &&...Args) {
	static_assert(std::is_invocable_v<VisitorT &, ArgT...>,
	              "Visitor 参数与遍历接口不匹配");
	using ResultT = std::invoke_result_t<VisitorT &, ArgT...>;
	if constexpr (std::is_void_v<ResultT>) {
		std::invoke(Visitor, std::forward<ArgT>(Args)...);
		return TraversalControl::Continue;
	} else if constexpr (
		std::is_same_v<std::decay_t<ResultT>, TraversalControl>) {
		return std::invoke(Visitor, std::forward<ArgT>(Args)...);
	} else {
		static_assert(std::is_convertible_v<ResultT, bool>,
		              "Visitor 必须返回 void、bool 或 TraversalControl");
		return static_cast<bool>(
			std::invoke(Visitor, std::forward<ArgT>(Args)...))
			       ? TraversalControl::Continue
			       : TraversalControl::Stop;
	}
}

// 对 intrusive list 做结构性修改前使用。快照保持原顺序，也保留空指针。
template <typename RangeT>
auto snapshotPointers(RangeT &Range) {
	using PointerT = std::decay_t<decltype(*std::begin(Range))>;
	static_assert(std::is_pointer_v<PointerT>,
	              "snapshotPointers 仅适用于裸指针范围");
	std::vector<PointerT> Result;
	for (PointerT Pointer : Range) Result.push_back(Pointer);
	return Result;
}

// 按 Module -> Function -> BasicBlock -> Instruction 的现有线性顺序访问。
// Visitor 可接收 (InstT *) 或 (BasicBlock *, InstT *)。遍历期间可以替换操作数，
// 但不得插入、删除或移动正在遍历的指令；这类变更应先用 collectInstructions 快照。
template <typename InstT = Instruction, typename IRUnitT, typename VisitorT>
bool forEachInstruction(IRUnitT &IRUnit, VisitorT &&Visitor) {
	static_assert(std::is_base_of_v<Instruction, InstT>,
	              "InstT 必须派生自 Instruction");
	using UnitT = std::remove_cv_t<std::remove_reference_t<IRUnitT>>;
	static_assert(std::is_same_v<UnitT, BasicBlock> ||
	                  std::is_same_v<UnitT, Function> ||
	                  std::is_same_v<UnitT, Module>,
	              "IRUnitT 必须是 BasicBlock、Function 或 Module");
	static_assert(!std::is_const_v<std::remove_reference_t<IRUnitT>>,
	              "forEachInstruction 当前仅访问可变 Instruction 指针");

	auto VisitBlock = [&Visitor](BasicBlock *BB) {
		if (!BB) return true;
		for (Instruction *I : *BB) {
			if (!I) continue;
			InstT *Typed = nullptr;
			if constexpr (std::is_same_v<InstT, Instruction>) {
				Typed = I;
			} else {
				Typed = dynamic_cast<InstT *>(I);
			}
			if (!Typed) continue;

			if constexpr (std::is_invocable_v<VisitorT &, BasicBlock *, InstT *>) {
				if (!invokeTraversalVisitor(Visitor, BB, Typed)) return false;
			} else {
				static_assert(std::is_invocable_v<VisitorT &, InstT *>,
				              "Visitor 必须接收 (InstT *) 或 (BasicBlock *, InstT *)");
				if (!invokeTraversalVisitor(Visitor, Typed)) return false;
			}
		}
		return true;
	};

	if constexpr (std::is_same_v<UnitT, BasicBlock>) {
		return VisitBlock(&IRUnit);
	} else if constexpr (std::is_same_v<UnitT, Function>) {
		for (BasicBlock *BB : IRUnit)
			if (!VisitBlock(BB)) return false;
	} else {
		for (const auto &FunctionPtr : IRUnit.getFunctions()) {
			Function *F = FunctionPtr.get();
			if (!F) continue;
			for (BasicBlock *BB : *F)
				if (!VisitBlock(BB)) return false;
		}
	}
	return true;
}

// 对调用方选定并排序的块集合做线性指令遍历；Loop 的 RPO、函数顺序、
// 含/不含子循环等语义由 BlockRange 明确决定，本接口不会替调用方改序或去重。
template <typename InstT = Instruction, typename BlockRangeT, typename VisitorT>
bool forEachInstructionInBlocks(const BlockRangeT &Blocks, VisitorT &&Visitor) {
	for (BasicBlock *BB : Blocks) {
		if (!BB) continue;
		if (!forEachInstruction<InstT>(*BB, Visitor)) return false;
	}
	return true;
}

// 保持 operand 索引顺序。Visitor 接收 (Index, Use *, Value *)。
// 遍历期间允许替换 usee，但不得增删 operand。
template <typename VisitorT>
bool forEachOperand(User *U, VisitorT &&Visitor) {
	if (!U) return true;
	const size_t NumOperands = U->getNumOperands();
	for (size_t Index = 0; Index < NumOperands; ++Index) {
		if (!invokeTraversalVisitor(Visitor, Index,
		                            U->getOperandUse(Index),
		                            U->getOperand(Index)))
			return false;
	}
	return true;
}

// 保持 PHI incoming 的顺序和重复前驱边。Visitor 接收
// (Index, IncomingValue, IncomingBlock)。重建 PHI 时仍应先 collectIncomingEdges。
template <typename VisitorT>
bool forEachPhiIncoming(PhiInst *Phi, VisitorT &&Visitor) {
	if (!Phi) return true;
	const unsigned NumIncoming = Phi->getNumIncomingValues();
	for (unsigned Index = 0; Index < NumIncoming; ++Index) {
		if (!invokeTraversalVisitor(Visitor, Index,
		                            Phi->getIncomingValue(Index),
		                            Phi->getIncomingBlock(Index)))
			return false;
	}
	return true;
}

// 第一层：模板特化层
// SuccessorProvider：提供不同遍历对象的“后继”定义
// 若后续新增遍历对象，只需补充对应特化
template <typename NodeT>
struct SuccessorProvider;

template <>
struct SuccessorProvider<BasicBlock> {
	static std::vector<BasicBlock *> get(BasicBlock *BB) {
		return BB ? BB->getSuccessors() : std::vector<BasicBlock *>{};
	}
};

template <>
struct SuccessorProvider<Loop> {
	static std::vector<Loop *> get(Loop *L) {
		if (!L) return {};
		const auto &Subs = L->getSubLoops();
		return std::vector<Loop *>(Subs.begin(), Subs.end());
	}
};

// 第二层：核心算法层（与具体遍历对象解耦）
template <typename NodeT, typename SuccessorFn>
void appendPostOrderIterative(NodeT *Start,
	                          std::unordered_set<NodeT *> &Vis,
	                          std::vector<NodeT *> &PostOrder,
	                          SuccessorFn &GetSuccessors) {
	if (!Start) return;
	if (!Vis.insert(Start).second) return;

	struct Frame {
		NodeT *Node;
		bool Expanded;
	};

	std::vector<Frame> Stack;
	Stack.push_back({Start, false});

	while (!Stack.empty()) {
		Frame Cur = Stack.back();
		Stack.pop_back();

		if (Cur.Expanded) {
			PostOrder.push_back(Cur.Node);
			continue;
		}

		Stack.push_back({Cur.Node, true});
		auto Succs = GetSuccessors(Cur.Node);
		for (auto It = Succs.rbegin(); It != Succs.rend(); ++It) {
			NodeT *Succ = *It;
			if (!Succ) continue;
			if (Vis.insert(Succ).second) Stack.push_back({Succ, false});
		}
	}
}

template <typename NodeT, typename SuccessorFn>
void appendPreOrderIterative(NodeT *Start,
	                         std::unordered_set<NodeT *> &Vis,
	                         std::vector<NodeT *> &PreOrder,
	                         SuccessorFn &GetSuccessors) {
	if (!Start || Vis.count(Start)) return;

	std::vector<NodeT *> Stack{Start};
	while (!Stack.empty()) {
		NodeT *Current = Stack.back();
		Stack.pop_back();
		if (!Current || !Vis.insert(Current).second) continue;
		PreOrder.push_back(Current);

		auto Succs = GetSuccessors(Current);
		for (auto It = Succs.rbegin(); It != Succs.rend(); ++It) {
			NodeT *Succ = *It;
			if (Succ && !Vis.count(Succ)) Stack.push_back(Succ);
		}
	}
}

// 弹栈时去重，等价于中端常用的显式 DFS preorder 写法。
// Visitor 返回 SkipSuccessors 可剪枝，返回 Stop（或 bool false）可提前终止。
template <typename NodeT, typename RootRangeT, typename SuccessorFn,
	      typename VisitorT>
bool forEachReachablePreOrderCustomSucc(
	const RootRangeT &Roots,
	SuccessorFn &&GetSuccessors,
	VisitorT &&Visitor) {
	std::unordered_set<NodeT *> Vis;
	std::vector<NodeT *> Stack;
	for (auto It = std::rbegin(Roots); It != std::rend(Roots); ++It)
		if (*It) Stack.push_back(*It);

	while (!Stack.empty()) {
		NodeT *Current = Stack.back();
		Stack.pop_back();
		if (!Current || !Vis.insert(Current).second) continue;

		TraversalControl Control =
			invokeTraversalControlVisitor(Visitor, Current);
		if (Control == TraversalControl::Stop) return false;
		if (Control == TraversalControl::SkipSuccessors) continue;

		auto Succs = GetSuccessors(Current);
		for (auto It = Succs.rbegin(); It != Succs.rend(); ++It)
			if (*It && !Vis.count(*It)) Stack.push_back(*It);
	}
	return true;
}

template <typename NodeT, typename RootRangeT, typename AllRangeT,
	      typename SuccessorFn>
std::vector<NodeT *> buildPreOrderCustomSucc(
	const RootRangeT &Roots,
	const AllRangeT &AllNodes,
	SuccessorFn &&GetSuccessors) {
	std::vector<NodeT *> PreOrder;
	std::unordered_set<NodeT *> Vis;

	for (NodeT *Root : Roots)
		appendPreOrderIterative(Root, Vis, PreOrder, GetSuccessors);
	for (NodeT *Node : AllNodes)
		appendPreOrderIterative(Node, Vis, PreOrder, GetSuccessors);
	return PreOrder;
}

template <typename NodeT, typename RootRangeT, typename SuccessorFn>
std::vector<NodeT *> buildPreOrderFromRootsCustomSucc(
	const RootRangeT &Roots,
	SuccessorFn &&GetSuccessors) {
	std::vector<NodeT *> PreOrder;
	std::unordered_set<NodeT *> Vis;
	for (NodeT *Root : Roots)
		appendPreOrderIterative(Root, Vis, PreOrder, GetSuccessors);
	return PreOrder;
}

template <typename NodeT,
	      WorkListOrder Order = WorkListOrder::LIFO,
	      typename RootRangeT, typename SuccessorFn>
std::unordered_set<NodeT *> collectReachableFromRootsCustomSucc(
	const RootRangeT &Roots,
	SuccessorFn &&GetSuccessors) {
	if constexpr (Order == WorkListOrder::FIFO) {
		std::unordered_set<NodeT *> Reachable;
		std::deque<NodeT *> Queue;
		for (NodeT *Root : Roots) {
			if (Root && Reachable.insert(Root).second)
				Queue.push_back(Root);
		}
		while (!Queue.empty()) {
			NodeT *Current = Queue.front();
			Queue.pop_front();
			auto Successors = GetSuccessors(Current);
			for (NodeT *Successor : Successors) {
				if (Successor && Reachable.insert(Successor).second)
					Queue.push_back(Successor);
			}
		}
		return Reachable;
	} else {
		auto PreOrder = buildPreOrderFromRootsCustomSucc<NodeT>(
			Roots, std::forward<SuccessorFn>(GetSuccessors));
		std::unordered_set<NodeT *> Reachable;
		for (NodeT *Node : PreOrder) Reachable.insert(Node);
		return Reachable;
	}
}

template <typename NodeT, typename RootRangeT, typename SuccessorFn>
bool isReachableFromRootsCustomSucc(
	const RootRangeT &Roots,
	NodeT *Target,
	SuccessorFn &&GetSuccessors) {
	if (!Target) return false;
	bool Found = false;
	forEachReachablePreOrderCustomSucc<NodeT>(
		Roots,
		std::forward<SuccessorFn>(GetSuccessors),
		[Target, &Found](NodeT *Current) {
			if (Current != Target) return true;
			Found = true;
			return false;
		});
	return Found;
}

template <typename NodeT, typename RootRangeT, typename AllRangeT, typename SuccessorFn>
std::vector<NodeT *> buildPostOrderCustomSucc(
	const RootRangeT &Roots,
	const AllRangeT &AllNodes,
	SuccessorFn &&GetSuccessors) {
	std::vector<NodeT *> PostOrder;
	std::unordered_set<NodeT *> Vis;

	for (NodeT *Root : Roots) appendPostOrderIterative(Root, Vis, PostOrder, GetSuccessors);
	for (NodeT *Node : AllNodes) appendPostOrderIterative(Node, Vis, PostOrder, GetSuccessors);

	return PostOrder;
}

template <typename NodeT, typename RootRangeT, typename SuccessorFn>
std::vector<NodeT *> buildPostOrderFromRootsCustomSucc(
	const RootRangeT &Roots,
	SuccessorFn &&GetSuccessors) {
	std::vector<NodeT *> PostOrder;
	std::unordered_set<NodeT *> Vis;

	for (NodeT *Root : Roots) appendPostOrderIterative(Root, Vis, PostOrder, GetSuccessors);
	return PostOrder;
}

template <typename NodeT>
std::vector<NodeT *> reversePostOrder(std::vector<NodeT *> PostOrder) {
	std::reverse(PostOrder.begin(), PostOrder.end());
	return PostOrder;
}



// 第三层：接口层
template <typename NodeT, typename RootRangeT, typename AllRangeT>
std::vector<NodeT *> buildPostOrder(const RootRangeT &Roots, const AllRangeT &AllNodes) {
	return buildPostOrderCustomSucc<NodeT>(
		Roots,
		AllNodes,
		[](NodeT *Node) { return SuccessorProvider<NodeT>::get(Node); });
}

template <typename NodeT, typename RootRangeT, typename AllRangeT>
std::vector<NodeT *> buildPreOrder(const RootRangeT &Roots,
	                               const AllRangeT &AllNodes) {
	return buildPreOrderCustomSucc<NodeT>(
		Roots,
		AllNodes,
		[](NodeT *Node) { return SuccessorProvider<NodeT>::get(Node); });
}

template <typename NodeT, typename RootRangeT>
std::vector<NodeT *> buildPreOrderFromRoots(const RootRangeT &Roots) {
	return buildPreOrderFromRootsCustomSucc<NodeT>(
		Roots,
		[](NodeT *Node) { return SuccessorProvider<NodeT>::get(Node); });
}

template <typename NodeT, typename RootRangeT, typename VisitorT>
bool forEachReachablePreOrder(const RootRangeT &Roots, VisitorT &&Visitor) {
	return forEachReachablePreOrderCustomSucc<NodeT>(
		Roots,
		[](NodeT *Node) { return SuccessorProvider<NodeT>::get(Node); },
		std::forward<VisitorT>(Visitor));
}

template <typename NodeT,
	      WorkListOrder Order = WorkListOrder::LIFO,
	      typename RootRangeT>
std::unordered_set<NodeT *> collectReachableFromRoots(const RootRangeT &Roots) {
	return collectReachableFromRootsCustomSucc<NodeT, Order>(
		Roots,
		[](NodeT *Node) { return SuccessorProvider<NodeT>::get(Node); });
}

template <typename NodeT, typename RootRangeT>
bool isReachableFromRoots(const RootRangeT &Roots, NodeT *Target) {
	return isReachableFromRootsCustomSucc<NodeT>(
		Roots,
		Target,
		[](NodeT *Node) { return SuccessorProvider<NodeT>::get(Node); });
}

template <typename NodeT, typename RootRangeT>
std::vector<NodeT *> buildPostOrderFromRoots(const RootRangeT &Roots) {
	return buildPostOrderFromRootsCustomSucc<NodeT>(
		Roots,
		[](NodeT *Node) { return SuccessorProvider<NodeT>::get(Node); });
}

template <typename NodeT, typename RootRangeT, typename AllRangeT>
std::vector<NodeT *> buildRPO(const RootRangeT &Roots, const AllRangeT &AllNodes) {
	return reversePostOrder(buildPostOrder<NodeT>(Roots, AllNodes));
}

template <typename NodeT, typename RootRangeT>
std::vector<NodeT *> buildRPOFromRoots(const RootRangeT &Roots) {
	return reversePostOrder(buildPostOrderFromRoots<NodeT>(Roots));
}

}
