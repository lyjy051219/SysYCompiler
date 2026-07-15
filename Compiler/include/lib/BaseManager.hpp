#pragma once
#include <vector>
#include <memory>
#include <string>
#include <cstdint>
#include <optional>
#include "TypeSystem.hpp"


//Value,User,Use
class IRType;
class VOIDType;
class ConstInt;
class ConstFloat;
class ConstBool;
class Function;
class BuildInFunction;
class Use;
class UseList;
class Value;
class User;
class BasicBlock;
class Instruction;
class AllocaInst;
class BinaryInst;
class RetInst;
class BrInst;
class StoreInst;
class LoadInst;
class GetElementPtrInst;
class CallInst;
class FPTOSIInst;
class SITOFPInst;
class ZextInst;
class Variable;
class PhiInst;

/*Use类解读，Use是链表的节点
每个Use链表中节点的共同点是他们都指向同一个usee
设计特点，禁止被移动和拷贝，禁止被继承*/

class Use final{
    friend class UseList;
    friend class Value;

private:
    //指向这条Use关系的使用者user
    User* user = nullptr;
    //指向这条Use关系的被使用者usee
    Value* usee = nullptr;
    //指向下一个关联相同usee的Use关系
    Use* next = nullptr;
    //指向本个/上一个关联相同usee的Use关系的next指针的地址
    Use** prev = nullptr;
    // 从当前 Value 的 use-list 解链；重复调用安全。
    void removeFromUseList();
    /*解析prev：
    prev:指向的是前一个Use对象的next的指针的地址
    *prev指向的是前一个Use对象的next地址/也可以理解成是当前Use对象的地址
    next指向的就是下一个对象的的地址
    好处：无需特别判断头结点的情况
    */
public:
    //禁止直接构造
    Use()=delete;
    //禁止拷贝和移动
    Use(const Use& u) = delete;
    Use(Use&& u) = delete;
    Use& operator=(const Use& u) = delete;
    Use& operator=(Use&& u) = delete;
    //构造方式：需要传入Value和User
    //通过Value访问UseList利用其addUseNode函数进行插入该结点
    Use(User* er ,Value* ee);
    ~Use();
    User* getUser() const;
    Value* getValue() const;
    std::optional<std::size_t> getOperandIndex() const;
    //替换当前 Use 关系中的目标 Value (被使用者)
    bool replaceUseeWith(Value* V);
};

class UseList final{
   friend class Use;
   friend class Value;

   private:
   Use* head = nullptr;
   Use*& getHead();
   Use* getHead() const;
   void addUseNode(Use* newNode);
   public:
   // 保留既有字段以兼容只读的后端调用；中端统一通过 Value 接口访问。
   int size = 0;
   UseList()= default;
   //禁止拷贝和移动
   UseList(const UseList&) = delete;
   UseList(UseList&&) = delete;
   UseList& operator=(const UseList& u) = delete;
   UseList& operator=(UseList&& u) = delete;
   bool is_empty() const;

   //实现迭代器--只能正向进行遍历
    class iterator{
        Use *ptr;
        public:
        explicit iterator(Use *_ptr);
        iterator& operator++();
        Use* operator*() const;
        bool operator==(const iterator& other) const;
        bool operator!=(const iterator& other) const;
    };
    iterator begin() const;
    iterator end() const;
    /*使用示例：
    for(UserList::iterator it = list.begin(); it != list.end(); ++it) {
    Use* currentUse = *it;
    }*/
};

class Value{
   private:
   UseList useList;     //存储所有的user
   static int generateNum(const std::string& s); //编号每次创建类的时候自动生成
   protected:
    std::shared_ptr<IRType> irtype;  //类型系统
   std::string ident;   //名称
   int num;
   public:
   enum class BooleanExtensionKind {
       None,
       PlainBool,
       ZeroExt,
       SignExt,
   };

   //调用Use的析构函数进行彻底删除
   virtual ~Value();
   Value() = delete;
   //只能通过这两种方式构造对象，其他方式构建对象不行
   Value(std::shared_ptr<IRType> irtype);
   Value(std::shared_ptr<IRType> irtype,std::string ident);
   virtual bool isConst();
   virtual bool isGlobal();
   std::shared_ptr<IRType> getIRType() const;
   void setIRType(std::shared_ptr<IRType> Type);
   void setIdent(std::string Ident);
   std::string getIdent() const;
   UseList& getUseList();
   const UseList& getUseList() const;
   bool isConstZero() const;
   bool isConstIntZero() const;
   bool isConstOne() const;
   bool isConstMinusOne() const;
   bool isUndef() const;
   bool tryGetBoolConst(bool &Out) const;
   bool tryGetIntConst(int32_t &Out) const;
   bool tryGetIntConstAtWidth(unsigned Bits, int32_t &Out) const;
   bool tryGetFloatConst(float &Out) const;
   bool tryGetNumericFloatConst(float &Out) const;
   std::optional<int64_t> getIntConstant() const;
   int integerBitWidth(int DefaultBits = 32) const;
   int countUses() const;
   int countInstructionUses(Instruction **SingleUser = nullptr) const;
   // 返回可跨 RAUW/换边使用的 Use 指针快照；删除其所属 User 会使对应指针失效。
   std::vector<Use *> getUses() const;
   Use *getSingleUse() const;
   std::vector<Instruction *> getInstructionUsers() const;
   bool hasUses() const;
   bool hasExactlyOneUse() const;
   bool isPointerValue() const;
   std::shared_ptr<IRType> getPointeeType() const;
   bool hasSameStorageType(const Value *Other) const;
   static std::shared_ptr<IRType> getPointeeType(const Value *V);
   static bool hasSameStorageType(const Value *A, const Value *B);
   bool isExternalVariable() const;
   static bool isConstZero(const Value *V);
   static bool isConstIntZero(const Value *V);
   static bool isConstOne(const Value *V);
   static bool isConstMinusOne(const Value *V);
   static bool isUndef(const Value *V);
   static bool tryGetBoolConst(const Value *V, bool &Out);
   static bool tryGetIntConst(const Value *V, int32_t &Out);
   static bool tryGetFloatConst(const Value *V, float &Out);
   static bool areEqualConstants(const Value *A, const Value *B);
   static bool tryGetIntIndex(const Value *V, int &Out);
   static Value *stripBooleanExtension(Value *V,
                                       BooleanExtensionKind &Kind);
   static Value *stripPointerCastsAndGEP(Value *V);
   static bool isLocalStackPointer(Value *V);
   static Value *makeZeroForType(const std::shared_ptr<IRType> &Ty);
   static Value *makeOneForType(const std::shared_ptr<IRType> &Ty);
   static Value *makeMinusOneForType(const std::shared_ptr<IRType> &Ty);
   static Value *makeUndefForType(const std::shared_ptr<IRType> &Ty);
   static Value *makeIntegerForType(int32_t Val,
                                    const std::shared_ptr<IRType> &Ty);
   static Value *makeIntegerLike(const Value *Model, int32_t Val);
   void replaceAllUsesWith(Value* Val);
   void dumpIR();
   //如果一个Value一定需要copy，就需要用到clone,不允许使用构造函数copyValue
   //virtual Value* clone(std::unordered_map<Value*,Value*>&) = 0;
   //类型转换函数：将Value安全转换成其派生类
   template<typename T>

    T* as(){return dynamic_cast<T*>(this);}
   template<typename T>
    const T* as() const {return dynamic_cast<const T*>(this);}
};


//User实质上就是Instruction，只不过我们将在User层面的东西拿出来进行整合
class User : public Value{
   protected:
   //每一个User对象持有一个vector，这个vector中的元素是每一个以该User为User的Use对象
   //通过userList[i]->Use对象->usee就可以遍历到所有该User使用的usee
   std::vector<std::unique_ptr<Use>> userList;

   public:
   ~User() override;
   User();
   User(std::shared_ptr<IRType> irtype);

    const std::vector<std::unique_ptr<Use>>& getUserList() const;
    size_t getNumOperands() const;
    Value *getOperand(size_t Index) const;
    Use *getOperandUse(size_t Index) const;
    bool replaceOperand(size_t Index, Value *V);
    bool eraseOperand(size_t Index);
    void clearOperands();
    //获得在UserList中的索引
    int getUserIndex(const Use* u) const;


   //这个是往UserList里面加结点的函数,同时调用了Use的构造函数，在UseList里面也创建了相应的结点
   void addUse(Value* v);
   //virtual Value* clone(std::unordered_map<Value*,Value*>&)override;
};
