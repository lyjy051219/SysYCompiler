#pragma once
#include "AST.hpp"
#include "BaseManager.hpp"
#include "TypeSystem.hpp"
#include "SymbolTable.hpp"
#include "CFG.hpp"
#include "Tool.hpp"
#include <vector>
#include <string>
#include <memory>
#include <stack>
/*所有关于上下文的东西都采用了栈的方式进行传入 */

class ContextType{
private:
    //这里存储的Type是IR生成后的type
    std::stack<TypeSystem> typestack;
public:
    void push_type(TypeSystem ty){ typestack.push(ty); }
    void pop_type(){  typestack.pop(); }
    TypeSystem getCurrentType() const{
        if(typestack.empty()){
            std::cerr<<"typestack操作不合法\n";
            assert(0);
        }
        return typestack.top();
    }
};

//这是IR生成相关类
class IRGenerator : public ASTVisitor{
private:
    bool enterUnaryExp = false;
    //这个Value stack是专门用来传递创建好的initval然后用于上一层addUse的
    std::stack<Value*> valueStack;
    //用来传递上下文的TypeSystem
    ContextType contextType;
    //这个Function stack用来处理当前用到的Function
    std::stack<Function*> functionStack;
    //当前正在构建的基本块
    //由于基本块不会嵌套，所以设立成指针即可，不需要用到栈
    BasicBlock* currentBasicBlock = nullptr;
    //condBasicBlock:遇到continue跳到条件基本块
    BasicBlock* continueBB = nullptr;  // 条件判断块
    //endBasicBlock：遇到break跳到循环结束的块
    BasicBlock* breakBB = nullptr;   // 循环结束块

    std::stack<std::vector<Value*>> argsStack; //实参传递时使用的vector

    std::stack<std::shared_ptr<IRType>> arrayTypeStack;

    std::shared_ptr<IRType> currentIRType = nullptr;


    TypeSystem ASTCastIRType(Type t); //将变量/常量类型映射为float/int
    Value* popValue();
    void pushValue(Value* val);
    void getLValPointer(LVal& node);
    void getOperand(LOrExp &node, BasicBlock* block, BasicBlock* is_true, BasicBlock* is_false);
    void getOperand(LAndExp &node, BasicBlock* block, BasicBlock* is_true, BasicBlock* is_false);
    template<typename T>
    Value* getOperand(const std::list<std::unique_ptr<T>>& operands,const std::list<Type>& ops);
    Value* getOperand(EqExp& node,BasicBlock* block);
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
template<typename T>
Value* IRGenerator::getOperand(const std::list<std::unique_ptr<T>>& operands,const std::list<Type>& ops){
    if(enterUnaryExp == true){
        enterUnaryExp = false;
        operands.front()->accept(*this);
        enterUnaryExp = true;
        Value* ptr = popValue();

            for(auto i=ops.rbegin();i!=ops.rend();i++){
                switch (*i)
                {
                case SY_NOT:
                    switch(ptr->getIRType()->getEnum())
                    {
                        case IR_INT:{
                            auto t = std::dynamic_pointer_cast<INTType>(ptr->getIRType());
                            if(t->getIsBool())
                                ptr=BasicBlock::genBinaryInst(currentBasicBlock,ptr,BinaryInst::E,ConstBool::newConstBool());
                            else
                                ptr=BasicBlock::genBinaryInst(currentBasicBlock,ptr,BinaryInst::E,ConstInt::newConstInt(0));
                            break;}
                        case IR_FLOAT:{ptr=BasicBlock::genBinaryInst(currentBasicBlock,ConstFloat::newConstFloat(),BinaryInst::E,ptr);break;}
                        case IR_POINTER:{ptr=BasicBlock::genBinaryInst(currentBasicBlock,ConstPtr::newConstPtr(ptr->getIRType()),BinaryInst::E,ptr);break;}
                        default:assert(0);
                    }
                case SY_ADD:
                    break;
                case SY_SUB:
                    if(ptr->getIRType()->getEnum()==IR_INT)
                        ptr=BasicBlock::genBinaryInst(currentBasicBlock,ConstInt::newConstInt(0),BinaryInst::SUB,ptr);
                    else
                        ptr=BasicBlock::genBinaryInst(currentBasicBlock,ConstFloat::newConstFloat(),BinaryInst::SUB,ptr);
                    break;
                default:
                    assert(0);
                    std::cerr<<"Wrong Operator in BinaryExp\n";
                    break;
                }
            }
            return ptr;

    }else{
    auto i = operands.begin();
    (*i)->accept(*this);
    auto oper = popValue();
    for(auto &j:ops){
        i++;
        (*i)->accept(*this);
        auto another = popValue();
        BinaryInst::Operation opcode;
        switch(j){
                case SY_ADD:opcode=BinaryInst::ADD;break;
                case SY_SUB:opcode=BinaryInst::SUB;break;
                case SY_DIV:opcode=BinaryInst::DIV;break;
                case SY_MUL:opcode=BinaryInst::MUL;break;
                case SY_EQ:opcode=BinaryInst::E;break;
                case SY_AND:opcode=BinaryInst::AND;break;
                case SY_GREAT:opcode=BinaryInst::G;break;
                case SY_LESS:opcode=BinaryInst::L;break;
                case SY_GREATEQ:opcode=BinaryInst::GE;break;
                case SY_LESSEQ:opcode=BinaryInst::LE;break;
                case SY_MOD:opcode=BinaryInst::MOD;break;
                case SY_NOTEQ:opcode=BinaryInst::NE;break;
                case SY_OR:opcode=BinaryInst::OR;break;
                default:
                    std::cerr<<"No such Opcode\n";
                    assert(0);
        }
        oper = BasicBlock::genBinaryInst(currentBasicBlock,oper,opcode,another);
    }
    return oper;
    }
}