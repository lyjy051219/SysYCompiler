#pragma once

// IRUtils 同时承载 CFG 所需的侵入式链表和依赖完整 CFG 的遍历工具
// 此处只引入基础部分，避免在 CFG 类型定义完成前展开遍历模板。
#define COMPILER_LIB_IRUTILS_BASE_ONLY
#include "IRUtils.hpp"
#undef COMPILER_LIB_IRUTILS_BASE_ONLY

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

//调用图：维护<函数，该函数调用的函数>
using FunctionCallGraph = std::unordered_map<Function *, std::vector<Function *>>;

//IR指令的基类
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
    Instruction(std::shared_ptr<IRType> irtype,InstType instType):User(std::move(irtype)){this->instType = instType;}
    Instruction(std::shared_ptr<IRType> irtype): User(std::move(irtype)){};
    virtual void dumpIR() = 0;


    Function* getParentFunction();
    static Function* getParentFunction(Instruction *I);
    // 将指令插入到Pos指令的前方或后方，位置无父块时失败。
    static bool insertBefore(Instruction *Pos, Instruction *NewI);
    static bool insertAfter(Instruction *Pos, Instruction *NewI);
    bool insertBefore(Instruction *NewI);
    bool insertAfter(Instruction *NewI);
    //判断当前指令是否直接属于指定 BasicBlock
    bool isInBlock(const BasicBlock *BB) const;

    //给定某个 Use，找到产生这个 Use 的指令位于哪个函数中
    static Function* getUserFunction(Use* U);
    //仅当两条指令属于同一基本块时，判断当前指令是否位于 Other 之前
    bool comesBefore(const Instruction *Other) const;
    //仅当两条指令属于同一基本块时，判断A是否位于B之前
    static bool comesBefore(const Instruction *A, const Instruction *B);
    Value *getUnaryOperand() const;
    //如果当前指令恰好有一个操作数，则返回该操作数，否则返回空指针
    static Value *getUnaryOperand(const Instruction *I);
    // 对支持的常量类型转换直接折叠，操作数或转换不受支持时返回空指针。
    static Value *foldConstantCast(
        InstType CastType, Value *Operand,
        const std::shared_ptr<IRType> &ResultType = nullptr);

    // 按值映射复制指令；无法构造合法副本时返回空指针。
    virtual Instruction* clone(const std::unordered_map<Value*, Value*>&) { return nullptr; }

    protected:
    //clone 指令时，如果某个操作数已经有克隆映射，就换成新值；否则保留原值
    static Value* remapVal(Value* v, const std::unordered_map<Value*, Value*>& vmap);
    //clone基本块时，如果某个操作数是clone基本块，就换成新值；否则保留原值
    static BasicBlock* remapBB(Value* v, const std::unordered_map<Value*, Value*>& vmap);
};

// 二元指令
// IR 结构：%r = add i32 %a, %b。
class BinaryInst : public Instruction{
    public:
    enum Operation{
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
    //判断操作码是否是比较操作码
    static bool isCompareOp(Operation Op);
    //判断该操作码是否是整数操作码（不包括浮点和比较）（UDIV，UREM，AND，OR，XOR，SHL，LSHR，ASHR）
    static bool isIntegerOnlyOp(Operation Op);
    static Instruction::InstType getInstType(Operation Op, TypeSystem OperandType);
    //判断该操作码是否是结合律操作码（ADD，MUL，AND，OR，XOR）
    static bool isAssociativeOp(Operation Op);
    //判断该操作码是否是交换律操作码（ADD，MUL，AND，OR，XOR，E，NE）
    static bool isCommutativeOp(Operation Op);
    //判断该操作码是否是可重新关联的操作码（ADD，MUL，AND，OR，XOR）
    static bool isReassociateOp(Operation Op);
    //返回比较操作码的反向比较操作码（E->NE，NE->E，GE->L，L->GE，LE->G，G->LE）
    static Operation negatePredicate(Operation Op);
    //交换左右操作数后，返回比较操作码的等价比较操作码（E->E，NE->NE，GE->LE，L->G，LE->GE，G->L）
    static Operation reversePredicate(Operation Op);
    //根据一个布尔值 Operand，构造一条新的 BinaryInst，计算这个布尔值的逻辑非 !Operand->Operand XOR true
    static BinaryInst *createBooleanNot(Value *Operand);
    // 折叠两个常量操作数；启用位宽规整时按目标整数位宽处理溢出和移位
    static Value *foldConstants(Operation Op, Value *LHS, Value *RHS,
                                bool NormalizeIntegerWidth = false);
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};


//Module：IR顶层
class Module final{
    private:
    //用户函数
    std::vector<std::unique_ptr<Function>> functions;
    //全局变量+全局常量
    std::vector<std::unique_ptr<Variable>> globalvars;
    //Runtime库函数+llvm的memcpy/memmove/memset函数
    std::set<std::string> declaredBuiltins;
    public:
    Module()=default;
    // 创建函数并由模块接管其所有权。
    Function& newFunction(TypeSystem returnType, std::string functionName);
    Function& newFunction(std::shared_ptr<IRType> returnType,
                          std::string functionName);
    std::vector<std::unique_ptr<Function>>& getFunctions();
    Function* getMainFunction();
    // 扫描当前函数所有权容器，避免函数删除后留下悬空名称索引。
    Function* findFunctionByName(const std::string &functionName) const;
    //获得唯一的Module实例
    static Module* getActiveModule();
    // 构建仅包含直接调用的函数调用图，重复调用保留重复边。
    FunctionCallGraph buildDirectCallGraph() const;
    // 从入口函数出发，在给定调用图上收集可达函数。
    static std::unordered_set<Function *> collectReachableFunctions(
        Function *Entry, const FunctionCallGraph &CallGraph);
    // 从函数的直接被调函数出发，判断是否能沿给定直接调用图回到该函数。
    // 查询至少经过一条调用边；调用边改变后，调用方必须传入重建后的图快照。
    static bool isFunctionRecursive(
        Function *Root, const FunctionCallGraph &CallGraph);
    std::vector<std::unique_ptr<Variable>>& getGlobalVar();
    // 按全局变量、函数的顺序输出整个模块的 LLVM IR。
    void dumpIR();
    //将运行时库函数加入declaredBuiltins
    void registerBuiltin(std::string ident) {
        declaredBuiltins.insert(ident);
    }
};

//多维数组初始化器，继承自Value和vector<Value*>，保存了多维数组的所有元素值
//Initializer本身是一个std::vector<Value*>，其中每个元素都是一个Value*，表示数组的一个元素值
class Initializer : public Value,public std::vector<Value*>{
    static bool isAllZero(Value *V);
    public:
    Initializer(std::shared_ptr<IRType> irtype);
    void dumpIR();
};

//函数形参：本身不持有任何操作数
class Param final : public Value {
    public:
    Param(std::shared_ptr<IRType> parameterType, std::string sourceName);
    bool isParam() final;
};

//全局变量/全局常量
// IR 结构：
//@g = global i32 0；
//@c = constant [4 x i32] [i32 1, i32 2, i32 3, i32 4]
/*@.G.matrix = global [2 x [3 x i32]] [
    [3 x i32] [i32 1, i32 2, i32 3],
    [3 x i32] [i32 4, i32 zeroinitializer, i32 zeroinitializer]
]*/
class Variable : public User{
    public:
    enum VarTag{
        GlobalVariable, // 全局变量
        GlobalConstant  // 全局常量
    };

    VarTag vartag;
    //判断是不是全局变量/全局常量
    virtual bool isGlobal() final;
    //检查全局变量是不是标量（int/float）
    bool isScalarGlobal();
    //传入数组多级索引Path，返回对应的常量元素值；若索引越界或类型不匹配则返回空指针
    Value *getConstantElement(const std::vector<int> &Path);
    static Value *getConstantElement(Variable *Source,
                                     const std::vector<int> &Path);

    Variable(VarTag variableTag, std::shared_ptr<IRType> objectType,
             std::string sourceName);
    void dumpIR();
    Value* getUsee();
};

class Function : public Value, public ChainList<Function,BasicBlock>{
    private:
    //函数形参
    std::vector<std::unique_ptr<Param>> params;
    public:
    // 按返回类型和名称创建函数，并自动建立入口基本块。
    Function(std::shared_ptr<IRType> returnType, std::string ident);
    // 将基本块加入函数的侵入式块链表。
    void pushBlock(BasicBlock*);
    //创建形参核心接口：为形参创建AllocaInst以及StoreInst，并将形参加入params vector
    AllocaInst* pushParamWithStackSlot(Param* parameterValue);
    std::vector<std::unique_ptr<Param>>& getParams();
    const std::vector<std::unique_ptr<Param>>& getParams() const;
    //获得形参vector的大小
    size_t getParamCount() const;
    Param *getParam(size_t Index) const;
    int getParamIndex(const Value *Param) const;
    bool eraseParam(size_t Index);
    //获得所有调用该函数的CallInst
    std::vector<CallInst *> getDirectCallSites() const;
    //检查该函数的所有Use是不是全都是直接调用
    bool hasOnlyDirectCallUses() const;
    //判断这个函数是不是main函数/Builtin函数/没有函数体的函数，这些函数特有，从而在优化阶段特殊处理
    bool hasReservedSignature() const;
    bool hasBlockIdent(const std::string &Name) const;
    std::string makeUniqueBlockIdent(const std::string &Base) const;
    // 从入口块出发计算可达块
    void collectReachableBlocks(std::unordered_set<BasicBlock *> &Reachable);
    // 返回限定块集合中非空且没有后继的出口基本块
    std::vector<BasicBlock *> findExitBlocks(
        const std::unordered_set<BasicBlock *> &Blocks) const;
    // 构造限定块集合内的反向CFG邻接表
    std::unordered_map<BasicBlock *, std::vector<BasicBlock *>>
    buildReverseCFG(
        const std::unordered_set<BasicBlock *> &Blocks) const;
    // 清空函数内所有指令的操作数关系，供批量销毁或回滚前使用
    void dropAllReferences();
    void dumpIR();
};

//BuildInFunction：由Runtime库函数和llvm的memcpy/memmove/memset函数组成
/*
memcpy：从源地址 src 开始，复制 len 个字节到目标地址 dest。（不允许重叠）
@llvm.memcpy.p0.p0.i32(ptr %dest, ptr %src, i32 %len, i1 %isvolatile)

memmove：从源地址 src 开始，复制 len 个字节到目标地址 dest。（允许重叠）
@llvm.memmove.p0.p0.i32(ptr %dest, ptr %src, i32 %len, i1 %isvolatile)

memset：从目标地址 dest 开始，将连续 len 个字节全部设置为同一个字节值 value。
@llvm.memset.p0.i32(ptr %dest, i8 %value, i32 %len, i1 %isvolatile)
*/
class BuildInFunction : public Value{
    BuildInFunction(std::shared_ptr<IRType>,std::string ident);
    public:
    struct Signature {
        //内置函数返回类型
        std::shared_ptr<IRType> returnType;
        //内置函数参数类型
        std::vector<std::shared_ptr<IRType>> parameterTypes;
        //指明第几个参数是指针，如果是指针参数则为true，否则为false
        std::vector<bool> opaquePointerParameters;
    };

    // 按名称返回已驻留的内建函数对象。
    static BuildInFunction* genBuildInFunction(std::string);
    //根据内置函数名查找对应的签名
    static std::optional<Signature> getSignature(const std::string &Name);
    //输出单个内置函数声明
    static bool dumpDeclaration(const std::string &Name);
    //输出所有内置函数声明
    static void dumpDefaultDeclarations();
    static bool isSupportedName(const std::string &Name);
    static bool isKnownRuntimeName(const std::string &Name);
    static bool isMemcpyName(const std::string &Name);
    static bool isMemmoveName(const std::string &Name);
    static bool isMemsetName(const std::string &Name);
    //是否是计时Runtime函数（starttime，stoptime）
    static bool isTimingName(const std::string &Name);
    //是否是标量IO函数：（getint，getch，getarray，putint，putch，putarray）
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
    virtual ~BasicBlock() = default;
    BasicBlock();
    void dumpIR();

//CFG查询接口：
    //后继按终结指令的操作数顺序返回，平行边保留重复项
    //平行边：br i1 %cond, label %same, label %same
    std::vector<BasicBlock*> getSuccessors();
    // 返回不重复的前驱基本块，不区分同一前驱产生的平行边
    std::vector<BasicBlock*> getPredecessors();
    Instruction *getTerminator();
    bool hasTerminator();
    BrInst* getBranchTerminator();
    static bool isCondBr(BrInst *Br);
    static bool isUncondBr(BrInst *Br);
    //统计该基本块到其后继的边数：正常为1，平行边为2
    int countEdgesToSucc(BasicBlock *To);
    //判断该基本块是否有多个不同的后继基本块
    bool hasMultipleDistinctSuccessors();
    //收集块开头连续的 PHI 指令
    std::vector<PhiInst*> collectPhiNodes();
    //判断基本块是否只包含若干 PHI 和一条终结指令
    bool onlyPhiAndTerminator();
    //先把该基本块中仍被其他块使用的非void指令值替换为undef，然后再删除该基本块
    void eraseBlockSafely();
    //清除基本块中的所有指令
    void clear();

//低层CFG改写接口：
//以下低层接口只改终结指令，不自动维护 PHI
    //将 Pred 中所有指向 OldSucc 的边全部改为 NewSucc
    static bool redirectBranchTarget(BasicBlock *Pred, BasicBlock *OldSucc, BasicBlock *NewSucc);
    //删除当前分支指令，并换成指向 Dst 的无条件分支
    static bool replaceCondBrWithUncond(BasicBlock *BB, BasicBlock *Dst);
    //用新指令替换当前块已有的终结指令
    bool replaceTerminator(Instruction *NewTerminator);
    static bool replaceTerminator(BasicBlock *BB,
                                  Instruction *NewTerminator);
    //从 Succ 开头的所有 PHI 中，删除来自 From 的指定数量的 incoming 项
    static void removeEdgeFromSuccPhis(BasicBlock *Succ, BasicBlock *From, int RemoveCount);
    //把 Succ 的所有 PHI 中，所有 OldPred 入边替换为 NewPred
    static void replaceIncomingPredInSuccPhis(BasicBlock *Succ, BasicBlock *OldPred, BasicBlock *NewPred);

//指令生成接口：
    // 生成带返回值的终结指令
    void genRetInst(Value* retVal);
    // 生成空返回终结指令：ret void
    void genRetInst();
    // 生成条件分支：br i1 %cond, label %ifTrue, label %ifFalse
    void genCondInst(Value* cond,BasicBlock* isTrue,BasicBlock* isFalse);
    // 生成无条件分支：br label %dest
    void genUnCondInst(BasicBlock* block);
    // 生成Alloca指令：%slot = alloca i32
    AllocaInst* genAllocaInst(std::shared_ptr<IRType> allocatedType);
    // 生成Load指令：%val = load i32, i32* %ptr。
    Value* genLoadInst(Value* v);
    // 生成Store指令：store i32 20, i32* %ptr。
    void genStoreInst(Value* val,Value* ptr);
    // 创建仅含基指针的 GEP：%1 = getelementptr inbounds [10 x i32], [10 x i32]* %arr
    //缺少后面的索引：, i32 0, i32 3
    Value* genGepInst(Value* ptr);
    // 生成二元指令
    Value* genBinaryInst(Value* A,BinaryInst::Operation op,Value* B);
    // 标量操作数全为常量时直接折叠；非常量路径插入给定基本块。
    static Value* genBinaryInst(BasicBlock*,Value*,BinaryInst::Operation,Value*);

    // 生成标量转换、函数调用和选择指令。
    Value* genSITOFPInst(Value* x);
    Value* genSITOFPInst(Value* x, std::shared_ptr<IRType> targetType);
    Value* genFPTOSIInst(Value* x);
    Value* genFPTOSIInst(Value* x, std::shared_ptr<IRType> targetType);
    Value* genCallInst(std::string ident,std::vector<Value*> params);
    Value* genZextInst(Value* x);
    Value* genZextInst(Value* x, std::shared_ptr<IRType> targetType);
    Value* genTruncInst(Value* x);
    Value* genTruncInst(Value* x, std::shared_ptr<IRType> targetType);
    Value* genSextInst(Value* x);
    Value* genSextInst(Value* x, std::shared_ptr<IRType> targetType);
    Value* genBitCastInst(Value* v, std::shared_ptr<IRType> targetType);
    Value* genSelectInst(Value* cond, Value* isTrue, Value* isFalse);
    //核心函数：将值转换为目标类型；必要时会在当前基本块中插入转换指令。
    Value *coerceValueToType(
        Value *Input, const std::shared_ptr<IRType> &TargetType);
    // 不生成 bitcast 的标量类型转换；无插入块时仍可完成常量折叠和 undef 传播。
    static Value *coerceValueToTypeWithoutBitCast(
        BasicBlock *InsertionBlock, Value *Input,
        const std::shared_ptr<IRType> &TargetType);
//基本块生成接口：
    //在当前基本块所属函数中创建新基本块，但没连接到CFG上（即没有生成终结指令）
    BasicBlock* genBlock();
    BasicBlock* genBlock(std::string ident);
//指令插入接口：
    bool insertBeforeTerminator(Instruction *I);
    bool insertAfterPhiNodes(Instruction *I);
    static bool insertBeforeTerminator(BasicBlock *BB, Instruction *I);
    static bool insertAfterPhiNodes(BasicBlock *BB, Instruction *I);
    // 将 Position 之后的指令按原顺序搬到目标块末尾，并同步转移终结边的 PHI 前驱。
    bool moveInstructionsAfter(Instruction *Position, BasicBlock *Destination);
    // 在 Position 之后切分基本块，新块自动加入同一函数并接管后半段指令。
    BasicBlock *splitBlockAfter(Instruction *Position);
};

// 返回指令
// IR 结构：ret i32 %val；ret void。
class RetInst : public Instruction{
    private:
    static std::shared_ptr<IRType>
    validateAndGetResultType(Value *ReturnValue);

    public:
    RetInst();
    RetInst(std::shared_ptr<IRType> irtype);
    RetInst(Value* retVal);
    Value *getReturnValue() const;
    void dumpIR() override;
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};

// 分支指令
// IR 结构：br i1 %cond, label %true, label %false；br label %dest。
class BrInst : public Instruction{
    private:
    bool isCond; // 为真时有两个后继，为假时只有一个后继。
    public:
    BrInst(Value*,BasicBlock*,BasicBlock*);
    BrInst(BasicBlock* bb);
    void dumpIR() override;
    //只读取isCond标记
    bool isConditional() const;
    //获取条件操作数，若是无条件分支则返回空指针
    Value *getCondition() const;
    unsigned getSuccessorCount() const;
    BasicBlock *getSuccessor(unsigned Index) const;
    bool setSuccessor(unsigned Index, BasicBlock *Successor);
    BasicBlock *getTrueSuccessor() const;
    BasicBlock *getFalseSuccessor() const;
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};

// 栈分配指令
// IR 结构：%a = alloca i32。
class AllocaInst : public Instruction{
    private:
    // AllocaInst 的公开构造参数统一表示被分配对象类型。
    static std::shared_ptr<IRType> makeResultType(
        const std::shared_ptr<IRType> &AllocatedType);

    public:
    explicit AllocaInst(std::shared_ptr<IRType> allocatedType);
    AllocaInst(std::string ident,
               std::shared_ptr<IRType> allocatedType);
    void dumpIR() override;
    std::shared_ptr<IRType> getAllocatedType() const;
    bool isArrayAllocation() const;
    static std::shared_ptr<IRType> getAllocatedType(const AllocaInst *AI);
    static bool isArrayAllocation(const AllocaInst *AI);
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;

};

// 内存写入指令
// IR 结构：store i32 3, i32* %a。
class StoreInst : public Instruction{
    private:
    static void validateOperands(Value *StoredValue, Value *Pointer);

    public:
    StoreInst(Value* val,Value* ptr);
    StoreInst(std::shared_ptr<IRType> irtype);
    Value* getValueOperand() const;
    Value* getPointerOperand() const;
    // 写入值类型与指针所指类型同形时，Store 完整覆盖该 pointee。
    bool fullyCoversItsPointer() const;
    void dumpIR() override;
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;

};

// 内存读取指令。
// IR 结构：%x = load i32, i32* %a。
class LoadInst : public Instruction{
    private:
    static std::shared_ptr<IRType>
    validateAndGetResultType(Value *Pointer);

    public:
    LoadInst(Value* v);
    LoadInst(std::shared_ptr<IRType> irtype);
    Value* getPointerOperand() const;
    void dumpIR() override;
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};

// GEP指令：地址计算指令
// IR 结构：%p = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i64 0, i64 3。
class GetElementPtrInst : public Instruction{
    private:
    //inBounds记录当前GEP是否承诺结果始终位于同一分配对象内。
    bool inBounds = true;
    static std::shared_ptr<IRType> getAggregateElementType(
        const std::shared_ptr<IRType> &Type);
    static bool tryDeriveResultType(
        Value *Base, const std::vector<Value *> &Indices,
        std::shared_ptr<IRType> &ResultType,
        std::vector<int64_t> *Strides = nullptr);
    static std::shared_ptr<IRType> requireResultType(
        Value *Base, const std::vector<Value *> &Indices);

    public:
    GetElementPtrInst(Value* v);
    GetElementPtrInst(Value* v,std::vector<Value*>& vv);
    GetElementPtrInst(std::shared_ptr<IRType> irtype);
    void dumpIR() override;
    //设置或查询GEP的inbounds语义标记。
    void setInBounds(bool InBounds);
    bool isInBounds() const;
    Value *getBasePointer() const;
    size_t getNumIndices() const;
    Value *getIndex(size_t Index) const;
    std::vector<Value *> getIndices() const;
    bool getBaseAndIndices(Value *&Base, std::vector<Value *> &Indices) const;
    static bool getBaseAndIndices(const GetElementPtrInst *GEP, Value *&Base,
                                  std::vector<Value *> &Indices);

    //判断所有索引是否都是编译期常量
    bool hasOnlyConstantIndices() const;
    // 按基指针和全部索引逐层剖析元素类型，重算 GEP 的结果指针类型。
    void updateType();
    //相比reset不改基指针和索引
    // 原子地重建基指针和索引操作数，并同步更新结果类型
    bool reset(Value *Base, const std::vector<Value *> &Indices);

    //返回最后一个索引每增加 1，地址变化多少字节
    std::optional<int64_t> getLastIndexStride();
    //把嵌套的 GEP 链展平成一个 Base 和一串索引
    bool flattenZeroBasedChain(Value *&Base, std::vector<Value *> &Indices);
    static bool flattenZeroBasedChain(Value *V, Value *&Base,
                                      std::vector<Value *> &Indices);
    //计算每一个索引对应的字节步长
    static bool getIndexStrides(Value *Base,
                                const std::vector<Value *> &Indices,
                                std::vector<int64_t> &Strides,
                                int64_t &FinalPointeeStride);
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};

// 函数调用指令
// IR 结构：%r = call i32 @foo(i32 %v)。
class CallInst : public Instruction{
    private:
    bool tailCall = false;
    static std::shared_ptr<IRType> validateAndGetResultType(
        Value *Callee, const std::vector<Value *> &Arguments);

    public:
    CallInst(Value*,std::vector<Value*>&,std::string="");
    CallInst(std::shared_ptr<IRType> irtype);
    void dumpIR() override;
    //TailCall:Call后面紧跟着Ret指令，且Call的返回值直接作为Ret的返回值
    bool isTailCall() const { return tailCall; }
    // 设置尾调用标记。
    void setTailCall(bool v);
    //返回第 0 个操作数：即被调用的函数
    Value *getCallee() const;
    //返回第 0 个操作数
    Function *getDirectCallee() const;
    //返回Callee是内置函数
    BuildInFunction *getBuiltinCallee() const;
    std::string getCalleeName() const;
    //返回第Index+1个操作数
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

// 浮点转有符号整数指令
// IR 结构：%i = fptosi float %f to i32。
class FPTOSIInst : public Instruction{
    public:
    FPTOSIInst(Value* v);
    FPTOSIInst(Value* v, std::shared_ptr<IRType> targetType);
    FPTOSIInst(std::shared_ptr<IRType> irtype);
    void dumpIR() override;
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};

// 有符号整数转浮点指令
// IR 结构：%f = sitofp i32 %i to float。
class SITOFPInst : public Instruction{
    public:
    SITOFPInst(Value* v);
    SITOFPInst(Value* v, std::shared_ptr<IRType> targetType);
    SITOFPInst(std::shared_ptr<IRType> irtype);
    void dumpIR() override;
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};


// 零扩展指令
// IR 结构：%x32 = zext i1 %b to i32。
class ZextInst : public Instruction{
    public:
    ZextInst(Value* v);
    ZextInst(Value* v, std::shared_ptr<IRType> targetType);
    ZextInst(std::shared_ptr<IRType> irtype);
    void dumpIR() override;
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};

// 整数截断指令
// IR 结构：%b = trunc i32 %x to i1。
class TruncInst : public Instruction{
    public:
    TruncInst(Value* v);
    TruncInst(Value* v, std::shared_ptr<IRType> targetType);
    TruncInst(std::shared_ptr<IRType> irtype);
    void dumpIR() override;
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};

// 符号扩展指令
// IR 结构：%x32 = sext i1 %b to i32。
class SextInst : public Instruction{
    public:
    SextInst(Value* v);
    SextInst(Value* v, std::shared_ptr<IRType> targetType);
    SextInst(std::shared_ptr<IRType> irtype);
    void dumpIR() override;
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};

// 条件选择指令
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

// PHI 指令
// IR 结构：%x = phi i32 [ %v1, %bb1 ], [ %v2, %bb2 ]。
class PhiInst : public Instruction{
    public:
    struct IncomingEdge {
        Value *V = nullptr;
        BasicBlock *Pred = nullptr;
    };
    PhiInst(std::shared_ptr<IRType> irtype);
    void dumpIR() override;
    //在当前PHI指令中添加一个IncomingEdge
    void addIncoming(Value* val, BasicBlock* bb);
    //获取当前PHI指令的IncomingEdge数量
    unsigned getNumIncomingValues() const;
    Value* getIncomingValue(unsigned i) const;
    BasicBlock* getIncomingBlock(unsigned i) const;
    void setIncomingValue(unsigned i, Value* val);
    bool setIncomingBlock(unsigned i, BasicBlock *BB);
    //统计来自指定前驱基本块的入边项数量，若前驱不存在则返回零
    unsigned countIncomingFromPred(BasicBlock *Pred) const;
    std::vector<IncomingEdge> collectIncomingEdges();
//PHI修改接口：
    //根据给定的IncomingEdge列表重建PHI指令
    //0->折叠为undef，1->折叠成唯一值，>1->创建新PHI
    static bool rebuildFromIncoming(PhiInst *Phi, const std::vector<IncomingEdge> &Incoming);
    //从 PHI 中删除来自指定前驱块的若干 incoming 项
    static bool removeIncomingByPred(PhiInst *Phi, BasicBlock *Pred, int RemoveCount);
    //精确删除来自 Pred 的第 Occurrence 个 incoming
    static bool removeIncomingEdge(PhiInst *Phi, BasicBlock *Pred,
                                   unsigned Occurrence);
    //把 PHI 中所有来自 OldPred 的 incoming 前驱都替换为 NewPred
    static bool replaceIncomingPred(PhiInst *Phi, BasicBlock *OldPred, BasicBlock *NewPred);
    //只替换来自 OldPred 的第 Occurrence 个 incoming 前驱
    static bool replaceIncomingPredEdge(PhiInst *Phi, BasicBlock *OldPred,
                                        BasicBlock *NewPred,
                                        unsigned Occurrence);
    //返回第一个来自 Pred 的 incoming value
    Value *getIncomingFromPred(BasicBlock *Pred);
    //判断两个 PHI 是否严格相同
    static bool areEquivalent(PhiInst *A, PhiInst *B);
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};

// 位转换指令
// IR 结构：%q = bitcast i32* %p to float*。
class BitCastInst : public Instruction {
private:
    static std::shared_ptr<IRType> validateAndGetResultType(
        Value *Source, const std::shared_ptr<IRType> &TargetType);

public:
    BitCastInst(Value* v, std::shared_ptr<IRType> targetType);
    void dumpIR() override;
    Instruction* clone(const std::unordered_map<Value*, Value*>& VMap) override;
};

// 常量系统
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
    int64_t val;
    ConstInt(int64_t val, std::shared_ptr<INTType> type);
    public:
    int64_t getVal() const;
    // 按整数值和存储字节数返回驻留常量。
    static ConstInt* newConstInt(int64_t val = 0, size_t size = 4);
    // 新代码使用显式整数类型，避免把字节数误当成位宽。
    static ConstInt* newConstIntForType(
        int64_t val, const std::shared_ptr<INTType> &type);
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
