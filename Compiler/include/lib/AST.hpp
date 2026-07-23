#pragma once
#include <memory>
#include <string>
#include <vector>
#include <list>

// 定义 SysY 抽象语法树；IR 生成等后续阶段通过访问器与节点结构解耦。
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

// 前向声明用于解除节点之间的声明顺序依赖。
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


// 所有语法树访问器共用的动态分派接口。
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


// 所有语法树节点的基类。
class BaseAST{
public:
    virtual ~BaseAST() = default;
    virtual void accept(ASTVisitor &visitor) = 0;
};
// 所有声明与语句节点的共同基类。
class Stmt : public BaseAST{
public:
};
// 所有可作为表达式操作数的节点共同基类。
class HasOperand : public BaseAST{
public:
};

// 编译单元按源码顺序持有声明和函数定义，并独占各子节点的生命周期。
class CompUnit : public BaseAST{
private:
  std::vector<std::unique_ptr<BaseAST>> items;
public:
  CompUnit(BaseAST * item){ items.push_back(std::unique_ptr<BaseAST>(item)); }
  void pushBack(BaseAST* item){ items.push_back(std::unique_ptr<BaseAST>(item)); }
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }
  // 返回只读节点序列，供访问器按源码顺序遍历。
  const std::vector<std::unique_ptr<BaseAST>>& getItems() const {return items;}
};

// 常量声明也是块内语句，因此继承 Stmt。
class ConstDecl : public Stmt{
  private:
    Type type;
    std::unique_ptr<ConstDefList> defList;
  public:
    ConstDecl(Type t,ConstDefList* d): type(t), defList(std::unique_ptr<ConstDefList>(d)) {}
    void accept(ASTVisitor &visitor) override { visitor.visit(*this); }
    Type getType() const {return type;}
    const std::unique_ptr<ConstDefList>& getConstDefList() const{ return defList; }
};
// 变量声明持有同一基础类型下的全部变量定义。
class VarDecl : public Stmt{
  private:
    Type type;
    std::unique_ptr<VarDefList>defList;
  public:
    VarDecl(Type t,VarDefList* d): type(t), defList(std::unique_ptr<VarDefList>(d)) {}
    void accept(ASTVisitor &visitor) override { visitor.visit(*this); }
    Type getType() const {return type;}
    const std::unique_ptr<VarDefList>& getVarDefList() const{ return defList; }

};
// 常量定义列表保持文法归约得到的源码顺序。
class ConstDefList : public Stmt{
private:
  std::vector<std::unique_ptr<ConstDef>> cdl;
public:
  ConstDefList(ConstDef* c){ cdl.push_back(std::unique_ptr<ConstDef>(c)); }
  void pushBack(ConstDef* c){ cdl.push_back(std::unique_ptr<ConstDef>(c)); }
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }
  const std::vector<std::unique_ptr<ConstDef>>& getDefs() const { return cdl; }

};
// 变量定义列表保持文法归约得到的源码顺序。
class VarDefList : public Stmt{
  private:
    std::vector<std::unique_ptr<VarDef>> vdl;
  public:
  VarDefList(VarDef* v){ vdl.push_back(std::unique_ptr<VarDef>(v)); }
  void pushBack(VarDef* v){ vdl.push_back(std::unique_ptr<VarDef>(v)); }
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }
  const std::vector<std::unique_ptr<VarDef>>& getDefs() const { return vdl; }
};

// 变量定义可选带数组维度和初始化值。
class VarDef : public Stmt{
  private:
    std::string ident;
    std::unique_ptr<ArrayList> expList;
    std::unique_ptr<InitVal> initVal;
  public:
    VarDef(const std::string& id): ident(id), expList(nullptr), initVal(nullptr) {}
    VarDef(const std::string& id, ArrayList* el, InitVal* iv): ident(id), expList(std::unique_ptr<ArrayList>(el)), initVal(std::unique_ptr<InitVal>(iv)) {}
    void accept(ASTVisitor &visitor) override { visitor.visit(*this); }
    std::string getIdent()const { return ident; }
    bool hasArray() const { return expList != nullptr; }
    ArrayList* getArray() const { return expList.get(); }
    bool hasInitVal() const { return initVal != nullptr; }
    InitVal* getInitVal() const { return initVal.get(); }
};
// 常量定义必须由语义阶段校验其初始化式为常量表达式。
class ConstDef : public Stmt{
private:
    std::string ident;
    std::unique_ptr<ArrayList> expList;
    std::unique_ptr<InitVal> initVal;
public:
    ConstDef(const std::string& id, ArrayList* el, InitVal* iv): ident(id), expList(std::unique_ptr<ArrayList>(el)), initVal(std::unique_ptr<InitVal>(iv)){}
    void accept(ASTVisitor &visitor) override { visitor.visit(*this); }
    std::string getIdent()const { return ident; }
    bool hasArray() const { return expList != nullptr; }
    ArrayList* getArray() const { return expList.get(); }
    bool hasInitVal() const { return initVal != nullptr; }
    InitVal* getInitVal() const { return initVal.get(); }
};

// 函数定义独占形参列表和函数体；无形参时 params 为空。
class FuncDef : public BaseAST{
  private:
  Type type;
  std::string ident;
  std::unique_ptr<FuncParamList> params;
  std::unique_ptr<Block> funcBody;
  public:
  FuncDef(Type t,std::string id,FuncParamList* p,Block* b):type(t),ident(id),params(std::unique_ptr<FuncParamList>(p)),funcBody(std::unique_ptr<Block>(b)){}
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }

   Type getType() const { return type; }
   const std::string& getIdent() const { return ident; }
   const std::unique_ptr<FuncParamList>& getParams() const { return params; }
   const std::unique_ptr<Block>& getFuncBody() const { return funcBody; }
};
// 函数形参记录首个空维度和后续显式维度，供数组形参类型降级使用。
class FuncParam : public BaseAST{
private:
  Type type;
  std::string ident;
  bool hasEmptyBracketsFlag;
  std::unique_ptr<ArrayList> dimensionList;
public:
  // 普通标量形参：Type IDENT。
  FuncParam(Type t, std::string id):
      type(t), ident(id), hasEmptyBracketsFlag(false), dimensionList(nullptr) {}

  // 省略首维的数组形参：Type IDENT []。
  FuncParam(Type t, std::string id, bool hasEmptyBrackets):
      type(t), ident(id), hasEmptyBracketsFlag(hasEmptyBrackets), dimensionList(nullptr) {}

  // 带后续维度的数组形参：Type IDENT [][Exp]...。
  FuncParam(Type t, std::string id, bool hasEmptyBrackets, ArrayList* dimList):
      type(t), ident(id), hasEmptyBracketsFlag(hasEmptyBrackets),
      dimensionList(std::unique_ptr<ArrayList>(dimList)) {}

  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }

  Type getType() const { return type; }
  const std::string& getIdent() const { return ident; }
  bool hasEmptyBrackets() const { return hasEmptyBracketsFlag; }
  bool hasDimension() const { return dimensionList != nullptr; }
  const std::unique_ptr<ArrayList>& getExpList() const { return dimensionList; }
};
// 函数定义使用的形参列表。
class FuncParamList : public BaseAST{
  private:
  std::vector<std::unique_ptr<FuncParam>> fpl;
  public:
  FuncParamList(FuncParam* f){ fpl.push_back(std::unique_ptr<FuncParam>(f)); }
  void pushBack(FuncParam* f){ fpl.push_back(std::unique_ptr<FuncParam>(f)); }
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }

  const std::vector<std::unique_ptr<FuncParam>>& getParams() const { return fpl; }
};

// 聚合初始化列表，可同时表示常量初始化和变量初始化。
class InitValList : public BaseAST{
  private:
  std::vector<std::unique_ptr<InitVal>> ivl;
  public:
  InitValList(InitVal* iv){ ivl.push_back(std::unique_ptr<InitVal>(iv)); }
  void pushBack(InitVal* iv){ ivl.push_back(std::unique_ptr<InitVal>(iv)); }
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }

  const std::vector<std::unique_ptr<InitVal>>& getInitVals() const { return ivl; }
};
// 初始化值为空、标量表达式或聚合初始化列表之一。
class InitVal : public BaseAST{
  private:
  std::unique_ptr<BaseAST> val;
  public:
  InitVal(): val(nullptr) {}
  InitVal(BaseAST* iv): val(std::unique_ptr<BaseAST>(iv)) {}
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }

  const std::unique_ptr<BaseAST>& getValue() const { return val; }
};


// 代码块可为空；非空时独占块项列表。
class Block : public Stmt{
  private:
  std::unique_ptr<BlockItemList> items;
  public:
  Block(BlockItemList* it): items(std::unique_ptr<BlockItemList>(it)) {}
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }

  const std::unique_ptr<BlockItemList>& getItems() const { return items; }
};
// 块项列表按源码顺序混合保存声明与语句。
class BlockItemList : public Stmt{
  private:
  std::vector<std::unique_ptr<Stmt>> st;
  public:
  BlockItemList(Stmt* s){ st.push_back(std::unique_ptr<Stmt>(s)); }
  void pushBack(Stmt* s){ st.push_back(std::unique_ptr<Stmt>(s)); }
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }

  const std::vector<std::unique_ptr<Stmt>>& getStmts() const { return st; }
};
// 赋值语句保存目标左值与右侧表达式。
class AssignStmt : public Stmt{
  private:
  std::unique_ptr<LVal> lv;
  std::unique_ptr<AddExp> ae;
  public:
  AssignStmt(LVal* lv,AddExp* ae): lv(std::unique_ptr<LVal>(lv)), ae(std::unique_ptr<AddExp>(ae)) {}
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }

  const std::unique_ptr<LVal>& getLVal() const { return lv; }
  const std::unique_ptr<AddExp>& getExp() const { return ae; }
};
// 表达式语句允许空表达式。
class ExpStmt : public Stmt{
  private:
  std::unique_ptr<AddExp> ae;
  public:
  ExpStmt(AddExp* ae): ae(std::unique_ptr<AddExp>(ae)) {}
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }

  const std::unique_ptr<AddExp>& getExp() const { return ae; }
};
// 条件语句可选带否分支。
class IfStmt : public Stmt{
  private:
  std::unique_ptr<LOrExp> condition;
  std::unique_ptr<Stmt> ifStmt,elseStmt;
  public:
  IfStmt(LOrExp* condition,Stmt* ifStmt):condition(std::unique_ptr<LOrExp>(condition)),ifStmt(std::unique_ptr<Stmt>(ifStmt)),elseStmt(nullptr){}
  IfStmt(LOrExp* condition,Stmt* ifStmt,Stmt* elseStmt):condition(std::unique_ptr<LOrExp>(condition)),ifStmt(std::unique_ptr<Stmt>(ifStmt)),elseStmt(std::unique_ptr<Stmt>(elseStmt)){}
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }

  const std::unique_ptr<LOrExp>& getCondition() const { return condition; }
  const std::unique_ptr<Stmt>& getIfStmt() const { return ifStmt; }
  bool hasElse() const { return elseStmt != nullptr; }
  const std::unique_ptr<Stmt>& getElseStmt() const { return elseStmt; }
};
// 循环语句保存条件和循环体。
class WhileStmt : public Stmt{
  private:
  std::unique_ptr<LOrExp> condition;
  std::unique_ptr<Stmt> whileStmt;
  public:
  WhileStmt(LOrExp* condition,Stmt* whileStmt):condition(std::unique_ptr<LOrExp>(condition)),whileStmt(std::unique_ptr<Stmt> (whileStmt)){}
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }

  const std::unique_ptr<LOrExp>& getCondition() const { return condition; }
  const std::unique_ptr<Stmt>& getWhileStmt() const { return whileStmt; }
};
// 退出当前循环的语句节点。
class BreakStmt : public Stmt{
  public:
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }

};
// 继续当前循环下一次迭代的语句节点。
class ContinueStmt : public Stmt{
  public:
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }
};
// 返回语句可选带返回值表达式。
class ReturnStmt : public Stmt{
  private:
  std::unique_ptr<AddExp> retVal;
  public:
  ReturnStmt(): retVal(nullptr) {}
  ReturnStmt(AddExp* ae): retVal(std::unique_ptr<AddExp>(ae)) {}
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }

  bool hasReturnValue() const { return retVal != nullptr; }
  const std::unique_ptr<AddExp>& getReturnValue() const { return retVal; }

};


// 左值表达式表示变量或数组元素，可出现在赋值号左侧。
class LVal : public HasOperand{
  private:
  std::string ident;
  std::unique_ptr<ArrayList> arr;
  public:
  LVal(std::string id):ident(id),arr(nullptr){}
  LVal(std::string id,ArrayList* e):ident(id),arr(std::unique_ptr<ArrayList>(e)){}
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }

  const std::string& getIdent() const { return ident; }
  bool hasArray() const { return arr != nullptr; }
  const std::unique_ptr<ArrayList>& getArray() const { return arr; }
};



// 同一优先级的表达式链；始终满足操作数数量比操作符数量多一。
template <typename T>
class BaseExp : public HasOperand{
  protected:
  std::list<Type> op;
  std::list<std::unique_ptr<T>> operand;
  public:
  BaseExp(T* data){ operand.push_back(std::unique_ptr<T>(data));}
  // 尾插用于左递归表达式；头插用于按文法顺序保存一元操作符。
  void pushBack(Type t){ op.push_back(t); }
  void pushBack(T* data){ operand.push_back(std::unique_ptr<T>(data)); }
  void pushFront(Type t){ op.push_front(t); }
  void pushFront(T* data){ operand.push_front(std::unique_ptr<T>(data)); }
  const std::list<Type>& getOps() const { return op;}
  const std::list<std::unique_ptr<T>>& getOperands() const {return operand;}

  size_t getOperandCount() const { return operand.size(); }
  size_t getOpCount() const { return op.size(); }
};
// 表达式层级从高到低依次为一元、乘除、加减、关系、相等、逻辑与、逻辑或。
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

// 函数调用记录被调函数、可选实参列表和源码行号。
class FuncCall : public HasOperand{
  private:
  std::string ident;
  std::unique_ptr<FuncRParamList> fp;
  int lineno;
  public:
  FuncCall(std::string id, FuncRParamList* fp, int line) : ident(id), fp(std::unique_ptr<FuncRParamList>(fp)), lineno(line) {}
  FuncCall(std::string id, int line) : ident(id), fp(nullptr), lineno(line) {}
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }

  const std::string& getIdent() const { return ident; }
  bool hasParams() const { return fp != nullptr; }
  const std::unique_ptr<FuncRParamList>& getParams() const { return fp; }
  int getLineNo() const { return lineno; }
};
// 数组维度或索引表达式列表，对应语法规则中的 ExpList。
class ArrayList : public BaseAST{
  private:
  std::vector<std::unique_ptr<AddExp>> al;
  public:
  ArrayList(AddExp* ae){ al.push_back(std::unique_ptr<AddExp>(ae)); }
  void pushBack(AddExp* ae){ al.push_back(std::unique_ptr<AddExp>(ae)); }
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }
  const std::vector<std::unique_ptr<AddExp>>& getExpressions() const { return al; }
};
// 函数调用的实参列表。
class FuncRParamList : public BaseAST{
  private:
  std::vector<std::unique_ptr<AddExp>> fp;
  public:
  FuncRParamList(AddExp* ae){ fp.push_back(std::unique_ptr<AddExp>(ae)); }
  void pushBack(AddExp* ae){ fp.push_back(std::unique_ptr<AddExp>(ae)); }
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }

  const std::vector<std::unique_ptr<AddExp>>& getParams() const { return fp; }
};


// 整数或浮点数字面量，对应 Number 文法节点。
template<typename T>
class ConValue : public HasOperand{
  private:
  T t;
  public:
  ConValue(T t):t(t){}
  void accept(ASTVisitor &visitor) override { visitor.visit(*this); }

  T getValue() const { return t; }
};
