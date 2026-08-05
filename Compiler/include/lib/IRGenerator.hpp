#pragma once
#include "AST.hpp"
#include "CFG.hpp"
#include "SymbolTable.hpp"
#include "TypeSystem.hpp"
#include <cstddef>
#include <memory>
#include <string>
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

    // 左值解析后统一返回值或地址 + 源码对象类型
    // isAddress 为 true 时，读取标量右值需要生成 load；数组地址则需要退化
    struct ResolvedLValue {
        Value* value = nullptr;
        std::shared_ptr<IRType> type;
        bool isAddress = false;
        bool isMutable = false;
    };

    TypeSystem lowerSourceType(Type type, bool allowVoid = false) const;
    bool isConstantPrimary(const BaseAST& node) const;
    bool isConstantExpression(const UnaryExp& node) const;
    template <typename OperandT>
    bool isConstantExpression(const BaseExp<OperandT>& node) const;
    bool isConstantInitializerSyntax(const InitVal& node) const;

    // 根据声明所处位置、常量属性和数组形状生成对象及其初始化指令。
    template <typename DefinitionT>
    void lowerDefinition(DefinitionT& node, TypeSystem declaredType,
                         bool isConstant);
    void lowerParameter(FuncParam& node);
    std::shared_ptr<IRType> lowerArrayType(
        const ArrayList& dimensions,
        std::shared_ptr<IRType> elementType);

    Value* lowerInitializer(const InitVal& node,
                            const std::shared_ptr<IRType>& expectedType,
                            InitializerRole role =
                                InitializerRole::CompleteObject);
    Initializer* lowerInitializerList(
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

    // 递归生成各优先级表达式，并对同级二元操作链执行类型统一。
    Value* lowerPrimary(const BaseAST& node);
    Value* lowerExpression(const UnaryExp& node);
    template <typename OperandT>
    Value* lowerExpression(const BaseExp<OperandT>& node);
    Value* lowerExpression(const FuncCall& node);

    BinaryInst::Operation lowerBinaryOperator(Type type) const;
    Value* emitBinary(Value* left, BinaryInst::Operation operation,
                      Value* right);

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
