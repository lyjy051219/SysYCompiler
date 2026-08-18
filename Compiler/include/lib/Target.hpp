#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

class IRType;
class INTType;

enum class TargetABI {
    LP64D,
};

enum class TargetCodeModel {
    MediumAny,
};

// 所有依赖目标的 IR 布局决策统一放在此处。
// 类型驻留层只负责类型身份；指针的存储大小和对齐由 DataLayout 决定。
class DataLayout final {
public:
    static DataLayout forTarget();

    unsigned getPointerBitWidth(unsigned AddressSpace = 0) const noexcept;
    unsigned getIndexBitWidth() const noexcept;
    std::shared_ptr<INTType> getIndexType() const;
    std::shared_ptr<INTType> getIntPtrType() const;

    // 存储大小不含对象尾部填充；分配大小会按 ABI 对齐向上取整。
    std::size_t getTypeStoreSize(const IRType *Type) const;
    std::size_t getTypeStoreSize(
        const std::shared_ptr<IRType> &Type) const {
        return getTypeStoreSize(Type.get());
    }
    std::size_t getTypeAllocSize(const IRType *Type) const;
    std::size_t getTypeAllocSize(
        const std::shared_ptr<IRType> &Type) const {
        return getTypeAllocSize(Type.get());
    }
    std::size_t getABITypeAlign(const IRType *Type) const;
    std::size_t getABITypeAlign(
        const std::shared_ptr<IRType> &Type) const {
        return getABITypeAlign(Type.get());
    }

    // 兼容旧查询；实际内存布局应优先使用上面的精确接口。
    std::size_t getTypeSize(const IRType *Type) const;
    std::size_t getTypeSize(const std::shared_ptr<IRType> &Type) const {
        return getTypeSize(Type.get());
    }
    std::size_t getTypeAlign(const IRType *Type) const;
    std::size_t getTypeAlign(const std::shared_ptr<IRType> &Type) const {
        return getTypeAlign(Type.get());
    }

private:
    DataLayout() = default;
};

// 进程级 RV64 目标配置。
class TargetInfo final {
public:
    static TargetInfo &get();

    TargetABI getABI() const noexcept;
    TargetCodeModel getCodeModel() const noexcept;
    const char *getLLVMTargetTriple() const noexcept;
    const char *getLLVMDataLayoutString() const noexcept;
    const DataLayout &getDataLayout() const noexcept { return Layout; }
    unsigned getStackAlignment() const noexcept { return 16; }
    unsigned getStackArgumentSlotSize() const noexcept { return 8; }

private:
    TargetInfo();

    DataLayout Layout;
};
