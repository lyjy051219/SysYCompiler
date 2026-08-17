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

// FixedSIMDMode 是固定宽度自动向量化的唯一目标开关。
// Disabled 保持当前 RV64GC 标量目标；SIMD64/SIMD128 分别声明目标原生
// 支持由 i32/float 组成的 2-lane/4-lane 固定向量；SIMD64And128 同时
// 开放两种宽度并优先选择 128 位。模式必须在建立分析管理器和优化流水线
// 之前设置，编译期间不得切换。
enum class FixedSIMDMode : unsigned {
    Disabled = 0,
    SIMD64,
    SIMD128,
    SIMD64And128,
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

    unsigned getNativeVectorBitWidth() const noexcept;
    bool supportsNativeVectorType(const IRType *Type) const;

private:
    DataLayout() = default;
};

// 进程级 RV64 目标配置。
class TargetInfo final {
public:
    static TargetInfo &get();

    // 这是考场需要操作的唯一接口。默认 Disabled；开启前必须确保后端
    // 已实现相同宽度的合法化、选指令、寄存器分配和汇编发射。
    void setFixedSIMDMode(FixedSIMDMode Mode) noexcept;
    FixedSIMDMode getFixedSIMDMode() const noexcept;

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
    FixedSIMDMode SIMDMode;
};
