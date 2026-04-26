#pragma once
#include "BaseManager.hpp"
#include <unordered_map>
#include <vector>
#include <string>
#include <cassert>

class Value;

class SymbolTable {
private:
    std::unordered_map<std::string, std::vector<Value*>> sym_dict;

    std::vector<std::vector<Value*>*> active_vars;

    std::vector<size_t> scope_markers;

public:
    inline void register_symbolTable(const std::string& name, Value* v) noexcept {
        // 哈希表查询/创建
        std::vector<Value*>& var_stack = sym_dict[name];
        var_stack.push_back(v);

        // 如果当前处于某个作用域内，记录这个“栈的指针”
        if (!scope_markers.empty()) {
            active_vars.push_back(&var_stack);
        }
    }

    inline void enter_scope() noexcept {
        // 只需记录当前 active_vars 的长度作为一个书签，没有任何内存分配开销
        scope_markers.push_back(active_vars.size());
    }

    inline void exit_scope() noexcept {
        if (scope_markers.empty()) return;

        // 获取当前作用域的起点位置
        size_t start_idx = scope_markers.back();
        scope_markers.pop_back();

        // 从后向前遍历当前作用域压入的变量，直接通过指针弹栈，极致缓存友好
        for (size_t i = active_vars.size(); i-- > start_idx; ) {
            active_vars[i]->pop_back();
        }

        // 截断数组，恢复到上一个作用域的状态
        active_vars.resize(start_idx);
    }

    inline Value* getValueByName(const std::string& name) const {
        auto iter = sym_dict.find(name);
        // 断言：变量必须存在，且其值栈不能为空
        assert(iter != sym_dict.end() && !iter->second.empty());
        return iter->second.back();
    }
};