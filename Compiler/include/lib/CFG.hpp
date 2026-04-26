#pragma once
#include "Tool.hpp"
#include "SymbolTable.hpp"
#include <memory>
#include <vector>
#include <string>
#include "BaseManager.hpp"
#include <set>
class Value;
class Function;
class BasicBlock;
class AllocaInst;
class BinaryInst;
class PhiInst;
class BrInst;

class Instruction : public User,public ChainNode<BasicBlock,Instruction>{
    public:
    enum InstType{
        RET,BR,
        ALLOCA,STORE,LOAD,GEP,
        ADD,FADD,SUB,FSUB,MUL,FMUL,SDIV,UDIV,FDIV,SREM,UREM,FREM,
        AND,OR,XOR,
        BinaryUnknown,
        ICMP,FCMP,
        SHL,LSHR,ASHR,
        SELECT,PHI,CALL,
        TRUNC,ZEXT,SEXT,FPTOSI,SITOFP,BITCAST
    };
    InstType instType;
    public:
    virtual ~Instruction()=default;
    Instruction(): User(){};
    Instruction(std::shared_ptr<IRType> irtype,InstType instType):User(std::move(irtype)){this->instType = instType;}
    Instruction(std::shared_ptr<IRType> irtype): User(std::move(irtype)){};
    virtual void dumpIR() = 0;
    virtual Value* getOprand();
    static Function* getUserFunction(Use* U);

    virtual Instruction* clone(const std::unordered_map<Value*, Value*>&) { return nullptr; }

    protected:
    //提供给所有子类 clone 函数复用的映射辅助函数
    static Value* remapVal(Value* v, const std::unordered_map<Value*, Value*>& vmap);
    static BasicBlock* remapBB(Value* v, const std::unordered_map<Value*, Value*>& vmap);
};

//二元操作指令
/*映射到LLVM IR是
add fadd sub fsub mul fmul sdiv fdiv srem frem（后面三个是取模）
and or xor
shl,lshr,ashr --这几条指令也没包括
icmp fcmp这些指令
*/
class BinaryInst : public Instruction{
    public:
    enum Operation{
        //注意这里没区别float和int的区别
        ADD,SUB,MUL,DIV,UDIV,MOD,UREM,
        AND,OR,XOR,SHL,LSHR,ASHR,
        E,NE,GE,L,LE,G
    };
    private:
    Operation op;
    public:
    void dumpIR() override;
    BinaryInst(std::shared_ptr<IRType> irtype);
    BinaryInst(Value* x,Operation op,Value* y);
    Operation getOp() const { return op; }
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};


/*PhiInst,BinaryInst等待实现*/
//类LLVM IR的内存结构，由BaseManager中Value/User/Use进行管理

//Module:对应AST中CompUnit
//由若干个Function和GlobalVar(全局变量)组成
class Module final{
    private:
    std::vector<std::unique_ptr<Function>> functions;
    std::vector<std::unique_ptr<Variable>> globalvars;
    SymbolTable symbolTable;
    std::set<std::string> declared_builtins;
    public:
    Module()=default;
    //函数管理
    //根据TypeSystem和ident创建新的Function（注册符号表，加入functions）
    Function& newFunction(TypeSystem ts,std::string ident);
    //注册符号表+push新Function
    void pushFunction(Function* func);
    std::vector<std::unique_ptr<Function>>& getFunctions();
    //从functions中删除指定的Function
    void deleteFunction(Function*ptr);
    Function* getMainFunction(); //寻找main函数并返回
    static Module* getActiveModule();
    //全局变量管理
    void pushGlobalVar(Variable* ptr); //只是将一个全局变量加入到Function
    std::vector<std::unique_ptr<Variable>>& getGlobalVar();
    //符号表管理
    SymbolTable& getSymbolTable();
    //调试信息打印
    void dumpIR();
    //生成重名函数后缀的函数
    std::string genFuncNameNum(std::string ident);
    void registerBuiltin(std::string ident) {
        declared_builtins.insert(ident);
    }
};

//初始化值类
//作为InitvalList的映射类型，用来保存数组初始化的多个初始化值
class Initializer : public Value,public std::vector<Value*>{
    public:
    Initializer(std::shared_ptr<IRType> irtype);
    void dumpIR();
    //根据初始化值生成GEP，Store指令，然后从BasicBlock中插入这些IR指令
    void Var2Store(BasicBlock *bb, const std::string &name,std::vector<int> &gep_data);
};

class Variable : public User{
    public:
    enum VarTag{
        GlobalVariable, //全局变量,常量数组和变量数组
        Constant,       //常量
        Param           //函数形参
    };

    VarTag vartag;
    //是不是全局变量/全局常量
    virtual bool isGlobal() final;
    //是不是函数形参
    virtual bool isParam() final;

    Variable(VarTag vt,std::shared_ptr<IRType> irtype,std::string ident);
    void dumpIR();
    Value* getUsee();
    //virtual Value* clone(std::unordered_map<Value*,Value*> &)override;
};

class Function : public Value, public ChainList<Function,BasicBlock>{
    private:
    //函数形参
    std::vector<std::unique_ptr<Value>> params;
    public:
    int blockNum =0;
    //指定返回类型和函数名称构造Function模块
    Function(TypeSystem ts,std::string ident);
    //同时向侵入式链表和vector中插入BasicBlock
    void pushBlock(BasicBlock*);
    //清理vector
//TODO：这个函数主要在优化的时候才会使用，等到后面再去实现这个函数
    //void insertBlock(BasicBlock* pre,BasicBlock* curr,BasicBlock* insert);
    //在curr之前插入 -- 这个函数用于初始化创建
    void insertBlock(BasicBlock* curr,BasicBlock* insert);
    //参数管理
    void pushParam(std::string ident,Variable* v);
    void pushParam(Variable* v);
    std::vector<std::unique_ptr<Value>>& getParams();
    //virtual Function* clone(std::unordered_map<Value*,Value*>&)override{return this;}
    //Inst插入
    void InsertAlloca(AllocaInst* ptr);
    bool isReachable(BasicBlock* BB);
    //调试信息打印
    void dumpIR();
    std::string getIdent(){ return ident;}
};

//内置函数系统 -- 用来包含LLVM内置的函数
//静态工厂函数，确保是单例
class BuildInFunction : public Value{
    BuildInFunction(std::shared_ptr<IRType>,std::string ident);
    public:
    //virtual BuildInFunction* clone(std::unordered_map<Value*,Value*>&)override{return this;}
    static BuildInFunction* genBuildInFunction(std::string);
    std::string getIdent(){ return ident;}
};

class BasicBlock : public Value, public ChainList<BasicBlock,Instruction>, public ChainNode<Function,BasicBlock>{
    public:
    //用于标记该basicblock在Function中的位置
    int num;

    virtual ~BasicBlock() = default;
    BasicBlock();
    void deleteBlock();
    void dumpIR();
    std::vector<BasicBlock*> getSuccessors();
    std::vector<BasicBlock*> getPredecessors();
    BrInst* getBranchTerminator();
    static bool isCondBr(BrInst *Br);
    static bool isUncondBr(BrInst *Br);
    int countEdgesToSucc(BasicBlock *To);
    static bool redirectBranchTarget(BasicBlock *Pred, BasicBlock *OldSucc, BasicBlock *NewSucc);
    static bool replaceCondBrWithUncond(BasicBlock *BB, BasicBlock *Dst);
    bool onlyPhiAndTerminator();
    static bool hasOnlyBlockLocalInstructionUsers(Instruction *I, BasicBlock *BB);
    void eraseBlockSafely();
    static void removeEdgeFromSuccPhis(BasicBlock *Succ, BasicBlock *From, int RemoveCount);
    static void replaceIncomingPredInSuccPhis(BasicBlock *Succ, BasicBlock *OldPred, BasicBlock *NewPred);
    // 收集块开头连续的 PHI 指令
    std::vector<PhiInst*> collectPhiNodes();
    // 若本块只有唯一前驱，则折叠退化 PHI
    bool simplifySinglePredPhis();
    //关于指令生成
    //终止指令
    //ret <type> <value>,type就是后面Value的type，所以这里只需要传入Value即可
    void genRetInst(Value* retVal);
    //ret void
    void genRetInst();
    //br i1 <cond>, label <iftrue>, label <iffalse>
    void genCondInst(Value* cond,BasicBlock* isTrue,BasicBlock* isFalse);
    //br label <dest>
    void genUnCondInst(BasicBlock* block);

    //内存操作指令
    //Alloca %x = alloca i32 --栈分配 --alloca可以直接访问指令的属性，返回alloca是因为可以直接让后面的load和store进行调用
    AllocaInst* genAllocaInst(std::shared_ptr<IRType> irtype,std::string ident);
    //Load %val = load i32, ptr %ptr
    Value* genLoadInst(Value* v);
    //Store store i32 20, i32* %y_ptr
    void genStoreInst(Value* val,Value* ptr);
    //Gep   %arrayidx = getelementptr i32, i32* %arr, i32 0
    Value* genGepInst(Value* ptr);
    //其他版本用的时候再写
 /*   //传入偏移量--单索引版本
    Value* getGepInst(Value* ptr,Value* x);
    //多索引版本
    Value* getGepInst(Value* ptr,std::vector<Value*> x);*/

    //二元运算指令
    /*
    add,fadd,sub,fsub,mul,fmul,sdiv,fdiv,srem,frem
    and,or,xor,shl,lshr,ashr
    icmp,fcmp
     */
    Value* genBinaryInst(Value* A,BinaryInst::Operation op,Value* B);
    //支持常量折叠的Binary生成版本
    static Value* genBinaryInst(BasicBlock*,Value*,BinaryInst::Operation,Value*);

    //转换运算
    Value* genSITOFPInst(Value* x);
    Value* genFPTOSIInst(Value* x);
    Value* genCallInst(std::string ident,std::vector<Value*> params);
    Value* genZextInst(Value* x);
    Value* genTruncInst(Value* x);
    Value* genSextInst(Value* x);
    Value* genBitCastInst(Value* v, std::shared_ptr<IRType> targetType);
    Value* genSelectInst(Value* cond, Value* isTrue, Value* isFalse);
    //基本块管理
    BasicBlock* genBlock();
    BasicBlock* genBlock(std::string ident);
    bool hasTerminator();
    void clear();

    /*目前用不到
    //Func相关指令
    //select
    Value* genSelectInst(Value* cond,Value* isTrue,Value* isFalse);
    //phi   --目前先省略，用到的时候再来补
    //具体函数类型的区分没写，等着以后去写
    Value* genSextInst(Value* x);
    Value* genTruncInst(Value* x);
    Value* genBitCastInst(Value* x);*/
    //BasicBlock* clone(std::unordered_map<Value*,Value*>&)override;
};




//终止指令：Ret Br
//Ret:返回指令：ret i32 %val/ret void
class RetInst : public Instruction{
    public:
    RetInst();
    RetInst(std::shared_ptr<IRType> irtype);
    RetInst(Value* retVal);
    //RetInst* clone(std::unordered_map<Value*, Value*>&)override;
    Value* getOprand()final;
    void dumpIR() override;
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};

//跳转指令
class BrInst : public Instruction{
    private:
    bool isCond; //true为条件分支，false为无条件分支
    //包括无条件跳转和有条件跳转
    public:
    BrInst(Value*,BasicBlock*,BasicBlock*);
    BrInst(BasicBlock* bb);
    BrInst(std::shared_ptr<IRType> irtype,bool is_Cond);
    Value* getOprand()final;
    void dumpIR() override;
    bool isConditional() const { return isCond; }
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};

//MemoryInst：Alloca Store Load GEP
//分配栈帧指令：%a = alloca i32
class AllocaInst : public Instruction{
    public:
    AllocaInst(std::shared_ptr<IRType> irtype);
    AllocaInst(std::string ident,std::shared_ptr<IRType> irtype);
    void dumpIR() override;
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;

};

//写入内存指令: store i32 3,i32* %a
class StoreInst : public Instruction{
    public:
    StoreInst(Value* val,Value* ptr);
    StoreInst(std::shared_ptr<IRType> irtype);
    Value* getOprand()final;
    void dumpIR() override;
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;

};

//内存读出指令: %x = load i32,i32* %a
class LoadInst : public Instruction{
    public:
    LoadInst(Value* v);
    LoadInst(std::shared_ptr<IRType> irtype);
    void dumpIR() override;
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};

//GEP:根据基地址与索引计算某元素地址，用于数组成员寻址
//%p = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i64 0, i64 3
class GetElementPtrInst : public Instruction{
    public:
    GetElementPtrInst(Value* v);
    GetElementPtrInst(Value* v,std::vector<Value*>& vv);
    GetElementPtrInst(std::shared_ptr<IRType> irtype);
    void dumpIR() override;
    void updateType();
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};




//Call:函数调用指令：%r = call i32 @foo(i32 %a)
//尾调用标记：tail call i32 @foo(i32 %a)
class CallInst : public Instruction{
    bool tailCall = false;
    public:
    CallInst(Value*,std::vector<Value*>&,std::string="");
    CallInst(std::shared_ptr<IRType> irtype);
    void dumpIR() override;
    bool isTailCall() const { return tailCall; }
    void setTailCall(bool v) { tailCall = v; }
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};



//float-> int: %i = fptosi float %f to i32
class FPTOSIInst : public Instruction{
    public:
    FPTOSIInst(Value* v);
    FPTOSIInst(std::shared_ptr<IRType> irtype);
    void dumpIR() override;
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};

//int -> float : %f = sitofp i32 to float
class SITOFPInst : public Instruction{
    public:
    SITOFPInst(Value* v);
    SITOFPInst(std::shared_ptr<IRType> irtype);
    void dumpIR() override;
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};


//零扩展:%x32 = zext i1 %b to i32
class ZextInst : public Instruction{
    public:
    ZextInst(Value* v);
    ZextInst(std::shared_ptr<IRType> irtype);
    void dumpIR() override;
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};

// 截断: i32 -> i1
class TruncInst : public Instruction{
    public:
    TruncInst(Value* v);
    TruncInst(std::shared_ptr<IRType> irtype);
    void dumpIR() override;
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};

// 符号扩展: i1 -> i32
class SextInst : public Instruction{
    public:
    SextInst(Value* v);
    SextInst(std::shared_ptr<IRType> irtype);
    void dumpIR() override;
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};

//函数操作指令
//Select:三元选择指令:cond ? __ : __
//%r = select i1 %c, i32 %a, i32 %b
class SelectInst : public Instruction{
    public:
    SelectInst(std::shared_ptr<IRType> irtype):Instruction(irtype){instType = InstType::SELECT;}
    SelectInst(Value* cond,Value* isTrue,Value* isFalse);
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
    void dumpIR() override;
};

//phi指令:%x = phi i32 [ %v1, %bb1 ], [ %v2, %bb2 ]
class PhiInst : public Instruction{
    public:
    struct IncomingEdge {
        Value *V = nullptr;
        BasicBlock *Pred = nullptr;
    };
    PhiInst(std::shared_ptr<IRType> irtype);
    //PhiInst* clone(std::unordered_map<Value*,Value*>&)override;
    void dumpIR() override;
    // 添加一个 incoming value: (Value*, BasicBlock*)
    void addIncoming(Value* val, BasicBlock* bb);
    // 获取 incoming 的数量 (每个 incoming 占 userList 中2个位置: val, bb)
    unsigned getNumIncomingValues() const { return userList.size() / 2; }
    // 获取第 i 个 incoming 的 Value
    Value* getIncomingValue(unsigned i) { return userList[i * 2]->getValue(); }
    // 获取第 i 个 incoming 的 BasicBlock
    BasicBlock* getIncomingBlock(unsigned i) { return dynamic_cast<BasicBlock*>(userList[i * 2 + 1]->getValue()); }
    // 设置第 i 个 incoming 的 Value (用于重命名时更新)
    void setIncomingValue(unsigned i, Value* val);
    std::vector<IncomingEdge> collectIncomingEdges();
    static bool rebuildFromIncoming(PhiInst *Phi, const std::vector<IncomingEdge> &Incoming);
    static bool removeIncomingByPred(PhiInst *Phi, BasicBlock *Pred, int RemoveCount);
    static bool replaceIncomingPred(PhiInst *Phi, BasicBlock *OldPred, BasicBlock *NewPred);
    Value *getIncomingFromPred(BasicBlock *Pred);
    static bool areEquivalent(PhiInst *A, PhiInst *B);
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};



//指针转换
class BitCastInst : public Instruction {
public:
    BitCastInst(Value* v, std::shared_ptr<IRType> targetType);
    void dumpIR() override;
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};


 //常量系统
 class ConstantSystem : public Value{
    public:
    ConstantSystem()=delete;
    ConstantSystem(std::shared_ptr<IRType> irtype);
    //virtual ConstantSystem* clone(std::unordered_map<Value*, Value*>&)override;
    virtual bool isConst(){return true;}
};


class ConstBool : public ConstantSystem{
    private:
    bool val;
    ConstBool(bool val);
    public:
    bool getVal();
    static ConstBool* newConstBool(bool = false);

};


 class ConstInt : public ConstantSystem{
    private:
    int val;
    ConstInt(int val);
    ConstInt(int val, size_t size);
    public:
    int getVal();
    //静态工厂方法
     static ConstInt* newConstInt(int val = 0, size_t size = 4);
 };

 class ConstFloat : public ConstantSystem{
    private:
    float val;
    ConstFloat(float val);
    public:
    float getVal();
    static ConstFloat* newConstFloat(float = 0);
};

 class ConstPtr : public ConstantSystem{
    private:
    ConstPtr(std::shared_ptr<IRType> irtype);
    public:
    static ConstPtr* newConstPtr(std::shared_ptr<IRType> irtype);
 };


 class UndefValue : public ConstantSystem{
    UndefValue(std::shared_ptr<IRType> irtype);
    public:
    static UndefValue* NewUndefValue(std::shared_ptr<IRType> it);
 };
