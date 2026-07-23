#include "TypeSystem.hpp"
#include <iostream>

// IR 类型通用查询与结构比较。
int IRType::getScope(){ return 0; }
bool IRType::isInt() const { return getEnum() == IR_INT; }
bool IRType::isFloat() const { return getEnum() == IR_FLOAT; }
bool IRType::isVoid() const { return getEnum() == IR_VOID; }
bool IRType::isPointer() const { return getEnum() == IR_POINTER; }
bool IRType::isArray() const { return getEnum() == IR_ARRAY; }
bool IRType::isBool() const {
    auto *IT = dynamic_cast<const INTType *>(this);
    return IT && IT->getIsBool();
}
bool IRType::isScalar() const { return isInt() || isFloat(); }
bool IRType::hasSameScalarType(const IRType &Other) const {
    return isScalar() && Other.isScalar() && getEnum() == Other.getEnum() &&
           getSize() == Other.getSize();
}
bool IRType::hasSameStorageType(const IRType &Other) const {
    return getEnum() == Other.getEnum() && getSize() == Other.getSize();
}
bool IRType::hasSameShape(const IRType &Other) const {
    // 数组除总大小外还必须逐层匹配元素数量和子类型形状。
    if (getEnum() != Other.getEnum() || getSize() != Other.getSize())
        return false;
    if (!isArray()) return true;

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
int IRType::countScalarElements(int Limit) const {
    // 自叶向外累计标量数量，并用除法提前拦截上限溢出。
    if (Limit < 1) return -1;
    if (isScalar()) return 1;
    auto *ArrayTy = dynamic_cast<const ARRAYType *>(this);
    if (!ArrayTy || ArrayTy->getElementCount() < 0 ||
        !ArrayTy->getElementType())
        return -1;
    int SubCount = ArrayTy->getElementType()->countScalarElements(Limit);
    if (SubCount < 0 || ArrayTy->getElementCount() > Limit / SubCount)
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
        if (Current->isBool()) ElementTy = INTType::NewBool();
        else if (Current->isInt()) ElementTy = INTType::NewInt(Current->getSize());
        else if (Current->isFloat()) ElementTy = FLOATType::NewFloat();
    }
    return ElementTy != nullptr;
}
int IRType::getIntegerBitWidth(int DefaultBits) const {
    if (!isInt()) return DefaultBits;
    int Bits = static_cast<int>(getSize() * 8);
    return Bits > 0 ? Bits : DefaultBits;
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
            return std::static_pointer_cast<IRType>(INTType::NewInt(4));
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

// 整数类型按字节数驻留，布尔类型使用独立的 i1 单例。
std::shared_ptr<INTType> INTType::NewBool(){
    static std::shared_ptr<INTType> single(new INTType(1,true));
    return single;
}
std::shared_ptr<INTType> INTType::NewInt(size_t size) {
    static std::map<size_t, std::shared_ptr<INTType>> cache;
    auto it = cache.find(size);
    if (it != cache.end()) {
        return it->second;
    }
    // 构造函数不可公开访问，因此由类成员中的工厂直接创建实例。
    std::shared_ptr<INTType> ptr(new INTType(size, false));
    cache[size] = ptr;
    return ptr;
}
void INTType::toString() const{
    if(isBool) std::cout << "i1";
    else std::cout << "i" << (size * 8);
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
