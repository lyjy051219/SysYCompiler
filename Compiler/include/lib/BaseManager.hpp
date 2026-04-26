#pragma once
#include <vector>
#include <memory>
#include <string>
#include <unordered_map>
#include <iostream>
#include "TypeSystem.hpp"


//Value,User,Use
class IRType;
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
public:
    //指向这条Use关系的使用者user
    User* user = nullptr;
    //指向这条Use关系的被使用者usee
    Value* usee = nullptr;
    //指向下一个关联相同usee的Use关系
    Use* next = nullptr;
    //指向本个/上一个关联相同usee的Use关系的next指针的地址
    Use** prev = nullptr;
    /*解析prev：
    prev:指向的是前一个Use对象的next的指针的地址
    *prev指向的是前一个Use对象的next地址/也可以理解成是当前Use对象的地址
    next指向的就是下一个对象的的地址
    好处：无需特别判断头结点的情况
    */
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
    User* getUser(){return user;}
    Value* getValue(){return usee;}
    //替换当前 Use 关系中的目标 Value (被使用者)
    bool replaceUseeWith(Value* V);
    //一定是User调用这个函数
    void removeFromUseList(User*);
};

class UseList final{
   private:
   Use* head = nullptr;
   public:
   int size = 0;
   UseList()= default;
   //禁止拷贝和移动
   UseList(const UseList&) = delete;
   UseList(UseList&&) = delete;
   UseList& operator=(const UseList& u) = delete;
   UseList& operator=(UseList&& u) = delete;
   //头插法
   void addUseNode(Use* newNode);

   Use* getBack();
   Use*& getHead(){return head;}
   bool is_empty(){return head == nullptr;}

   //实现迭代器--只能正向进行遍历
    class iterator{
        Use *ptr;
        public:
        iterator(Use *_ptr):ptr(_ptr){}

        iterator& operator++(){
            ptr=ptr->next;
            return *this;
        }

        Use* operator*(){return ptr;}
        bool operator==(const iterator& other)const{return ptr==other.ptr;}
        bool operator!=(const iterator& other)const{return ptr!=other.ptr;}
    };
    iterator begin(){return iterator(this->head);}
    iterator end(){return iterator(nullptr);}
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
   //调用Use的析构函数进行彻底删除
   virtual ~Value();
   Value() = delete;
   //只能通过这两种方式构造对象，其他方式构建对象不行
   Value(std::shared_ptr<IRType> irtype);
   Value(std::shared_ptr<IRType> irtype,std::string ident);
   virtual bool isConst(){ return false; }
   virtual bool isGlobal(){ return false; }
   std::shared_ptr<IRType> getIRType(){return irtype;}
   void setIRType(std::shared_ptr<IRType> it){this->irtype = it;}
   void setIdent(std::string id){this->ident = id;}
   std::string getIdent(){ return ident;}
   UseList& getUseList(){return useList;}
   bool isConstZero();
   void replaceAllUsesWith(Value* val){
        this->useList.size = 0;
        Use* head = this->useList.getHead();
        this->useList.getHead() = nullptr;
        while(head){
            head->usee = val;
            Use* temp = head->next;
            // 断开旧链接
            head->next = nullptr;
            head->prev = nullptr;
            // 插入到新 Value 的 UseList
            val->getUseList().addUseNode(head);
            head = temp;
        }
   }
   void dumpIR();
   //如果一个Value一定需要copy，就需要用到clone,不允许使用构造函数copyValue
   //virtual Value* clone(std::unordered_map<Value*,Value*>&) = 0;
   //类型转换函数：将Value安全转换成其派生类
   template<typename T>

    T* as(){return dynamic_cast<T*>(this);}
};


//User实质上就是Instruction，只不过我们将在User层面的东西拿出来进行整合
class User : public Value{
   protected:
   //每一个User对象持有一个vector，这个vector中的元素是每一个以该User为User的Use对象
   //通过userList[i]->Use对象->usee就可以遍历到所有该User使用的usee
   std::vector<std::unique_ptr<Use>> userList;

   public:
   virtual ~User()=default;
   User();
   User(std::shared_ptr<IRType> irtype);

    std::vector<std::unique_ptr<Use>>& getUserList(){return this->userList;}
    //获得在UserList中的索引
    int getUserIndex(Use* u);


   //这个是往UserList里面加结点的函数,同时调用了Use的构造函数，在UseList里面也创建了相应的结点
   void addUse(Value* v);
   //virtual Value* clone(std::unordered_map<Value*,Value*>&)override;
};


inline Use::Use(User* er ,Value* ee) : user(er), usee(ee) {
    usee->getUseList().addUseNode(this);
}

inline Use::~Use() {
    removeFromUseList(user);
}

inline void Use::removeFromUseList(User*) {
    if (usee) {
        usee->getUseList().size--;
        if (prev) {
            *prev = next;
        }
        if (next != nullptr) {
            next->prev = prev;
        }
    }
}

inline void UseList::addUseNode(Use* newNode) {
    size++;
    newNode->next = head;
    if (head != nullptr) {
        head->prev = &(newNode->next);
    }
    newNode->prev = &head;
    head = newNode;
}

inline Use* UseList::getBack() {
    if (is_empty()) {
        return nullptr;
    }
    Use* current = head;
    while (current->next != nullptr) {
        current = current->next;
    }
    return current;
}

inline int Value::generateNum(const std::string& s) {
    static std::unordered_map<std::string, int> cnt;
    return cnt[s]++;
}

inline Value::~Value(){while (!useList.is_empty()) delete useList.getHead()->user;}

inline Value::Value(std::shared_ptr<IRType> irtype) : irtype(irtype) {
    ident = ".";
    ident += std::to_string(generateNum("."));
}

inline Value::Value(std::shared_ptr<IRType> irtype, std::string ident)
    : irtype(irtype), ident(ident), num(generateNum(ident)) {}


inline User::User():Value(VOIDType::NewVoid()){} // 需要传入有效的IRType

inline User::User(std::shared_ptr<IRType> irtype) : Value(irtype) {}

inline int User::getUserIndex(Use* u) {
    for (size_t i = 0; i < userList.size(); i++) {
        if (userList[i].get() == u)
            return i;
    }
    return -1;
}

inline void User::addUse(Value* v) {
    userList.push_back(std::make_unique<Use>(this, v));
}

inline bool Use::replaceUseeWith(Value* V) {
    if (!V || usee == V) return false;
    removeFromUseList(user);
    usee = V;
    V->getUseList().addUseNode(this);
    return true;
}