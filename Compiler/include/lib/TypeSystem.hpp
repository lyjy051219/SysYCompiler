#pragma once
#include <memory>
#include <vector>
#include <map>
#include <utility>
#include <string>
#include <cstdint>
#include <cstddef>

//IR类型系统
//使用静态工厂形式+Shared_ptr设计：相同结构的类型结构会被缓存并复用，避免重复创建和内存浪费

enum TypeSystem{
    IR_INT,IR_FLOAT,IR_VOID,IR_POINTER,IR_ARRAY,IR_UNDEF
};

class INTType;

/*
 * IR类型与对应的IR类型语法：
 * IR_INT：
 *     i1  —— 1 位整数，通常用于表示布尔值
 *     i8  —— 8 位整数
 *     i32 —— 32 位整数，SysY 的 int 通常映射为该类型
 *     i64 —— 64 位整数
 *
 * IR_FLOAT：
 *     float —— 32 位单精度浮点数
 *
 * IR_VOID：
 *     void —— 空类型，通常用于函数返回值
 *
 * IR_POINTER：
 *   指针类型，语法为 <被指向类型>*
 *   例如：
 *     i32*        —— 指向 i32 的指针
 *     float*      —— 指向 float 的指针
 *     i32**       —— 指向 i32* 的二级指针
 *     [10 x i32]* —— 指向包含 10 个 i32 元素的数组的指针
 *
 * IR_ARRAY：
 *   定长数组类型，语法为 [元素数量 x 元素类型]
 *   例如：
 *     [10 x i32]            —— 包含 10 个 i32 元素的一维数组
 *     [4 x float]           —— 包含 4 个 float 元素的一维数组
 *     [3 x [4 x i32]]       —— 3×4 的二维 i32 数组
 *     [2 x [3 x [4 x i32]]] —— 2×3×4 的三维 i32 数组
 *
 * IR_UNDEF：
 *   用于表示未定义值的 undef
 */

class IRType{
    private:
    //基类持有TypeSystem
    TypeSystem ts;
    protected:
    // 类型的存储大小，单位为字节
    size_t size;
    explicit IRType(TypeSystem ts):ts(ts){size = 0;}
    public:
    virtual ~IRType()=default;
    //dumpIR接口
    virtual void toString() const = 0;
    //返回复合类型嵌套层数；标量类型返回0
    virtual int getScope();
    TypeSystem getTypeSystem() const {return ts;}
    //返回Size
    //标量类型直接返回，目标平台相关类型(指针、数组)通过TargetInfo::get().getDataLayout()计算
    size_t getSize() const;
    //如果当前类型是 INTType，返回实际位宽
    int getIntegerBitWidth(int DefaultBits = 32) const;
    //检查当前类型是否为指定位宽的整数类型
    bool hasIntegerBitWidth(unsigned BitWidth) const;

    //类型判断接口：
    bool isInt() const;
    bool isFloat() const;
    bool isBool() const;
    bool isVoid() const;
    bool isPointer() const;
    bool isArray() const;
    bool isScalar() const; //检查是否是标量（整数或浮点数）
    //first-class value 类型仅包括标量和指针，严格排除 void 与聚合类型
    bool isFirstClassValueType() const;
    //静态类型判断接口：
    static bool isInt(const std::shared_ptr<IRType> &Ty);
    static bool isFloat(const std::shared_ptr<IRType> &Ty);
    static bool isBool(const std::shared_ptr<IRType> &Ty);
    static bool isPointer(const std::shared_ptr<IRType> &Ty);
    static bool isArray(const std::shared_ptr<IRType> &Ty);
    static bool isScalar(const std::shared_ptr<IRType> &Ty);
    static bool isFirstClassValueType(const std::shared_ptr<IRType> &Ty);
    //取得整数或浮点标量的逻辑位宽
    bool getLogicalBitWidth(std::size_t &BitWidth) const;
    static bool getLogicalBitWidth(const std::shared_ptr<IRType> &Ty,
                                   std::size_t &BitWidth);
    //空指针安全地检查整数类型位宽
    static bool hasIntegerBitWidth(const std::shared_ptr<IRType> &Ty,
                                   unsigned BitWidth);

    //比较两个标量类型(int/float)是否相同：比较种类和位宽
    bool hasSameScalarType(const IRType &Other) const;
    //比较两个相同类型的字节数
    bool hasSameStorageType(const IRType &Other) const;
    //比较两个相同类型的字节数+完整递归结构
    bool hasSameShape(const IRType &Other) const;
    static bool hasSameScalarType(const std::shared_ptr<IRType> &A,
                                  const std::shared_ptr<IRType> &B);
    static bool hasSameShape(const std::shared_ptr<IRType> &A,
                             const std::shared_ptr<IRType> &B);

    //尝试把当前类型转换为 POINTERType
    std::shared_ptr<IRType> getPointeeType() const;
    //空指针安全地尝试获取指针指向的类型
    static std::shared_ptr<IRType> getPointeeType(
        const std::shared_ptr<IRType> &Ty);
    //向下剥离一层，便于某些中端pass遍历；当前仅支持指针和数组
    std::shared_ptr<IRType> getIndexableElementType() const;
    //剥离多层：仅支持数组类型
    static std::shared_ptr<IRType> resolveArrayElementType(
        const std::shared_ptr<IRType> &Ty,
        const std::vector<int> &Path);

    //统计数组标量数
    //比如：[3 x [4 x i32]] 有 12 个标量元素
    int countScalarElements(int Limit) const;
    //多维索引转换为线性索引
    //比如：[3 x [4 x i32]] 的索引 [2,1] 对应线性索引 2*4 + 1 = 9
    bool flattenScalarPath(const std::vector<int> &Path,int &FlatIndex) const;
    //线性索引转换为多维索引
    //比如：[3 x [4 x i32]] 的线性索引 9 对应多维索引 [2,1]
    bool unflattenScalarPath(int FlatIndex,
                             std::vector<int> &Path,
                             std::shared_ptr<IRType> &ElementTy) const;
    static int countScalarElements(const std::shared_ptr<IRType> &Ty,
                                   int Limit);
    static bool flattenScalarPath(const std::shared_ptr<IRType> &Ty,
                                  const std::vector<int> &Path,
                                  int &FlatIndex);
    static bool unflattenScalarPath(const std::shared_ptr<IRType> &Ty,
                                    int FlatIndex,
                                    std::vector<int> &Path,
                                    std::shared_ptr<IRType> &ElementTy);

    //仅创建无需结构参数的标量类型；指针和数组返回空指针（这些无法只通过typesystem创建）
    static std::shared_ptr<IRType> NewTypeByTypeSystem(TypeSystem type);
};

class VOIDType : public IRType{
    VOIDType():IRType(IR_VOID){size = 0;}
    public:
    // 静态工厂接口：复用唯一的 void 类型
    static std::shared_ptr<VOIDType> NewVoid();
    void toString() const override;
};

class INTType : public IRType{
    //位宽
    unsigned bitWidth;
    explicit INTType(unsigned bitWidth)
        : IRType(IR_INT), bitWidth(bitWidth) {
        //向上取整
        size = (static_cast<size_t>(bitWidth) + 7U) / 8U;
    }
    public:
    // 静态工厂接口：按位宽驻留复用整数类型
    static std::shared_ptr<INTType> get(unsigned bitWidth);
    static std::shared_ptr<INTType> getBoolTy() { return get(1); }
    static std::shared_ptr<INTType> getInt8Ty() { return get(8); }
    static std::shared_ptr<INTType> getInt32Ty() { return get(32); }
    static std::shared_ptr<INTType> getInt64Ty() { return get(64); }

    void toString() const override;
    bool getIsBool() const { return bitWidth == 1; }
    unsigned getBitWidth() const { return bitWidth; }
    size_t getStorageSize() const {
        //bitWidth向上取整为存储字节数
        return (static_cast<size_t>(bitWidth) + 7U) / 8U;
    }
};


class FLOATType : public IRType{
    FLOATType():IRType(IR_FLOAT){size = 4;}
    public:
    static std::shared_ptr<FLOATType> NewFloat();
    void toString() const override;
};


class POINTERType : public IRType{
    //指针指向的类型
    std::shared_ptr<IRType> pointerType;
    //指针嵌套层数
    /*
    i32*                 → 1
    i32**                → 2
    [10 x i32]*          → 2
    [2 x [3 x i32]]*     → 3
    统计的是总体复合嵌套层数，不只统计指针层数
    */
    int scope;
    POINTERType(std::shared_ptr<IRType> pointerType)
        :IRType(IR_POINTER),
         pointerType(std::move(pointerType)){
            scope = this->pointerType->getScope() + 1;
            size = 0;}
    public:
    static std::shared_ptr<POINTERType> NewPointer(std::shared_ptr<IRType> pointerType);
    void toString() const override;
    //获取指针指向的类型
    std::shared_ptr<IRType> getPointerType() const { return pointerType; }
    //递归剥离所有指针层，返回最内层基础类型
    std::shared_ptr<IRType> getBaseType() const;
    int getScope()final;
};


class ARRAYType : public IRType{
    //数组元素类型
    std::shared_ptr<IRType> elementType;
    //总体嵌套层数
    int scope;
    //这一维的元素数量
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
    //返回最内层基础类型
    std::shared_ptr<IRType> getBaseType() const;
    int getScope()final;
};
