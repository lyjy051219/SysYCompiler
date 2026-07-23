#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class Function;
class Initializer;
class IRType;
class Value;

// SymbolTable 是 IRGenerator 专用的源码名称环境，所有 IR 指针均为非拥有引用。
class SymbolTable final {
private:
    using NameId = std::uint32_t;
    using DefIndex = std::uint32_t;

    static constexpr NameId InvalidNameId = 0;
    static constexpr DefIndex InvalidDefIndex = 0;

public:
    enum class ObjectKind : std::uint8_t {
        ObjectAddress,
        ConstantValue,
        ArrayParameter,
    };

    // ObjectBinding 集中保存 IRGenerator 解析左值和常量折叠所需的信息。
    struct ObjectBinding final {
        ObjectKind kind = ObjectKind::ObjectAddress;
        Value* value = nullptr;
        std::shared_ptr<IRType> objectType;
        bool isMutable = true;
        Initializer* constantInitializer = nullptr;
    };

    // ScopeGuard 以 RAII 方式保证所有正常和提前返回路径都能退出作用域。
    class ScopeGuard final {
    public:
        ~ScopeGuard() {
            if (owner != nullptr)
                owner->exitScope();
        }

        ScopeGuard(const ScopeGuard&) = delete;
        ScopeGuard& operator=(const ScopeGuard&) = delete;

        ScopeGuard(ScopeGuard&& other) noexcept
            : owner(std::exchange(other.owner, nullptr)) {}

        ScopeGuard& operator=(ScopeGuard&& other) noexcept {
            if (this == &other)
                return *this;

            if (owner != nullptr)
                owner->exitScope();
            owner = std::exchange(other.owner, nullptr);
            return *this;
        }

    private:
        friend class SymbolTable;

        explicit ScopeGuard(SymbolTable& table) : owner(&table) {
            table.enterScope();
        }

        // 非拥有指针指向被守卫的符号表，移动后源 guard 自动失效。
        SymbolTable* owner = nullptr;
    };

    SymbolTable()
        : objectDefinitionHeads(1, InvalidDefIndex),
          functionsByName(1, nullptr),
          scopeMarkers(1, 0) {}

    ~SymbolTable() = default;

    SymbolTable(const SymbolTable&) = delete;
    SymbolTable& operator=(const SymbolTable&) = delete;
    SymbolTable(SymbolTable&&) = delete;
    SymbolTable& operator=(SymbolTable&&) = delete;

    // 创建一个新词法作用域守卫，析构时自动恢复外层对象定义。
    [[nodiscard]] ScopeGuard scoped() { return ScopeGuard(*this); }

    // 声明普通对象地址；同层已有同名对象时拒绝且不改变符号表。
    bool declareObject(const std::string& name, Value* value,
                       std::shared_ptr<IRType> objectType,
                       bool isMutable = true,
                       Initializer* constantInitializer = nullptr) {
        return declareObjectDefinition(
            name, ObjectBinding{ObjectKind::ObjectAddress, value,
                                std::move(objectType), isMutable,
                                constantInitializer});
    }

    // 声明直接以 Value 表示的标量常量。
    bool declareConstant(const std::string& name, Value* value,
                         std::shared_ptr<IRType> objectType) {
        return declareObjectDefinition(
            name, ObjectBinding{ObjectKind::ConstantValue, value,
                                std::move(objectType), false, nullptr});
    }

    // 声明保存数组实参指针的参数槽位。
    bool declareArrayParameter(const std::string& name, Value* value,
                               std::shared_ptr<IRType> pointeeType,
                               bool isMutable = true) {
        return declareObjectDefinition(
            name, ObjectBinding{ObjectKind::ArrayParameter, value,
                                std::move(pointeeType), isMutable, nullptr});
    }

    // 查询当前可见对象；返回指针在后续对象声明或退出作用域后可能失效。
    ObjectBinding* lookupObject(const std::string& name) noexcept {
        return const_cast<ObjectBinding*>(
            static_cast<const SymbolTable&>(*this).lookupObject(name));
    }

    const ObjectBinding* lookupObject(
        const std::string& name) const noexcept {
        const NameId nameId = findName(name);
        if (nameId == InvalidNameId)
            return nullptr;

        const DefIndex definition = objectDefinitionHeads[nameId];
        if (definition == InvalidDefIndex)
            return nullptr;
        return &activeDefinitions[definition - 1].binding;
    }

    // 判断当前层是否已经声明同名对象，外层遮蔽定义不算作冲突。
    bool containsObjectInCurrentScope(
        const std::string& name) const noexcept {
        const NameId nameId = findName(name);
        if (nameId == InvalidNameId)
            return false;

        const DefIndex definition = objectDefinitionHeads[nameId];
        return definition != InvalidDefIndex &&
               static_cast<std::size_t>(definition) >
                   scopeMarkers.back();
    }

    // 声明全局函数；对象与函数使用独立槽位，允许二者同名。
    bool declareFunction(const std::string& name, Function* function) {
        if (name.empty() || function == nullptr)
            return false;

        const NameId nameId = internName(name);
        if (functionsByName[nameId] != nullptr)
            return false;

        functionsByName[nameId] = function;
        return true;
    }

    // 查询已声明函数；函数不随对象词法作用域退出。
    Function* lookupFunction(const std::string& name) const noexcept {
        const NameId nameId = findName(name);
        return nameId == InvalidNameId ? nullptr : functionsByName[nameId];
    }

    // 判断函数命名空间是否已有同名声明。
    bool containsFunction(const std::string& name) const noexcept {
        return lookupFunction(name) != nullptr;
    }

private:
    struct ActiveDefinition final {
        NameId name = InvalidNameId;
        DefIndex shadowed = InvalidDefIndex;
        ObjectBinding binding;
    };

    // 仅声明对象的统一入口，先完成所有可能失败的操作再更新可见 head。
    bool declareObjectDefinition(const std::string& name,
                                 ObjectBinding binding) {
        if (name.empty() || binding.value == nullptr ||
            binding.objectType == nullptr) {
            return false;
        }

        const NameId nameId = internName(name);
        const DefIndex previousDefinition = objectDefinitionHeads[nameId];
        if (previousDefinition != InvalidDefIndex &&
            static_cast<std::size_t>(previousDefinition) >
                scopeMarkers.back()) {
            return false;
        }

        if (activeDefinitions.size() >=
            std::numeric_limits<DefIndex>::max()) {
            throw std::length_error("too many active symbol definitions");
        }

        const DefIndex newDefinition =
            static_cast<DefIndex>(activeDefinitions.size() + 1);
        activeDefinitions.push_back(
            ActiveDefinition{nameId, previousDefinition,
                             std::move(binding)});
        objectDefinitionHeads[nameId] = newDefinition;
        return true;
    }

    // 驻留新名称并同步扩展两个稠密 NameId 索引，0 永远保留为无效哨兵。
    NameId internName(const std::string& name) {
        const auto existing = internedNames.find(name);
        if (existing != internedNames.end())
            return existing->second;

        if (internedNames.size() >=
            std::numeric_limits<NameId>::max()) {
            throw std::length_error("too many interned symbol names");
        }

        const NameId newName =
            static_cast<NameId>(internedNames.size() + 1);
        // 先按 vector 自身的几何策略扩容，避免逐个 reserve 导致二次复杂度。
        objectDefinitionHeads.push_back(InvalidDefIndex);
        try {
            functionsByName.push_back(nullptr);
        } catch (...) {
            objectDefinitionHeads.pop_back();
            throw;
        }

        try {
            const auto insertion = internedNames.emplace(name, newName);
            if (!insertion.second) {
                functionsByName.pop_back();
                objectDefinitionHeads.pop_back();
                return insertion.first->second;
            }
        } catch (...) {
            functionsByName.pop_back();
            objectDefinitionHeads.pop_back();
            throw;
        }
        return newName;
    }

    // 只读查找不驻留未知名称，避免错误引用持续扩大名称索引。
    NameId findName(const std::string& name) const noexcept {
        const auto found = internedNames.find(name);
        return found == internedNames.end() ? InvalidNameId
                                            : found->second;
    }

    // 进入作用域时只记录定义栈长度，操作成本为摊销常数。
    void enterScope() {
        scopeMarkers.push_back(activeDefinitions.size());
    }

    // 退出作用域时逆序恢复被遮蔽 head，并释放本层全部绑定元数据。
    void exitScope() noexcept {
        if (scopeMarkers.size() == 1)
            return;

        const std::size_t definitionMarker = scopeMarkers.back();
        for (std::size_t index = activeDefinitions.size();
             index > definitionMarker; --index) {
            const ActiveDefinition& definition =
                activeDefinitions[index - 1];
            objectDefinitionHeads[definition.name] =
                definition.shadowed;
        }

        activeDefinitions.resize(definitionMarker);
        scopeMarkers.pop_back();
    }

    // 名称哈希表是字符串的唯一拥有者，并为每个拼写分配稳定内部 NameId。
    std::unordered_map<std::string, NameId> internedNames;

    // 稠密 head 数组按 NameId 直接定位当前可见对象定义。
    std::vector<DefIndex> objectDefinitionHeads;

    // 连续定义栈保存当前活动对象，退出作用域时直接截断释放局部元数据。
    std::vector<ActiveDefinition> activeDefinitions;

    // 函数命名空间按 NameId 稠密存储，独立于对象遮蔽规则。
    std::vector<Function*> functionsByName;

    // marker 栈始终保留全局层的 0，避免特殊化同层重复判断。
    std::vector<std::size_t> scopeMarkers;
};
