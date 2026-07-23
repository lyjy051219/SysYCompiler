#pragma once

#include "AST.hpp"
#include "CFG.hpp"
#include "SymbolTable.hpp"
#include "TypeSystem.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

// 顶层节点通过 ASTVisitor 动态分派；产生值的子树由显式返回值接口递归生成 IR。
class IRGenerator final : public ASTVisitor {
private:
    struct LoopTargets {
        BasicBlock* continueTarget = nullptr;
        BasicBlock* breakTarget = nullptr;
    };

    struct GenerationContext {
        Function* function = nullptr;
        BasicBlock* block = nullptr;
        std::vector<LoopTargets> loops;

        bool inFunction() const noexcept { return function != nullptr; }
    };

    enum class InitializerRole {
        CompleteObject,
        ArrayElement,
    };

    // value 可表示常量、退化后的指针或对象地址，objectType 描述源码对象类型。
    // isAddress 指示读取右值时是否需要生成内存读取指令。
    struct LValueRef {
        Value* value = nullptr;
        std::shared_ptr<IRType> objectType;
        bool isAddress = false;
        bool isMutable = false;
    };

    GenerationContext context;
    // 源码符号只在 IR 生成期间存活，不随 Module 进入后续优化阶段。
    SymbolTable symbols;

    // 将源码类型转换为 IR 类型，并判断表达式或初始化式能否在编译期求值。
    BasicBlock& currentBlock() const;
    TypeSystem lowerSourceType(Type type, bool allowVoid = false) const;
    std::shared_ptr<IRType> lowerScalarType(Type type) const;
    bool isConstantPrimary(const HasOperand& node) const;
    bool isConstantExpression(const UnaryExp& node) const;
    bool isConstantExpression(const MulExp& node) const;
    bool isConstantExpression(const AddExp& node) const;
    bool isConstantExpression(const RelExp& node) const;
    bool isConstantExpression(const EqExp& node) const;
    template <typename OperandT>
    bool hasConstantOperands(const BaseExp<OperandT>& node) const;
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
    LValueRef lowerLValue(const LVal& node);
    Value* lowerRValue(const LVal& node);
    Value* decayArrayPointer(Value* pointer);
    Value* tryLowerConstantArrayElement(
        const SymbolTable::ObjectBinding& binding,
        const std::vector<Value*>& indices);

    // 递归生成各优先级表达式，并对同级二元操作链执行类型统一。
    Value* lowerPrimary(const HasOperand& node);
    Value* lowerExpression(const UnaryExp& node);
    Value* lowerExpression(const MulExp& node);
    Value* lowerExpression(const AddExp& node);
    Value* lowerExpression(const RelExp& node);
    Value* lowerExpression(const EqExp& node);
    Value* lowerExpression(const FuncCall& node);
    Value* lowerExpression(const ConValue<int>& node);
    Value* lowerExpression(const ConValue<float>& node);

    template <typename OperandT>
    Value* lowerBinaryChain(const BaseExp<OperandT>& node);
    BinaryInst::Operation lowerBinaryOperator(Type type) const;
    Value* emitBinary(Value* left, BinaryInst::Operation operation,
                      Value* right);

    // 分别按用户函数签名和 SysY 运行库约定检查、转换调用实参。
    std::vector<Value*> lowerArguments(const FuncRParamList& node);
    Value* emitCall(const FuncCall& node, std::vector<Value*> arguments);
    bool coerceCallArguments(const std::string& callee,
                             std::vector<Value*>& arguments);

    Value* coerce(Value* value, const std::shared_ptr<IRType>& targetType);
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

    // 这些入口仅兼容通用访问器；生成器内部通过显式返回值接口取得表达式结果。
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
