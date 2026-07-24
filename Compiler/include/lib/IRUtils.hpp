// 本文件分为不依赖完整 CFG 的基础部分和遍历工具部分。
// CFG.hpp 通过 COMPILER_LIB_IRUTILS_BASE_ONLY 只引入前者，以打破
// ChainList/ChainNode 与 CFG 遍历模板之间的循环包含。
#ifndef COMPILER_LIB_IRUTILS_BASE_HPP
#define COMPILER_LIB_IRUTILS_BASE_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <iterator>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class BasicBlock;
class Function;
class Instruction;
class IRType;
class Loop;
class Value;

template <typename OwnerTy, typename ItemTy>
class ChainList;

template <typename OwnerTy, typename ItemTy>
class ChainNode {
    friend class ChainList<OwnerTy, ItemTy>;

private:
    ItemTy* prev = nullptr;
    ItemTy* next = nullptr;
    OwnerTy* owner = nullptr;

public:
    ChainNode() : prev(nullptr), next(nullptr) {}

    // 节点析构时自动解除其与所属链表的链接。
    ~ChainNode() {
        if (!owner) return;
        eraseFromParent();
    }

    // 节点与特定链表绑定，因此禁止拷贝和移动。
    ChainNode(const ChainNode&) = delete;
    ChainNode(ChainNode&&) = delete;
    ChainNode& operator=(const ChainNode&) = delete;
    ChainNode& operator=(ChainNode&&) = delete;

    // 更新节点所属的链表。
    void setParent(OwnerTy* owner) {
        this->owner = owner;
    }

    // 以常数时间解除节点与所属链表的链接。
    void eraseFromParentImpl() {
        if (!owner) return;

        auto* currentOwner = this->owner;

        if (prev) {
            prev->next = next;
        } else {
            currentOwner->first = next;
        }

        if (next) {
            next->prev = prev;
        } else {
            currentOwner->last = prev;
        }

        currentOwner->count--;
        owner = nullptr;
        prev = nullptr;
        next = nullptr;
    }

    // 从所属链表移除此节点。
    void eraseFromParent() {
        eraseFromParentImpl();
    }

    // 用新节点替换当前位置，并解除当前节点的链接。
    void replaceWith(ItemTy* newNode) {
        if (!newNode || !owner) return;

        newNode->owner = this->owner;
        newNode->prev = this->prev;
        newNode->next = this->next;

        if (!prev) {
            owner->first = newNode;
        } else {
            prev->next = newNode;
        }

        if (!next) {
            owner->last = newNode;
        } else {
            next->prev = newNode;
        }

        this->prev = nullptr;
        this->next = nullptr;
        this->owner = nullptr;
    }

    // 返回节点所属的链表。
    OwnerTy* getParent() const {
        return owner;
    }

    // 返回相邻节点。
    ItemTy* getNext() const { return next; }
    ItemTy* getPrev() const { return prev; }
};


template <typename ListOwner, typename ListItem>
class ChainList {
    friend class ChainNode<ListOwner, ListItem>;

public:
    ListItem* first = nullptr;
    ListItem* last = nullptr;
    int count = 0;

    ChainList() = default;

    // 链表直接持有节点链接，因此禁止拷贝和移动。
    ChainList(const ChainList&) = delete;
    ChainList(ChainList&&) = delete;
    ChainList& operator=(const ChainList&) = delete;
    ChainList& operator=(ChainList&&) = delete;

    // 析构时释放链表中的全部节点。
    ~ChainList() {
        clear();
    }


    class Iterator {
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = ListItem*;
        using difference_type = std::ptrdiff_t;
        using pointer = ListItem**;
        using reference = ListItem*&;

        ListItem* pos;

    public:
        Iterator() : pos(nullptr) {}
        explicit Iterator(ListItem* ptr) : pos(ptr) {}

        // 移动到后继节点。
        Iterator& operator++() {
            if (pos) pos = pos->next;
            return *this;
        }

        Iterator operator++(int) {
            Iterator tmp = *this;
            ++(*this);
            return tmp;
        }

        // 移动到前驱节点。
        Iterator& operator--() {
            if (pos) pos = pos->prev;
            return *this;
        }

        Iterator operator--(int) {
            Iterator tmp = *this;
            --(*this);
            return tmp;
        }

        value_type operator*() const { return pos; }
        bool operator==(const Iterator& rhs) const { return pos == rhs.pos; }
        bool operator!=(const Iterator& rhs) const { return pos != rhs.pos; }

        // 在当前位置之前插入节点，参数无效时返回空迭代器。
        Iterator insertBefore(ListItem* newNode) {
            if (!pos || !pos->owner || !newNode) return Iterator(nullptr);
            ListOwner* owner = pos->owner;

            if (pos == owner->first) {
                owner->pushFront(newNode);
            } else {
                newNode->setParent(owner);
                auto* prevItem = pos->prev;

                newNode->prev = prevItem;
                newNode->next = pos;

                prevItem->next = newNode;
                pos->prev = newNode;

                owner->count++;
            }
            return Iterator(newNode);
        }

        // 在当前位置之后插入节点，参数无效时返回空迭代器。
        Iterator insertAfter(ListItem* newNode) {
            if (!pos || !pos->owner || !newNode) return Iterator(nullptr);
            ListOwner* owner = pos->owner;

            if (pos == owner->last) {
                owner->pushBack(newNode);
            } else {
                newNode->setParent(owner);
                auto* nextItem = pos->next;

                newNode->next = nextItem;
                newNode->prev = pos;

                pos->next = newNode;
                nextItem->prev = newNode;

                owner->count++;
            }
            return Iterator(newNode);
        }
    };

    // 返回正向或反向遍历的边界迭代器。
    Iterator begin() { return Iterator(first); }
    Iterator begin() const { return Iterator(first); }
    Iterator end() { return Iterator(nullptr); }
    Iterator end() const { return Iterator(nullptr); }
    Iterator rbegin() { return Iterator(last); }
    Iterator rbegin() const { return Iterator(last); }
    Iterator rend() { return Iterator(nullptr); }
    Iterator rend() const { return Iterator(nullptr); }

    // 将现有节点链连接到空列表，并更新各节点的所属关系。
    void connectSequence(ListItem* startNode, ListItem* endNode) {
        if (first || last) return;

        first = startNode;
        last = endNode;
        count = 0;

        ListItem* walker = first;
        while (walker != nullptr) {
            walker->setParent(static_cast<ListOwner*>(this));
            count++;
            walker = walker->next;
        }
    }

    // 提取闭区间节点链，参数无效时返回一对空指针。
    std::pair<ListItem*, ListItem*> extract(ListItem* startNode, ListItem* endNode) {
        auto* selfPtr = static_cast<ListOwner*>(this);
        if (!startNode || !endNode || startNode->owner != selfPtr ||
            endNode->owner != selfPtr) {
            return {nullptr, nullptr};
        }

        // 确认结束节点可达并统计待提取的节点数。
        int removedCount = 0;
        bool reachesEnd = false;
        for (ListItem* walker = startNode; walker != nullptr; walker = walker->next) {
            removedCount++;
            if (walker == endNode) {
                reachesEnd = true;
                break;
            }
        }
        if (!reachesEnd) return {nullptr, nullptr};

        ListItem* preStart = startNode->prev;
        ListItem* postEnd = endNode->next;

        // 连接提取区间两侧的节点。
        if (startNode == first) {
            first = postEnd;
        } else if (preStart) {
            preStart->next = postEnd;
        }

        if (endNode == last) {
            last = preStart;
        } else if (postEnd) {
            postEnd->prev = preStart;
        }

        // 清除提取链的外部链接并更新节点数。
        startNode->prev = nullptr;
        endNode->next = nullptr;
        count -= removedCount;

        return {startNode, endNode};
    }

    // 按照 extract 的语义分割节点链。
    std::pair<ListItem*, ListItem*> split(ListItem* startNode, ListItem* endNode) {
        return extract(startNode, endNode);
    }

    // 以常数时间返回节点数。
    int size() const { return count; }

    bool empty() const { return count == 0; }

    // 在链表尾部添加节点。
    void pushBack(ListItem* newNode) {
        newNode->setParent(static_cast<ListOwner*>(this));

        if (!first) {
            first = newNode;
            last = newNode;
        } else {
            last->next = newNode;
            newNode->prev = last;
            last = newNode;
        }
        count++;
    }

    // 在链表头部添加节点。
    void pushFront(ListItem* newNode) {
        newNode->setParent(static_cast<ListOwner*>(this));

        if (!first) {
            first = newNode;
            last = newNode;
        } else {
            first->prev = newNode;
            newNode->next = first;
            first = newNode;
        }
        count++;
    }

    // 按照给定顺序重新连接起止位置之间的节点。
    void rearrangeItems(ListItem* start, ListItem* end, std::list<ListItem*>& items) {
        auto* selfPtr = static_cast<ListOwner*>(this);
        // end 为空表示重排区间一直延伸到链表末尾。
        if (!start || items.empty() || start->owner != selfPtr ||
            (end && end->owner != selfPtr)) {
            return;
        }

        // 修改链接前验证所有节点均属于当前链表。
        for (auto* item : items) {
            if (!item || item->getParent() != selfPtr) return;
        }

        auto* linkPrev = start->prev;
        auto* linkNext = end;

        if (!linkPrev) {
            first = items.front();
        }

        // 按照输入顺序连接节点。
        ListItem* prevSlot = linkPrev;
        for (auto* item : items) {
            item->prev = prevSlot;
            if (prevSlot) {
                prevSlot->next = item;
            }
            prevSlot = item;
        }

        ListItem* lastItem = items.back();
        lastItem->next = linkNext;

        if (linkNext) {
            linkNext->prev = lastItem;
        }
    }

    // 删除链表中的全部节点。
    void clear() {
        while (first) {
            ListItem* temp = first;
            first = first->next;
            if (temp) delete temp;
        }
        last = nullptr;
        count = 0;
    }

    ListItem* front() { return first; }
    const ListItem* front() const { return first; }
    ListItem* back() { return last; }
    const ListItem* back() const { return last; }
};

using ValueToValueMap = std::unordered_map<Value *, Value *>;

// 控制克隆块的命名方式，并可清除克隆调用上的尾调用标记。
struct CloneBlockRegionOptions {
    std::function<std::string(const BasicBlock *, std::size_t)>
        MakeBlockIdent;
    bool ClearTailCallFlags = false;
};

// 保存源块到克隆块、源指令到克隆指令的完整对应关系。
struct ClonedBlockRegion {
    ValueToValueMap VMap;
    std::vector<BasicBlock *> SourceBlocks;
    std::vector<BasicBlock *> ClonedBlocks;
    std::vector<std::pair<Instruction *, Instruction *>> InstPairs;

    BasicBlock *getClonedBlock(const BasicBlock *Source) const;
    Instruction *getClonedInstruction(const Instruction *Source) const;
};

// 事务性克隆同一函数中的块区域；任一步失败都会回滚，区域外出口保持原目标。
// InitialMap 可预置外部值映射，Options 可为新块命名并调整调用标记。
std::optional<ClonedBlockRegion> cloneBasicBlockRegion(
    Function &Destination,
    const std::vector<BasicBlock *> &SourceBlocks,
    const ValueToValueMap &InitialMap = {},
    const CloneBlockRegionOptions &Options = {});

// 提供优化过程中常用的定宽整数运算，失败返回值表示溢出或参数无效。
class IRMath {
public:
    static bool isPowerOf2(int64_t Val);
    static bool isUnsignedPowerOf2(uint64_t Val);
    static int log2u(uint64_t Val);
    static int log2i(int64_t Val);
    static bool addI32(int64_t A, int64_t B, int64_t &Out);
    static bool addNoOverflow(int64_t A, int64_t B, int64_t &Out);
    static int64_t ceilDivPositive(int64_t Num, int64_t Den);
    static int64_t normalizeSignedToWidth(int64_t Val, unsigned Bits);
};

// 为分析缓存构造进程内稳定键；结果只在本次编译进程中有效。
class IRKey {
public:
    static std::string pointer(const void *Ptr);
    static std::string type(const std::shared_ptr<IRType> &Ty);
};

#endif // COMPILER_LIB_IRUTILS_BASE_HPP

#ifndef COMPILER_LIB_IRUTILS_BASE_ONLY
#ifndef COMPILER_LIB_IRUTILS_TRAVERSAL_HPP
#define COMPILER_LIB_IRUTILS_TRAVERSAL_HPP

#include "CFG.hpp"

// 提供中端所需的工作队列、作用域标记和通用图遍历算法。

enum class TraversalControl {
	Continue,
	SkipSuccessors,
	Stop,
};

enum class WorkListOrder {
	FIFO,
	LIFO,
};

// 只对当前仍在队列中的元素去重；元素出队后可以再次入队。
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
// 它不是全局已访问集合，遇环时的处理结果仍由调用方决定。
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

// 回调返回 void 时完整遍历，返回可转换为 bool 的类型时 false 表示提前停止。
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

// 对侵入式链表做结构性修改前使用；快照保持原顺序，也保留空指针。
template <typename RangeT>
auto snapshotPointers(RangeT &Range) {
	using PointerT = std::decay_t<decltype(*std::begin(Range))>;
	static_assert(std::is_pointer_v<PointerT>,
	              "snapshotPointers 仅适用于裸指针范围");
	std::vector<PointerT> Result;
	for (PointerT Pointer : Range) Result.push_back(Pointer);
	return Result;
}

// 按模块、函数、基本块、指令的现有线性顺序访问。
// 回调可接收 (InstT *) 或 (BasicBlock *, InstT *)。遍历期间可以替换操作数，
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

// 对调用方选定并排序的块集合做线性指令遍历；循环逆后序、函数顺序、
// 含/不含子循环等语义由 BlockRange 明确决定，本接口不会替调用方改序或去重。
template <typename InstT = Instruction, typename BlockRangeT, typename VisitorT>
bool forEachInstructionInBlocks(const BlockRangeT &Blocks, VisitorT &&Visitor) {
	for (BasicBlock *BB : Blocks) {
		if (!BB) continue;
		if (!forEachInstruction<InstT>(*BB, Visitor)) return false;
	}
	return true;
}

// 保持操作数索引顺序；回调接收 (Index, Use *, Value *)。
// 遍历期间允许替换被使用值，但不得增删操作数。
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

// 保持 PHI 入边项的顺序和重复前驱边；回调接收
// (Index, IncomingValue, IncomingBlock)。重建 PHI 时仍应先收集入边快照。
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
// SuccessorProvider 为不同遍历对象提供“后继”定义。
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
	template <typename LoopT = Loop>
	static std::vector<Loop *> get(Loop *L) {
		if (!L) return {};
		const auto &Subs = static_cast<LoopT *>(L)->getSubLoops();
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

// 弹栈时去重，等价于显式深度优先先序遍历。
// 回调返回 SkipSuccessors 可剪枝，返回 Stop 或 false 可提前终止。
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



// 第三层：公共接口层。
// 带 AllNodes 的接口会在根可达序列之后补入尚未访问的节点。
// 名称带 FromRoots 的接口只处理根可达区域，RPO 接口返回后序的逆序结果。
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

#endif // COMPILER_LIB_IRUTILS_TRAVERSAL_HPP
#endif // COMPILER_LIB_IRUTILS_BASE_ONLY
