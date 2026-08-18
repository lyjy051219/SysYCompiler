#pragma once
#include "AST.hpp"
#include "CFG.hpp"
#include "SymbolTable.hpp"
#include "TypeSystem.hpp"
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// IRGenerator将AST降低为非SSA IR
class IRGenerator final : public ASTVisitor {
private:
    struct LoopTargets {
        BasicBlock* continueTarget = nullptr;
        BasicBlock* breakTarget = nullptr;
    };

    //IR生成的核心状态：
    //当前函数和基本块
    Function* currentFunction = nullptr;
    BasicBlock* currentBlock = nullptr;
    //嵌套循环使用：每一层循环保存continue和break目标块，遇到嵌套循环时压栈，退出循环时弹栈
    std::vector<LoopTargets> loopStack;

    //符号表
    SymbolTable symbols;

    enum class InitializerRole {
        CompleteObject,
        ArrayElement,
    };

    // 左值解析后统一返回值或地址 + 源码对象类型。
    // isAddress 为 true 时，读取 first-class value 需生成 load；
    // 数组地址需要退化，tensor 子对象额外保留剩余形状。
    struct ResolvedLValue {
        Value* value = nullptr;
        std::shared_ptr<IRType> type;
        bool isAddress = false;
        bool isMutable = false;
        bool isTensor = false;
        std::vector<std::size_t> tensorShape;
        bool hasFlatTensorAddress = false;
    };

    // 源码 tensor 是带形状的内存值。
    struct SourceValueType {
        bool valid = false;
        bool isTensor = false;
        bool isVoid = false;
        ASTScalarKind elementKind = ASTScalarKind::Invalid;
        std::vector<std::size_t> shape;
    };

    struct TensorValue {
        Value* address = nullptr;
        std::shared_ptr<IRType> elementType;
        ASTScalarKind elementKind = ASTScalarKind::Invalid;
        std::vector<std::size_t> shape;
        bool hasFlatAddress = false;
        bool isTemporary = false;
    };

    // tensor 循环使用正的静态迭代次数，循环体至少执行一次。
    struct TensorCountedLoop {
        BasicBlock* preheader = nullptr;
        BasicBlock* header = nullptr;
        BasicBlock* body = nullptr;
        BasicBlock* exit = nullptr;
        PhiInst* index = nullptr;
        std::size_t tripCount = 0;
        std::string label;
        bool headerTested = true;
    };

    struct FunctionSpecialization {
        const FuncDef* source = nullptr;
        Function* function = nullptr;
        std::string irName;
        std::vector<SourceValueType> parameterTypes;
        SourceValueType returnType;
        bool isLowering = false;
        bool isComplete = false;
    };

    std::unordered_map<const Value*, TensorValue> tensorValues;
    std::map<std::string, const FuncDef*> sourceFunctions;
    std::map<std::string, FunctionSpecialization> functionSpecializations;
    FunctionSpecialization* currentSpecialization = nullptr;
    std::vector<SourceValueType> currentParameterTypes;
    AllocaInst* currentTensorReturnSlot = nullptr;
    ASTScalarKind currentTensorReturnElement = ASTScalarKind::Invalid;
    bool strictTensorInitializer = false;

    std::shared_ptr<IRType> lowerSourceType(
        const ASTType& type, bool allowVoid = false) const;
    std::shared_ptr<IRType> lowerScalarKind(ASTScalarKind kind) const;
    std::shared_ptr<IRType> makeTensorStorageType(
        const std::shared_ptr<IRType>& elementType,
        const std::vector<std::size_t>& shape) const;
    bool lowerStaticShape(const ArrayList& dimensions,
                          std::vector<std::size_t>& shape);
    std::size_t tensorElementCount(
        const std::vector<std::size_t>& shape) const;
    ASTScalarKind sourceScalarKind(
        const std::shared_ptr<IRType>& type) const;
    [[noreturn]] void reportTensorError(const std::string& message) const;
    bool isConstantPrimary(const BaseAST& node) const;
    bool isConstantExpression(const UnaryExp& node) const;
    template <typename OperandT>
    bool isConstantExpression(const BaseExp<OperandT>& node) const;
    bool isConstantInitializerSyntax(const InitVal& node) const;
    std::size_t countInitializerLeaves(const InitVal& node) const;

    // 根据声明所处位置、常量属性和数组形状生成对象及其初始化指令。
    template <typename DefinitionT>
    void lowerDefinition(DefinitionT& node,
                         const std::shared_ptr<IRType>& declaredType,
                         bool isConstant,
                         const std::vector<std::size_t>* tensorShape =
                             nullptr);
    void lowerParameter(FuncParam& node,
                        const SourceValueType* concreteType = nullptr);
    std::shared_ptr<IRType> lowerArrayType(
        const ArrayList& dimensions,
        std::shared_ptr<IRType> elementType);

    Value* lowerInitializer(const InitVal& node,
                            const std::shared_ptr<IRType>& expectedType,
                            InitializerRole role =
                                InitializerRole::CompleteObject);
    Value* lowerInitializerList(
        const InitValList& node,
        const std::shared_ptr<IRType>& expectedType);
    std::shared_ptr<IRType> initializerLeafType(
        std::shared_ptr<IRType> type) const;
    std::size_t initializerLeafCount(
        std::shared_ptr<IRType> type) const;
    void emitLocalArrayInitialization(AllocaInst* destination,
                                      const std::shared_ptr<IRType>& arrayType,
                                      Value* initializer);
    void emitDynamicInitializerStores(Value* destination,
                                      Initializer& initializer,
                                      std::vector<int>& indices);

    // 解析符号绑定和下标，区分常量值、普通对象地址与数组形参地址。
    ResolvedLValue lowerLValue(const LVal& node);
    Value* lowerRValue(const LVal& node);
    Value* decayArrayPointer(Value* pointer);
    Value* tryLowerConstantArrayElement(
        const SymbolTable::ObjectBinding& binding,
        const std::vector<Value*>& indices);
    bool classifyIndexedAccess(
        const std::shared_ptr<IRType>& rootType,
        bool isArrayParameter,
        const std::vector<Value*>& indices,
        std::vector<Value*>& aggregateIndices,
        std::shared_ptr<IRType>& selectedType) const;

    // 递归生成各优先级表达式，并对同级二元操作链执行类型统一。
    Value* lowerPrimary(const BaseAST& node,
                        TensorValue* destination = nullptr);
    Value* lowerExpression(const UnaryExp& node,
                           TensorValue* destination = nullptr);
    template <typename OperandT>
    Value* lowerExpression(const BaseExp<OperandT>& node,
                           TensorValue* destination = nullptr);
    Value* lowerExpression(const FuncCall& node,
                           TensorValue* destination = nullptr);

    BinaryInst::Operation lowerBinaryOperator(Type type) const;
    Value* emitBinary(Value* left, BinaryInst::Operation operation,
                      Value* right, TensorValue* destination = nullptr);
    Value* emitTensorBinary(Value* left,
                            BinaryInst::Operation operation,
                            Value* right,
                            TensorValue* destination = nullptr);
    Value* emitTensorUnaryMinus(Value* value,
                                TensorValue* destination = nullptr);
    Value* emitMatrixMultiply(Value* left, Value* right,
                              TensorValue* destination = nullptr);
    Value* allocateTensor(ASTScalarKind elementKind,
                          const std::vector<std::size_t>& shape);
    Value* tensorElementAddress(const TensorValue& value,
                                std::size_t flatIndex);
    Value* tensorElementAddress(const TensorValue& value,
                                Value* flatIndex);
    Value* tensorIndexedAddress(const TensorValue& value,
                                const std::vector<Value*>& indices,
                                std::size_t consumedDimensions);
    Value* tensorFlatAddress(const TensorValue& value);
    TensorValue makeFlatTensorValue(const TensorValue& value);
    Value* tensorStorageRoot(Value* address) const;
    bool canWriteTensorResultDirectly(
        const TensorValue& destination,
        const std::vector<TensorValue>& inputs,
        bool allowExactInPlace) const;
    Value* selectTensorResult(
        TensorValue* destination,
        ASTScalarKind elementKind,
        const std::vector<std::size_t>& shape,
        const std::vector<TensorValue>& inputs,
        bool allowExactInPlace);
    Value* materializeTensorResult(
        Value* source, TensorValue* destination);
    TensorCountedLoop beginTensorCountedLoop(
        std::size_t tripCount, const std::string& label,
        bool headerTested = true);
    void endTensorCountedLoop(const TensorCountedLoop& loop);
    void emitTensorLoopNest(
        const std::vector<std::size_t>& shape,
        std::size_t depth,
        std::vector<Value*>& coordinates,
        const std::string& label,
        const std::function<void(const std::vector<Value*>&)>& body);
    Value* registerTensor(Value* address, ASTScalarKind elementKind,
                          const std::vector<std::size_t>& shape,
                          bool hasFlatAddress,
                          bool isTemporary = false);
    const TensorValue* lookupTensor(Value* value) const;
    bool copyTensor(const TensorValue& destination,
                    const TensorValue& source);

    SourceValueType sourceTypeOfValue(Value* value) const;
    std::string makeSpecializationKey(
        const FuncDef& node,
        const std::vector<SourceValueType>& argumentTypes) const;
    FunctionSpecialization* getOrCreateFunctionSpecialization(
        const FuncDef& node,
        const std::vector<SourceValueType>& argumentTypes);
    void lowerFunctionSpecialization(FunctionSpecialization& specialization);
    Value* lowerSourceFunctionCall(
        const FuncCall& node, const FuncDef& function,
        std::vector<Value*> arguments,
        TensorValue* destination = nullptr);
    bool validateTensorParameter(const FuncParam& parameter,
                                 const SourceValueType& actualType);

    // 分别按用户函数签名和 SysY 运行库约定检查、转换调用实参。
    bool coerceCallArguments(const std::string& callee,
                             std::vector<Value*>& arguments);

    Value* toCondition(Value* value);
    Value* logicalNot(Value* value);
    Value* zeroValue(const std::shared_ptr<IRType>& type) const;

    // 为逻辑与、逻辑或建立短路控制流；丢弃结果时汇合到新的继续块。
    void emitCondition(const LOrExp& node, BasicBlock* entry,
                       BasicBlock* isTrue, BasicBlock* isFalse);
    void emitCondition(const LAndExp& node, BasicBlock* entry,
                       BasicBlock* isTrue, BasicBlock* isFalse);
    template <typename ConditionT>
    void emitDiscardedCondition(const ConditionT& node);

public:
    // 编译单元
    void visit(CompUnit& node) override;

    // 声明
    void visit(ConstDecl& node) override;
    void visit(VarDecl& node) override;
    void visit(ConstDefList& node) override;
    void visit(VarDefList& node) override;
    void visit(ConstDef& node) override;
    void visit(VarDef& node) override;

    // 函数
    void visit(FuncDef& node) override;
    void visit(FuncParam& node) override;
    void visit(FuncParamList& node) override;

    // 初始化
    void visit(InitValList& node) override;
    void visit(InitVal& node) override;

    // 语句
    void visit(Block& node) override;
    void visit(BlockItemList& node) override;
    void visit(AssignStmt& node) override;
    void visit(ExpStmt& node) override;
    void visit(IfStmt& node) override;
    void visit(WhileStmt& node) override;
    void visit(BreakStmt& node) override;
    void visit(ContinueStmt& node) override;
    void visit(ReturnStmt& node) override;

    // 表达式
    void visit(LVal& node) override;
    void visit(FuncCall& node) override;
    void visit(ArrayList& node) override;
    void visit(FuncRParamList& node) override;
    void visit(UnaryExp& node) override;
    void visit(MulExp& node) override;
    void visit(AddExp& node) override;
    void visit(RelExp& node) override;
    void visit(EqExp& node) override;
    void visit(LAndExp& node) override;
    void visit(LOrExp& node) override;
    void visit(ConValue<int>& node) override;
    void visit(ConValue<float>& node) override;
};
/*
IR生成主干图：
CompUnit
│
├── ConstDecl
│   └── ConstDefList
│       └── ConstDef
│           ├── ArrayList?
│           │   └── AddExp*
│           └── InitVal
│               ├── AddExp
│               └── InitValList
│                   └── InitVal*
│
├── VarDecl
│   └── VarDefList
│       └── VarDef
│           ├── ArrayList?
│           │   └── AddExp*
│           └── InitVal?
│               ├── AddExp
│               └── InitValList
│                   └── InitVal*
│
└── FuncDef
    ├── FuncParamList?
    │   └── FuncParam*
    │       └── ArrayList?
    │           └── AddExp*
    │
    └── Block
        └── BlockItemList?
            ├── ConstDecl
            ├── VarDecl
            ├── Block
            ├── AssignStmt
            │   ├── LVal
            │   │   └── ArrayList?
            │   │       └── AddExp*
            │   └── AddExp
            ├── ExpStmt
            │   └── AddExp?
            ├── IfStmt
            │   ├── LOrExp
            │   ├── thenBranch
            │   └── elseBranch?
            ├── WhileStmt
            │   ├── LOrExp
            │   └── body
            ├── BreakStmt
            ├── ContinueStmt
            └── ReturnStmt
                └── AddExp?

表达式：
LOrExp
└── LAndExp*
    └── EqExp*
        └── RelExp*
            └── AddExp*
                └── MulExp*
                    └── UnaryExp*
                        └── Primary
                            ├── AddExp
                            ├── LVal
                            │   └── ArrayList?
                            │       └── AddExp*
                            ├── FuncCall
                            │   └── FuncRParamList?
                            │       └── AddExp*
                            ├── ConValue<int>
                            └── ConValue<float>
*/
