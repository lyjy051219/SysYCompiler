#pragma once
#include "Tool.hpp"
#include "SymbolTable.hpp"
#include <memory>
#include <vector>
#include <string>
#include "BaseManager.hpp"
#include <cstdint>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <type_traits>
class Value;
class Function;
class BasicBlock;
class AllocaInst;
class BinaryInst;
class PhiInst;
class BrInst;

// CFG 边由源基本块和终结指令中的后继下标唯一标识，避免平行边退化成同一组块指针。
struct CFGEdge {
    BasicBlock *From = nullptr;
    unsigned SuccessorIndex = 0;
    // 可选的目标校验；CFG 改写后不再匹配时，该瞬时句柄自动失效。
    BasicBlock *ExpectedSuccessor = nullptr;
};

// 重定向边时对旧后继 PHI 的处理策略。
enum class CFGEdgePhiPolicy {
    // 旧边彻底消失，删除旧后继中与该边对应的 incoming。
    RemoveOldIncoming,
    // 新块继续无条件跳向旧后继，把对应 incoming 的前驱改为新块。
    PreserveIncomingThroughNewBlock,
};

using FunctionCallGraph =
    std::unordered_map<Function *, std::vector<Function *>>;

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
    Function* getParentFunction();
    static Function* getParentFunction(Instruction *I);
    static bool insertBefore(Instruction *Pos, Instruction *NewI);
    static bool insertAfter(Instruction *Pos, Instruction *NewI);
    bool insertBefore(Instruction *NewI);
    bool insertAfter(Instruction *NewI);
    bool isInBlock(const BasicBlock *BB) const;
    bool comesBefore(const Instruction *Other) const;
    static bool comesBefore(const Instruction *A, const Instruction *B);
    Value *getUnaryOperand() const;
    static Value *getUnaryOperand(const Instruction *I);
    static Value *foldConstantCast(InstType CastType, Value *Operand);

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
    bool getOperands(Value *&LHS, Value *&RHS) const;
    static bool getOperands(const BinaryInst *BI, Value *&LHS, Value *&RHS);
    bool setOperands(Value *LHS, Value *RHS);
    static bool isCompareOp(Operation Op);
    // 统一维护操作码的类型约束和指令类别映射，避免生成器重复判断。
    static bool isIntegerOnlyOp(Operation Op);
    static Instruction::InstType getInstType(Operation Op, TypeSystem OperandType);
    static bool isAssociativeOp(Operation Op);
    static bool isCommutativeOp(Operation Op);
    static bool isReassociateOp(Operation Op);
    static Operation negatePredicate(Operation Op);
    static Operation reversePredicate(Operation Op);
    static BinaryInst *createBooleanNot(Value *Operand);
    static Value *foldConstants(Operation Op, Value *LHS, Value *RHS,
                                bool NormalizeIntegerWidth = false);
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
    std::vector<Instruction *> getInstructions();
    template <typename InstT = Instruction>
    std::vector<InstT *> collectInstructions();
    //从functions中删除指定的Function
    void deleteFunction(Function*ptr);
    Function* getMainFunction(); //寻找main函数并返回
    static Module* getActiveModule();
    FunctionCallGraph buildDirectCallGraph() const;
    static std::unordered_set<Function *> collectReachableFunctions(
        Function *Entry, const FunctionCallGraph &CallGraph);
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
    static bool isAllZero(Value *V);

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
    bool isScalarGlobal();
    Value *getConstantElement(const std::vector<int> &Path);
    static Value *getConstantElement(Variable *Source,
                                     const std::vector<int> &Path);

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
    //在curr之前插入 -- 这个函数用于初始化创建
    void insertBlock(BasicBlock* curr,BasicBlock* insert);
    //参数管理
    void pushParam(std::string ident,Variable* v);
    void pushParam(Variable* v);
    std::vector<std::unique_ptr<Value>>& getParams();
    const std::vector<std::unique_ptr<Value>>& getParams() const;
    size_t getParamCount() const;
    Value *getParam(size_t Index) const;
    int getParamIndex(const Value *Param) const;
    bool eraseParam(size_t Index);
    std::vector<CallInst *> getDirectCallSites() const;
    bool hasOnlyDirectCallUses() const;
    bool hasReservedSignature() const;
    bool hasBlockIdent(const std::string &Name) const;
    std::string makeUniqueBlockIdent(const std::string &Base) const;
    //virtual Function* clone(std::unordered_map<Value*,Value*>&)override{return this;}
    //Inst插入
    void InsertAlloca(AllocaInst* ptr);
    bool isReachable(BasicBlock* BB);
    void collectReachableBlocks(std::unordered_set<BasicBlock *> &Reachable);
    std::vector<BasicBlock *> getReachableBlocks();
    std::vector<Instruction *> getInstructions();
    template <typename InstT = Instruction>
    std::vector<InstT *> collectInstructions();
    void dropAllReferences();
    //调试信息打印
    void dumpIR();
    std::string getIdent() const { return ident;}
};

//内置函数系统 -- 用来包含LLVM内置的函数
//静态工厂函数，确保是单例
class BuildInFunction : public Value{
    BuildInFunction(std::shared_ptr<IRType>,std::string ident);
    public:
    //virtual BuildInFunction* clone(std::unordered_map<Value*,Value*>&)override{return this;}
    static BuildInFunction* genBuildInFunction(std::string);
    static bool isSupportedName(const std::string &Name);
    static bool isKnownRuntimeName(const std::string &Name);
    static bool isMemcpyName(const std::string &Name);
    static bool isMemmoveName(const std::string &Name);
    static bool isMemsetName(const std::string &Name);
    static bool isTimingName(const std::string &Name);
    static bool isScalarIOName(const std::string &Name);
    static bool isScalarOutputName(const std::string &Name);
    static bool isArrayInputName(const std::string &Name);
    static bool isArrayOutputName(const std::string &Name);
    static bool isOutputName(const std::string &Name);
    static const char *getDefaultMemcpyName();
    static const char *getDefaultMemmoveName();
    static const char *getDefaultMemsetName();
    std::string getIdent() const { return ident;}
};

class BasicBlock : public Value, public ChainList<BasicBlock,Instruction>, public ChainNode<Function,BasicBlock>{
    public:
    //用于标记该basicblock在Function中的位置
    int num;

    virtual ~BasicBlock() = default;
    BasicBlock();
    void dumpIR();
    std::vector<BasicBlock*> getSuccessors();
    std::vector<BasicBlock*> getPredecessors();
    Instruction *getTerminator();
    BrInst* getBranchTerminator();
    static bool isCondBr(BrInst *Br);
    static bool isUncondBr(BrInst *Br);
    int countEdgesToSucc(BasicBlock *To);
    bool hasMultipleDistinctSuccessors();
    static BasicBlock *getEdgeSuccessor(const CFGEdge &Edge);
    static bool isCriticalEdge(const CFGEdge &Edge);
    static bool redirectEdge(const CFGEdge &Edge, BasicBlock *NewSucc,
                             CFGEdgePhiPolicy PhiPolicy);
    static bool foldConditionalBranch(BasicBlock *BB,
                                      unsigned TakenSuccessorIndex);
    static BasicBlock *splitEdge(Function &F, const CFGEdge &Edge);
    // 以下低层接口只改终结指令，不自动维护 PHI；调用方须在同一事务中处理 incoming。
    static bool redirectBranchTarget(BasicBlock *Pred, BasicBlock *OldSucc, BasicBlock *NewSucc);
    static bool replaceCondBrWithUncond(BasicBlock *BB, BasicBlock *Dst);
    bool replaceTerminator(Instruction *NewTerminator);
    static bool replaceTerminator(BasicBlock *BB,
                                  Instruction *NewTerminator);
    bool onlyPhiAndTerminator();
    static bool hasOnlyBlockLocalInstructionUsers(Instruction *I, BasicBlock *BB);
    void eraseBlockSafely();
    static void removeEdgeFromSuccPhis(BasicBlock *Succ, BasicBlock *From, int RemoveCount);
    static void replaceIncomingPredInSuccPhis(BasicBlock *Succ, BasicBlock *OldPred, BasicBlock *NewPred);
    // 收集块开头连续的 PHI 指令
    std::vector<PhiInst*> collectPhiNodes();
    std::vector<Instruction *> getInstructions();
    template <typename InstT = Instruction>
    std::vector<InstT *> collectInstructions();
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
    bool insertBeforeTerminator(Instruction *I);
    bool insertAfterPhiNodes(Instruction *I);
    static bool insertBeforeTerminator(BasicBlock *BB, Instruction *I);
    static bool insertAfterPhiNodes(BasicBlock *BB, Instruction *I);
    // 将 Position 之后的指令按原顺序搬到目标块末尾，并同步转移终结边的 PHI 前驱。
    bool moveInstructionsAfter(Instruction *Position, BasicBlock *Destination);
    // 在 Position 之后切分基本块，新块自动加入同一函数并接管后半段指令。
    BasicBlock *splitBlockAfter(Instruction *Position);
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

// CFGSnapshot 是只读快照；CFG 发生修改后，调用方必须重新构造快照。
class CFGSnapshot final {
    std::unordered_map<BasicBlock *, std::vector<BasicBlock *>> SuccessorEdges;
    std::unordered_map<BasicBlock *, std::vector<BasicBlock *>> PredecessorEdges;
    std::unordered_map<BasicBlock *, std::vector<BasicBlock *>> DistinctPredecessors;

public:
    explicit CFGSnapshot(Function &F);
    const std::vector<BasicBlock *> &successorEdges(BasicBlock *BB) const;
    const std::vector<BasicBlock *> &predecessorEdges(BasicBlock *BB) const;
    const std::vector<BasicBlock *> &distinctPredecessors(BasicBlock *BB) const;
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
    Value *getReturnValue() const;
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
    bool isConditional() const;
    Value *getCondition() const;
    unsigned getSuccessorCount() const;
    BasicBlock *getSuccessor(unsigned Index) const;
    bool setSuccessor(unsigned Index, BasicBlock *Successor);
    BasicBlock *getTrueSuccessor() const;
    BasicBlock *getFalseSuccessor() const;
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};

//MemoryInst：Alloca Store Load GEP
//分配栈帧指令：%a = alloca i32
class AllocaInst : public Instruction{
    public:
    AllocaInst(std::shared_ptr<IRType> irtype);
    AllocaInst(std::string ident,std::shared_ptr<IRType> irtype);
    void dumpIR() override;
    std::shared_ptr<IRType> getAllocatedType() const;
    bool isArrayAllocation() const;
    static std::shared_ptr<IRType> getAllocatedType(const AllocaInst *AI);
    static bool isArrayAllocation(const AllocaInst *AI);
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;

};

//写入内存指令: store i32 3,i32* %a
class StoreInst : public Instruction{
    public:
    StoreInst(Value* val,Value* ptr);
    StoreInst(std::shared_ptr<IRType> irtype);
    Value* getOprand()final;
    Value* getValueOperand() const;
    Value* getPointerOperand() const;
    void dumpIR() override;
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;

};

//内存读出指令: %x = load i32,i32* %a
class LoadInst : public Instruction{
    public:
    LoadInst(Value* v);
    LoadInst(std::shared_ptr<IRType> irtype);
    Value* getPointerOperand() const;
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
    Value *getBasePointer() const;
    size_t getNumIndices() const;
    Value *getIndex(size_t Index) const;
    std::vector<Value *> getIndices() const;
    bool hasOnlyConstantIndices() const;
    bool getBaseAndIndices(Value *&Base, std::vector<Value *> &Indices) const;
    static bool getBaseAndIndices(const GetElementPtrInst *GEP, Value *&Base,
                                  std::vector<Value *> &Indices);
    bool reset(Value *Base, const std::vector<Value *> &Indices);
    std::optional<int64_t> getLastIndexStride();
    bool flattenZeroBasedChain(Value *&Base, std::vector<Value *> &Indices);
    static bool flattenZeroBasedChain(Value *V, Value *&Base,
                                      std::vector<Value *> &Indices);
    static bool getIndexStrides(Value *Base,
                                const std::vector<Value *> &Indices,
                                std::vector<int64_t> &Strides,
                                int64_t &FinalPointeeStride);
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
    Value *getCallee() const;
    Function *getDirectCallee() const;
    BuildInFunction *getBuiltinCallee() const;
    std::string getCalleeName() const;
    Value *getArg(unsigned Index) const;
    std::vector<Value *> getArgs() const;
    unsigned getArgCount() const;
    int getArgIndex(const Use *U) const;
    bool isCalleeUse(const Use *U) const;
    bool eraseArg(unsigned Index);
    bool isMemcpyLike() const;
    bool isNonVolatileMemcpy() const;
    static bool isMemcpyLike(const CallInst *CI);
    static bool isNonVolatileMemcpy(const CallInst *CI);
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
    Value *getCondition() const;
    Value *getTrueValue() const;
    Value *getFalseValue() const;
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
    // 添加一个 incoming value: (Value*, BasicBlock*)。若同一前驱存在平行边，
    // 同前驱 incoming 的顺序必须与该前驱终结指令中的同目标边顺序一致。
    void addIncoming(Value* val, BasicBlock* bb);
    // 获取 incoming 的数量 (每个 incoming 占 userList 中2个位置: val, bb)
    unsigned getNumIncomingValues() const;
    // 获取第 i 个 incoming 的 Value
    Value* getIncomingValue(unsigned i) const;
    // 获取第 i 个 incoming 的 BasicBlock
    BasicBlock* getIncomingBlock(unsigned i) const;
    // 设置第 i 个 incoming 的 Value (用于重命名时更新)
    void setIncomingValue(unsigned i, Value* val);
    bool setIncomingBlock(unsigned i, BasicBlock *BB);
    unsigned countIncomingFromPred(BasicBlock *Pred) const;
    std::vector<IncomingEdge> collectIncomingEdges();
    // 重建可能折叠并删除 Phi；成功后调用方不得继续使用旧 Phi 指针。
    static bool rebuildFromIncoming(PhiInst *Phi, const std::vector<IncomingEdge> &Incoming);
    static bool removeIncomingByPred(PhiInst *Phi, BasicBlock *Pred, int RemoveCount);
    static bool removeIncomingEdge(PhiInst *Phi, BasicBlock *Pred,
                                   unsigned Occurrence);
    static bool replaceIncomingPred(PhiInst *Phi, BasicBlock *OldPred, BasicBlock *NewPred);
    static bool replaceIncomingPredEdge(PhiInst *Phi, BasicBlock *OldPred,
                                        BasicBlock *NewPred,
                                        unsigned Occurrence);
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
    bool getVal() const;
    static ConstBool* newConstBool(bool = false);

};


 class ConstInt : public ConstantSystem{
    private:
    int val;
    ConstInt(int val);
    ConstInt(int val, size_t size);
    public:
    int getVal() const;
    //静态工厂方法
     static ConstInt* newConstInt(int val = 0, size_t size = 4);
 };

 class ConstFloat : public ConstantSystem{
    private:
    float val;
    ConstFloat(float val);
    public:
    float getVal() const;
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

// 模板必须放在头文件中；统一的类型过滤接口可替代 Pass 内重复的双层遍历。
template <typename InstT>
std::vector<InstT *> BasicBlock::collectInstructions() {
    static_assert(std::is_base_of_v<Instruction, InstT>,
                  "InstT 必须派生自 Instruction");
    std::vector<InstT *> Result;
    for (Instruction *I : *this) {
        if (!I) continue;
        if constexpr (std::is_same_v<InstT, Instruction>) {
            Result.push_back(I);
        } else if (auto *Typed = dynamic_cast<InstT *>(I)) {
            Result.push_back(Typed);
        }
    }
    return Result;
}

template <typename InstT>
std::vector<InstT *> Function::collectInstructions() {
    static_assert(std::is_base_of_v<Instruction, InstT>,
                  "InstT 必须派生自 Instruction");
    std::vector<InstT *> Result;
    for (BasicBlock *BB : *this) {
        if (!BB) continue;
        for (Instruction *I : *BB) {
            if (!I) continue;
            if constexpr (std::is_same_v<InstT, Instruction>) {
                Result.push_back(I);
            } else if (auto *Typed = dynamic_cast<InstT *>(I)) {
                Result.push_back(Typed);
            }
        }
    }
    return Result;
}

template <typename InstT>
std::vector<InstT *> Module::collectInstructions() {
    static_assert(std::is_base_of_v<Instruction, InstT>,
                  "InstT 必须派生自 Instruction");
    std::vector<InstT *> Result;
    for (const auto &FunctionPtr : getFunctions()) {
        Function *F = FunctionPtr.get();
        if (!F) continue;
        for (BasicBlock *BB : *F) {
            if (!BB) continue;
            for (Instruction *I : *BB) {
                if (!I) continue;
                if constexpr (std::is_same_v<InstT, Instruction>) {
                    Result.push_back(I);
                } else if (auto *Typed = dynamic_cast<InstT *>(I)) {
                    Result.push_back(Typed);
                }
            }
        }
    }
    return Result;
}
