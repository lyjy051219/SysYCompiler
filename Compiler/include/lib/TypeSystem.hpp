#pragma once
#include <memory>
#include <vector>
#include <map>
#include <utility>
#include <string>
#include <cstdint>

/*ARRAY获取所有维度和获取总元素数量的接口有待观察*/

//IR类型系统
//单例+静态工厂模式--子类使用+shared_ptr包装裸指针
//复合类型的嵌套层数在构造时计算并缓存

enum TypeSystem{
    IR_INT,IR_FLOAT,IR_VOID,IR_POINTER,IR_ARRAY,IR_UNDEF
};
class IRType{
    private:
    TypeSystem ts;
    protected:
    size_t size; //类型的位宽，单位byte
    explicit IRType(TypeSystem ts):ts(ts){size = 0;}
    public:
    //=default：析构的时候正确调用析构链进行析构
    virtual ~IRType()=default;
    //getter：打印/获取类型/获取位宽/获取作用域层数
    //=0:强制子类实现
    virtual void to_string() const = 0;
    //这个函数只有复合类型等嵌套类型才会去使用这个，这里的scope不是符号表中的scope，而是用来表明复合类型嵌套了多少层
    virtual int getScope();
    //这两个函数子类都不用实现，直接调用基类的就可以返回相应的类型
    TypeSystem getEnum() const {return ts;}
    size_t getSize() const {return size;}
    bool isInt() const;
    bool isFloat() const;
    bool isBool() const;
    bool isVoid() const;
    bool isPointer() const;
    bool isArray() const;
    bool isScalar() const;
    bool hasSameScalarType(const IRType &Other) const;
    bool hasSameStorageType(const IRType &Other) const;
    bool hasSameShape(const IRType &Other) const;
    std::shared_ptr<IRType> getPointeeType() const;
    int countScalarElements(int Limit) const;
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
    //复合类型需要额外结构信息，不能仅通过枚举创建。
    static std::shared_ptr<IRType> NewTypeByTypeSystem(TypeSystem type);

};

class VOIDType : public IRType{
    VOIDType():IRType(IR_VOID){size = 0;}
    public:
    //静态工厂函数：通过protected构造函数，使外界只能通过静态工厂函数构造对象
    static std::shared_ptr<VOIDType> NewVoid();
   void to_string() const override;

};

class INTType : public IRType{
    bool isBool;
    INTType(size_t size, bool isBool = false):IRType(IR_INT),isBool(isBool){this->size = size;}
    public:
    static std::shared_ptr<INTType> NewInt(size_t size = 4);
    static std::shared_ptr<INTType> NewBool();
   void to_string() const override;
    bool getIsBool() const { return isBool; }
};



class FLOATType : public IRType{
    FLOATType():IRType(IR_FLOAT){size = 4;}
    public:
    static std::shared_ptr<FLOATType> NewFloat();
   void to_string() const override;
};


//下面2个是复合类型
class POINTERType : public IRType{
    std::shared_ptr<IRType> pointerType;  //指向的类型
    int scope;
            POINTERType(std::shared_ptr<IRType> pointerType)
        :IRType(IR_POINTER),
         pointerType(std::move(pointerType)){
            scope = this->pointerType->getScope() + 1;
            size = 4;}
    public:
    static std::shared_ptr<POINTERType> NewPointer(std::shared_ptr<IRType> pointerType);
   void to_string() const override;
    //获取指向的类型
    std::shared_ptr<IRType> getPointerType() const { return pointerType; }
    //获取最基础的类型
    std::shared_ptr<IRType> getBaseType() const;
    //获取scope
    int getScope()final;
};



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
   void to_string() const override;
    //获取元素类型
    std::shared_ptr<IRType> getElementType() const { return elementType; }
    //获取元素数量
    int getElementCount() const { return elementCount; }
    //获取最内层的类型
    std::shared_ptr<IRType> getBaseType() const;
    //获取scope
    int getScope()final;
    //获取总元素数量和获取所有维度不知道用不用，有待观察
};
