#pragma once
#include "BaseManager.hpp"
#include <unordered_map>
#include <vector>
#include <string>

class Value;

// 同名符号使用栈保存；离开作用域时按登记顺序恢复被遮蔽的定义。
class SymbolTable {
private:
    std::unordered_map<std::string, std::vector<Value*>> symDict;

    std::vector<std::vector<Value*>*> activeVars;

    std::vector<size_t> scopeMarkers;

public:
    inline void registerSymbolTable(const std::string& name, Value* v) noexcept {
        // 查找或创建同名符号栈并记录当前定义。
        std::vector<Value*>& varStack = symDict[name];
        varStack.push_back(v);

        // 在活动作用域中记录需要回退的符号栈。
        if (!scopeMarkers.empty()) {
            activeVars.push_back(&varStack);
        }
    }

    inline void enterScope() noexcept {
        // 记录新作用域开始时的活动符号数量。
        scopeMarkers.push_back(activeVars.size());
    }

    inline void exitScope() noexcept {
        if (scopeMarkers.empty()) return;

        // 恢复进入当前作用域之前的符号定义。
        size_t startIdx = scopeMarkers.back();
        scopeMarkers.pop_back();

        for (size_t i = activeVars.size(); i-- > startIdx; ) {
            activeVars[i]->pop_back();
        }

        activeVars.resize(startIdx);
    }

    inline Value* getValueByName(const std::string& name) const {
        auto iter = symDict.find(name);
        if (iter == symDict.end() || iter->second.empty()) return nullptr;
        return iter->second.back();
    }
};
