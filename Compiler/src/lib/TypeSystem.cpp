#include "TypeSystem.hpp"
#include <iostream>
//IRType
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
        case IR_LABEL:
            return std::static_pointer_cast<IRType>(LABELType::NewLabel());
        case IR_ARRAY:
            //ARRAY类型的具体创建在外面ArrayList visit的相关地方，所以传入这个函数的这个类型不需要去进行动了
        default:
            // 对于不包括的类型（IR_POINTER, IR_FUNCTION, IR_ARRAY），返回nullptr
            return nullptr;
    }
}

//Void
std::shared_ptr<VOIDType> VOIDType::NewVoid(){
    static std::shared_ptr<VOIDType> single(new VOIDType());
    return single;
}
void VOIDType::to_string() const { std::cout << "void"; }

//Label
std::shared_ptr<LABELType> LABELType::NewLabel(){
    static std::shared_ptr<LABELType> single(new LABELType());
    return single;
}
void LABELType::to_string() const { std::cout << "label"; }

//Int
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
    // 使用 new 而不是 std::make_shared
    std::shared_ptr<INTType> ptr(new INTType(size, false));
    cache[size] = ptr;
    return ptr;
}
void INTType::to_string() const{
    if(isBool) std::cout << "i1";
    else std::cout << "i" << (size * 8);  // 直接输出
}

//Float
std::shared_ptr<FLOATType> FLOATType::NewFloat(){
    static std::shared_ptr<FLOATType> single(new FLOATType());
    return single;
}
void FLOATType::to_string() const { std::cout << "float"; }

//复合类型
/*
//Function
std::shared_ptr<FUNCTIONType> FUNCTIONType::NewFunction(const std::shared_ptr<IRType> &returnType,
                                                     const std::vector<std::shared_ptr<IRType>> &paramTypes){
     static std::map<std::pair<std::shared_ptr<IRType>, std::vector<std::shared_ptr<IRType>>>,
                        std::shared_ptr<FUNCTIONType>> m;

        auto key = std::make_pair(returnType, paramTypes);
        auto& ptr = m[key];
        if (!ptr) {
            ptr = std::shared_ptr<FUNCTIONType>(new FUNCTIONType(returnType, paramTypes));
        }
        return ptr;
}
std::string FUNCTIONType::to_string()const{
    std::string res = returnType->to_string() +"(";
    for(size_t i = 0;i < paramTypes.size();i++){
        if(i>0) res += ",";
        res += paramTypes[i]->to_string();
    }
    res += ")";
    return res;
}
*/
//Pointer
std::shared_ptr<POINTERType> POINTERType::NewPointer(std::shared_ptr<IRType> pointerType){
    static std::map<std::shared_ptr<IRType>,std::shared_ptr<POINTERType>> m;
    auto& ptr = m[pointerType];
    if(!ptr){
        ptr = std::shared_ptr<POINTERType>(new POINTERType(pointerType));
    }
    return ptr;
}
void POINTERType::to_string() const{
    pointerType->to_string();  // 递归调用
    std::cout << "*";  // 直接输出
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


//Array
std::shared_ptr<ARRAYType> ARRAYType::NewArray(std::shared_ptr<IRType> elementType,int count){
    static std::map<std::pair<std::shared_ptr<IRType>, int>, std::shared_ptr<ARRAYType>> m;
    auto key = std::make_pair(elementType, count);
        auto& ptr = m[key];
        if (ptr == nullptr) {
            ptr = std::shared_ptr<ARRAYType>(new ARRAYType(elementType, count));
        }
        return ptr;
    }


void ARRAYType::to_string() const{
    std::cout << "[" << elementCount << " x ";
    elementType->to_string();  // 递归调用
    std::cout << "]";  // 直接输出
}
std::shared_ptr<IRType> ARRAYType::getBaseType() const{
     if (auto arr = std::dynamic_pointer_cast<ARRAYType>(elementType))
            return arr->getBaseType();

        return elementType;
}
int ARRAYType::getScope(){return scope;}
