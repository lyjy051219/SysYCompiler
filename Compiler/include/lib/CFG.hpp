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

// CFG 边由源基本块和终结指令中的后继下标唯一标识，从而区分目标相同的平行边。
struct CFGEdge {
    BasicBlock *From = nullptr;
    unsigned SuccessorIndex = 0;
    // 可选的目标校验；CFG 改写后不再匹配时，该瞬时句柄失效。
    BasicBlock *ExpectedSuccessor = nullptr;
};

// 重定向边时对旧后继 PHI 的处理策略。
enum class CFGEdgePhiPolicy {
    // 旧边彻底消失，删除旧后继中与该边对应的 PHI 入边项。
    RemoveOldIncoming,
    // 新块继续无条件跳向旧后继，把对应 PHI 入边项的前驱改为新块。
    PreserveIncomingThroughNewBlock,
};

using FunctionCallGraph =
    std::unordered_map<Function *, std::vector<Function *>>;

// 所有 IR 指令的基类，同时维护操作数使用关系和所属基本块。
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
        TRUNC,ZEXT,SEXT,FPTOSI,SITOFP,FPEXT,BITCAST
    };
    InstType instType;
    public:
    virtual ~Instruction()=default;
    Instruction(): User(){};
    Instruction(std::shared_ptr<IRType> irtype,InstType instType):User(std::move(irtype)){this->instType = instType;}
    Instruction(std::shared_ptr<IRType> irtype): User(std::move(irtype)){};
    // 按 LLVM IR 文本形式输出当前指令。
    virtual void dumpIR() = 0;
    static Function* getUserFunction(Use* U);
    Function* getParentFunction();
    static Function* getParentFunction(Instruction *I);
    // 将指令插入到已挂接位置的前方或后方，位置无父块时失败。
    static bool insertBefore(Instruction *Pos, Instruction *NewI);
    static bool insertAfter(Instruction *Pos, Instruction *NewI);
    bool insertBefore(Instruction *NewI);
    bool insertAfter(Instruction *NewI);
    bool isInBlock(const BasicBlock *BB) const;
    // 仅在两条指令属于同一基本块时比较线性顺序。
    bool comesBefore(const Instruction *Other) const;
    static bool comesBefore(const Instruction *A, const Instruction *B);
    Value *getUnaryOperand() const;
    static Value *getUnaryOperand(const Instruction *I);
    // 对支持的常量类型转换直接折叠，操作数或转换不受支持时返回空指针。
    static Value *foldConstantCast(InstType CastType, Value *Operand);

    // 按值映射复制指令；无法构造合法副本时返回空指针。
    virtual Instruction* clone(const std::unordered_map<Value*, Value*>&) { return nullptr; }

    protected:
    // 供各指令副本复用：映射内的值换成新值，区域外的值保持不变。
    static Value* remapVal(Value* v, const std::unordered_map<Value*, Value*>& vmap);
    static BasicBlock* remapBB(Value* v, const std::unordered_map<Value*, Value*>& vmap);
};

// 二元指令统一表示算术、位运算和比较，再根据操作数类型选择整数或浮点操作码。
// IR 结构：%r = add i32 %a, %b；%c = icmp slt i32 %a, %b。
class BinaryInst : public Instruction{
    public:
    enum Operation{
        // 语义操作与标量类型解耦，具体 IR 操作码由操作数类型决定。
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
    // 统一维护操作码的类型约束和指令类别映射。
    static bool isIntegerOnlyOp(Operation Op);
    static Instruction::InstType getInstType(Operation Op, TypeSystem OperandType);
    static bool isAssociativeOp(Operation Op);
    static bool isCommutativeOp(Operation Op);
    static bool isReassociateOp(Operation Op);
    static Operation negatePredicate(Operation Op);
    static Operation reversePredicate(Operation Op);
    static BinaryInst *createBooleanNot(Value *Operand);
    // 折叠两个常量操作数；启用位宽规整时按目标整数位宽处理溢出和移位。
    static Value *foldConstants(Operation Op, Value *LHS, Value *RHS,
                                bool NormalizeIntegerWidth = false);
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};


// 模块对应 AST 中的编译单元，统一拥有函数、全局变量和符号表。
class Module final{
    private:
    std::vector<std::unique_ptr<Function>> functions;
    std::vector<std::unique_ptr<Variable>> globalvars;
    SymbolTable symbolTable;
    std::set<std::string> declaredBuiltins;
    public:
    Module()=default;
    // 创建函数并同时完成符号注册和模块所有权接管。
    Function& newFunction(TypeSystem ts,std::string ident);
    // 接管已创建函数的所有权并注册其符号。
    void pushFunction(Function* func);
    std::vector<std::unique_ptr<Function>>& getFunctions();
    // 从模块中删除指定函数及其所有内容。
    void deleteFunction(Function*ptr);
    Function* getMainFunction();
    static Module* getActiveModule();
    // 构建仅包含直接调用的函数调用图，重复调用保留重复边。
    FunctionCallGraph buildDirectCallGraph() const;
    // 从入口函数出发，在给定调用图上收集可达函数。
    static std::unordered_set<Function *> collectReachableFunctions(
        Function *Entry, const FunctionCallGraph &CallGraph);
    // 将全局变量加入模块并接管所有权。
    void pushGlobalVar(Variable* ptr);
    std::vector<std::unique_ptr<Variable>>& getGlobalVar();
    SymbolTable& getSymbolTable();
    // 按全局变量、函数的顺序输出整个模块的 LLVM IR。
    void dumpIR();
    // 生成当前模块中未被占用的函数名后缀。
    std::string genFuncNameNum(std::string ident);
    void registerBuiltin(std::string ident) {
        declaredBuiltins.insert(ident);
    }
};

// 聚合初始值按数组层次保存子初始值，缺省元素由输出和生成阶段补零。
class Initializer : public Value,public std::vector<Value*>{
    static bool isAllZero(Value *V);

    public:
    Initializer(std::shared_ptr<IRType> irtype);
    void dumpIR();
    // 递归找出动态元素，按索引路径生成 GEP 和 Store，并将常量模板中的对应元素置零。
    void var2Store(BasicBlock *bb, const std::string &name,std::vector<int> &gepData);
};

class Variable : public User{
    public:
    enum VarTag{
        GlobalVariable, // 全局变量
        Constant,       // 全局常量
        Param           // 函数形参
    };

    VarTag vartag;
    // 判断该值是否以全局对象形式存储。
    virtual bool isGlobal() final;
    virtual bool isParam() final;
    bool isScalarGlobal();
    // 按多维索引查找常量初始值，省略的元素返回对应类型的零。
    Value *getConstantElement(const std::vector<int> &Path);
    static Value *getConstantElement(Variable *Source,
                                     const std::vector<int> &Path);

    Variable(VarTag vt,std::shared_ptr<IRType> irtype,std::string ident);
    void dumpIR();
    Value* getUsee();
};

class Function : public Value, public ChainList<Function,BasicBlock>{
    private:
    // 保存 IR 形参值；函数入口块中的形参槽位由 pushParam 同步建立。
    std::vector<std::unique_ptr<Value>> params;
    public:
    int blockNum =0;
    // 按返回类型和名称创建函数，并自动建立入口基本块。
    Function(TypeSystem ts,std::string ident);
    // 将基本块加入函数的侵入式块链表。
    void pushBlock(BasicBlock*);
    // 使新块无条件跳向 curr，再将新块加入函数。
    void insertBlock(BasicBlock* curr,BasicBlock* insert);
    // 接管形参，并在需要时于入口块建立槽位和 Store。
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
    // 将栈分配指令插入函数入口块。
    void InsertAlloca(AllocaInst* ptr);
    // 从入口块出发计算可达块，getReachableBlocks 按函数中的原有顺序返回结果。
    bool isReachable(BasicBlock* BB);
    void collectReachableBlocks(std::unordered_set<BasicBlock *> &Reachable);
    std::vector<BasicBlock *> getReachableBlocks();
    // 清空函数内所有指令的操作数关系，供批量销毁或回滚前使用。
    void dropAllReferences();
    // 输出函数签名、基本块及指令的 LLVM IR。
    void dumpIR();
};

// 内建函数代表 SysY 运行库和前端可生成的 LLVM 内建调用，同名对象由工厂复用。
class BuildInFunction : public Value{
    BuildInFunction(std::shared_ptr<IRType>,std::string ident);
    public:
    // 按名称返回已驻留的内建函数对象。
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
};

class BasicBlock : public Value, public ChainList<BasicBlock,Instruction>, public ChainNode<Function,BasicBlock>{
    public:
    // 标记基本块在所属函数中的创建序号。
    int num;

    virtual ~BasicBlock() = default;
    BasicBlock();
    void dumpIR();
    // 后继按终结指令的操作数顺序返回，平行边保留重复项。
    std::vector<BasicBlock*> getSuccessors();
    // 返回不重复的前驱基本块，不区分同一前驱产生的平行边。
    std::vector<BasicBlock*> getPredecessors();
    Instruction *getTerminator();
    BrInst* getBranchTerminator();
    static bool isCondBr(BrInst *Br);
    static bool isUncondBr(BrInst *Br);
    int countEdgesToSucc(BasicBlock *To);
    bool hasMultipleDistinctSuccessors();
    // 解析边句柄并校验预期目标，句柄已失效时返回空指针。
    static BasicBlock *getEdgeSuccessor(const CFGEdge &Edge);
    static bool isCriticalEdge(const CFGEdge &Edge);
    // 精确重定向一条边，并按策略同步更新旧后继的 PHI 入边项。
    static bool redirectEdge(const CFGEdge &Edge, BasicBlock *NewSucc,
                             CFGEdgePhiPolicy PhiPolicy);
    // 将条件分支折叠为选定的无条件边，并删除未选边对应的 PHI 入边项。
    static bool foldConditionalBranch(BasicBlock *BB,
                                      unsigned TakenSuccessorIndex);
    // 在指定边中插入新块，同时保留原后继 PHI 中的值和边顺序。
    static BasicBlock *splitEdge(Function &F, const CFGEdge &Edge);
    // 以下低层接口只改终结指令，不自动维护 PHI；调用方须同步处理入边项。
    static bool redirectBranchTarget(BasicBlock *Pred, BasicBlock *OldSucc, BasicBlock *NewSucc);
    static bool replaceCondBrWithUncond(BasicBlock *BB, BasicBlock *Dst);
    bool replaceTerminator(Instruction *NewTerminator);
    static bool replaceTerminator(BasicBlock *BB,
                                  Instruction *NewTerminator);
    bool onlyPhiAndTerminator();
    static bool hasOnlyBlockLocalInstructionUsers(Instruction *I, BasicBlock *BB);
    // 用未定义值替换仍被引用的非空类型指令，再删除整个基本块。
    void eraseBlockSafely();
    static void removeEdgeFromSuccPhis(BasicBlock *Succ, BasicBlock *From, int RemoveCount);
    static void replaceIncomingPredInSuccPhis(BasicBlock *Succ, BasicBlock *OldPred, BasicBlock *NewPred);
    // 收集块开头连续的 PHI 指令。
    std::vector<PhiInst*> collectPhiNodes();
    // 若本块只有唯一前驱，则折叠仅有一个入边项的 PHI。
    bool simplifySinglePredPhis();
    // 生成带返回值的终结指令：ret i32 %value。
    void genRetInst(Value* retVal);
    // 生成空返回终结指令：ret void。
    void genRetInst();
    // 生成条件分支：br i1 %cond, label %ifTrue, label %ifFalse。
    void genCondInst(Value* cond,BasicBlock* isTrue,BasicBlock* isFalse);
    // 生成无条件分支：br label %dest。
    void genUnCondInst(BasicBlock* block);

    // 在函数入口块生成栈分配：%x = alloca i32。
    AllocaInst* genAllocaInst(std::shared_ptr<IRType> irtype,std::string ident);
    // 生成内存读取：%val = load i32, i32* %ptr。
    Value* genLoadInst(Value* v);
    // 生成内存写入：store i32 20, i32* %ptr。
    void genStoreInst(Value* val,Value* ptr);
    // 创建仅含基指针的 GEP，调用方添加索引后需更新结果类型。
    Value* genGepInst(Value* ptr);
    // 统一标量类型后生成二元指令：%r = add i32 %a, %b。
    Value* genBinaryInst(Value* A,BinaryInst::Operation op,Value* B);
    // 操作数全为常量时直接折叠，否则将指令插入给定基本块。
    static Value* genBinaryInst(BasicBlock*,Value*,BinaryInst::Operation,Value*);

    // 生成标量转换、函数调用和选择指令。
    Value* genSITOFPInst(Value* x);
    Value* genFPTOSIInst(Value* x);
    Value* genFPExtInst(Value* x);
    Value* genCallInst(std::string ident,std::vector<Value*> params);
    Value* genZextInst(Value* x);
    Value* genTruncInst(Value* x);
    Value* genSextInst(Value* x);
    Value* genBitCastInst(Value* v, std::shared_ptr<IRType> targetType);
    Value* genSelectInst(Value* cond, Value* isTrue, Value* isFalse);
    // 在同一函数中创建新基本块并维护指令插入位置。
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
};

// CFG 只读快照同时保存边序列和去重前驱；CFG 改写后必须重新构造。
class CFGSnapshot final {
    std::unordered_map<BasicBlock *, std::vector<BasicBlock *>> SuccessorEdges;
    std::unordered_map<BasicBlock *, std::vector<BasicBlock *>> PredecessorEdges;
    std::unordered_map<BasicBlock *, std::vector<BasicBlock *>> DistinctPredecessors;

public:
    explicit CFGSnapshot(Function &F);
    // 后继边和前驱边保留平行边，去重前驱每个源块仅保留一次。
    const std::vector<BasicBlock *> &successorEdges(BasicBlock *BB) const;
    const std::vector<BasicBlock *> &predecessorEdges(BasicBlock *BB) const;
    const std::vector<BasicBlock *> &distinctPredecessors(BasicBlock *BB) const;
};




// 返回指令结束当前函数执行。
// IR 结构：ret i32 %val；ret void。
class RetInst : public Instruction{
    public:
    RetInst();
    RetInst(std::shared_ptr<IRType> irtype);
    RetInst(Value* retVal);
    Value *getReturnValue() const;
    void dumpIR() override;
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};

// 分支指令同时表示条件跳转和无条件跳转。
// IR 结构：br i1 %cond, label %true, label %false；br label %dest。
class BrInst : public Instruction{
    private:
    bool isCond; // 为真时有两个后继，为假时只有一个后继。
    public:
    BrInst(Value*,BasicBlock*,BasicBlock*);
    BrInst(BasicBlock* bb);
    BrInst(std::shared_ptr<IRType> irtype,bool isCond);
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

// 栈分配指令在当前函数栈帧中创建对象。
// IR 结构：%a = alloca i32。
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

// 内存写入指令的第一个操作数是值，第二个操作数是目标指针。
// IR 结构：store i32 3, i32* %a。
class StoreInst : public Instruction{
    public:
    StoreInst(Value* val,Value* ptr);
    StoreInst(std::shared_ptr<IRType> irtype);
    Value* getValueOperand() const;
    Value* getPointerOperand() const;
    void dumpIR() override;
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;

};

// 内存读取指令从指针操作数指向的对象中取值。
// IR 结构：%x = load i32, i32* %a。
class LoadInst : public Instruction{
    public:
    LoadInst(Value* v);
    LoadInst(std::shared_ptr<IRType> irtype);
    Value* getPointerOperand() const;
    void dumpIR() override;
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};

// 元素地址指令根据基指针和逐层索引计算子对象地址。
// IR 结构：%p = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i64 0, i64 3。
class GetElementPtrInst : public Instruction{
    public:
    GetElementPtrInst(Value* v);
    GetElementPtrInst(Value* v,std::vector<Value*>& vv);
    GetElementPtrInst(std::shared_ptr<IRType> irtype);
    void dumpIR() override;
    // 按基指针和全部索引逐层剖析元素类型，重算 GEP 的结果指针类型。
    void updateType();
    Value *getBasePointer() const;
    size_t getNumIndices() const;
    Value *getIndex(size_t Index) const;
    std::vector<Value *> getIndices() const;
    bool hasOnlyConstantIndices() const;
    bool getBaseAndIndices(Value *&Base, std::vector<Value *> &Indices) const;
    static bool getBaseAndIndices(const GetElementPtrInst *GEP, Value *&Base,
                                  std::vector<Value *> &Indices);
    // 原子地重建基指针和索引操作数，并同步更新结果类型。
    bool reset(Value *Base, const std::vector<Value *> &Indices);
    // 返回最后一个索引对应的字节步长，类型链无效时返回空值。
    std::optional<int64_t> getLastIndexStride();
    // 仅合并内层首索引为零的 GEP 链，保留展平后的索引顺序。
    bool flattenZeroBasedChain(Value *&Base, std::vector<Value *> &Indices);
    static bool flattenZeroBasedChain(Value *V, Value *&Base,
                                      std::vector<Value *> &Indices);
    // 按类型层次计算每个索引的字节步长及最终指向对象的步长。
    static bool getIndexStrides(Value *Base,
                                const std::vector<Value *> &Indices,
                                std::vector<int64_t> &Strides,
                                int64_t &FinalPointeeStride);
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};




// 函数调用指令把第一个操作数作为被调函数，其余操作数作为实参。
// IR 结构：%r = call i32 @foo(i32 %a)；%r = tail call i32 @foo(i32 %a)。
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



// 浮点转有符号整数指令：%i = fptosi float %f to i32。
class FPTOSIInst : public Instruction{
    public:
    FPTOSIInst(Value* v);
    FPTOSIInst(std::shared_ptr<IRType> irtype);
    void dumpIR() override;
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};

// 有符号整数转浮点指令：%f = sitofp i32 %i to float。
class SITOFPInst : public Instruction{
    public:
    SITOFPInst(Value* v);
    SITOFPInst(std::shared_ptr<IRType> irtype);
    void dumpIR() override;
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};

// float 扩展为 double，仅用于满足 putf 可变参数的 C ABI 默认提升。
class FPExtInst : public Instruction{
    public:
    explicit FPExtInst(Value* v);
    explicit FPExtInst(std::shared_ptr<IRType> irtype);
    void dumpIR() override;
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};


// 零扩展指令：%x32 = zext i1 %b to i32。
class ZextInst : public Instruction{
    public:
    ZextInst(Value* v);
    ZextInst(std::shared_ptr<IRType> irtype);
    void dumpIR() override;
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};

// 整数截断指令：%b = trunc i32 %x to i1。
class TruncInst : public Instruction{
    public:
    TruncInst(Value* v);
    TruncInst(std::shared_ptr<IRType> irtype);
    void dumpIR() override;
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};

// 符号扩展指令：%x32 = sext i1 %b to i32。
class SextInst : public Instruction{
    public:
    SextInst(Value* v);
    SextInst(std::shared_ptr<IRType> irtype);
    void dumpIR() override;
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};

// 条件选择指令根据 i1 条件在两个同类型值之间选择。
// IR 结构：%r = select i1 %c, i32 %a, i32 %b。
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

// PHI 指令按实际到达的前驱边选择一个输入值。
// IR 结构：%x = phi i32 [ %v1, %bb1 ], [ %v2, %bb2 ]。
class PhiInst : public Instruction{
    public:
    struct IncomingEdge {
        Value *V = nullptr;
        BasicBlock *Pred = nullptr;
    };
    PhiInst(std::shared_ptr<IRType> irtype);
    void dumpIR() override;
    // 添加一个“输入值、前驱块”对；平行边的顺序必须与前驱终结指令中的边顺序一致。
    void addIncoming(Value* val, BasicBlock* bb);
    // 每个入边项占用两个操作数位置：输入值在前，前驱块在后。
    unsigned getNumIncomingValues() const;
    Value* getIncomingValue(unsigned i) const;
    BasicBlock* getIncomingBlock(unsigned i) const;
    void setIncomingValue(unsigned i, Value* val);
    bool setIncomingBlock(unsigned i, BasicBlock *BB);
    unsigned countIncomingFromPred(BasicBlock *Pred) const;
    std::vector<IncomingEdge> collectIncomingEdges();
    // 重建时零个或一个入边项会折叠并删除原 PHI，成功后原指针立即失效。
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



// 位转换指令仅改变值的静态类型，不改变其位模式。
// IR 结构：%q = bitcast i32* %p to float*。
class BitCastInst : public Instruction {
public:
    BitCastInst(Value* v, std::shared_ptr<IRType> targetType);
    void dumpIR() override;
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};


// 常量对象通过各子类工厂创建，并在实现允许时复用已有实例。
class ConstantSystem : public Value{
    public:
    ConstantSystem()=delete;
    ConstantSystem(std::shared_ptr<IRType> irtype);
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
    // 按整数值和存储字节数返回驻留常量。
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

// 按模块、函数或基本块的现有线性顺序收集指定类型的可变指令指针。
template <typename InstT = Instruction, typename IRUnitT>
std::vector<InstT *> collectInstructions(IRUnitT &IRUnit) {
    static_assert(std::is_base_of_v<Instruction, InstT>,
                  "InstT 必须派生自 Instruction");
    using UnitT = std::remove_cv_t<std::remove_reference_t<IRUnitT>>;
    static_assert(std::is_same_v<UnitT, BasicBlock> ||
                      std::is_same_v<UnitT, Function> ||
                      std::is_same_v<UnitT, Module>,
                  "IRUnitT 必须是 BasicBlock、Function 或 Module");
    static_assert(!std::is_const_v<std::remove_reference_t<IRUnitT>>,
                  "collectInstructions 当前仅返回可变 Instruction 指针");

    std::vector<InstT *> Result;
    auto AppendInstructions = [&Result](auto &Self, auto &Current) -> void {
        using CurrentT =
            std::remove_cv_t<std::remove_reference_t<decltype(Current)>>;
        if constexpr (std::is_same_v<CurrentT, BasicBlock>) {
            for (Instruction *I : Current) {
                if (!I) continue;
                if constexpr (std::is_same_v<InstT, Instruction>) {
                    Result.push_back(I);
                } else if (auto *Typed = dynamic_cast<InstT *>(I)) {
                    Result.push_back(Typed);
                }
            }
        } else if constexpr (std::is_same_v<CurrentT, Function>) {
            for (BasicBlock *BB : Current) {
                if (BB) Self(Self, *BB);
            }
        } else {
            for (const auto &FunctionPtr : Current.getFunctions()) {
                if (Function *F = FunctionPtr.get()) Self(Self, *F);
            }
        }
    };

    AppendInstructions(AppendInstructions, IRUnit);
    return Result;
}
