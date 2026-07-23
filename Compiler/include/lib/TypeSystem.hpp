#pragma once
#include <memory>
#include <vector>
#include <map>
#include <utility>
#include <string>
#include <cstdint>

// IR 类型由静态工厂驻留复用；复合类型在构造时缓存嵌套层数和存储大小。

enum TypeSystem{
    IR_INT,IR_FLOAT,IR_DOUBLE,IR_VOID,IR_POINTER,IR_ARRAY,IR_UNDEF
};
class IRType{
    private:
    TypeSystem ts;
    protected:
    size_t size; // 类型的存储大小，单位为字节。
    explicit IRType(TypeSystem ts):ts(ts){size = 0;}
    public:
    // 虚析构保证通过基类指针释放完整派生对象。
    virtual ~IRType()=default;
    // 按 LLVM IR 语法输出类型文本，具体类型必须实现。
    virtual void toString() const = 0;
    // 返回复合类型嵌套层数，与符号表作用域无关；标量类型返回 0。
    virtual int getScope();
    TypeSystem getEnum() const {return ts;}
    size_t getSize() const {return size;}
    bool isInt() const;
    bool isFloat() const;
    bool isBool() const;
    bool isVoid() const;
    bool isPointer() const;
    bool isArray() const;
    bool isScalar() const;
    // 标量相同比较种类和位宽；存储相同比较种类和总字节数；形状相同还递归比较数组维度。
    bool hasSameScalarType(const IRType &Other) const;
    bool hasSameStorageType(const IRType &Other) const;
    bool hasSameShape(const IRType &Other) const;
    std::shared_ptr<IRType> getPointeeType() const;
    // 统计数组叶标量数；结构非法或超过 Limit 时返回 -1。
    int countScalarElements(int Limit) const;
    // 在多维索引与线性叶索引之间转换，越界或未落到标量时返回 false。
    bool flattenScalarPath(const std::vector<int> &Path,
                           int &FlatIndex) const;
    bool unflattenScalarPath(int FlatIndex,
                             std::vector<int> &Path,
                             std::shared_ptr<IRType> &ElementTy) const;
    int getIntegerBitWidth(int DefaultBits = 32) const;
    static bool isInt(const std::shared_ptr<IRType> &Ty);
    static bool isFloat(const std::shared_ptr<IRType> &Ty);
    static bool isBool(const std::shared_ptr<IRType> &Ty);
    static bool isPointer(const std::shared_ptr<IRType> &Ty);
    static bool isArray(const std::shared_ptr<IRType> &Ty);
    static bool isScalar(const std::shared_ptr<IRType> &Ty);
    static bool hasSameScalarType(const std::shared_ptr<IRType> &A,
                                  const std::shared_ptr<IRType> &B);
    static bool hasSameShape(const std::shared_ptr<IRType> &A,
                             const std::shared_ptr<IRType> &B);
    static int countScalarElements(const std::shared_ptr<IRType> &Ty,
                                   int Limit);
    static bool flattenScalarPath(const std::shared_ptr<IRType> &Ty,
                                  const std::vector<int> &Path,
                                  int &FlatIndex);
    static bool unflattenScalarPath(const std::shared_ptr<IRType> &Ty,
                                    int FlatIndex,
                                    std::vector<int> &Path,
                                    std::shared_ptr<IRType> &ElementTy);
    // 仅创建无需结构参数的标量类型；指针、数组和失败哨兵返回空指针。
    static std::shared_ptr<IRType> NewTypeByTypeSystem(TypeSystem type);

};

class VOIDType : public IRType{
    VOIDType():IRType(IR_VOID){size = 0;}
    public:
    // 空类型为全局驻留的单例。
    static std::shared_ptr<VOIDType> NewVoid();
   void toString() const override;

};

class INTType : public IRType{
    bool isBool;
    INTType(size_t size, bool isBool = false):IRType(IR_INT),isBool(isBool){this->size = size;}
    public:
    static std::shared_ptr<INTType> NewInt(size_t size = 4);
    static std::shared_ptr<INTType> NewBool();
   void toString() const override;
    bool getIsBool() const { return isBool; }
};



class FLOATType : public IRType{
    FLOATType():IRType(IR_FLOAT){size = 4;}
    public:
    static std::shared_ptr<FLOATType> NewFloat();
   void toString() const override;
};

// double 只用于 putf 可变参数的 ABI 默认提升，不属于 SysY 源语言类型。
class DOUBLEType : public IRType{
    DOUBLEType():IRType(IR_DOUBLE){size = 8;}
    public:
    static std::shared_ptr<DOUBLEType> NewDouble();
    void toString() const override;
};


// 指针类型按被指向类型驻留复用。
class POINTERType : public IRType{
    std::shared_ptr<IRType> pointerType;
    int scope;
            POINTERType(std::shared_ptr<IRType> pointerType)
        :IRType(IR_POINTER),
         pointerType(std::move(pointerType)){
            scope = this->pointerType->getScope() + 1;
            size = 4;}
    public:
    static std::shared_ptr<POINTERType> NewPointer(std::shared_ptr<IRType> pointerType);
   void toString() const override;
    std::shared_ptr<IRType> getPointerType() const { return pointerType; }
    // 递归剥离所有指针层，返回最内层基础类型。
    std::shared_ptr<IRType> getBaseType() const;
    int getScope()final;
};



// 数组类型按元素类型和元素数量驻留复用。
class ARRAYType : public IRType{
    std::shared_ptr<IRType> elementType;
    int scope;
    int elementCount;
    ARRAYType(std::shared_ptr<IRType> elementType,int elementCount)
        :IRType(IR_ARRAY),
         elementType(std::move(elementType)),
         elementCount(elementCount){
            scope = this->elementType->getScope() + 1;
            size = this->elementType->getSize()*elementCount;}
    public:
    static std::shared_ptr<ARRAYType> NewArray(std::shared_ptr<IRType> elementType,int count);
   void toString() const override;
    std::shared_ptr<IRType> getElementType() const { return elementType; }
    int getElementCount() const { return elementCount; }
    // 递归剥离所有数组层，返回叶元素类型。
    std::shared_ptr<IRType> getBaseType() const;
    int getScope()final;
};
