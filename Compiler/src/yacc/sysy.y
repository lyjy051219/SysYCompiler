%header "include/yacc/Bison.hpp"
%output "src/yacc/Bison.cpp"
// 生成文件路径相对于项目根目录。
%language "c++"
// 令牌由 yy::parser::make_* 构造函数创建。
%define api.token.constructor
// 语义值使用 std::variant 保存，避免手动管理联合体生命周期。
%define api.value.type variant
%require "3.2"

// 以下依赖写入生成的 Bison.hpp。
%code requires{
  #include "../lib/AST.hpp"
  #include <memory>
  #include <iostream>

  // 调用行号必须在词法器识别函数名时锁定，不能在右括号归约时读取。
  struct LocatedIdentifier {
      std::string text;
      int line;
  };
}

// 以下实现写入生成的 Bison.cpp。
%code{
extern yy::parser::symbol_type yylex();
extern std::unique_ptr<CompUnit> ASTRoot;
extern int yylineno;

void yy::parser::error(const std::string &err)
{
    std::cerr << "语法错误：第 " << yylineno << " 行：" << err << '\n';
}
}

// 终结符定义。
%token <int> INTCONST
%token <float> FLOATCONST
%token <LocatedIdentifier> IDENT
%token CONST
%token INT
%token FLOAT
%token VOID
%token TENSOR
%token GEMM
%token OR
%token AND
%token NOT
%token EQ
%token NOTEQ
%token LESS
%token GREAT
%token LESSEQ
%token GREATEQ
%token ADD
%token SUB
%token MUL
%token DIV
%token MOD
%token ASSIGN
%token RETURN
%token IF
%token ELSE
%token WHILE
%token BREAK
%token CONTINUE
%token L_SMALL_BRACKET
%token R_SMALL_BRACKET
%token L_MIDDLE_BRACKET
%token R_MIDDLE_BRACKET
%token L_BIG_BRACKET
%token R_BIG_BRACKET
%token SEMICOLON
%token COMMA
%token END


// 非终结符及其语义值类型。
%nterm <CompUnit*> CompUnit
%nterm <BaseAST*> Decl
%nterm <ConstDecl*> ConstDecl
%nterm <ConstDefList*> ConstDefList
%nterm <ConstDef*> ConstDef
%nterm <InitValList*> ConstInitValList
%nterm <InitVal*> ConstInitVal
%nterm <InitValList*> InitValList
%nterm <InitVal*> InitVal
%nterm <VarDecl*> VarDecl
%nterm <VarDefList*> VarDefList
%nterm <VarDef*> VarDef
%nterm <FuncDef*> FuncDef
%nterm <FuncParamList*> FuncFParamList
%nterm <FuncParam*> FuncFParam
%nterm <Block*> Block
%nterm <BlockItemList*> BlockItemList
%nterm <BaseAST*> BlockItem
%nterm <BaseAST*> Stmt
// 常量维度与运行时下标分别使用不同的列表产生式。
%nterm <ArrayList*> ConstExpList
%nterm <ArrayList*> ExpList
// 函数实参列表。
%nterm <FuncRParamList*> FuncRParamList

%nterm <LVal*> LVal
%nterm <BaseAST*> PrimaryExp
%nterm <AddExp*> AddExp
%nterm <LOrExp*> LOrExp
%nterm <UnaryExp*> UnaryExp
%nterm <MulExp*> MulExp
%nterm <RelExp*> RelExp
%nterm <EqExp*> EqExp
%nterm <LAndExp*> LAndExp
%nterm <ASTType> Type

%start Start

%%
Start: CompUnit { ASTRoot = std::unique_ptr<CompUnit>($1); }

// 编译单元使用左递归产生式，并按源码顺序在列表尾部追加节点。
CompUnit
  : CompUnit Decl{$$ = $1; $$->pushBack($2);}
  | CompUnit FuncDef{$$ = $1; $$->pushBack($2);}
  | Decl{$$ = new CompUnit($1);}
  | FuncDef {$$ = new CompUnit($1);}
  ;

// 常量声明与变量声明统一转换为 AST 节点。
Decl
  : ConstDecl { $$ = $1; }
  | VarDecl { $$ = $1; }
  ;

// 常量声明由基础类型和同类型定义列表组成。
ConstDecl
  : CONST Type ConstDefList SEMICOLON{ $$ = new ConstDecl($2,$3); }
  ;

ConstDefList
  : ConstDefList COMMA ConstDef{ $$ = $1; $1->pushBack($3); }
  | ConstDef{ $$ = new ConstDefList($1); }
  ;

ConstDef
  : IDENT ASSIGN ConstInitVal{ $$ = new ConstDef($1.text,nullptr,$3); }
  | IDENT ConstExpList ASSIGN ConstInitVal{ $$ = new ConstDef($1.text,$2,$4); }
  ;

ConstExpList
 : ConstExpList L_MIDDLE_BRACKET AddExp R_MIDDLE_BRACKET{ $$ = $1; $$->pushBack($3); }
 | L_MIDDLE_BRACKET AddExp R_MIDDLE_BRACKET { $$ = new ArrayList($2); }
 ;

ConstInitVal
 : AddExp{ $$ = new InitVal($1); }
 | L_BIG_BRACKET R_BIG_BRACKET{ $$ = new InitVal(); }
 | L_BIG_BRACKET ConstInitValList R_BIG_BRACKET{ $$ = new InitVal($2); }
 ;

ConstInitValList
 : ConstInitVal{ $$ = new InitValList($1); }
 | ConstInitValList COMMA ConstInitVal{ $$ = $1; $$->pushBack($3); }
 ;

VarDecl
 : Type VarDefList SEMICOLON{ $$ = new VarDecl($1,$2); }
 ;

VarDefList
 : VarDef{ $$ = new VarDefList($1); }
 | VarDefList COMMA VarDef{ $$ = $1; $$->pushBack($3); }
 ;

VarDef
 : IDENT { $$ = new VarDef($1.text); }
 | IDENT ASSIGN InitVal{ $$ = new VarDef($1.text,nullptr,$3); }
 | IDENT ConstExpList { $$ = new VarDef($1.text,$2,nullptr); }
 | IDENT ConstExpList ASSIGN InitVal { $$ = new VarDef($1.text,$2,$4); }
 ;

InitVal
 : AddExp { $$ = new InitVal($1); }
 | L_BIG_BRACKET R_BIG_BRACKET { $$ = new InitVal(nullptr); }
 | L_BIG_BRACKET InitValList R_BIG_BRACKET { $$ = new InitVal($2); }
 ;

InitValList
 : InitVal { $$ = new InitValList($1); }
 | InitValList COMMA InitVal { $$ = $1; $$->pushBack($3); }
 ;

FuncDef
 : Type IDENT L_SMALL_BRACKET R_SMALL_BRACKET Block{ $$ = new FuncDef($1, $2.text, nullptr, $5); }
 | Type IDENT L_SMALL_BRACKET FuncFParamList R_SMALL_BRACKET Block{ $$ = new FuncDef($1, $2.text, $4, $6); }
  ;

FuncFParamList
 : FuncFParam { $$ = new FuncParamList($1); }
 | FuncFParamList COMMA FuncFParam { $$ = $1; $$->pushBack($3); }
 ;

FuncFParam
 : Type IDENT { $$ = new FuncParam($1,$2.text); }
 | Type IDENT L_MIDDLE_BRACKET R_MIDDLE_BRACKET { $$ = new FuncParam($1,$2.text,true); }
 | Type IDENT ExpList { $$ = new FuncParam($1,$2.text,false,$3); }
 | Type IDENT L_MIDDLE_BRACKET R_MIDDLE_BRACKET ExpList { $$ = new FuncParam($1,$2.text,true,$5); }
 ;

Block
 : L_BIG_BRACKET R_BIG_BRACKET{ $$ = new Block(nullptr); }
 | L_BIG_BRACKET BlockItemList R_BIG_BRACKET { $$ = new Block($2); }
 ;

BlockItemList
 : BlockItem { $$ = new BlockItemList($1); }
 | BlockItemList BlockItem { $$ = $1; $$->pushBack($2); }
 ;

BlockItem
 : Decl{ $$ = $1; }
 | Stmt{ $$ = $1; }
  ;

// 语句产生式覆盖赋值、分支、循环、跳转和返回。
Stmt
 : LVal ASSIGN AddExp SEMICOLON{ $$ = new AssignStmt($1,$3); }
 | SEMICOLON { $$ = new ExpStmt(nullptr); }
 | AddExp SEMICOLON { $$ = new ExpStmt($1); }
 | Block { $$ = $1; }
 | IF L_SMALL_BRACKET LOrExp R_SMALL_BRACKET Stmt ELSE Stmt { $$ = new IfStmt($3,$5,$7); }
 | IF L_SMALL_BRACKET LOrExp R_SMALL_BRACKET Stmt { $$ = new IfStmt($3,$5); }
 | WHILE L_SMALL_BRACKET LOrExp R_SMALL_BRACKET Stmt { $$ = new WhileStmt($3,$5); }
 | BREAK SEMICOLON { $$ = new BreakStmt(); }
 | CONTINUE SEMICOLON { $$ = new ContinueStmt(); }
 | RETURN SEMICOLON { $$ = new ReturnStmt(); }
 | RETURN AddExp SEMICOLON { $$ = new ReturnStmt($2); }
  ;


LVal
 : IDENT{ $$ = new LVal($1.text); }
 | IDENT ExpList { $$ = new LVal($1.text,$2); }
 ;

ExpList
 : L_MIDDLE_BRACKET AddExp R_MIDDLE_BRACKET{ $$ = new ArrayList($2); }
 | ExpList L_MIDDLE_BRACKET AddExp R_MIDDLE_BRACKET{ $$ = $1; $$->pushBack($3); }
 ;

PrimaryExp
 : L_SMALL_BRACKET AddExp R_SMALL_BRACKET { $$ = $2; }
 | LVal { $$ = $1; }
 | INTCONST{ $$ = new ConValue<int>($1); }
 | FLOATCONST{ $$ = new ConValue<float>($1); }
 | IDENT L_SMALL_BRACKET R_SMALL_BRACKET {$$ = new FuncCall($1.text, $1.line);}
 | IDENT L_SMALL_BRACKET FuncRParamList R_SMALL_BRACKET{$$ = new FuncCall($1.text, $3, $1.line);}
  ;


// 一元运算符按从外到内的顺序插入表达式链表头部。
UnaryExp
 : PrimaryExp{ $$ = new UnaryExp($1); }
 | ADD UnaryExp{ $$ = $2; $$->pushFront(SY_ADD); }
 | SUB UnaryExp{ $$ = $2; $$->pushFront(SY_SUB); }
 | NOT UnaryExp{ $$ = $2; $$->pushFront(SY_NOT); }
  ;


FuncRParamList
 : AddExp { $$ = new FuncRParamList($1); }
 | FuncRParamList COMMA AddExp{ $$ = $1; $$->pushBack($3); }
 ;

MulExp
 : UnaryExp { $$ = new MulExp($1); }
 | MulExp MUL UnaryExp { $$ = $1; $$->pushBack(SY_MUL); $$->pushBack($3); }
 | MulExp DIV UnaryExp { $$ = $1; $$->pushBack(SY_DIV); $$->pushBack($3); }
 | MulExp MOD UnaryExp { $$ = $1; $$->pushBack(SY_MOD); $$->pushBack($3); }
 | MulExp GEMM UnaryExp { $$ = $1; $$->pushBack(SY_GEMM); $$->pushBack($3); }
 ;

AddExp
 : MulExp{ $$ = new AddExp($1); }
 | AddExp ADD MulExp{ $$ = $1; $$->pushBack(SY_ADD); $$->pushBack($3); }
 | AddExp SUB MulExp{ $$ = $1; $$->pushBack(SY_SUB); $$->pushBack($3); }
 ;

RelExp
 : AddExp {$$ = new RelExp($1);}
 | RelExp LESS AddExp{ $$ = $1; $$->pushBack(SY_LESS); $$->pushBack($3); }
 | RelExp GREAT AddExp{ $$ = $1; $$->pushBack(SY_GREAT); $$->pushBack($3); }
 | RelExp LESSEQ AddExp{ $$ = $1; $$->pushBack(SY_LESSEQ); $$->pushBack($3); }
 | RelExp GREATEQ AddExp{ $$ = $1; $$->pushBack(SY_GREATEQ); $$->pushBack($3); }
 ;

EqExp
 : RelExp{ $$ = new EqExp($1); }
 | EqExp EQ RelExp{ $$ = $1; $$->pushBack(SY_EQ); $$->pushBack($3); }
 | EqExp NOTEQ RelExp{ $$ = $1; $$->pushBack(SY_NOTEQ); $$->pushBack($3); }
 ;

LAndExp
 : EqExp { $$ = new LAndExp($1); }
 | LAndExp AND EqExp{ $$ = $1; $$->pushBack(SY_AND); $$->pushBack($3); }
 ;

LOrExp
 : LAndExp{ $$ = new LOrExp($1); }
 | LOrExp OR LAndExp{ $$ = $1; $$->pushBack(SY_OR); $$->pushBack($3); }
 ;


Type
 : VOID { $$ = ASTType::makeScalar(ASTScalarKind::Void); }
 | INT { $$ = ASTType::makeScalar(ASTScalarKind::Int32); }
 | FLOAT { $$ = ASTType::makeScalar(ASTScalarKind::Float32); }
 | TENSOR INT { $$ = ASTType::makeTensor(ASTScalarKind::Int32); }
 | TENSOR FLOAT { $$ = ASTType::makeTensor(ASTScalarKind::Float32); }
 ;

%%
