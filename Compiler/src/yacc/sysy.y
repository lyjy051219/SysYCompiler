%header "include/yacc/Bison.hpp"
%output "src/yacc/Bison.cpp"
%define parse.trace
//注意上面这两行是在项目根目录下运行指令，然后自动会将两个文件生成到对应的目录下
%language "c++"
//使用yy::parser::make
%define api.token.constructor
//不用union存放语义值，用std::variant<...>
%define api.value.type variant
%require "3.2"

//放在bison,hpp头文件里面的
%code requires{
  #include "../lib/AST.hpp"
  #include<memory>
  #include <iostream>
}

//放在bison.cpp里面的
%code{
extern yy::parser::symbol_type yylex();
extern std::unique_ptr<CompUnit> ASTRoot;
extern int yylineno;
namespace yy {
    void parser::error(const std::string &err)
    {
        std::cerr << "Bison error: " << err << '\n';
    }
}
}

//终结符
%token <int> INTCONST
%token <float> FLOATCONST
%token <std::string> IDENT
%token CONST
%token INT
%token FLOAT
%token VOID
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


//非终结符
%nterm <CompUnit*> CompUnit
%nterm <Stmt*> Decl
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
%nterm <Stmt*> BlockItem
%nterm <Stmt*> Stmt
//这两个是关于数组维度列表的
%nterm <ArrayList*> ConstExpList
%nterm <ArrayList*> ExpList
//函数实参
%nterm <FuncRParamList*> FuncRParamList

%nterm <LVal*> LVal
%nterm <HasOperand*> PrimaryExp
%nterm <AddExp*> AddExp
%nterm <LOrExp*> LOrExp
%nterm <UnaryExp*> UnaryExp
%nterm <MulExp*> MulExp
%nterm <RelExp*> RelExp
%nterm <EqExp*> EqExp
%nterm <LAndExp*> LAndExp
%nterm <Type> Type

%start Start

%%
Start: CompUnit { ASTRoot = std::unique_ptr<CompUnit>($1); }

//如果写成左递归的话就是push_back，然后就是使用vector进行保存数据
CompUnit
  : CompUnit Decl{$$ = $1; $$->push_back((BaseAST*)$2);}
  | CompUnit FuncDef{$$ = $1; $$->push_back((BaseAST*)$2);}
  | Decl{$$ = new CompUnit((BaseAST*)$1);}
  | FuncDef {$$ = new CompUnit((BaseAST*)$1);}
  ;

//直接用ConstDecl/VarDecl替换Decl，将BaseAST*替换成Stmt*
Decl
  : ConstDecl { $$ = (Stmt*)$1; }
  | VarDecl { $$ = (Stmt*)$1; }
  ;

//根据ConstDefList和Type创建ConDecl对象
ConstDecl
  : CONST Type ConstDefList SEMICOLON{ $$ = new ConstDecl($2,$3); }
  ;

ConstDefList
  : ConstDefList COMMA ConstDef{ $$ = $1; $1->push_back($3); }
  | ConstDef{ $$ = new ConstDefList($1); }
  ;

ConstDef
  : IDENT ASSIGN ConstInitVal{ $$ = new ConstDef($1,nullptr,$3); }
  | IDENT ConstExpList ASSIGN ConstInitVal{ $$ = new ConstDef($1,$2,$4); }
  ;

ConstExpList
 : ConstExpList L_MIDDLE_BRACKET AddExp R_MIDDLE_BRACKET{ $$ = $1; $$->push_back($3); }
 | L_MIDDLE_BRACKET AddExp R_MIDDLE_BRACKET { $$ = new ArrayList($2); }
 ;

ConstInitVal
 : AddExp{ $$ = new InitVal((BaseAST*)$1); }
 | L_BIG_BRACKET R_BIG_BRACKET{ $$ = new InitVal(); }
 | L_BIG_BRACKET ConstInitValList R_BIG_BRACKET{ $$ = new InitVal((BaseAST*)$2); }
 ;

ConstInitValList
 : ConstInitVal{ $$ = new InitValList($1); }
 | ConstInitValList COMMA ConstInitVal{ $$ = $1; $$->push_back($3); }
 ;

VarDecl
 : Type VarDefList SEMICOLON{ $$ = new VarDecl($1,$2); }
 ;

VarDefList
 : VarDef{ $$ = new VarDefList($1); }
 | VarDefList COMMA VarDef{ $$ = $1; $$->push_back($3); }
 ;

VarDef
 : IDENT { $$ = new VarDef($1); }
 | IDENT ASSIGN InitVal{ $$ = new VarDef($1,nullptr,$3); }
 | IDENT ConstExpList { $$ = new VarDef($1,$2,nullptr); }
 | IDENT ConstExpList ASSIGN InitVal { $$ = new VarDef($1,$2,$4); }
 ;

InitVal
 : AddExp { $$ = new InitVal((BaseAST*)$1); }
 | L_BIG_BRACKET R_BIG_BRACKET { $$ = new InitVal(nullptr); }
 | L_BIG_BRACKET InitValList R_BIG_BRACKET { $$ = new InitVal((BaseAST*)$2); }
 ;

InitValList
 : InitVal { $$ = new InitValList($1); }
 | InitValList COMMA InitVal { $$ = $1; $$->push_back($3); }
 ;

FuncDef
 : Type IDENT L_SMALL_BRACKET R_SMALL_BRACKET Block{ $$ = new FuncDef($1, $2, nullptr, $5); }
 | Type IDENT L_SMALL_BRACKET FuncFParamList R_SMALL_BRACKET Block{ $$ = new FuncDef($1, $2, $4, $6); }
 ;

FuncFParamList
 : FuncFParam { $$ = new FuncParamList($1); }
 | FuncFParamList COMMA FuncFParam { $$ = $1; $$->push_back($3); }
 ;

FuncFParam
 : Type IDENT { $$ = new FuncParam($1,$2); }
 | Type IDENT L_MIDDLE_BRACKET R_MIDDLE_BRACKET { $$ = new FuncParam($1,$2,true); }
 | Type IDENT ExpList { $$ = new FuncParam($1,$2,false,$3); }
 | Type IDENT L_MIDDLE_BRACKET R_MIDDLE_BRACKET ExpList { $$ = new FuncParam($1,$2,true,$5); }
 ;

Block
 : L_BIG_BRACKET R_BIG_BRACKET{ $$ = new Block(nullptr); }
 | L_BIG_BRACKET BlockItemList R_BIG_BRACKET { $$ = new Block($2); }
 ;

BlockItemList
 : BlockItem { $$ = new BlockItemList($1); }
 | BlockItemList BlockItem { $$ = $1; $$->push_back($2); }
 ;

BlockItem
 : Decl{ $$ = (Stmt*)$1; }
 | Stmt{ $$ = (Stmt*)$1; }
 ;

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
 : IDENT{ $$ = new LVal($1); }
 | IDENT ExpList { $$ = new LVal($1,$2); }
 ;

ExpList
 : L_MIDDLE_BRACKET AddExp R_MIDDLE_BRACKET{ $$ = new ArrayList($2); }
 | ExpList L_MIDDLE_BRACKET AddExp R_MIDDLE_BRACKET{ $$ = $1; $$->push_back($3); }
 ;

PrimaryExp
 : L_SMALL_BRACKET AddExp R_SMALL_BRACKET { $$ = (HasOperand*)$2; }
 | LVal { $$ = (HasOperand*)$1; }
 | INTCONST{ $$ = (HasOperand*)(new ConValue<int>($1)); }
 | FLOATCONST{ $$ = (HasOperand*)(new ConValue<float>($1)); }
 | IDENT L_SMALL_BRACKET R_SMALL_BRACKET {$$ = (HasOperand*)(new FuncCall($1, yylineno));}
 | IDENT L_SMALL_BRACKET FuncRParamList R_SMALL_BRACKET{$$ = (HasOperand*)(new FuncCall($1, $3, yylineno));}
 ;


//只有一元表达式这里用到了push_front()
UnaryExp
 : PrimaryExp{ $$ = new UnaryExp($1); }
 | ADD UnaryExp{ $$ = $2; $$->push_front(SY_ADD); }
 | SUB UnaryExp{ $$ = $2; $$->push_front(SY_SUB); }
 | NOT UnaryExp{ $$ = $2; $$->push_front(SY_NOT); }
 ;


FuncRParamList
 : AddExp { $$ = new FuncRParamList($1); }
 | FuncRParamList COMMA AddExp{ $$ = $1; $$->push_back($3); }
 ;

MulExp
 : UnaryExp { $$ = new MulExp($1); }
 | MulExp MUL UnaryExp { $$ = $1; $$->push_back(SY_MUL); $$->push_back($3); }
 | MulExp DIV UnaryExp { $$ = $1; $$->push_back(SY_DIV); $$->push_back($3); }
 | MulExp MOD UnaryExp { $$ = $1; $$->push_back(SY_MOD); $$->push_back($3); }
 ;

AddExp
 : MulExp{ $$ = new AddExp($1); }
 | AddExp ADD MulExp{ $$ = $1; $$->push_back(SY_ADD); $$->push_back($3); }
 | AddExp SUB MulExp{ $$ = $1; $$->push_back(SY_SUB); $$->push_back($3); }
 ;

RelExp
 : AddExp {$$ = new RelExp($1);}
 | RelExp LESS AddExp{ $$ = $1; $$->push_back(SY_LESS); $$->push_back($3); }
 | RelExp GREAT AddExp{ $$ = $1; $$->push_back(SY_GREAT); $$->push_back($3); }
 | RelExp LESSEQ AddExp{ $$ = $1; $$->push_back(SY_LESSEQ); $$->push_back($3); }
 | RelExp GREATEQ AddExp{ $$ = $1; $$->push_back(SY_GREATEQ); $$->push_back($3); }
 ;

EqExp
 : RelExp{ $$ = new EqExp($1); }
 | EqExp EQ RelExp{ $$ = $1; $$->push_back(SY_EQ); $$->push_back($3); }
 | EqExp NOTEQ RelExp{ $$ = $1; $$->push_back(SY_NOTEQ); $$->push_back($3); }
 ;

LAndExp
 : EqExp { $$ = new LAndExp($1); }
 | LAndExp AND EqExp{ $$ = $1; $$->push_back(SY_AND); $$->push_back($3); }
 ;

LOrExp
 : LAndExp{ $$ = new LOrExp($1); }
 | LOrExp OR LAndExp{ $$ = $1; $$->push_back(SY_OR); $$->push_back($3); }
 ;


Type
 : VOID{$$=SY_VOID;}
 | INT {$$=SY_INT;}
 | FLOAT {$$=SY_FLOAT;}
 ;

%%
