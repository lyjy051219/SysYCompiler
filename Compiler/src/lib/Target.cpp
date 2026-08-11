#include "lib/Target.hpp"

#include "lib/TypeSystem.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace {

std::size_t getScalarBitWidth(const IRType *Type) {
    if (const auto *Integer = dynamic_cast<const INTType *>(Type))
        return Integer->getBitWidth();
    if (Type && Type->isFloat())
        return 32;
    return 0;
}

std::size_t nextPowerOfTwo(std::size_t Value) {
    if (Value <= 1)
        return 1;
    std::size_t Result = 1;
    while (Result < Value && Result <= 8)
        Result <<= 1U;
    return std::min<std::size_t>(Result, 16);
}

std::size_t alignTo(std::size_t Value, std::size_t Alignment) {
    if (Alignment <= 1)
        return Value;
    const std::size_t Remainder = Value % Alignment;
    if (Remainder == 0)
        return Value;
    const std::size_t Padding = Alignment - Remainder;
    if (Value > std::numeric_limits<std::size_t>::max() - Padding)
        throw std::overflow_error("type allocation size overflow");
    return Value + Padding;
}

} // 匿名命名空间

DataLayout DataLayout::forTarget() {
    return DataLayout();
}

unsigned DataLayout::getPointerBitWidth(unsigned AddressSpace) const noexcept {
    (void)AddressSpace;
    // SysY2026 RV64 目标使用 64 位地址。
    return 64;
}

unsigned DataLayout::getIndexBitWidth() const noexcept {
    return getPointerBitWidth();
}

std::shared_ptr<INTType> DataLayout::getIndexType() const {
    return INTType::get(getIndexBitWidth());
}

std::shared_ptr<INTType> DataLayout::getIntPtrType() const {
    return INTType::get(getPointerBitWidth());
}

std::size_t DataLayout::getTypeStoreSize(const IRType *Type) const {
    if (!Type)
        return 0;

    switch (Type->getTypeSystem()) {
    case IR_VOID:
    case IR_UNDEF:
        return 0;
    case IR_INT: {
        const auto *Integer = dynamic_cast<const INTType *>(Type);
        return Integer ? Integer->getStorageSize() : 0;
    }
    case IR_FLOAT:
        return 4;
    case IR_POINTER:
        return getPointerBitWidth() / 8;
    case IR_ARRAY: {
        const auto *Array = dynamic_cast<const ARRAYType *>(Type);
        if (!Array || Array->getElementCount() < 0)
            return 0;
        // 数组元素之间必须保留元素自身的尾部填充。
        const std::size_t ElementSize =
            getTypeAllocSize(Array->getElementType());
        const std::size_t Count =
            static_cast<std::size_t>(Array->getElementCount());
        if (ElementSize != 0 &&
            Count > std::numeric_limits<std::size_t>::max() / ElementSize)
            throw std::overflow_error("array type size overflow");
        return ElementSize * Count;
    }
    case IR_VECTOR: {
        const auto *Vector = dynamic_cast<const VECTORType *>(Type);
        if (!Vector)
            return 0;
        const std::size_t ElementBits =
            getScalarBitWidth(Vector->getElementType().get());
        const std::size_t Count = Vector->getElementCount();
        if (ElementBits == 0)
            return 0;
        if (Count >
            (std::numeric_limits<std::size_t>::max() - 7U) / ElementBits)
            throw std::overflow_error("vector type size overflow");
        return (ElementBits * Count + 7U) / 8U;
    }
    }
    return 0;
}

std::size_t DataLayout::getABITypeAlign(const IRType *Type) const {
    if (!Type)
        return 1;

    switch (Type->getTypeSystem()) {
    case IR_VOID:
    case IR_UNDEF:
        return 1;
    case IR_INT: {
        const auto *Integer = dynamic_cast<const INTType *>(Type);
        return Integer
                   ? std::min<std::size_t>(
                         8, nextPowerOfTwo(Integer->getStorageSize()))
                   : 1;
    }
    case IR_FLOAT:
        return 4;
    case IR_POINTER:
        return getPointerBitWidth() / 8;
    case IR_ARRAY: {
        const auto *Array = dynamic_cast<const ARRAYType *>(Type);
        return Array ? getABITypeAlign(Array->getElementType()) : 1;
    }
    case IR_VECTOR: {
        const std::size_t Size = getTypeStoreSize(Type);
        if (Size == 0)
            return 1;
        // ABI 对齐始终保持二次幂；非标准宽度向量向上取整，但最高按
        // 16 字节对齐，后续合法化阶段可据此拆分。
        return nextPowerOfTwo(Size);
    }
    }
    return 1;
}

std::size_t DataLayout::getTypeAllocSize(const IRType *Type) const {
    return alignTo(getTypeStoreSize(Type), getABITypeAlign(Type));
}

std::size_t DataLayout::getTypeSize(const IRType *Type) const {
    return getTypeAllocSize(Type);
}

std::size_t DataLayout::getTypeAlign(const IRType *Type) const {
    return getABITypeAlign(Type);
}

unsigned DataLayout::getNativeVectorBitWidth() const noexcept {
    // 当前 RV64GC 不含 V 扩展。
    return 0;
}

bool DataLayout::supportsNativeVectorType(const IRType *Type) const {
    (void)Type;
    return false;
}

TargetInfo::TargetInfo()
    : Layout(DataLayout::forTarget()) {}

TargetInfo &TargetInfo::get() {
    static TargetInfo Instance;
    return Instance;
}

TargetABI TargetInfo::getABI() const noexcept {
    return TargetABI::LP64D;
}

TargetCodeModel TargetInfo::getCodeModel() const noexcept {
    return TargetCodeModel::MediumAny;
}

const char *TargetInfo::getLLVMTargetTriple() const noexcept {
    return "riscv64-unknown-linux-gnu";
}

const char *TargetInfo::getLLVMDataLayoutString() const noexcept {
    // 字符串与当前竞赛工具链的 Clang/LLVM 默认布局保持一致。
    return "e-m:e-p:64:64-i64:64-i128:128-n64-S128";
}
