%option outfile="src/yacc/Flex.cpp" header-file="include/yacc/Flex.hpp"
%option yylineno

%{
#include "yacc/Bison.hpp"
// Flex 生成的扫描器向 Bison 返回强类型令牌。
#define YY_DECL yy::parser::symbol_type yylex(void)
%}

Identifier          ([a-zA-Z]|_)([a-zA-Z]|[0-9]|_)*
Hex                 0[xX][0-9a-fA-F]*
Octal               0[0-7]*
Decimal             [1-9][0-9]*
HEXPREFIX (0x)|(0X)
HEXNUM [0-9a-fA-F]
DECIMALNUM [0-9]
HEX_FLOAT_TAIL [pP][+-]?{DECIMALNUM}+
DEC_FLOAT_TAIL [eE][+-]?{DECIMALNUM}+
HEX_FLOAT_FORM1 {HEXPREFIX}{HEXNUM}*\.{HEXNUM}+{HEX_FLOAT_TAIL}?
HEX_FLOAT_FORM2 {HEXPREFIX}{HEXNUM}+\.?{HEX_FLOAT_TAIL}
HEX_FLOAT_FORM3 {HEXPREFIX}{HEXNUM}+\.

DEC_FLOAT_FORM1 {DECIMALNUM}*\.{DECIMALNUM}+{DEC_FLOAT_TAIL}?
DEC_FLOAT_FORM2 {DECIMALNUM}+\.?{DEC_FLOAT_TAIL}
DEC_FLOAT_FORM3 {DECIMALNUM}+\.

HEX_FLOAT {HEX_FLOAT_FORM1}|{HEX_FLOAT_FORM2}|{HEX_FLOAT_FORM3}
DEC_FLOAT {DEC_FLOAT_FORM1}|{DEC_FLOAT_FORM2}|{DEC_FLOAT_FORM3}

Float {DEC_FLOAT}|{HEX_FLOAT}
%x COMMENT

%%
"int"               { return yy::parser::make_INT(); }
"float"             { return yy::parser::make_FLOAT();}
"void"              { return yy::parser::make_VOID();}
"tensor"            { return yy::parser::make_TENSOR();}
"const"             { return yy::parser::make_CONST();}
"return"            { return yy::parser::make_RETURN();}
"if"                { return yy::parser::make_IF();}
"else"              { return yy::parser::make_ELSE();}
"while"             { return yy::parser::make_WHILE();}
"break"             { return yy::parser::make_BREAK();}
"continue"          { return yy::parser::make_CONTINUE();}
"("                 { return yy::parser::make_L_SMALL_BRACKET(); }
")"                 { return yy::parser::make_R_SMALL_BRACKET(); }
"["                 { return yy::parser::make_L_MIDDLE_BRACKET();}
"]"                 { return yy::parser::make_R_MIDDLE_BRACKET();}
"{"                 { return yy::parser::make_L_BIG_BRACKET();}
"}"                 { return yy::parser::make_R_BIG_BRACKET();}
";"                 { return yy::parser::make_SEMICOLON();}
","                 { return yy::parser::make_COMMA();}

"||"                { return yy::parser::make_OR(); }
"&&"                { return yy::parser::make_AND(); }
"!"                 { return yy::parser::make_NOT();}
"=="                { return yy::parser::make_EQ();}
"!="                { return yy::parser::make_NOTEQ();}
"<"                 { return yy::parser::make_LESS();}
">"                 { return yy::parser::make_GREAT();}
"<="                { return yy::parser::make_LESSEQ();}
">="                { return yy::parser::make_GREATEQ();}
"+"                 { return yy::parser::make_ADD();}
"-"                 { return yy::parser::make_SUB();}
"*"                 { return yy::parser::make_MUL();}
"/"                 { return yy::parser::make_DIV();}
"%"                 { return yy::parser::make_MOD();}
"="                 { return yy::parser::make_ASSIGN();}
"@"                 { return yy::parser::make_GEMM();}

{Identifier}        {return yy::parser::make_IDENT(LocatedIdentifier{std::string(yytext), yylineno});}
{Float}             {return yy::parser::make_FLOATCONST(std::stof(yytext));}
{Hex}               {return yy::parser::make_INTCONST(std::stoi(yytext,nullptr,16));}
{Octal}             {return yy::parser::make_INTCONST(std::stoi(yytext,nullptr,8));}
{Decimal}           {return yy::parser::make_INTCONST(std::stoi(yytext));}


<<EOF>>          {
    return 0;
}

"//".*              {}
"/*" {BEGIN COMMENT;}
<COMMENT>"*/" {BEGIN INITIAL;}
<COMMENT>.|\n {}
.|\n                {}

%%

int yywrap() {
    return 1;
}
