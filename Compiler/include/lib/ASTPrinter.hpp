#pragma once
#include "AST.hpp"

// 将语法树输出为便于人工检查的类 SysY 源码文本。
class ASTPrinter : public ASTVisitor {
public:
    // 编译单元
    void visit(CompUnit &node) override;
    // 声明
    void visit(ConstDecl &node) override;
    void visit(VarDecl &node) override;
    void visit(ConstDefList &node) override;
    void visit(VarDefList &node) override;
    void visit(ConstDef &node) override;
    void visit(VarDef &node) override;
    // 函数
    void visit(FuncDef &node) override;
    void visit(FuncParam &node) override;
    void visit(FuncParamList &node) override;
    // 初始化
    void visit(InitValList &node) override;
    void visit(InitVal &node) override;

    // 语句
    void visit(Block &node) override;
    void visit(BlockItemList &node) override;
    void visit(AssignStmt &node) override;
    void visit(ExpStmt &node) override;
    void visit(IfStmt &node) override;
    void visit(WhileStmt &node) override;
    void visit(BreakStmt &node) override;
    void visit(ContinueStmt &node) override;
    void visit(ReturnStmt &node) override;
    // 表达式
    void visit(LVal &node) override;
    void visit(FuncCall &node) override;
    void visit(ArrayList &node) override;
    void visit(FuncRParamList &node) override;
    // 表达式类
    void visit(UnaryExp &node) override;
    void visit(MulExp &node) override;
    void visit(AddExp &node) override;
    void visit(RelExp &node) override;
    void visit(EqExp &node) override;
    void visit(LAndExp &node) override;
    void visit(LOrExp &node) override;
    // 常量
    void visit(ConValue<int> &node) override;
    void visit(ConValue<float> &node) override;
};
