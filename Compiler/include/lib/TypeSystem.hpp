#pragma once
#include <memory>
#include <vector>
#include <map>
#include <utility>
#include <string>

/*ARRAY获取所有维度和获取总元素数量的接口有待观察*/

//IR类型系统
//单例+静态工厂模式--子类使用+shared_ptr包装裸指针
//对于嵌套的函数使用了符号表中的层数进行静态返回，而不是动态计算

enum TypeSystem{
    IR_INT,IR_FLOAT,IR_VOID,IR_POINTER,IR_FUNCTION,IR_ARRAY,IR_LABEL,IR_UNDEF
    /*,IR_VECTOR,ARM会用到这个类型*/
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
    //这个版本的不包括Function，Pointer，Array
    static std::shared_ptr<IRType> NewTypeByTypeSystem(TypeSystem type);

};

class VOIDType : public IRType{
    VOIDType():IRType(IR_VOID){size = 0;}
    public:
    //静态工厂函数：通过protected构造函数，使外界只能通过静态工厂函数构造对象
    static std::shared_ptr<VOIDType> NewVoid();
   void to_string() const override;

};

class LABELType : public IRType{
    LABELType():IRType(IR_LABEL){size = 0;}
    public:
    static std::shared_ptr<LABELType> NewLabel();
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

/*
//函数类型属于组合类型不属于嵌套类型
//函数类型，函数类型包括返回值类型和参数类型
class FUNCTIONType : public IRType{
    std::shared_ptr<IRType> returnType;
    std::vector<std::shared_ptr<IRType>> paramTypes;
    FUNCTIONType(std::shared_ptr<IRType> returnType, std::vector<std::shared_ptr<IRType>> paramTypes)
        : IRType(IR_FUNCTION),
         returnType(std::move(returnType)),
         paramTypes(std::move(paramTypes)){size = 0;} //函数类型没有大小
    public:
    static std::shared_ptr<FUNCTIONType> NewFunction(const std::shared_ptr<IRType> &returnType,const std::vector<std::shared_ptr<IRType>> & paramTypes);
   void to_string() const override;
    std::shared_ptr<IRType> getReturnType() const { return returnType; }
    std::vector<std::shared_ptr<IRType>> getParamTypes() const { return paramTypes; }

};*/

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

/*
如果后面需要写arm的话，就需要写一下vector类型
class VECTORType{

};
*/
