#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
class Function;
class Initializer;
class IRType;
class Value;

// SymbolTable 只保存源码名称到 IR 对象的非拥有绑定，生命周期仅覆盖 IR 生成阶段
class SymbolTable final {
public:
    //Value的三种解释：
    enum class ObjectKind {
        // 普通变量或数组：value 指向对象存储地址，需要生成load才能读取标量值
        Address,
        // 标量常量：value 本身就是常量值，直接参与计算，不需要load
        Constant,
        // 数组实参：先load指针，再GEP
        ArrayParameter,
    };

    struct ObjectBinding final {
        ObjectKind kind = ObjectKind::Address;
        Value* value = nullptr;
        std::shared_ptr<IRType> type;
        bool isMutable = true;
        Initializer* constantInitializer = nullptr;
        // 源码 tensor 使用数组内存表示，完整形状必须独立于 IRType 保留。
        bool isTensor = false;
        std::vector<std::size_t> tensorShape;
        // 数组对象为 false；函数形参等已经是标量首元素指针时为 true。
        bool hasFlatTensorAddress = false;
    };

    // 构造时保留全局对象层；后续退出作用域不会弹出这一层
    SymbolTable() : objectScopes(1) {}
    SymbolTable(const SymbolTable&) = default;
    SymbolTable& operator=(const SymbolTable&) = default;
    SymbolTable(SymbolTable&&) noexcept = default;
    SymbolTable& operator=(SymbolTable&&) noexcept = default;

    // 延迟生成被调函数时只继承全局对象，不能看见调用者的局部绑定。
    void keepGlobalScopeOnly() {
        if (objectScopes.size() > 1)
            objectScopes.resize(1);
    }

    // IRGenerator 的函数体和 Block 都按现有规则显式进入、退出作用域。
    void enterScope() { objectScopes.emplace_back(); }

    void exitScope() noexcept {
        if (objectScopes.size() > 1)
            objectScopes.pop_back();
    }

    //检查当前作用域是否包含指定名称的对象
    bool containsObjectInCurrentScope(
        const std::string& name) const noexcept {
        const auto& currentScope = objectScopes.back();
        return currentScope.find(name) != currentScope.end();
    }

    // 所有对象都通过这一入口登记，调用处直接给出绑定种类
    ObjectBinding* declareObject(const std::string& name,
                                 ObjectBinding binding) {
        if (name.empty() || binding.value == nullptr ||
            binding.type == nullptr) {
            return nullptr;
        }

        auto [position, inserted] =
            objectScopes.back().emplace(name, std::move(binding));
        return inserted ? &position->second : nullptr;
    }

    // 从当前层向全局层查找第一个同名对象，即源码中实际可见的定义。
    const ObjectBinding* lookupObject(
        const std::string& name) const noexcept {
        for (auto scope = objectScopes.rbegin();
             scope != objectScopes.rend(); ++scope) {
            const auto found = scope->find(name);
            if (found != scope->end())
                return &found->second;
        }
        return nullptr;
    }

    // 函数在生成函数体之前登记，因此函数可以递归调用自身。
    // 函数与对象分属两个命名空间，也不会随对象作用域退出而消失
    bool declareFunction(const std::string& name, Function* function) {
        if (name.empty() || function == nullptr)
            return false;
        return functions.emplace(name, function).second;
    }

    Function* lookupFunction(const std::string& name) const noexcept {
        const auto found = functions.find(name);
        return found == functions.end() ? nullptr : found->second;
    }

private:
    using ObjectScope = std::unordered_map<std::string, ObjectBinding>;

    // 第 0 层始终是全局对象层，back() 始终可以安全访问
    std::vector<ObjectScope> objectScopes;
    std::unordered_map<std::string, Function*> functions;
};
