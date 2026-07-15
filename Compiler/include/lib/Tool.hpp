#pragma once
#include <algorithm>
#include <list>
#include <memory>
#include <vector>
#include <cstddef>
#include <type_traits>

// 前置声明
class Function;
class BasicBlock;
class Instruction;

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
