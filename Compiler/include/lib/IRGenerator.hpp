#pragma once

#include "AST.hpp"
#include "CFG.hpp"
#include "TypeSystem.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// ASTVisitor 仍负责顶层、声明和语句的动态分派；表达式及其他会产生结果的
// 子树由显式返回值的 lowering 接口处理。这样父子节点之间不再依赖共享栈。
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

    enum class BindingKind {
        ConstantValue,
        ObjectAddress,
        ArrayParameter,
    };

    enum class InitializerRole {
        CompleteObject,
        ArrayElement,
    };

    struct Binding {
        BindingKind kind = BindingKind::ObjectAddress;
        std::shared_ptr<IRType> objectType;
        bool isMutable = true;
        Initializer* constantInitializer = nullptr;
    };

    // value 是常量/已退化指针或对象地址；objectType 明确描述当前对象，
    // isAddress 决定是否 load，避免再通过下标数量和指针层级猜测。
    struct LValueRef {
        Value* value = nullptr;
        std::shared_ptr<IRType> objectType;
        bool isAddress = false;
        bool isMutable = false;
    };

    GenerationContext context_;
    std::unordered_map<std::string, Function*> functions_;
    std::unordered_map<Value*, Binding> bindings_;

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

    LValueRef lowerLValue(const LVal& node);
    Value* lowerRValue(const LVal& node);
    Value* decayArrayPointer(Value* pointer);
    Value* tryLowerConstantArrayElement(
        const Binding& binding, const std::vector<Value*>& indices);

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

    std::vector<Value*> lowerArguments(const FuncRParamList& node);
    Value* emitCall(const FuncCall& node, std::vector<Value*> arguments);
    bool coerceCallArguments(const std::string& callee,
                             std::vector<Value*>& arguments);

    Value* coerce(Value* value, const std::shared_ptr<IRType>& targetType);
    Value* toCondition(Value* value);
    Value* logicalNot(Value* value);
    Value* zeroValue(const std::shared_ptr<IRType>& type) const;

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

    // 表达式兼容入口。IRGenerator 内部不会用这些 void Visitor 回传值。
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
