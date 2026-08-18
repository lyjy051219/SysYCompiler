#include "lib/TypeSystem.hpp"
#include "lib/Target.hpp"
#include <iostream>
#include <stdexcept>

// IR 类型通用查询与结构比较。
int IRType::getScope(){ return 0; }
size_t IRType::getSize() const {
    if (isPointer() || isArray())
        return TargetInfo::get().getDataLayout().getTypeSize(this);
    return size;
}
bool IRType::isInt() const { return getTypeSystem() == IR_INT; }
bool IRType::isFloat() const { return getTypeSystem() == IR_FLOAT; }
bool IRType::isVoid() const { return getTypeSystem() == IR_VOID; }
bool IRType::isPointer() const { return getTypeSystem() == IR_POINTER; }
bool IRType::isArray() const { return getTypeSystem() == IR_ARRAY; }
bool IRType::isBool() const {
    auto *IT = dynamic_cast<const INTType *>(this);
    return IT && IT->getIsBool();
}
bool IRType::isScalar() const { return isInt() || isFloat(); }
bool IRType::isFirstClassValueType() const {
    return isScalar() || isPointer();
}
bool IRType::hasSameScalarType(const IRType &Other) const {
    if (!isScalar() || !Other.isScalar() || getTypeSystem() != Other.getTypeSystem())
        return false;
    if (const auto *ThisInt = dynamic_cast<const INTType *>(this)) {
        const auto *OtherInt = dynamic_cast<const INTType *>(&Other);
        return OtherInt && ThisInt->getBitWidth() == OtherInt->getBitWidth();
    }
    return getSize() == Other.getSize();
}
bool IRType::hasSameStorageType(const IRType &Other) const {
    if (getTypeSystem() != Other.getTypeSystem())
        return false;
    if (const auto *ThisInt = dynamic_cast<const INTType *>(this)) {
        const auto *OtherInt = dynamic_cast<const INTType *>(&Other);
        return OtherInt &&
               ThisInt->getStorageSize() == OtherInt->getStorageSize();
    }
    return getSize() == Other.getSize();
}
bool IRType::hasSameShape(const IRType &Other) const {
    // 数组除总大小外还必须逐层匹配元素数量和子类型形状。
    if (getTypeSystem() != Other.getTypeSystem() || getSize() != Other.getSize())
        return false;
    if (isPointer()) {
        auto *ThisPointer = dynamic_cast<const POINTERType *>(this);
        auto *OtherPointer = dynamic_cast<const POINTERType *>(&Other);
        return ThisPointer && OtherPointer &&
               ThisPointer->getPointerType() &&
               OtherPointer->getPointerType() &&
               ThisPointer->getPointerType()->hasSameShape(
                   *OtherPointer->getPointerType());
    }
    if (!isArray()) {
        if (isInt())
            return hasSameScalarType(Other);
        return true;
    }

    auto *ThisArray = dynamic_cast<const ARRAYType *>(this);
    auto *OtherArray = dynamic_cast<const ARRAYType *>(&Other);
    return ThisArray && OtherArray &&
           ThisArray->getElementCount() == OtherArray->getElementCount() &&
           ThisArray->getElementType() && OtherArray->getElementType() &&
           ThisArray->getElementType()->hasSameShape(
               *OtherArray->getElementType());
}
std::shared_ptr<IRType> IRType::getPointeeType() const {
    auto *PtrTy = dynamic_cast<const POINTERType *>(this);
    return PtrTy ? PtrTy->getPointerType() : nullptr;
}
std::shared_ptr<IRType> IRType::getPointeeType(
    const std::shared_ptr<IRType> &Ty) {
    return Ty ? Ty->getPointeeType() : nullptr;
}
std::shared_ptr<IRType> IRType::getIndexableElementType() const {
    if (auto *PtrTy = dynamic_cast<const POINTERType *>(this))
        return PtrTy->getPointerType();
    if (auto *ArrayTy = dynamic_cast<const ARRAYType *>(this))
        return ArrayTy->getElementType();
    return nullptr;
}
std::shared_ptr<IRType> IRType::resolveArrayElementType(
    const std::shared_ptr<IRType> &Ty,
    const std::vector<int> &Path) {
    std::shared_ptr<IRType> Current = Ty;
    for (int Index : Path) {
        // 每一维都同时验证数组种类和下标范围，避免调用方重复类型转换。
        if (auto ArrayTy = std::dynamic_pointer_cast<ARRAYType>(Current);
            ArrayTy && Index >= 0 &&
            Index < ArrayTy->getElementCount()) {
            Current = ArrayTy->getElementType();
            continue;
        }
        return nullptr;
    }
    return Current;
}
int IRType::countScalarElements(int Limit) const {
    // 自叶向外累计标量数量，并用除法提前拦截上限溢出。
    if (Limit < 1) return -1;
    if (isScalar()) return 1;
    auto *ArrayTy = dynamic_cast<const ARRAYType *>(this);
    if (!ArrayTy || ArrayTy->getElementCount() < 0 ||
        !ArrayTy->getElementType())
        return -1;
    int SubCount = ArrayTy->getElementType()->countScalarElements(Limit);
    if (SubCount < 0) return -1;
    // 零长子数组不含标量，也不能进入后续的除法上限检查。
    if (SubCount == 0) return 0;
    if (ArrayTy->getElementCount() > Limit / SubCount)
        return -1;
    return SubCount * ArrayTy->getElementCount();
}
bool IRType::flattenScalarPath(const std::vector<int> &Path,
                               int &FlatIndex) const {
    // 每层索引乘以内层标量跨度，累加得到行主序线性位置。
    FlatIndex = 0;
    const IRType *Current = this;
    for (int Index : Path) {
        auto *ArrayTy = dynamic_cast<const ARRAYType *>(Current);
        if (!ArrayTy || Index < 0 || Index >= ArrayTy->getElementCount() ||
            !ArrayTy->getElementType())
            return false;
        int SubCount = ArrayTy->getElementType()->countScalarElements(1 << 20);
        if (SubCount < 0) return false;
        FlatIndex += Index * SubCount;
        Current = ArrayTy->getElementType().get();
    }
    return Current && Current->isScalar();
}
bool IRType::unflattenScalarPath(
    int FlatIndex, std::vector<int> &Path,
    std::shared_ptr<IRType> &ElementTy) const {
    // 逐层用内层跨度做商和余数，恢复每一维索引及最终叶类型。
    Path.clear();
    ElementTy = nullptr;
    if (FlatIndex < 0) return false;

    const IRType *Current = this;
    while (auto *ArrayTy = dynamic_cast<const ARRAYType *>(Current)) {
        std::shared_ptr<IRType> ChildTy = ArrayTy->getElementType();
        if (!ChildTy) return false;
        int SubCount = ChildTy->countScalarElements(1 << 20);
        if (SubCount <= 0) return false;
        int Index = FlatIndex / SubCount;
        if (Index < 0 || Index >= ArrayTy->getElementCount()) return false;
        Path.push_back(Index);
        FlatIndex %= SubCount;
        ElementTy = ChildTy;
        Current = ChildTy.get();
    }

    if (FlatIndex != 0 || !Current || !Current->isScalar()) return false;
    if (!ElementTy) {
        if (Current->isBool()) ElementTy = INTType::getBoolTy();
        else if (Current->isInt())
            ElementTy = INTType::get(
                static_cast<const INTType *>(Current)->getBitWidth());
        else if (Current->isFloat()) ElementTy = FLOATType::NewFloat();
    }
    return ElementTy != nullptr;
}
int IRType::getIntegerBitWidth(int DefaultBits) const {
    auto *Integer = dynamic_cast<const INTType *>(this);
    return Integer && Integer->getBitWidth() != 0
               ? static_cast<int>(Integer->getBitWidth())
               : DefaultBits;
}
bool IRType::hasIntegerBitWidth(unsigned BitWidth) const {
    auto *Integer = dynamic_cast<const INTType *>(this);
    return Integer && Integer->getBitWidth() == BitWidth;
}
bool IRType::isInt(const std::shared_ptr<IRType> &Ty) {
    return Ty && Ty->isInt();
}
bool IRType::isFloat(const std::shared_ptr<IRType> &Ty) {
    return Ty && Ty->isFloat();
}
bool IRType::isBool(const std::shared_ptr<IRType> &Ty) {
    return Ty && Ty->isBool();
}
bool IRType::isPointer(const std::shared_ptr<IRType> &Ty) {
    return Ty && Ty->isPointer();
}
bool IRType::isArray(const std::shared_ptr<IRType> &Ty) {
    return Ty && Ty->isArray();
}
bool IRType::isScalar(const std::shared_ptr<IRType> &Ty) {
    return Ty && Ty->isScalar();
}
bool IRType::isFirstClassValueType(const std::shared_ptr<IRType> &Ty) {
    return Ty && Ty->isFirstClassValueType();
}
bool IRType::getLogicalBitWidth(std::size_t &BitWidth) const {
    BitWidth = 0;

    const IRType *ElementType = this;

    std::size_t ElementBits = 0;
    if (const auto *Integer =
            dynamic_cast<const INTType *>(ElementType)) {
        ElementBits = Integer->getBitWidth();
    } else if (ElementType && ElementType->isFloat()) {
        ElementBits = 32;
    } else {
        return false;
    }

    if (ElementBits == 0)
        return false;
    BitWidth = ElementBits;
    return true;
}
bool IRType::getLogicalBitWidth(
    const std::shared_ptr<IRType> &Ty, std::size_t &BitWidth) {
    if (!Ty) {
        BitWidth = 0;
        return false;
    }
    return Ty->getLogicalBitWidth(BitWidth);
}
bool IRType::hasIntegerBitWidth(const std::shared_ptr<IRType> &Ty,
                                unsigned BitWidth) {
    return Ty && Ty->hasIntegerBitWidth(BitWidth);
}
bool IRType::hasSameScalarType(const std::shared_ptr<IRType> &A,
                               const std::shared_ptr<IRType> &B) {
    return A && B && A->hasSameScalarType(*B);
}
bool IRType::hasSameShape(const std::shared_ptr<IRType> &A,
                          const std::shared_ptr<IRType> &B) {
    return A && B && A->hasSameShape(*B);
}
int IRType::countScalarElements(const std::shared_ptr<IRType> &Ty,
                                int Limit) {
    return Ty ? Ty->countScalarElements(Limit) : -1;
}
bool IRType::flattenScalarPath(const std::shared_ptr<IRType> &Ty,
                               const std::vector<int> &Path,
                               int &FlatIndex) {
    if (!Ty) {
        FlatIndex = 0;
        return false;
    }
    return Ty->flattenScalarPath(Path, FlatIndex);
}
bool IRType::unflattenScalarPath(const std::shared_ptr<IRType> &Ty,
                                 int FlatIndex,
                                 std::vector<int> &Path,
                                 std::shared_ptr<IRType> &ElementTy) {
    if (!Ty) {
        Path.clear();
        ElementTy = nullptr;
        return false;
    }
    return Ty->unflattenScalarPath(FlatIndex, Path, ElementTy);
}

std::shared_ptr<IRType> IRType::NewTypeByTypeSystem(TypeSystem type){
 switch (type) {
        case IR_INT:
            return std::static_pointer_cast<IRType>(INTType::getInt32Ty());
        case IR_FLOAT:
            return std::static_pointer_cast<IRType>(FLOATType::NewFloat());
        case IR_VOID:
            return std::static_pointer_cast<IRType>(VOIDType::NewVoid());
        case IR_POINTER:
        case IR_ARRAY:
        case IR_UNDEF:
            // 复合类型缺少结构信息，IR_UNDEF 只是前端失败哨兵。
            return nullptr;
    }
    return nullptr;
}

// 空类型。
std::shared_ptr<VOIDType> VOIDType::NewVoid(){
    static std::shared_ptr<VOIDType> single(new VOIDType());
    return single;
}
void VOIDType::toString() const { std::cout << "void"; }

// 整数类型身份由 LLVM 位宽决定；i1 与 i8 即使都至少占一个可寻址字节，
// 仍然是两个不同类型。
std::shared_ptr<INTType> INTType::get(unsigned bitWidth) {
    if (bitWidth == 0)
        throw std::invalid_argument("integer bit width cannot be zero");
    static std::map<unsigned, std::shared_ptr<INTType>> Cache;
    auto &Type = Cache[bitWidth];
    if (!Type)
        Type = std::shared_ptr<INTType>(new INTType(bitWidth));
    return Type;
}

void INTType::toString() const{
    std::cout << "i" << bitWidth;
}

// 浮点类型。
std::shared_ptr<FLOATType> FLOATType::NewFloat(){
    static std::shared_ptr<FLOATType> single(new FLOATType());
    return single;
}
void FLOATType::toString() const { std::cout << "float"; }

// 指针类型按被指向类型作为键驻留，并递归输出基础类型。
std::shared_ptr<POINTERType> POINTERType::NewPointer(std::shared_ptr<IRType> pointerType){
    static std::map<std::shared_ptr<IRType>,std::shared_ptr<POINTERType>> m;
    auto& ptr = m[pointerType];
    if(!ptr){
        ptr = std::shared_ptr<POINTERType>(new POINTERType(pointerType));
    }
    return ptr;
}
void POINTERType::toString() const{
    pointerType->toString();
    std::cout << "*";
}
std::shared_ptr<IRType> POINTERType::getBaseType() const{
        std::shared_ptr<IRType> baseType = pointerType;
        for (int i = 0; i < scope - 1; i++) {
            if (auto ptr = std::dynamic_pointer_cast<POINTERType>(baseType)) {
                baseType = ptr->getPointerType();
            } else {
                break;
            }
        }
        return baseType;
    }
int POINTERType::getScope(){return scope;}

// 数组类型按元素类型和元素数量组成的结构键驻留。
std::shared_ptr<ARRAYType> ARRAYType::NewArray(std::shared_ptr<IRType> elementType,int count){
    static std::map<std::pair<std::shared_ptr<IRType>, int>, std::shared_ptr<ARRAYType>> m;
    auto key = std::make_pair(elementType, count);
        auto& ptr = m[key];
        if (ptr == nullptr) {
            ptr = std::shared_ptr<ARRAYType>(new ARRAYType(elementType, count));
        }
        return ptr;
    }

void ARRAYType::toString() const{
    std::cout << "[" << elementCount << " x ";
    elementType->toString();
    std::cout << "]";
}
std::shared_ptr<IRType> ARRAYType::getBaseType() const{
     if (auto arr = std::dynamic_pointer_cast<ARRAYType>(elementType))
            return arr->getBaseType();

        return elementType;
}
int ARRAYType::getScope(){return scope;}
