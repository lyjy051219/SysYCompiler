#pragma once
#include <vector>
#include <memory>
#include <string>
#include <cstdint>
#include <optional>
#include "TypeSystem.hpp"


//仿照LLVM实现Value/User/Use

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
class Param;
class PhiInst;

// Use 同时记录使用者和被使用值，并作为被使用值的使用链节点。
// 节点地址参与链表连接，因此禁止复制、移动和继承。
class Use final{
    friend class UseList;
    friend class Value;

private:
    // 持有该操作数的使用者。
    User* user = nullptr;
    // 当前操作数引用的值。
    Value* usee = nullptr;
    // 指向同一值的下一条使用关系。
    Use* next = nullptr;
    // 指向当前节点入口槽位，借助二级指针统一处理头节点和普通节点解链。
    Use** prev = nullptr;
    // 从当前值的使用链解链；重复调用不会产生副作用。
    void removeFromUseList();
public:
    // Use 必须由具体使用者和值共同构造。
    Use()=delete;
    Use(const Use& u) = delete;
    Use(Use&& u) = delete;
    Use& operator=(const Use& u) = delete;
    Use& operator=(Use&& u) = delete;
    // 构造时自动挂入被使用值的使用链。
    Use(User* er ,Value* ee);
    ~Use();
    User* getUser() const;
    Value* getValue() const;
    std::optional<std::size_t> getOperandIndex() const;
    // 将当前操作数换绑到新值，并同步维护新旧两条使用链。
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
   // 缓存节点数量；外部查询优先使用 Value 提供的接口。
   int size = 0;
   UseList()= default;
   // 链表节点拥有稳定地址，因此容器禁止复制和移动。
   UseList(const UseList&) = delete;
   UseList(UseList&&) = delete;
   UseList& operator=(const UseList& u) = delete;
   UseList& operator=(UseList&& u) = delete;
   bool isEmpty() const;

   // 使用链只提供正向迭代。
    class iterator{
        Use *ptr;
        public:
        explicit iterator(Use *ptr);
        iterator& operator++();
        Use* operator*() const;
        bool operator==(const iterator& other) const;
        bool operator!=(const iterator& other) const;
    };
    iterator begin() const;
    iterator end() const;
};

class Value{
   private:
   UseList useList;     // 保存所有以当前值为操作数的使用关系。
   //为IR生成唯一编号
   static int generateNum(const std::string& s);
   protected:
   std::shared_ptr<IRType> irtype;
   std::string ident;
   int num;
   public:
   //描述该值是bool还是bool的零扩展/符号扩展，或其他类型
   enum class BooleanExtensionKind {
       None,
       PlainBool,
       ZeroExt,
       SignExt,
   };

   // 析构值时删除仍引用它的使用者，保证使用链不悬空
   virtual ~Value();
   Value() = delete;
   // 所有值必须携带 IR 类型，可选携带显式标识符
   Value(std::shared_ptr<IRType> irtype);
   Value(std::shared_ptr<IRType> irtype,std::string ident);

   virtual bool isConst();
   virtual bool isGlobal();
   virtual bool isParam();
   std::shared_ptr<IRType> getIRType() const;
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
   bool tryGetInt64Const(int64_t &Out) const;
   bool tryGetInt64ConstAtWidth(unsigned Bits, int64_t &Out) const;
   bool tryGetFloatConst(float &Out) const;
   bool tryGetNumericFloatConst(float &Out) const;
   std::optional<int64_t> getIntConstant() const;
   int integerBitWidth(int DefaultBits = 32) const;
   int countUses() const;
   int countInstructionUses(Instruction **SingleUser = nullptr) const;
   std::vector<Use *> getUses() const;
   Use *getSingleUse() const;
   std::vector<Instruction *> getInstructionUsers() const;
   bool hasUses() const;
   bool hasExactlyOneUse() const;
   bool isPointerValue() const;
   std::shared_ptr<IRType> getPointeeType() const;
   static std::shared_ptr<IRType> getPointeeType(const Value *V);
   //判断当前值是否是外部变量（全局变量或函数参数）
   bool isExternalVariable() const;
   static bool isConstZero(const Value *V);
   static bool isConstOne(const Value *V);
   static bool isConstMinusOne(const Value *V);
   static bool isUndef(const Value *V);
   static bool tryGetBoolConst(const Value *V, bool &Out);
   static bool tryGetIntConst(const Value *V, int32_t &Out);
   static bool tryGetFloatConst(const Value *V, float &Out);
   //比较两个常量值是否相等
   static bool areEqualConstants(const Value *A, const Value *B);

   //识别IR Value中的常量Value，如果是常量将其提取为int
   static bool tryGetIntIndex(const Value *V, int &Out);

   // 识别布尔值及其零扩展或符号扩展形式，并返回扩展前的值。
   static Value *stripBooleanExtension(Value *V,
                                       BooleanExtensionKind &Kind);
   // 反复剥离BitCast和 GEP；遇到环时停止并返回当前值。
   static Value *stripPointerCastsAndGEP(Value *V);
   static Value *makeZeroForType(const std::shared_ptr<IRType> &Ty);
   static Value *makeOneForType(const std::shared_ptr<IRType> &Ty);
   static Value *makeMinusOneForType(const std::shared_ptr<IRType> &Ty);
   static Value *makeUndefForType(const std::shared_ptr<IRType> &Ty);
   static Value *makeIntegerForType(int64_t Val,const std::shared_ptr<IRType> &Ty);
   static Value *makeIntegerLike(const Value *Model, int64_t Val);

   // 将所有现有 Use 换绑到目标值，保持每个 User 的操作数位置不变
   void replaceAllUsesWith(Value* Val);
   // 输出当前值在 LLVM IR 中的引用形式
   void dumpIR();
   //相当于dynamic_cas，只是写法更简洁
   template<typename T>
   T* as(){return dynamic_cast<T*>(this);}
   template<typename T>
    const T* as() const {return dynamic_cast<const T*>(this);}
};


class User : public Value{
   protected:
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
    // 删除全部操作数，同时让各 Use 从被使用值的使用链解链
    void clearOperands();
    // 返回给定 Use 的操作数下标，不属于当前 User 时返回 -1
    int getUserIndex(const Use* u) const;
   // 追加操作数，并在目标值的使用链中注册对应 Use
   void addUse(Value* v);
};
