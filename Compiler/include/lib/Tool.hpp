#pragma once
#include <algorithm>
#include <cassert>
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
    ItemTy* prev_ = nullptr;
    ItemTy* next_ = nullptr;
    OwnerTy* owner_ = nullptr;

public:
    ChainNode() : prev_(nullptr), next_(nullptr) {}

    // 非虚析构函数：编译器和调用者都知道具体类型
    // 注意：此类设计为被继承，不独立使用
    ~ChainNode() {
        if (!owner_) return;
        EraseFromParent();
    }

    // 禁止拷贝和移动（节点绑定到特定链表）
    ChainNode(const ChainNode&) = delete;
    ChainNode(ChainNode&&) = delete;
    ChainNode& operator=(const ChainNode&) = delete;
    ChainNode& operator=(ChainNode&&) = delete;

    // ====================================================================
    // 内部接口（仅 ChainList 可用）
    // ====================================================================
    void SetParent_(OwnerTy* owner) {
        this->owner_ = owner;
    }

    // 从父列表中移除此节点 (O(1) 直接指针操作)
    // 此方法是最关键的性能优化点：
    //   - 不遍历列表
    //   - 只修改邻近指针
    //   - 统一更新计数
    void EraseFromParent_() {
        assert(owner_ != nullptr && "Cannot erase a node without owner");

        auto* current_owner = this->owner_;

        if (prev_) {
            prev_->next_ = next_;
        } else {
            current_owner->first_ = next_;
        }

        if (next_) {
            next_->prev_ = prev_;
        } else {
            current_owner->last_ = prev_;
        }

        current_owner->count_--;
        owner_ = nullptr;
        prev_ = nullptr;
        next_ = nullptr;
    }

    // ====================================================================
    // 公共接口
    // ====================================================================

    // 从父列表中移除此节点 (O(1))
    // 这是向后兼容的公共接口，由 ChainList 的友元访问
    void EraseFromParent() {
        EraseFromParent_();
    }

    // 用 new_node 替换此节点在链表中的位置 (O(1))
    // 此操作将 new_node 在链表中替换 this，this 会被断开所有链接
    void replaceWith(ItemTy* new_node) {
        if (!new_node) return;

        new_node->owner_ = this->owner_;
        new_node->prev_ = this->prev_;
        new_node->next_ = this->next_;

        if (!prev_) {
            owner_->first_ = new_node;
        } else {
            prev_->next_ = new_node;
        }

        if (!next_) {
            owner_->last_ = new_node;
        } else {
            next_->prev_ = new_node;
        }

        this->prev_ = nullptr;
        this->next_ = nullptr;
        this->owner_ = nullptr;
    }

    // ====================================================================
    // 公共接口
    // ====================================================================

    // 获取所属的链表所有者 (O(1))
    OwnerTy* getParent() const {
        return owner_;
    }

    // 链表导航 (O(1))
    ItemTy* getNext() const { return next_; }
    ItemTy* getPrev() const { return prev_; }
};


template <typename ListOwner, typename ListItem>
class ChainList {
    friend class ChainNode<ListOwner, ListItem>;

public:
    ListItem* first_ = nullptr;
    ListItem* last_ = nullptr;
    int count_ = 0;

    ChainList() = default;

    // 禁止拷贝和移动（列表包含指向外部的指针）
    ChainList(const ChainList&) = delete;
    ChainList(ChainList&&) = delete;
    ChainList& operator=(const ChainList&) = delete;
    ChainList& operator=(ChainList&&) = delete;

    // 非虚析构函数
    ~ChainList() {
        clear();
    }


    class Iterator {
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = ListItem*;
        using difference_type = std::ptrdiff_t;
        using pointer = ListItem**;
        using reference = ListItem*&;

        ListItem* pos_;

    public:
        Iterator() : pos_(nullptr) {}
        explicit Iterator(ListItem* ptr) : pos_(ptr) {}

        // 前进 (O(1))
        Iterator& operator++() {
            if (pos_) pos_ = pos_->next_;
            return *this;
        }

        Iterator operator++(int) {
            Iterator tmp = *this;
            ++(*this);
            return tmp;
        }

        // 后退 (O(1))
        Iterator& operator--() {
            if (pos_) pos_ = pos_->prev_;
            return *this;
        }

        Iterator operator--(int) {
            Iterator tmp = *this;
            --(*this);
            return tmp;
        }

        // 解引用
        value_type operator*() const { return pos_; }
        bool operator==(const Iterator& rhs) const { return pos_ == rhs.pos_; }
        bool operator!=(const Iterator& rhs) const { return pos_ != rhs.pos_; }

        // 在当前位置之前插入 (O(1))
        // 失败条件：pos_ 为空或无 owner
        Iterator insertBefore(ListItem* new_node) {
            assert(pos_ && pos_->owner_ && new_node);
            ListOwner* owner = pos_->owner_;

            if (pos_ == owner->first_) {
                owner->push_front(new_node);
            } else {
                new_node->SetParent_(owner);
                auto* prev_item = pos_->prev_;

                new_node->prev_ = prev_item;
                new_node->next_ = pos_;

                prev_item->next_ = new_node;
                pos_->prev_ = new_node;

                owner->count_++;
            }
            return Iterator(new_node);
        }

        // 在当前位置之后插入 (O(1))
        // 失败条件：pos_ 为空或无 owner
        Iterator insertAfter(ListItem* new_node) {
            assert(pos_ && pos_->owner_ && new_node);
            ListOwner* owner = pos_->owner_;

            if (pos_ == owner->last_) {
                owner->push_back(new_node);
            } else {
                new_node->SetParent_(owner);
                auto* next_item = pos_->next_;

                new_node->next_ = next_item;
                new_node->prev_ = pos_;

                pos_->next_ = new_node;
                next_item->prev_ = new_node;

                owner->count_++;
            }
            return Iterator(new_node);
        }
    };

    // 标准迭代器接口 (O(1) 获取)
    Iterator begin() { return Iterator(first_); }
    Iterator begin() const { return Iterator(first_); }
    Iterator end() { return Iterator(nullptr); }
    Iterator end() const { return Iterator(nullptr); }
    Iterator rbegin() { return Iterator(last_); }
    Iterator rbegin() const { return Iterator(last_); }
    Iterator rend() { return Iterator(nullptr); }
    Iterator rend() const { return Iterator(nullptr); }

    // 从某处连接一条已存在的链 (O(n) where n = 链的长度)
    // 警告：这个操作有线性遍历成本，应该尽量避免使用
    void connectSequence(ListItem* start_node, ListItem* end_node) {
        if (first_ || last_) {
            assert(false && "List is not empty, cannot connect");
        }

        first_ = start_node;
        last_ = end_node;
        count_ = 0;

        ListItem* walker = first_;
        while (walker != nullptr) {
            walker->SetParent_(static_cast<ListOwner*>(this));
            count_++;
            walker = walker->next_;
        }
    }

    std::pair<ListItem*, ListItem*> extract(ListItem* start_node, ListItem* end_node) {
        assert(start_node && end_node);
        auto* self_ptr = static_cast<ListOwner*>(this);
        assert(start_node->owner_ == self_ptr && end_node->owner_ == self_ptr);

        ListItem* pre_start = start_node->prev_;
        ListItem* post_end = end_node->next_;

        // ====================================================================
        // 关键优化：只遍历被切走的部分，O(k) where k = removed nodes
        // ====================================================================
        int removed_count = 0;
        for (ListItem* walker = start_node; walker != nullptr; walker = walker->next_) {
            removed_count++;
            if (walker == end_node) break;  // 关键：及时停止，不继续遍历
        }

        // 修复链表指针（不需要遍历）
        if (start_node == first_) {
            first_ = post_end;
        } else if (pre_start) {
            pre_start->next_ = post_end;
        }

        if (end_node == last_) {
            last_ = pre_start;
        } else if (post_end) {
            post_end->prev_ = pre_start;
        }

        // 清理被切走部分的边界指针
        start_node->prev_ = nullptr;
        end_node->next_ = nullptr;

        // 更新计数器（O(1) 数学运算，零遍历！）
        // 这是性能优化的核心，避免了原来的 O(N) 重遍历
        count_ -= removed_count;

        return {start_node, end_node};
    }

    // split: extract 的语义别名，含义更清晰
    // 完全等价，但名称暗示"分割"而非"提取"
    std::pair<ListItem*, ListItem*> split(ListItem* start_node, ListItem* end_node) {
        return extract(start_node, end_node);
    }

    // 标准容器接口

    // 获取当前大小 (O(1) - 常数时间)
    // 不需要遍历，维护的 count_ 保证 O(1) 性能
    int Size() const { return count_; }

    // 检查是否为空 (O(1))
    bool empty() const { return count_ == 0; }

    // 向后添加 (O(1))
    void push_back(ListItem* new_node) {
        new_node->SetParent_(static_cast<ListOwner*>(this));

        if (!first_) {
            first_ = new_node;
            last_ = new_node;
        } else {
            last_->next_ = new_node;
            new_node->prev_ = last_;
            last_ = new_node;
        }
        count_++;
    }

    // 向前添加 (O(1))
    void push_front(ListItem* new_node) {
        new_node->SetParent_(static_cast<ListOwner*>(this));

        if (!first_) {
            first_ = new_node;
            last_ = new_node;
        } else {
            first_->prev_ = new_node;
            new_node->next_ = first_;
            first_ = new_node;
        }
        count_++;
    }

    // 重新排列一组项 (O(m) where m = items.size())
    void rearrangeItems(ListItem* start, ListItem* end, std::list<ListItem*>& items) {
        assert(!items.empty());

        auto* link_prev = start->prev_;
        auto* link_next = end;

        if (!link_prev) {
            first_ = items.front();
        }

        // 验证所有条目都属于此列表
        for (auto* item : items) {
            if (item->getParent() != static_cast<ListOwner*>(this)) {
                assert(false && "Item parent mismatch");
            }
        }

        // 重新链接项目序列
        ListItem* prev_slot = link_prev;
        for (auto* item : items) {
            item->prev_ = prev_slot;
            if (prev_slot) {
                prev_slot->next_ = item;
            }
            prev_slot = item;
        }

        // 连接序列尾部
        ListItem* last_item = items.back();
        last_item->next_ = link_next;

        if (link_next) {
            link_next->prev_ = last_item;
        }
    }

    // 清空链表，删除所有节点 (O(n))
    void clear() {
        while (first_) {
            ListItem* temp = first_;
            first_ = first_->next_;
            if (temp) delete temp;
        }
        last_ = nullptr;
        count_ = 0;
    }

    // 获取首尾节点 (O(1))
    ListItem* front() { return first_; }
    const ListItem* front() const { return first_; }
    ListItem* back() { return last_; }
    const ListItem* back() const { return last_; }
};