#pragma once

#include "lib/CFG.hpp"
#include "ir/analysis/LoopInfo.hpp"

#include <algorithm>
#include <unordered_set>
#include <vector>

//用于支持中端各种遍历对象和遍历方法的工具库

namespace TraverseTools {

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
