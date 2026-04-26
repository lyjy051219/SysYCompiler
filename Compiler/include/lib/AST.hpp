#pragma once
#include <memory>
#include <string>
#include <vector>
#include <list>
#include <iostream>

//本文件是用来创建AST的
//使用解耦的办法，将IR生成的部分解耦交给Visitor来做
enum Type{
    SY_INT,
    SY_FLOAT,
    SY_VOID,

    SY_ADD,
    SY_SUB,
    SY_MUL,
    SY_MOD,
    SY_DIV,

    SY_GREAT,
    SY_GREATEQ,
    SY_LESS,
    SY_LESSEQ,
    SY_EQ,
    SY_NOTEQ,

    SY_OR,
    SY_AND,
    SY_NOT,

    SY_ASSIGN
};

//前向声明，防止前面编译的用到后面编译的类从而报错
class ConstDefList;
class VarDefList;
class ConstDef;
class VarDef;
class ArrayList;
class InitVal;
class BlockItemList;
class Block;
class LVal;
class FuncRParamList;
class FuncParamList;
class FuncCall;
template<typename T> class BaseExp;
template<typename T> class ConValue;
class CompUnit;
class ConstDecl;
class VarDecl;
class FuncDef;
class FuncParam;
class InitValList;
class AssignStmt;
class ExpStmt;
class IfStmt;
class WhileStmt;
class BreakStmt;
class ContinueStmt;
class ReturnStmt;
class UnaryExp;
class MulExp;
class AddExp;
class RelExp;
class EqExp;
class LAndExp;
class LOrExp;


//visitor基类
class ASTVisitor {
public:
    virtual ~ASTVisitor() = default;

    // 编译单元
    virtual void visit(CompUnit &node) = 0;
    // 声明
    virtual void visit(ConstDecl &node) = 0;
    virtual void visit(VarDecl &node) = 0;
    virtual void visit(ConstDefList &node) = 0;
    virtual void visit(VarDefList &node) = 0;
    virtual void visit(ConstDef &node) = 0;
    virtual void visit(VarDef &node) = 0;
    // 函数
    virtual void visit(FuncDef &node) = 0;
    virtual void visit(FuncParam &node) = 0;
    virtual void visit(FuncParamList &node) = 0;
    // 初始化
    virtual void visit(InitValList &node) = 0;
    virtual void visit(InitVal &node) = 0;
    // 语句
    virtual void visit(Block &node) = 0;
    virtual void visit(BlockItemList &node) = 0;
    virtual void visit(AssignStmt &node) = 0;
    virtual void visit(ExpStmt &node) = 0;
    virtual void visit(IfStmt &node) = 0;
    virtual void visit(WhileStmt &node) = 0;
    virtual void visit(BreakStmt &node) = 0;
    virtual void visit(ContinueStmt &node) = 0;
    virtual void visit(ReturnStmt &node) = 0;
    // 表达式
    virtual void visit(LVal &node) = 0;
    virtual void visit(FuncCall &node) = 0;
    virtual void visit(ArrayList &node) = 0;
    virtual void visit(FuncRParamList &node) = 0;
    virtual void visit(UnaryExp &node) = 0;
    virtual void visit(MulExp &node) = 0;
    virtual void visit(AddExp &node) = 0;
    virtual void visit(RelExp &node) = 0;
    virtual void visit(EqExp &node) = 0;
    virtual void visit(LAndExp &node) = 0;
    virtual void visit(LOrExp &node) = 0;
    virtual void visit(ConValue<int> &node) = 0;
    virtual void visit(ConValue<float> &node) = 0;

};


//基础类：BaseAST，Stmt，HasOperand
//所有AST节点的基类
class BaseAST{
public:
    virtual ~BaseAST() = default;
    virtual void accept(ASTVisitor &visitor) = 0;
};
//语句基类
class Stmt : public BaseAST{
public:
};
//有表达式基类
class HasOperand : public BaseAST{
public:
};

//编译单元：由若干个Decl和FuncDef实现
//由于我在bison中写的都是左递归，所以这里用vector+智能指针实现
class CompUnit : public BaseAST{
private:
  std::vector<std::unique_ptr<BaseAST>> items;
public:
  CompUnit(BaseAST * item){ items.push_back(std::unique_ptr<BaseAST>(item)); }
  void push_back(BaseAST* item){ items.push_back(std::unique_ptr<BaseAST>(item)); }
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }
  //添加一个访问items的函数，用来访问并遍历私有成员
  const std::vector<std::unique_ptr<BaseAST>>& get_items() const {return items;}
};

//声明和定义相关
//常量声明
//这里需要理解一下声明语句也是语句的一部分，所以直接继承Stmt
class ConstDecl : public Stmt{
  //ConstDecl包括一个type和一个ConstDefList，这个ConstDefList用智能指针实现
  private:
    Type type;
    std::unique_ptr<ConstDefList> def_list;
  public:
    ConstDecl(Type t,ConstDefList* d): type(t), def_list(std::unique_ptr<ConstDefList>(d)) {}
    void accept(ASTVisitor &visitor) override { visitor.visit(*this); }
    Type get_type() const {return type;}
    const std::unique_ptr<ConstDefList>& get_ConstDefList() const{ return def_list; }
};
//变量声明
class VarDecl : public Stmt{
  private:
    Type type;
    std::unique_ptr<VarDefList>def_list;
  public:
    VarDecl(Type t,VarDefList* d): type(t), def_list(std::unique_ptr<VarDefList>(d)) {}
    void accept(ASTVisitor &visitor) override { visitor.visit(*this); }
    Type get_type() const {return type;}
    const std::unique_ptr<VarDefList>& get_VarDefList() const{ return def_list; }

};
//常量定义列表
class ConstDefList : public Stmt{
private:
  std::vector<std::unique_ptr<ConstDef>> cdl;
public:
  ConstDefList(ConstDef* c){ cdl.push_back(std::unique_ptr<ConstDef>(c)); }
  void push_back(ConstDef* c){ cdl.push_back(std::unique_ptr<ConstDef>(c)); }
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }
  const std::vector<std::unique_ptr<ConstDef>>& get_defs() const { return cdl; }

};
//变量定义列表
class VarDefList : public Stmt{
  private:
    std::vector<std::unique_ptr<VarDef>> vdl;
  public:
  VarDefList(VarDef* v){ vdl.push_back(std::unique_ptr<VarDef>(v)); }
  void push_back(VarDef* v){ vdl.push_back(std::unique_ptr<VarDef>(v)); }
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }
  const std::vector<std::unique_ptr<VarDef>>& get_defs() const { return vdl; }
};

//变量定义
class VarDef : public Stmt{
  private:
    std::string ident;
    std::unique_ptr<ArrayList> exp_list;
    std::unique_ptr<InitVal> init_val;
  public:
    VarDef(const std::string& id): ident(id), exp_list(nullptr), init_val(nullptr) {}
   /* VarDef(const std::string& id, InitVal* iv): ident(id), exp_list(nullptr), init_val(iv) {}
    VarDef(const std::string& id, ArrayList* el): ident(id), exp_list(el), init_val(nullptr) {}*/
    VarDef(const std::string& id, ArrayList* el, InitVal* iv): ident(id), exp_list(std::unique_ptr<ArrayList>(el)), init_val(std::unique_ptr<InitVal>(iv)) {}
    void accept(ASTVisitor &visitor) override { visitor.visit(*this); }
    std::string get_ident()const { return ident; }
    bool has_array() const { return exp_list != nullptr; }
    ArrayList* get_array() const { return exp_list.get(); }
    bool has_initval() const { return init_val != nullptr; }
    InitVal* get_initval() const { return init_val.get(); }
};
//常量定义
class ConstDef : public Stmt{
private:
    std::string ident;
    std::unique_ptr<ArrayList> exp_list;
    std::unique_ptr<InitVal> init_val;
public:
   // ConstDef(const std::string& id, InitVal* iv): ident(id), exp_list(nullptr), init_val(std::unique_ptr<InitVal>(iv)){}
    ConstDef(const std::string& id, ArrayList* el, InitVal* iv): ident(id), exp_list(std::unique_ptr<ArrayList>(el)), init_val(std::unique_ptr<InitVal>(iv)){}
    void accept(ASTVisitor &visitor) override { visitor.visit(*this); }
    std::string get_ident()const { return ident; }
    bool has_array() const { return exp_list != nullptr; }
    ArrayList* get_array() const { return exp_list.get(); }
    bool has_initval() const { return init_val != nullptr; }
    InitVal* get_initval() const { return init_val.get(); }
};

//函数相关
//函数定义
class FuncDef : public BaseAST{
  private:
  Type type;
  std::string ident;
  std::unique_ptr<FuncParamList> params; //函数形参列表
  std::unique_ptr<Block> funcBody; //函数体
  public:
  //FuncDef(Type t,std::string id,Block* b):type(t),ident(id),params(nullptr),funcBody(b){}
  FuncDef(Type t,std::string id,FuncParamList* p,Block* b):type(t),ident(id),params(std::unique_ptr<FuncParamList>(p)),funcBody(std::unique_ptr<Block>(b)){}
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }

   Type get_type() const { return type; }
   const std::string& get_ident() const { return ident; }
   const std::unique_ptr<FuncParamList>& get_params() const { return params; }
   const std::unique_ptr<Block>& get_funcBody() const { return funcBody; }
};
//函数形参
class FuncParam : public BaseAST{
private:
  Type type;
  std::string ident;
  bool has_empty_brackets_;  // 是否有空方括号 []
  std::unique_ptr<ArrayList> dimension_list;  // 更清晰的命名
public:
 // 规则1：普通参数 Type IDENT
  FuncParam(Type t, std::string id):
      type(t), ident(id), has_empty_brackets_(false), dimension_list(nullptr) {}

  // 规则2：空方括号参数 Type IDENT []
  FuncParam(Type t, std::string id, bool has_empty_brackets):
      type(t), ident(id), has_empty_brackets_(has_empty_brackets), dimension_list(nullptr) {}

  // 规则3和规则4：带维度参数
  FuncParam(Type t, std::string id, bool has_empty_brackets, ArrayList* dim_list):
      type(t), ident(id), has_empty_brackets_(has_empty_brackets),
      dimension_list(std::unique_ptr<ArrayList>(dim_list)) {}

  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }

  Type get_type() const { return type; }
  const std::string& get_ident() const { return ident; }
  bool has_empty_brackets() const { return has_empty_brackets_; }  // 是否有 []
  bool has_dimension() const { return dimension_list != nullptr; }  // 是否有具体维度
  const std::unique_ptr<ArrayList>& get_exp_list() const { return dimension_list; }
};
//函数形参列表，函数定义使用的
class FuncParamList : public BaseAST{
  private:
  std::vector<std::unique_ptr<FuncParam>> fpl;
  public:
  FuncParamList(FuncParam* f){ fpl.push_back(std::unique_ptr<FuncParam>(f)); }
  void push_back(FuncParam* f){ fpl.push_back(std::unique_ptr<FuncParam>(f)); }
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }

  const std::vector<std::unique_ptr<FuncParam>>& get_params() const { return fpl; }
};

//初始化相关--包括常量初始化和变量初始化
//初始化列表
class InitValList : public BaseAST{
  private:
  std::vector<std::unique_ptr<InitVal>> ivl;
  public:
  InitValList(InitVal* iv){ ivl.push_back(std::unique_ptr<InitVal>(iv)); }
  void push_back(InitVal* iv){ ivl.push_back(std::unique_ptr<InitVal>(iv)); }
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }

  const std::vector<std::unique_ptr<InitVal>>& get_initVals() const { return ivl; }
};
//初始化值
class InitVal : public BaseAST{
  private:
  std::unique_ptr<BaseAST> val;
  public:
  InitVal(): val(nullptr) {}
  InitVal(BaseAST* iv): val(std::unique_ptr<BaseAST>(iv)) {}
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }

  const std::unique_ptr<BaseAST>& get_value() const { return val; }
};


//下面是语句的相关子类
//语句块--即代码块
class Block : public Stmt{
  private:
  std::unique_ptr<BlockItemList> items;
  public:
  Block(BlockItemList* it): items(std::unique_ptr<BlockItemList>(it)) {}
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }

  const std::unique_ptr<BlockItemList>& get_items() const { return items; }
};
//语句块列表：语句块内部的声明和语句列表
class BlockItemList : public Stmt{
  private:
  std::vector<std::unique_ptr<Stmt>> st;
  public:
  BlockItemList(Stmt* s){ st.push_back(std::unique_ptr<Stmt>(s)); }
  void push_back(Stmt* s){ st.push_back(std::unique_ptr<Stmt>(s)); }
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }

  const std::vector<std::unique_ptr<Stmt>>& get_stmts() const { return st; }
};
//赋值语句
class AssignStmt : public Stmt{
  private:
  std::unique_ptr<LVal> lv;
  std::unique_ptr<AddExp> ae;
  public:
  AssignStmt(LVal* lv,AddExp* ae): lv(std::unique_ptr<LVal>(lv)), ae(std::unique_ptr<AddExp>(ae)) {}
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }

  const std::unique_ptr<LVal>& get_lval() const { return lv; }
  const std::unique_ptr<AddExp>& get_exp() const { return ae; }
};
//表达式语句
class ExpStmt : public Stmt{
  private:
  std::unique_ptr<AddExp> ae;
  public:
  ExpStmt(AddExp* ae): ae(std::unique_ptr<AddExp>(ae)) {}
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }

  const std::unique_ptr<AddExp>& get_exp() const { return ae; }
};
//if语句
class IfStmt : public Stmt{
  private:
  std::unique_ptr<LOrExp> condition;
  std::unique_ptr<Stmt> if_st,else_st;
  public:
  IfStmt(LOrExp* condition,Stmt* if_st):condition(std::unique_ptr<LOrExp>(condition)),if_st(std::unique_ptr<Stmt>(if_st)),else_st(nullptr){}
  IfStmt(LOrExp* condition,Stmt* if_st,Stmt* else_st):condition(std::unique_ptr<LOrExp>(condition)),if_st(std::unique_ptr<Stmt>(if_st)),else_st(std::unique_ptr<Stmt>(else_st)){}
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }

  const std::unique_ptr<LOrExp>& get_condition() const { return condition; }
  const std::unique_ptr<Stmt>& get_if_stmt() const { return if_st; }
  bool has_else() const { return else_st != nullptr; }
  const std::unique_ptr<Stmt>& get_else_stmt() const { return else_st; }
};
//while语句
class WhileStmt : public Stmt{
  private:
  std::unique_ptr<LOrExp> condition;
  std::unique_ptr<Stmt> while_st;
  public:
  WhileStmt(LOrExp* condition,Stmt* while_st):condition(std::unique_ptr<LOrExp>(condition)),while_st(std::unique_ptr<Stmt> (while_st)){}
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }

  const std::unique_ptr<LOrExp>& get_condition() const { return condition; }
  const std::unique_ptr<Stmt>& get_while_stmt() const { return while_st; }
};
//break语句
class BreakStmt : public Stmt{
  public:
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }

};
//continue语句
class ContinueStmt : public Stmt{
  public:
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }
};//--这里注意，在现在编写的这个阶段，break和continue两个好像没什么区别
//return语句
class ReturnStmt : public Stmt{
  private:
  std::unique_ptr<AddExp> ret_val;
  public:
  ReturnStmt(): ret_val(nullptr) {}
  ReturnStmt(AddExp* ae): ret_val(std::unique_ptr<AddExp>(ae)) {}
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }

  bool has_return_value() const { return ret_val != nullptr; }
  const std::unique_ptr<AddExp>& get_return_value() const { return ret_val; }

};


//左值表达式：变量，数组元素等可以出现在=左边的表达式
class LVal : public HasOperand{
  private:
  std::string ident;
  std::unique_ptr<ArrayList> arr;
  public:
  LVal(std::string id):ident(id),arr(nullptr){}
  LVal(std::string id,ArrayList* e):ident(id),arr(std::unique_ptr<ArrayList>(e)){}
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }

  const std::string& get_ident() const { return ident; }
  bool has_array() const { return arr != nullptr; }
  const std::unique_ptr<ArrayList>& get_array() const { return arr; }
};



//下面是有操作数的表达式的相关子类
//基础表达式模板
template <typename T>
class BaseExp : public HasOperand{
  protected:
  //因为只有一元表达式涉及了右递归，所以这里我统一用链表进行实现
  std::list<Type> op;  //操作符序列
  std::list<std::unique_ptr<T>> operand;  //操作数序列
  public:
  BaseExp(T* data){ operand.push_back(std::unique_ptr<T>(data));}
  //接受第一个操作数，然后进行创建对象
  void push_back(Type t){ op.push_back(t); }
  void push_back(T* data){ operand.push_back(std::unique_ptr<T>(data)); }
  void push_front(Type t){ op.push_front(t); }
  void push_front(T* data){ operand.push_front(std::unique_ptr<T>(data)); }
  const std::list<Type>& getOps() const { return op;}
  const std::list<std::unique_ptr<T>>& getOperands() const {return operand;}

  size_t get_operand_count() const { return operand.size(); }
  size_t get_op_count() const { return op.size(); }
};
// 具体表达式类型
//UnaryExp → MulExp → AddExp → RelExp → EqExp → LAndExp → LOrExp
class UnaryExp : public BaseExp<HasOperand> {
public:
    using BaseExp<HasOperand>::BaseExp;
    void accept(ASTVisitor &visitor) override { visitor.visit(*this); }
};

class MulExp : public BaseExp<UnaryExp> {
public:
    using BaseExp<UnaryExp>::BaseExp;
    void accept(ASTVisitor &visitor) override { visitor.visit(*this); }
};

class AddExp : public BaseExp<MulExp> {
public:
    using BaseExp<MulExp>::BaseExp;
    void accept(ASTVisitor &visitor) override { visitor.visit(*this); }
};

class RelExp : public BaseExp<AddExp> {
public:
    using BaseExp<AddExp>::BaseExp;
    void accept(ASTVisitor &visitor) override { visitor.visit(*this); }
};

class EqExp : public BaseExp<RelExp> {
public:
    using BaseExp<RelExp>::BaseExp;
    void accept(ASTVisitor &visitor) override { visitor.visit(*this); }
};

class LAndExp : public BaseExp<EqExp> {
public:
    using BaseExp<EqExp>::BaseExp;
    void accept(ASTVisitor &visitor) override { visitor.visit(*this); }
};

class LOrExp : public BaseExp<LAndExp> {
public:
    using BaseExp<LAndExp>::BaseExp;
    void accept(ASTVisitor &visitor) override { visitor.visit(*this); }
};

//函数调用的表达式
class FuncCall : public HasOperand{
  private:
  std::string ident;
  std::unique_ptr<FuncRParamList> fp;
  int lineno;
  public:
  FuncCall(std::string id, FuncRParamList* fp, int line) : ident(id), fp(std::unique_ptr<FuncRParamList>(fp)), lineno(line) {}
  FuncCall(std::string id, int line) : ident(id), fp(nullptr), lineno(line) {}
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }

  const std::string& get_ident() const { return ident; }
  bool has_params() const { return fp != nullptr; }
  const std::unique_ptr<FuncRParamList>& get_params() const { return fp; }
  int get_lineno() const { return lineno; }
};
//表达式列表
//这里对应Bison中的ExpList--即数组维度列表
class ArrayList : public BaseAST{
  private:
  std::vector<std::unique_ptr<AddExp>> al;
  public:
  ArrayList(AddExp* ae){ al.push_back(std::unique_ptr<AddExp>(ae)); }
  void push_back(AddExp* ae){ al.push_back(std::unique_ptr<AddExp>(ae)); }
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }
  const std::vector<std::unique_ptr<AddExp>>& get_expressions() const { return al; }
};
//函数调用时的实参列表
class FuncRParamList : public BaseAST{
  private:
  std::vector<std::unique_ptr<AddExp>> fp;
  public:
  FuncRParamList(AddExp* ae){ fp.push_back(std::unique_ptr<AddExp>(ae)); }
  void push_back(AddExp* ae){ fp.push_back(std::unique_ptr<AddExp>(ae)); }
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }

  const std::vector<std::unique_ptr<AddExp>>& get_params() const { return fp; }
};


//字面量常量
//Number->INTCONST|FLOAT_CONST
template<typename T>
class ConValue : public HasOperand{
  private:
  T t;
  public:
  ConValue(T t):t(t){}
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }

  T get_value() const { return t; }
};