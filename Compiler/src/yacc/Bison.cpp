// A Bison parser, made by GNU Bison 3.8.2.

// Skeleton implementation for Bison LALR(1) parsers in C++

// Copyright (C) 2002-2015, 2018-2021 Free Software Foundation, Inc.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

// As a special exception, you may create a larger work that contains
// part or all of the Bison parser skeleton and distribute that work
// under terms of your choice, so long as that work isn't itself a
// parser generator using the skeleton or a modified version thereof
// as a parser skeleton.  Alternatively, if you modify or redistribute
// the parser skeleton itself, you may (at your option) remove this
// special exception, which will cause the skeleton and the resulting
// Bison output files to be licensed under the GNU General Public
// License without this special exception.

// This special exception was added by the Free Software Foundation in
// version 2.2 of Bison.

// DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
// especially those whose name start with YY_ or yy_.  They are
// private implementation details that can be changed or removed.





#include "Bison.hpp"


// Unqualified %code blocks.
#line 25 "src/yacc/sysy.y"

extern yy::parser::symbol_type yylex();
extern std::unique_ptr<CompUnit> ASTRoot;
extern int yylineno;

void yy::parser::error(const std::string &err)
{
    std::cerr << "语法错误：第 " << yylineno << " 行：" << err << '\n';
}

#line 57 "src/yacc/Bison.cpp"


#ifndef YY_
# if defined YYENABLE_NLS && YYENABLE_NLS
#  if ENABLE_NLS
#   include <libintl.h> // FIXME: INFRINGES ON USER NAME SPACE.
#   define YY_(msgid) dgettext ("bison-runtime", msgid)
#  endif
# endif
# ifndef YY_
#  define YY_(msgid) msgid
# endif
#endif


// Whether we are compiled with exception support.
#ifndef YY_EXCEPTIONS
# if defined __GNUC__ && !defined __EXCEPTIONS
#  define YY_EXCEPTIONS 0
# else
#  define YY_EXCEPTIONS 1
# endif
#endif



// Enable debugging if requested.
#if YYDEBUG

// A pseudo ostream that takes yydebug_ into account.
# define YYCDEBUG if (yydebug_) (*yycdebug_)

# define YY_SYMBOL_PRINT(Title, Symbol)         \
  do {                                          \
    if (yydebug_)                               \
    {                                           \
      *yycdebug_ << Title << ' ';               \
      yy_print_ (*yycdebug_, Symbol);           \
      *yycdebug_ << '\n';                       \
    }                                           \
  } while (false)

# define YY_REDUCE_PRINT(Rule)          \
  do {                                  \
    if (yydebug_)                       \
      yy_reduce_print_ (Rule);          \
  } while (false)

# define YY_STACK_PRINT()               \
  do {                                  \
    if (yydebug_)                       \
      yy_stack_print_ ();                \
  } while (false)

#else // !YYDEBUG

# define YYCDEBUG if (false) std::cerr
# define YY_SYMBOL_PRINT(Title, Symbol)  YY_USE (Symbol)
# define YY_REDUCE_PRINT(Rule)           static_cast<void> (0)
# define YY_STACK_PRINT()                static_cast<void> (0)

#endif // !YYDEBUG

#define yyerrok         (yyerrstatus_ = 0)
#define yyclearin       (yyla.clear ())

#define YYACCEPT        goto yyacceptlab
#define YYABORT         goto yyabortlab
#define YYERROR         goto yyerrorlab
#define YYRECOVERING()  (!!yyerrstatus_)

namespace yy {
#line 130 "src/yacc/Bison.cpp"

  /// Build a parser object.
  parser::parser ()
#if YYDEBUG
    : yydebug_ (false),
      yycdebug_ (&std::cerr)
#else

#endif
  {}

  parser::~parser ()
  {}

  parser::syntax_error::~syntax_error () YY_NOEXCEPT YY_NOTHROW
  {}

  /*---------.
  | symbol.  |
  `---------*/



  // by_state.
  parser::by_state::by_state () YY_NOEXCEPT
    : state (empty_state)
  {}

  parser::by_state::by_state (const by_state& that) YY_NOEXCEPT
    : state (that.state)
  {}

  void
  parser::by_state::clear () YY_NOEXCEPT
  {
    state = empty_state;
  }

  void
  parser::by_state::move (by_state& that)
  {
    state = that.state;
    that.clear ();
  }

  parser::by_state::by_state (state_type s) YY_NOEXCEPT
    : state (s)
  {}

  parser::symbol_kind_type
  parser::by_state::kind () const YY_NOEXCEPT
  {
    if (state == empty_state)
      return symbol_kind::S_YYEMPTY;
    else
      return YY_CAST (symbol_kind_type, yystos_[+state]);
  }

  parser::stack_symbol_type::stack_symbol_type ()
  {}

  parser::stack_symbol_type::stack_symbol_type (YY_RVREF (stack_symbol_type) that)
    : super_type (YY_MOVE (that.state))
  {
    switch (that.kind ())
    {
      case symbol_kind::S_Type: // Type
        value.YY_MOVE_OR_COPY< ASTType > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_AddExp: // AddExp
        value.YY_MOVE_OR_COPY< AddExp* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_ConstExpList: // ConstExpList
      case symbol_kind::S_ExpList: // ExpList
        value.YY_MOVE_OR_COPY< ArrayList* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_Decl: // Decl
      case symbol_kind::S_BlockItem: // BlockItem
      case symbol_kind::S_Stmt: // Stmt
      case symbol_kind::S_PrimaryExp: // PrimaryExp
        value.YY_MOVE_OR_COPY< BaseAST* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_Block: // Block
        value.YY_MOVE_OR_COPY< Block* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_BlockItemList: // BlockItemList
        value.YY_MOVE_OR_COPY< BlockItemList* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_CompUnit: // CompUnit
        value.YY_MOVE_OR_COPY< CompUnit* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_ConstDecl: // ConstDecl
        value.YY_MOVE_OR_COPY< ConstDecl* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_ConstDef: // ConstDef
        value.YY_MOVE_OR_COPY< ConstDef* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_ConstDefList: // ConstDefList
        value.YY_MOVE_OR_COPY< ConstDefList* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_EqExp: // EqExp
        value.YY_MOVE_OR_COPY< EqExp* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_FuncDef: // FuncDef
        value.YY_MOVE_OR_COPY< FuncDef* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_FuncFParam: // FuncFParam
        value.YY_MOVE_OR_COPY< FuncParam* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_FuncFParamList: // FuncFParamList
        value.YY_MOVE_OR_COPY< FuncParamList* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_FuncRParamList: // FuncRParamList
        value.YY_MOVE_OR_COPY< FuncRParamList* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_ConstInitVal: // ConstInitVal
      case symbol_kind::S_InitVal: // InitVal
        value.YY_MOVE_OR_COPY< InitVal* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_ConstInitValList: // ConstInitValList
      case symbol_kind::S_InitValList: // InitValList
        value.YY_MOVE_OR_COPY< InitValList* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_LAndExp: // LAndExp
        value.YY_MOVE_OR_COPY< LAndExp* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_LOrExp: // LOrExp
        value.YY_MOVE_OR_COPY< LOrExp* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_LVal: // LVal
        value.YY_MOVE_OR_COPY< LVal* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_IDENT: // IDENT
        value.YY_MOVE_OR_COPY< LocatedIdentifier > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_MulExp: // MulExp
        value.YY_MOVE_OR_COPY< MulExp* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_RelExp: // RelExp
        value.YY_MOVE_OR_COPY< RelExp* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_UnaryExp: // UnaryExp
        value.YY_MOVE_OR_COPY< UnaryExp* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_VarDecl: // VarDecl
        value.YY_MOVE_OR_COPY< VarDecl* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_VarDef: // VarDef
        value.YY_MOVE_OR_COPY< VarDef* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_VarDefList: // VarDefList
        value.YY_MOVE_OR_COPY< VarDefList* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_FLOATCONST: // FLOATCONST
        value.YY_MOVE_OR_COPY< float > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_INTCONST: // INTCONST
        value.YY_MOVE_OR_COPY< int > (YY_MOVE (that.value));
        break;

      default:
        break;
    }

#if 201103L <= YY_CPLUSPLUS
    // that is emptied.
    that.state = empty_state;
#endif
  }

  parser::stack_symbol_type::stack_symbol_type (state_type s, YY_MOVE_REF (symbol_type) that)
    : super_type (s)
  {
    switch (that.kind ())
    {
      case symbol_kind::S_Type: // Type
        value.move< ASTType > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_AddExp: // AddExp
        value.move< AddExp* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_ConstExpList: // ConstExpList
      case symbol_kind::S_ExpList: // ExpList
        value.move< ArrayList* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_Decl: // Decl
      case symbol_kind::S_BlockItem: // BlockItem
      case symbol_kind::S_Stmt: // Stmt
      case symbol_kind::S_PrimaryExp: // PrimaryExp
        value.move< BaseAST* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_Block: // Block
        value.move< Block* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_BlockItemList: // BlockItemList
        value.move< BlockItemList* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_CompUnit: // CompUnit
        value.move< CompUnit* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_ConstDecl: // ConstDecl
        value.move< ConstDecl* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_ConstDef: // ConstDef
        value.move< ConstDef* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_ConstDefList: // ConstDefList
        value.move< ConstDefList* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_EqExp: // EqExp
        value.move< EqExp* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_FuncDef: // FuncDef
        value.move< FuncDef* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_FuncFParam: // FuncFParam
        value.move< FuncParam* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_FuncFParamList: // FuncFParamList
        value.move< FuncParamList* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_FuncRParamList: // FuncRParamList
        value.move< FuncRParamList* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_ConstInitVal: // ConstInitVal
      case symbol_kind::S_InitVal: // InitVal
        value.move< InitVal* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_ConstInitValList: // ConstInitValList
      case symbol_kind::S_InitValList: // InitValList
        value.move< InitValList* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_LAndExp: // LAndExp
        value.move< LAndExp* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_LOrExp: // LOrExp
        value.move< LOrExp* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_LVal: // LVal
        value.move< LVal* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_IDENT: // IDENT
        value.move< LocatedIdentifier > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_MulExp: // MulExp
        value.move< MulExp* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_RelExp: // RelExp
        value.move< RelExp* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_UnaryExp: // UnaryExp
        value.move< UnaryExp* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_VarDecl: // VarDecl
        value.move< VarDecl* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_VarDef: // VarDef
        value.move< VarDef* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_VarDefList: // VarDefList
        value.move< VarDefList* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_FLOATCONST: // FLOATCONST
        value.move< float > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_INTCONST: // INTCONST
        value.move< int > (YY_MOVE (that.value));
        break;

      default:
        break;
    }

    // that is emptied.
    that.kind_ = symbol_kind::S_YYEMPTY;
  }

#if YY_CPLUSPLUS < 201103L
  parser::stack_symbol_type&
  parser::stack_symbol_type::operator= (const stack_symbol_type& that)
  {
    state = that.state;
    switch (that.kind ())
    {
      case symbol_kind::S_Type: // Type
        value.copy< ASTType > (that.value);
        break;

      case symbol_kind::S_AddExp: // AddExp
        value.copy< AddExp* > (that.value);
        break;

      case symbol_kind::S_ConstExpList: // ConstExpList
      case symbol_kind::S_ExpList: // ExpList
        value.copy< ArrayList* > (that.value);
        break;

      case symbol_kind::S_Decl: // Decl
      case symbol_kind::S_BlockItem: // BlockItem
      case symbol_kind::S_Stmt: // Stmt
      case symbol_kind::S_PrimaryExp: // PrimaryExp
        value.copy< BaseAST* > (that.value);
        break;

      case symbol_kind::S_Block: // Block
        value.copy< Block* > (that.value);
        break;

      case symbol_kind::S_BlockItemList: // BlockItemList
        value.copy< BlockItemList* > (that.value);
        break;

      case symbol_kind::S_CompUnit: // CompUnit
        value.copy< CompUnit* > (that.value);
        break;

      case symbol_kind::S_ConstDecl: // ConstDecl
        value.copy< ConstDecl* > (that.value);
        break;

      case symbol_kind::S_ConstDef: // ConstDef
        value.copy< ConstDef* > (that.value);
        break;

      case symbol_kind::S_ConstDefList: // ConstDefList
        value.copy< ConstDefList* > (that.value);
        break;

      case symbol_kind::S_EqExp: // EqExp
        value.copy< EqExp* > (that.value);
        break;

      case symbol_kind::S_FuncDef: // FuncDef
        value.copy< FuncDef* > (that.value);
        break;

      case symbol_kind::S_FuncFParam: // FuncFParam
        value.copy< FuncParam* > (that.value);
        break;

      case symbol_kind::S_FuncFParamList: // FuncFParamList
        value.copy< FuncParamList* > (that.value);
        break;

      case symbol_kind::S_FuncRParamList: // FuncRParamList
        value.copy< FuncRParamList* > (that.value);
        break;

      case symbol_kind::S_ConstInitVal: // ConstInitVal
      case symbol_kind::S_InitVal: // InitVal
        value.copy< InitVal* > (that.value);
        break;

      case symbol_kind::S_ConstInitValList: // ConstInitValList
      case symbol_kind::S_InitValList: // InitValList
        value.copy< InitValList* > (that.value);
        break;

      case symbol_kind::S_LAndExp: // LAndExp
        value.copy< LAndExp* > (that.value);
        break;

      case symbol_kind::S_LOrExp: // LOrExp
        value.copy< LOrExp* > (that.value);
        break;

      case symbol_kind::S_LVal: // LVal
        value.copy< LVal* > (that.value);
        break;

      case symbol_kind::S_IDENT: // IDENT
        value.copy< LocatedIdentifier > (that.value);
        break;

      case symbol_kind::S_MulExp: // MulExp
        value.copy< MulExp* > (that.value);
        break;

      case symbol_kind::S_RelExp: // RelExp
        value.copy< RelExp* > (that.value);
        break;

      case symbol_kind::S_UnaryExp: // UnaryExp
        value.copy< UnaryExp* > (that.value);
        break;

      case symbol_kind::S_VarDecl: // VarDecl
        value.copy< VarDecl* > (that.value);
        break;

      case symbol_kind::S_VarDef: // VarDef
        value.copy< VarDef* > (that.value);
        break;

      case symbol_kind::S_VarDefList: // VarDefList
        value.copy< VarDefList* > (that.value);
        break;

      case symbol_kind::S_FLOATCONST: // FLOATCONST
        value.copy< float > (that.value);
        break;

      case symbol_kind::S_INTCONST: // INTCONST
        value.copy< int > (that.value);
        break;

      default:
        break;
    }

    return *this;
  }

  parser::stack_symbol_type&
  parser::stack_symbol_type::operator= (stack_symbol_type& that)
  {
    state = that.state;
    switch (that.kind ())
    {
      case symbol_kind::S_Type: // Type
        value.move< ASTType > (that.value);
        break;

      case symbol_kind::S_AddExp: // AddExp
        value.move< AddExp* > (that.value);
        break;

      case symbol_kind::S_ConstExpList: // ConstExpList
      case symbol_kind::S_ExpList: // ExpList
        value.move< ArrayList* > (that.value);
        break;

      case symbol_kind::S_Decl: // Decl
      case symbol_kind::S_BlockItem: // BlockItem
      case symbol_kind::S_Stmt: // Stmt
      case symbol_kind::S_PrimaryExp: // PrimaryExp
        value.move< BaseAST* > (that.value);
        break;

      case symbol_kind::S_Block: // Block
        value.move< Block* > (that.value);
        break;

      case symbol_kind::S_BlockItemList: // BlockItemList
        value.move< BlockItemList* > (that.value);
        break;

      case symbol_kind::S_CompUnit: // CompUnit
        value.move< CompUnit* > (that.value);
        break;

      case symbol_kind::S_ConstDecl: // ConstDecl
        value.move< ConstDecl* > (that.value);
        break;

      case symbol_kind::S_ConstDef: // ConstDef
        value.move< ConstDef* > (that.value);
        break;

      case symbol_kind::S_ConstDefList: // ConstDefList
        value.move< ConstDefList* > (that.value);
        break;

      case symbol_kind::S_EqExp: // EqExp
        value.move< EqExp* > (that.value);
        break;

      case symbol_kind::S_FuncDef: // FuncDef
        value.move< FuncDef* > (that.value);
        break;

      case symbol_kind::S_FuncFParam: // FuncFParam
        value.move< FuncParam* > (that.value);
        break;

      case symbol_kind::S_FuncFParamList: // FuncFParamList
        value.move< FuncParamList* > (that.value);
        break;

      case symbol_kind::S_FuncRParamList: // FuncRParamList
        value.move< FuncRParamList* > (that.value);
        break;

      case symbol_kind::S_ConstInitVal: // ConstInitVal
      case symbol_kind::S_InitVal: // InitVal
        value.move< InitVal* > (that.value);
        break;

      case symbol_kind::S_ConstInitValList: // ConstInitValList
      case symbol_kind::S_InitValList: // InitValList
        value.move< InitValList* > (that.value);
        break;

      case symbol_kind::S_LAndExp: // LAndExp
        value.move< LAndExp* > (that.value);
        break;

      case symbol_kind::S_LOrExp: // LOrExp
        value.move< LOrExp* > (that.value);
        break;

      case symbol_kind::S_LVal: // LVal
        value.move< LVal* > (that.value);
        break;

      case symbol_kind::S_IDENT: // IDENT
        value.move< LocatedIdentifier > (that.value);
        break;

      case symbol_kind::S_MulExp: // MulExp
        value.move< MulExp* > (that.value);
        break;

      case symbol_kind::S_RelExp: // RelExp
        value.move< RelExp* > (that.value);
        break;

      case symbol_kind::S_UnaryExp: // UnaryExp
        value.move< UnaryExp* > (that.value);
        break;

      case symbol_kind::S_VarDecl: // VarDecl
        value.move< VarDecl* > (that.value);
        break;

      case symbol_kind::S_VarDef: // VarDef
        value.move< VarDef* > (that.value);
        break;

      case symbol_kind::S_VarDefList: // VarDefList
        value.move< VarDefList* > (that.value);
        break;

      case symbol_kind::S_FLOATCONST: // FLOATCONST
        value.move< float > (that.value);
        break;

      case symbol_kind::S_INTCONST: // INTCONST
        value.move< int > (that.value);
        break;

      default:
        break;
    }

    // that is emptied.
    that.state = empty_state;
    return *this;
  }
#endif

  template <typename Base>
  void
  parser::yy_destroy_ (const char* yymsg, basic_symbol<Base>& yysym) const
  {
    if (yymsg)
      YY_SYMBOL_PRINT (yymsg, yysym);
  }

#if YYDEBUG
  template <typename Base>
  void
  parser::yy_print_ (std::ostream& yyo, const basic_symbol<Base>& yysym) const
  {
    std::ostream& yyoutput = yyo;
    YY_USE (yyoutput);
    if (yysym.empty ())
      yyo << "empty symbol";
    else
      {
        symbol_kind_type yykind = yysym.kind ();
        yyo << (yykind < YYNTOKENS ? "token" : "nterm")
            << ' ' << yysym.name () << " (";
        YY_USE (yykind);
        yyo << ')';
      }
  }
#endif

  void
  parser::yypush_ (const char* m, YY_MOVE_REF (stack_symbol_type) sym)
  {
    if (m)
      YY_SYMBOL_PRINT (m, sym);
    yystack_.push (YY_MOVE (sym));
  }

  void
  parser::yypush_ (const char* m, state_type s, YY_MOVE_REF (symbol_type) sym)
  {
#if 201103L <= YY_CPLUSPLUS
    yypush_ (m, stack_symbol_type (s, std::move (sym)));
#else
    stack_symbol_type ss (s, sym);
    yypush_ (m, ss);
#endif
  }

  void
  parser::yypop_ (int n) YY_NOEXCEPT
  {
    yystack_.pop (n);
  }

#if YYDEBUG
  std::ostream&
  parser::debug_stream () const
  {
    return *yycdebug_;
  }

  void
  parser::set_debug_stream (std::ostream& o)
  {
    yycdebug_ = &o;
  }


  parser::debug_level_type
  parser::debug_level () const
  {
    return yydebug_;
  }

  void
  parser::set_debug_level (debug_level_type l)
  {
    yydebug_ = l;
  }
#endif // YYDEBUG

  parser::state_type
  parser::yy_lr_goto_state_ (state_type yystate, int yysym)
  {
    int yyr = yypgoto_[yysym - YYNTOKENS] + yystate;
    if (0 <= yyr && yyr <= yylast_ && yycheck_[yyr] == yystate)
      return yytable_[yyr];
    else
      return yydefgoto_[yysym - YYNTOKENS];
  }

  bool
  parser::yy_pact_value_is_default_ (int yyvalue) YY_NOEXCEPT
  {
    return yyvalue == yypact_ninf_;
  }

  bool
  parser::yy_table_value_is_error_ (int yyvalue) YY_NOEXCEPT
  {
    return yyvalue == yytable_ninf_;
  }

  int
  parser::operator() ()
  {
    return parse ();
  }

  int
  parser::parse ()
  {
    int yyn;
    /// Length of the RHS of the rule being reduced.
    int yylen = 0;

    // Error handling.
    int yynerrs_ = 0;
    int yyerrstatus_ = 0;

    /// The lookahead symbol.
    symbol_type yyla;

    /// The return value of parse ().
    int yyresult;

#if YY_EXCEPTIONS
    try
#endif // YY_EXCEPTIONS
      {
    YYCDEBUG << "Starting parse\n";


    /* Initialize the stack.  The initial state will be set in
       yynewstate, since the latter expects the semantical and the
       location values to have been already stored, initialize these
       stacks with a primary value.  */
    yystack_.clear ();
    yypush_ (YY_NULLPTR, 0, YY_MOVE (yyla));

  /*-----------------------------------------------.
  | yynewstate -- push a new symbol on the stack.  |
  `-----------------------------------------------*/
  yynewstate:
    YYCDEBUG << "Entering state " << int (yystack_[0].state) << '\n';
    YY_STACK_PRINT ();

    // Accept?
    if (yystack_[0].state == yyfinal_)
      YYACCEPT;

    goto yybackup;


  /*-----------.
  | yybackup.  |
  `-----------*/
  yybackup:
    // Try to take a decision without lookahead.
    yyn = yypact_[+yystack_[0].state];
    if (yy_pact_value_is_default_ (yyn))
      goto yydefault;

    // Read a lookahead token.
    if (yyla.empty ())
      {
        YYCDEBUG << "Reading a token\n";
#if YY_EXCEPTIONS
        try
#endif // YY_EXCEPTIONS
          {
            symbol_type yylookahead (yylex ());
            yyla.move (yylookahead);
          }
#if YY_EXCEPTIONS
        catch (const syntax_error& yyexc)
          {
            YYCDEBUG << "Caught exception: " << yyexc.what() << '\n';
            error (yyexc);
            goto yyerrlab1;
          }
#endif // YY_EXCEPTIONS
      }
    YY_SYMBOL_PRINT ("Next token is", yyla);

    if (yyla.kind () == symbol_kind::S_YYerror)
    {
      // The scanner already issued an error message, process directly
      // to error recovery.  But do not keep the error token as
      // lookahead, it is too special and may lead us to an endless
      // loop in error recovery. */
      yyla.kind_ = symbol_kind::S_YYUNDEF;
      goto yyerrlab1;
    }

    /* If the proper action on seeing token YYLA.TYPE is to reduce or
       to detect an error, take that action.  */
    yyn += yyla.kind ();
    if (yyn < 0 || yylast_ < yyn || yycheck_[yyn] != yyla.kind ())
      {
        goto yydefault;
      }

    // Reduce or error.
    yyn = yytable_[yyn];
    if (yyn <= 0)
      {
        if (yy_table_value_is_error_ (yyn))
          goto yyerrlab;
        yyn = -yyn;
        goto yyreduce;
      }

    // Count tokens shifted since error; after three, turn off error status.
    if (yyerrstatus_)
      --yyerrstatus_;

    // Shift the lookahead token.
    yypush_ ("Shifting", state_type (yyn), YY_MOVE (yyla));
    goto yynewstate;


  /*-----------------------------------------------------------.
  | yydefault -- do the default action for the current state.  |
  `-----------------------------------------------------------*/
  yydefault:
    yyn = yydefact_[+yystack_[0].state];
    if (yyn == 0)
      goto yyerrlab;
    goto yyreduce;


  /*-----------------------------.
  | yyreduce -- do a reduction.  |
  `-----------------------------*/
  yyreduce:
    yylen = yyr2_[yyn];
    {
      stack_symbol_type yylhs;
      yylhs.state = yy_lr_goto_state_ (yystack_[yylen].state, yyr1_[yyn]);
      /* Variants are always initialized to an empty instance of the
         correct type. The default '$$ = $1' action is NOT applied
         when using variants.  */
      switch (yyr1_[yyn])
    {
      case symbol_kind::S_Type: // Type
        yylhs.value.emplace< ASTType > ();
        break;

      case symbol_kind::S_AddExp: // AddExp
        yylhs.value.emplace< AddExp* > ();
        break;

      case symbol_kind::S_ConstExpList: // ConstExpList
      case symbol_kind::S_ExpList: // ExpList
        yylhs.value.emplace< ArrayList* > ();
        break;

      case symbol_kind::S_Decl: // Decl
      case symbol_kind::S_BlockItem: // BlockItem
      case symbol_kind::S_Stmt: // Stmt
      case symbol_kind::S_PrimaryExp: // PrimaryExp
        yylhs.value.emplace< BaseAST* > ();
        break;

      case symbol_kind::S_Block: // Block
        yylhs.value.emplace< Block* > ();
        break;

      case symbol_kind::S_BlockItemList: // BlockItemList
        yylhs.value.emplace< BlockItemList* > ();
        break;

      case symbol_kind::S_CompUnit: // CompUnit
        yylhs.value.emplace< CompUnit* > ();
        break;

      case symbol_kind::S_ConstDecl: // ConstDecl
        yylhs.value.emplace< ConstDecl* > ();
        break;

      case symbol_kind::S_ConstDef: // ConstDef
        yylhs.value.emplace< ConstDef* > ();
        break;

      case symbol_kind::S_ConstDefList: // ConstDefList
        yylhs.value.emplace< ConstDefList* > ();
        break;

      case symbol_kind::S_EqExp: // EqExp
        yylhs.value.emplace< EqExp* > ();
        break;

      case symbol_kind::S_FuncDef: // FuncDef
        yylhs.value.emplace< FuncDef* > ();
        break;

      case symbol_kind::S_FuncFParam: // FuncFParam
        yylhs.value.emplace< FuncParam* > ();
        break;

      case symbol_kind::S_FuncFParamList: // FuncFParamList
        yylhs.value.emplace< FuncParamList* > ();
        break;

      case symbol_kind::S_FuncRParamList: // FuncRParamList
        yylhs.value.emplace< FuncRParamList* > ();
        break;

      case symbol_kind::S_ConstInitVal: // ConstInitVal
      case symbol_kind::S_InitVal: // InitVal
        yylhs.value.emplace< InitVal* > ();
        break;

      case symbol_kind::S_ConstInitValList: // ConstInitValList
      case symbol_kind::S_InitValList: // InitValList
        yylhs.value.emplace< InitValList* > ();
        break;

      case symbol_kind::S_LAndExp: // LAndExp
        yylhs.value.emplace< LAndExp* > ();
        break;

      case symbol_kind::S_LOrExp: // LOrExp
        yylhs.value.emplace< LOrExp* > ();
        break;

      case symbol_kind::S_LVal: // LVal
        yylhs.value.emplace< LVal* > ();
        break;

      case symbol_kind::S_IDENT: // IDENT
        yylhs.value.emplace< LocatedIdentifier > ();
        break;

      case symbol_kind::S_MulExp: // MulExp
        yylhs.value.emplace< MulExp* > ();
        break;

      case symbol_kind::S_RelExp: // RelExp
        yylhs.value.emplace< RelExp* > ();
        break;

      case symbol_kind::S_UnaryExp: // UnaryExp
        yylhs.value.emplace< UnaryExp* > ();
        break;

      case symbol_kind::S_VarDecl: // VarDecl
        yylhs.value.emplace< VarDecl* > ();
        break;

      case symbol_kind::S_VarDef: // VarDef
        yylhs.value.emplace< VarDef* > ();
        break;

      case symbol_kind::S_VarDefList: // VarDefList
        yylhs.value.emplace< VarDefList* > ();
        break;

      case symbol_kind::S_FLOATCONST: // FLOATCONST
        yylhs.value.emplace< float > ();
        break;

      case symbol_kind::S_INTCONST: // INTCONST
        yylhs.value.emplace< int > ();
        break;

      default:
        break;
    }



      // Perform the reduction.
      YY_REDUCE_PRINT (yyn);
#if YY_EXCEPTIONS
      try
#endif // YY_EXCEPTIONS
        {
          switch (yyn)
            {
  case 2: // Start: CompUnit
#line 118 "src/yacc/sysy.y"
                { ASTRoot = std::unique_ptr<CompUnit>(yystack_[0].value.as < CompUnit* > ()); }
#line 1121 "src/yacc/Bison.cpp"
    break;

  case 3: // CompUnit: CompUnit Decl
#line 122 "src/yacc/sysy.y"
                 {yylhs.value.as < CompUnit* > () = yystack_[1].value.as < CompUnit* > (); yylhs.value.as < CompUnit* > ()->pushBack(yystack_[0].value.as < BaseAST* > ());}
#line 1127 "src/yacc/Bison.cpp"
    break;

  case 4: // CompUnit: CompUnit FuncDef
#line 123 "src/yacc/sysy.y"
                    {yylhs.value.as < CompUnit* > () = yystack_[1].value.as < CompUnit* > (); yylhs.value.as < CompUnit* > ()->pushBack(yystack_[0].value.as < FuncDef* > ());}
#line 1133 "src/yacc/Bison.cpp"
    break;

  case 5: // CompUnit: Decl
#line 124 "src/yacc/sysy.y"
        {yylhs.value.as < CompUnit* > () = new CompUnit(yystack_[0].value.as < BaseAST* > ());}
#line 1139 "src/yacc/Bison.cpp"
    break;

  case 6: // CompUnit: FuncDef
#line 125 "src/yacc/sysy.y"
            {yylhs.value.as < CompUnit* > () = new CompUnit(yystack_[0].value.as < FuncDef* > ());}
#line 1145 "src/yacc/Bison.cpp"
    break;

  case 7: // Decl: ConstDecl
#line 130 "src/yacc/sysy.y"
              { yylhs.value.as < BaseAST* > () = yystack_[0].value.as < ConstDecl* > (); }
#line 1151 "src/yacc/Bison.cpp"
    break;

  case 8: // Decl: VarDecl
#line 131 "src/yacc/sysy.y"
            { yylhs.value.as < BaseAST* > () = yystack_[0].value.as < VarDecl* > (); }
#line 1157 "src/yacc/Bison.cpp"
    break;

  case 9: // ConstDecl: CONST Type ConstDefList SEMICOLON
#line 136 "src/yacc/sysy.y"
                                     { yylhs.value.as < ConstDecl* > () = new ConstDecl(yystack_[2].value.as < ASTType > (),yystack_[1].value.as < ConstDefList* > ()); }
#line 1163 "src/yacc/Bison.cpp"
    break;

  case 10: // ConstDefList: ConstDefList COMMA ConstDef
#line 140 "src/yacc/sysy.y"
                               { yylhs.value.as < ConstDefList* > () = yystack_[2].value.as < ConstDefList* > (); yystack_[2].value.as < ConstDefList* > ()->pushBack(yystack_[0].value.as < ConstDef* > ()); }
#line 1169 "src/yacc/Bison.cpp"
    break;

  case 11: // ConstDefList: ConstDef
#line 141 "src/yacc/sysy.y"
            { yylhs.value.as < ConstDefList* > () = new ConstDefList(yystack_[0].value.as < ConstDef* > ()); }
#line 1175 "src/yacc/Bison.cpp"
    break;

  case 12: // ConstDef: IDENT ASSIGN ConstInitVal
#line 145 "src/yacc/sysy.y"
                             { yylhs.value.as < ConstDef* > () = new ConstDef(yystack_[2].value.as < LocatedIdentifier > ().text,nullptr,yystack_[0].value.as < InitVal* > ()); }
#line 1181 "src/yacc/Bison.cpp"
    break;

  case 13: // ConstDef: IDENT ConstExpList ASSIGN ConstInitVal
#line 146 "src/yacc/sysy.y"
                                          { yylhs.value.as < ConstDef* > () = new ConstDef(yystack_[3].value.as < LocatedIdentifier > ().text,yystack_[2].value.as < ArrayList* > (),yystack_[0].value.as < InitVal* > ()); }
#line 1187 "src/yacc/Bison.cpp"
    break;

  case 14: // ConstExpList: ConstExpList L_MIDDLE_BRACKET AddExp R_MIDDLE_BRACKET
#line 150 "src/yacc/sysy.y"
                                                        { yylhs.value.as < ArrayList* > () = yystack_[3].value.as < ArrayList* > (); yylhs.value.as < ArrayList* > ()->pushBack(yystack_[1].value.as < AddExp* > ()); }
#line 1193 "src/yacc/Bison.cpp"
    break;

  case 15: // ConstExpList: L_MIDDLE_BRACKET AddExp R_MIDDLE_BRACKET
#line 151 "src/yacc/sysy.y"
                                            { yylhs.value.as < ArrayList* > () = new ArrayList(yystack_[1].value.as < AddExp* > ()); }
#line 1199 "src/yacc/Bison.cpp"
    break;

  case 16: // ConstInitVal: AddExp
#line 155 "src/yacc/sysy.y"
         { yylhs.value.as < InitVal* > () = new InitVal(yystack_[0].value.as < AddExp* > ()); }
#line 1205 "src/yacc/Bison.cpp"
    break;

  case 17: // ConstInitVal: L_BIG_BRACKET R_BIG_BRACKET
#line 156 "src/yacc/sysy.y"
                              { yylhs.value.as < InitVal* > () = new InitVal(); }
#line 1211 "src/yacc/Bison.cpp"
    break;

  case 18: // ConstInitVal: L_BIG_BRACKET ConstInitValList R_BIG_BRACKET
#line 157 "src/yacc/sysy.y"
                                               { yylhs.value.as < InitVal* > () = new InitVal(yystack_[1].value.as < InitValList* > ()); }
#line 1217 "src/yacc/Bison.cpp"
    break;

  case 19: // ConstInitValList: ConstInitVal
#line 161 "src/yacc/sysy.y"
               { yylhs.value.as < InitValList* > () = new InitValList(yystack_[0].value.as < InitVal* > ()); }
#line 1223 "src/yacc/Bison.cpp"
    break;

  case 20: // ConstInitValList: ConstInitValList COMMA ConstInitVal
#line 162 "src/yacc/sysy.y"
                                      { yylhs.value.as < InitValList* > () = yystack_[2].value.as < InitValList* > (); yylhs.value.as < InitValList* > ()->pushBack(yystack_[0].value.as < InitVal* > ()); }
#line 1229 "src/yacc/Bison.cpp"
    break;

  case 21: // VarDecl: Type VarDefList SEMICOLON
#line 166 "src/yacc/sysy.y"
                            { yylhs.value.as < VarDecl* > () = new VarDecl(yystack_[2].value.as < ASTType > (),yystack_[1].value.as < VarDefList* > ()); }
#line 1235 "src/yacc/Bison.cpp"
    break;

  case 22: // VarDefList: VarDef
#line 170 "src/yacc/sysy.y"
         { yylhs.value.as < VarDefList* > () = new VarDefList(yystack_[0].value.as < VarDef* > ()); }
#line 1241 "src/yacc/Bison.cpp"
    break;

  case 23: // VarDefList: VarDefList COMMA VarDef
#line 171 "src/yacc/sysy.y"
                          { yylhs.value.as < VarDefList* > () = yystack_[2].value.as < VarDefList* > (); yylhs.value.as < VarDefList* > ()->pushBack(yystack_[0].value.as < VarDef* > ()); }
#line 1247 "src/yacc/Bison.cpp"
    break;

  case 24: // VarDef: IDENT
#line 175 "src/yacc/sysy.y"
         { yylhs.value.as < VarDef* > () = new VarDef(yystack_[0].value.as < LocatedIdentifier > ().text); }
#line 1253 "src/yacc/Bison.cpp"
    break;

  case 25: // VarDef: IDENT ASSIGN InitVal
#line 176 "src/yacc/sysy.y"
                       { yylhs.value.as < VarDef* > () = new VarDef(yystack_[2].value.as < LocatedIdentifier > ().text,nullptr,yystack_[0].value.as < InitVal* > ()); }
#line 1259 "src/yacc/Bison.cpp"
    break;

  case 26: // VarDef: IDENT ConstExpList
#line 177 "src/yacc/sysy.y"
                      { yylhs.value.as < VarDef* > () = new VarDef(yystack_[1].value.as < LocatedIdentifier > ().text,yystack_[0].value.as < ArrayList* > (),nullptr); }
#line 1265 "src/yacc/Bison.cpp"
    break;

  case 27: // VarDef: IDENT ConstExpList ASSIGN InitVal
#line 178 "src/yacc/sysy.y"
                                     { yylhs.value.as < VarDef* > () = new VarDef(yystack_[3].value.as < LocatedIdentifier > ().text,yystack_[2].value.as < ArrayList* > (),yystack_[0].value.as < InitVal* > ()); }
#line 1271 "src/yacc/Bison.cpp"
    break;

  case 28: // InitVal: AddExp
#line 182 "src/yacc/sysy.y"
          { yylhs.value.as < InitVal* > () = new InitVal(yystack_[0].value.as < AddExp* > ()); }
#line 1277 "src/yacc/Bison.cpp"
    break;

  case 29: // InitVal: L_BIG_BRACKET R_BIG_BRACKET
#line 183 "src/yacc/sysy.y"
                               { yylhs.value.as < InitVal* > () = new InitVal(nullptr); }
#line 1283 "src/yacc/Bison.cpp"
    break;

  case 30: // InitVal: L_BIG_BRACKET InitValList R_BIG_BRACKET
#line 184 "src/yacc/sysy.y"
                                           { yylhs.value.as < InitVal* > () = new InitVal(yystack_[1].value.as < InitValList* > ()); }
#line 1289 "src/yacc/Bison.cpp"
    break;

  case 31: // InitValList: InitVal
#line 188 "src/yacc/sysy.y"
           { yylhs.value.as < InitValList* > () = new InitValList(yystack_[0].value.as < InitVal* > ()); }
#line 1295 "src/yacc/Bison.cpp"
    break;

  case 32: // InitValList: InitValList COMMA InitVal
#line 189 "src/yacc/sysy.y"
                             { yylhs.value.as < InitValList* > () = yystack_[2].value.as < InitValList* > (); yylhs.value.as < InitValList* > ()->pushBack(yystack_[0].value.as < InitVal* > ()); }
#line 1301 "src/yacc/Bison.cpp"
    break;

  case 33: // FuncDef: Type IDENT L_SMALL_BRACKET R_SMALL_BRACKET Block
#line 193 "src/yacc/sysy.y"
                                                   { yylhs.value.as < FuncDef* > () = new FuncDef(yystack_[4].value.as < ASTType > (), yystack_[3].value.as < LocatedIdentifier > ().text, nullptr, yystack_[0].value.as < Block* > ()); }
#line 1307 "src/yacc/Bison.cpp"
    break;

  case 34: // FuncDef: Type IDENT L_SMALL_BRACKET FuncFParamList R_SMALL_BRACKET Block
#line 194 "src/yacc/sysy.y"
                                                                  { yylhs.value.as < FuncDef* > () = new FuncDef(yystack_[5].value.as < ASTType > (), yystack_[4].value.as < LocatedIdentifier > ().text, yystack_[2].value.as < FuncParamList* > (), yystack_[0].value.as < Block* > ()); }
#line 1313 "src/yacc/Bison.cpp"
    break;

  case 35: // FuncFParamList: FuncFParam
#line 198 "src/yacc/sysy.y"
              { yylhs.value.as < FuncParamList* > () = new FuncParamList(yystack_[0].value.as < FuncParam* > ()); }
#line 1319 "src/yacc/Bison.cpp"
    break;

  case 36: // FuncFParamList: FuncFParamList COMMA FuncFParam
#line 199 "src/yacc/sysy.y"
                                   { yylhs.value.as < FuncParamList* > () = yystack_[2].value.as < FuncParamList* > (); yylhs.value.as < FuncParamList* > ()->pushBack(yystack_[0].value.as < FuncParam* > ()); }
#line 1325 "src/yacc/Bison.cpp"
    break;

  case 37: // FuncFParam: Type IDENT
#line 203 "src/yacc/sysy.y"
              { yylhs.value.as < FuncParam* > () = new FuncParam(yystack_[1].value.as < ASTType > (),yystack_[0].value.as < LocatedIdentifier > ().text); }
#line 1331 "src/yacc/Bison.cpp"
    break;

  case 38: // FuncFParam: Type IDENT L_MIDDLE_BRACKET R_MIDDLE_BRACKET
#line 204 "src/yacc/sysy.y"
                                                { yylhs.value.as < FuncParam* > () = new FuncParam(yystack_[3].value.as < ASTType > (),yystack_[2].value.as < LocatedIdentifier > ().text,true); }
#line 1337 "src/yacc/Bison.cpp"
    break;

  case 39: // FuncFParam: Type IDENT ExpList
#line 205 "src/yacc/sysy.y"
                      { yylhs.value.as < FuncParam* > () = new FuncParam(yystack_[2].value.as < ASTType > (),yystack_[1].value.as < LocatedIdentifier > ().text,false,yystack_[0].value.as < ArrayList* > ()); }
#line 1343 "src/yacc/Bison.cpp"
    break;

  case 40: // FuncFParam: Type IDENT L_MIDDLE_BRACKET R_MIDDLE_BRACKET ExpList
#line 206 "src/yacc/sysy.y"
                                                        { yylhs.value.as < FuncParam* > () = new FuncParam(yystack_[4].value.as < ASTType > (),yystack_[3].value.as < LocatedIdentifier > ().text,true,yystack_[0].value.as < ArrayList* > ()); }
#line 1349 "src/yacc/Bison.cpp"
    break;

  case 41: // Block: L_BIG_BRACKET R_BIG_BRACKET
#line 210 "src/yacc/sysy.y"
                              { yylhs.value.as < Block* > () = new Block(nullptr); }
#line 1355 "src/yacc/Bison.cpp"
    break;

  case 42: // Block: L_BIG_BRACKET BlockItemList R_BIG_BRACKET
#line 211 "src/yacc/sysy.y"
                                             { yylhs.value.as < Block* > () = new Block(yystack_[1].value.as < BlockItemList* > ()); }
#line 1361 "src/yacc/Bison.cpp"
    break;

  case 43: // BlockItemList: BlockItem
#line 215 "src/yacc/sysy.y"
             { yylhs.value.as < BlockItemList* > () = new BlockItemList(yystack_[0].value.as < BaseAST* > ()); }
#line 1367 "src/yacc/Bison.cpp"
    break;

  case 44: // BlockItemList: BlockItemList BlockItem
#line 216 "src/yacc/sysy.y"
                           { yylhs.value.as < BlockItemList* > () = yystack_[1].value.as < BlockItemList* > (); yylhs.value.as < BlockItemList* > ()->pushBack(yystack_[0].value.as < BaseAST* > ()); }
#line 1373 "src/yacc/Bison.cpp"
    break;

  case 45: // BlockItem: Decl
#line 220 "src/yacc/sysy.y"
       { yylhs.value.as < BaseAST* > () = yystack_[0].value.as < BaseAST* > (); }
#line 1379 "src/yacc/Bison.cpp"
    break;

  case 46: // BlockItem: Stmt
#line 221 "src/yacc/sysy.y"
       { yylhs.value.as < BaseAST* > () = yystack_[0].value.as < BaseAST* > (); }
#line 1385 "src/yacc/Bison.cpp"
    break;

  case 47: // Stmt: LVal ASSIGN AddExp SEMICOLON
#line 226 "src/yacc/sysy.y"
                               { yylhs.value.as < BaseAST* > () = new AssignStmt(yystack_[3].value.as < LVal* > (),yystack_[1].value.as < AddExp* > ()); }
#line 1391 "src/yacc/Bison.cpp"
    break;

  case 48: // Stmt: SEMICOLON
#line 227 "src/yacc/sysy.y"
             { yylhs.value.as < BaseAST* > () = new ExpStmt(nullptr); }
#line 1397 "src/yacc/Bison.cpp"
    break;

  case 49: // Stmt: AddExp SEMICOLON
#line 228 "src/yacc/sysy.y"
                    { yylhs.value.as < BaseAST* > () = new ExpStmt(yystack_[1].value.as < AddExp* > ()); }
#line 1403 "src/yacc/Bison.cpp"
    break;

  case 50: // Stmt: Block
#line 229 "src/yacc/sysy.y"
         { yylhs.value.as < BaseAST* > () = yystack_[0].value.as < Block* > (); }
#line 1409 "src/yacc/Bison.cpp"
    break;

  case 51: // Stmt: IF L_SMALL_BRACKET LOrExp R_SMALL_BRACKET Stmt ELSE Stmt
#line 230 "src/yacc/sysy.y"
                                                            { yylhs.value.as < BaseAST* > () = new IfStmt(yystack_[4].value.as < LOrExp* > (),yystack_[2].value.as < BaseAST* > (),yystack_[0].value.as < BaseAST* > ()); }
#line 1415 "src/yacc/Bison.cpp"
    break;

  case 52: // Stmt: IF L_SMALL_BRACKET LOrExp R_SMALL_BRACKET Stmt
#line 231 "src/yacc/sysy.y"
                                                  { yylhs.value.as < BaseAST* > () = new IfStmt(yystack_[2].value.as < LOrExp* > (),yystack_[0].value.as < BaseAST* > ()); }
#line 1421 "src/yacc/Bison.cpp"
    break;

  case 53: // Stmt: WHILE L_SMALL_BRACKET LOrExp R_SMALL_BRACKET Stmt
#line 232 "src/yacc/sysy.y"
                                                     { yylhs.value.as < BaseAST* > () = new WhileStmt(yystack_[2].value.as < LOrExp* > (),yystack_[0].value.as < BaseAST* > ()); }
#line 1427 "src/yacc/Bison.cpp"
    break;

  case 54: // Stmt: BREAK SEMICOLON
#line 233 "src/yacc/sysy.y"
                   { yylhs.value.as < BaseAST* > () = new BreakStmt(); }
#line 1433 "src/yacc/Bison.cpp"
    break;

  case 55: // Stmt: CONTINUE SEMICOLON
#line 234 "src/yacc/sysy.y"
                      { yylhs.value.as < BaseAST* > () = new ContinueStmt(); }
#line 1439 "src/yacc/Bison.cpp"
    break;

  case 56: // Stmt: RETURN SEMICOLON
#line 235 "src/yacc/sysy.y"
                    { yylhs.value.as < BaseAST* > () = new ReturnStmt(); }
#line 1445 "src/yacc/Bison.cpp"
    break;

  case 57: // Stmt: RETURN AddExp SEMICOLON
#line 236 "src/yacc/sysy.y"
                           { yylhs.value.as < BaseAST* > () = new ReturnStmt(yystack_[1].value.as < AddExp* > ()); }
#line 1451 "src/yacc/Bison.cpp"
    break;

  case 58: // LVal: IDENT
#line 241 "src/yacc/sysy.y"
        { yylhs.value.as < LVal* > () = new LVal(yystack_[0].value.as < LocatedIdentifier > ().text); }
#line 1457 "src/yacc/Bison.cpp"
    break;

  case 59: // LVal: IDENT ExpList
#line 242 "src/yacc/sysy.y"
                 { yylhs.value.as < LVal* > () = new LVal(yystack_[1].value.as < LocatedIdentifier > ().text,yystack_[0].value.as < ArrayList* > ()); }
#line 1463 "src/yacc/Bison.cpp"
    break;

  case 60: // ExpList: L_MIDDLE_BRACKET AddExp R_MIDDLE_BRACKET
#line 246 "src/yacc/sysy.y"
                                           { yylhs.value.as < ArrayList* > () = new ArrayList(yystack_[1].value.as < AddExp* > ()); }
#line 1469 "src/yacc/Bison.cpp"
    break;

  case 61: // ExpList: ExpList L_MIDDLE_BRACKET AddExp R_MIDDLE_BRACKET
#line 247 "src/yacc/sysy.y"
                                                   { yylhs.value.as < ArrayList* > () = yystack_[3].value.as < ArrayList* > (); yylhs.value.as < ArrayList* > ()->pushBack(yystack_[1].value.as < AddExp* > ()); }
#line 1475 "src/yacc/Bison.cpp"
    break;

  case 62: // PrimaryExp: L_SMALL_BRACKET AddExp R_SMALL_BRACKET
#line 251 "src/yacc/sysy.y"
                                          { yylhs.value.as < BaseAST* > () = yystack_[1].value.as < AddExp* > (); }
#line 1481 "src/yacc/Bison.cpp"
    break;

  case 63: // PrimaryExp: LVal
#line 252 "src/yacc/sysy.y"
        { yylhs.value.as < BaseAST* > () = yystack_[0].value.as < LVal* > (); }
#line 1487 "src/yacc/Bison.cpp"
    break;

  case 64: // PrimaryExp: INTCONST
#line 253 "src/yacc/sysy.y"
           { yylhs.value.as < BaseAST* > () = new ConValue<int>(yystack_[0].value.as < int > ()); }
#line 1493 "src/yacc/Bison.cpp"
    break;

  case 65: // PrimaryExp: FLOATCONST
#line 254 "src/yacc/sysy.y"
             { yylhs.value.as < BaseAST* > () = new ConValue<float>(yystack_[0].value.as < float > ()); }
#line 1499 "src/yacc/Bison.cpp"
    break;

  case 66: // PrimaryExp: IDENT L_SMALL_BRACKET R_SMALL_BRACKET
#line 255 "src/yacc/sysy.y"
                                         {yylhs.value.as < BaseAST* > () = new FuncCall(yystack_[2].value.as < LocatedIdentifier > ().text, yystack_[2].value.as < LocatedIdentifier > ().line);}
#line 1505 "src/yacc/Bison.cpp"
    break;

  case 67: // PrimaryExp: IDENT L_SMALL_BRACKET FuncRParamList R_SMALL_BRACKET
#line 256 "src/yacc/sysy.y"
                                                       {yylhs.value.as < BaseAST* > () = new FuncCall(yystack_[3].value.as < LocatedIdentifier > ().text, yystack_[1].value.as < FuncRParamList* > (), yystack_[3].value.as < LocatedIdentifier > ().line);}
#line 1511 "src/yacc/Bison.cpp"
    break;

  case 68: // UnaryExp: PrimaryExp
#line 262 "src/yacc/sysy.y"
             { yylhs.value.as < UnaryExp* > () = new UnaryExp(yystack_[0].value.as < BaseAST* > ()); }
#line 1517 "src/yacc/Bison.cpp"
    break;

  case 69: // UnaryExp: ADD UnaryExp
#line 263 "src/yacc/sysy.y"
               { yylhs.value.as < UnaryExp* > () = yystack_[0].value.as < UnaryExp* > (); yylhs.value.as < UnaryExp* > ()->pushFront(SY_ADD); }
#line 1523 "src/yacc/Bison.cpp"
    break;

  case 70: // UnaryExp: SUB UnaryExp
#line 264 "src/yacc/sysy.y"
               { yylhs.value.as < UnaryExp* > () = yystack_[0].value.as < UnaryExp* > (); yylhs.value.as < UnaryExp* > ()->pushFront(SY_SUB); }
#line 1529 "src/yacc/Bison.cpp"
    break;

  case 71: // UnaryExp: NOT UnaryExp
#line 265 "src/yacc/sysy.y"
               { yylhs.value.as < UnaryExp* > () = yystack_[0].value.as < UnaryExp* > (); yylhs.value.as < UnaryExp* > ()->pushFront(SY_NOT); }
#line 1535 "src/yacc/Bison.cpp"
    break;

  case 72: // FuncRParamList: AddExp
#line 270 "src/yacc/sysy.y"
          { yylhs.value.as < FuncRParamList* > () = new FuncRParamList(yystack_[0].value.as < AddExp* > ()); }
#line 1541 "src/yacc/Bison.cpp"
    break;

  case 73: // FuncRParamList: FuncRParamList COMMA AddExp
#line 271 "src/yacc/sysy.y"
                              { yylhs.value.as < FuncRParamList* > () = yystack_[2].value.as < FuncRParamList* > (); yylhs.value.as < FuncRParamList* > ()->pushBack(yystack_[0].value.as < AddExp* > ()); }
#line 1547 "src/yacc/Bison.cpp"
    break;

  case 74: // MulExp: UnaryExp
#line 275 "src/yacc/sysy.y"
            { yylhs.value.as < MulExp* > () = new MulExp(yystack_[0].value.as < UnaryExp* > ()); }
#line 1553 "src/yacc/Bison.cpp"
    break;

  case 75: // MulExp: MulExp MUL UnaryExp
#line 276 "src/yacc/sysy.y"
                       { yylhs.value.as < MulExp* > () = yystack_[2].value.as < MulExp* > (); yylhs.value.as < MulExp* > ()->pushBack(SY_MUL); yylhs.value.as < MulExp* > ()->pushBack(yystack_[0].value.as < UnaryExp* > ()); }
#line 1559 "src/yacc/Bison.cpp"
    break;

  case 76: // MulExp: MulExp DIV UnaryExp
#line 277 "src/yacc/sysy.y"
                       { yylhs.value.as < MulExp* > () = yystack_[2].value.as < MulExp* > (); yylhs.value.as < MulExp* > ()->pushBack(SY_DIV); yylhs.value.as < MulExp* > ()->pushBack(yystack_[0].value.as < UnaryExp* > ()); }
#line 1565 "src/yacc/Bison.cpp"
    break;

  case 77: // MulExp: MulExp MOD UnaryExp
#line 278 "src/yacc/sysy.y"
                       { yylhs.value.as < MulExp* > () = yystack_[2].value.as < MulExp* > (); yylhs.value.as < MulExp* > ()->pushBack(SY_MOD); yylhs.value.as < MulExp* > ()->pushBack(yystack_[0].value.as < UnaryExp* > ()); }
#line 1571 "src/yacc/Bison.cpp"
    break;

  case 78: // MulExp: MulExp GEMM UnaryExp
#line 279 "src/yacc/sysy.y"
                        { yylhs.value.as < MulExp* > () = yystack_[2].value.as < MulExp* > (); yylhs.value.as < MulExp* > ()->pushBack(SY_GEMM); yylhs.value.as < MulExp* > ()->pushBack(yystack_[0].value.as < UnaryExp* > ()); }
#line 1577 "src/yacc/Bison.cpp"
    break;

  case 79: // AddExp: MulExp
#line 283 "src/yacc/sysy.y"
         { yylhs.value.as < AddExp* > () = new AddExp(yystack_[0].value.as < MulExp* > ()); }
#line 1583 "src/yacc/Bison.cpp"
    break;

  case 80: // AddExp: AddExp ADD MulExp
#line 284 "src/yacc/sysy.y"
                    { yylhs.value.as < AddExp* > () = yystack_[2].value.as < AddExp* > (); yylhs.value.as < AddExp* > ()->pushBack(SY_ADD); yylhs.value.as < AddExp* > ()->pushBack(yystack_[0].value.as < MulExp* > ()); }
#line 1589 "src/yacc/Bison.cpp"
    break;

  case 81: // AddExp: AddExp SUB MulExp
#line 285 "src/yacc/sysy.y"
                    { yylhs.value.as < AddExp* > () = yystack_[2].value.as < AddExp* > (); yylhs.value.as < AddExp* > ()->pushBack(SY_SUB); yylhs.value.as < AddExp* > ()->pushBack(yystack_[0].value.as < MulExp* > ()); }
#line 1595 "src/yacc/Bison.cpp"
    break;

  case 82: // RelExp: AddExp
#line 289 "src/yacc/sysy.y"
          {yylhs.value.as < RelExp* > () = new RelExp(yystack_[0].value.as < AddExp* > ());}
#line 1601 "src/yacc/Bison.cpp"
    break;

  case 83: // RelExp: RelExp LESS AddExp
#line 290 "src/yacc/sysy.y"
                     { yylhs.value.as < RelExp* > () = yystack_[2].value.as < RelExp* > (); yylhs.value.as < RelExp* > ()->pushBack(SY_LESS); yylhs.value.as < RelExp* > ()->pushBack(yystack_[0].value.as < AddExp* > ()); }
#line 1607 "src/yacc/Bison.cpp"
    break;

  case 84: // RelExp: RelExp GREAT AddExp
#line 291 "src/yacc/sysy.y"
                      { yylhs.value.as < RelExp* > () = yystack_[2].value.as < RelExp* > (); yylhs.value.as < RelExp* > ()->pushBack(SY_GREAT); yylhs.value.as < RelExp* > ()->pushBack(yystack_[0].value.as < AddExp* > ()); }
#line 1613 "src/yacc/Bison.cpp"
    break;

  case 85: // RelExp: RelExp LESSEQ AddExp
#line 292 "src/yacc/sysy.y"
                       { yylhs.value.as < RelExp* > () = yystack_[2].value.as < RelExp* > (); yylhs.value.as < RelExp* > ()->pushBack(SY_LESSEQ); yylhs.value.as < RelExp* > ()->pushBack(yystack_[0].value.as < AddExp* > ()); }
#line 1619 "src/yacc/Bison.cpp"
    break;

  case 86: // RelExp: RelExp GREATEQ AddExp
#line 293 "src/yacc/sysy.y"
                        { yylhs.value.as < RelExp* > () = yystack_[2].value.as < RelExp* > (); yylhs.value.as < RelExp* > ()->pushBack(SY_GREATEQ); yylhs.value.as < RelExp* > ()->pushBack(yystack_[0].value.as < AddExp* > ()); }
#line 1625 "src/yacc/Bison.cpp"
    break;

  case 87: // EqExp: RelExp
#line 297 "src/yacc/sysy.y"
         { yylhs.value.as < EqExp* > () = new EqExp(yystack_[0].value.as < RelExp* > ()); }
#line 1631 "src/yacc/Bison.cpp"
    break;

  case 88: // EqExp: EqExp EQ RelExp
#line 298 "src/yacc/sysy.y"
                  { yylhs.value.as < EqExp* > () = yystack_[2].value.as < EqExp* > (); yylhs.value.as < EqExp* > ()->pushBack(SY_EQ); yylhs.value.as < EqExp* > ()->pushBack(yystack_[0].value.as < RelExp* > ()); }
#line 1637 "src/yacc/Bison.cpp"
    break;

  case 89: // EqExp: EqExp NOTEQ RelExp
#line 299 "src/yacc/sysy.y"
                     { yylhs.value.as < EqExp* > () = yystack_[2].value.as < EqExp* > (); yylhs.value.as < EqExp* > ()->pushBack(SY_NOTEQ); yylhs.value.as < EqExp* > ()->pushBack(yystack_[0].value.as < RelExp* > ()); }
#line 1643 "src/yacc/Bison.cpp"
    break;

  case 90: // LAndExp: EqExp
#line 303 "src/yacc/sysy.y"
         { yylhs.value.as < LAndExp* > () = new LAndExp(yystack_[0].value.as < EqExp* > ()); }
#line 1649 "src/yacc/Bison.cpp"
    break;

  case 91: // LAndExp: LAndExp AND EqExp
#line 304 "src/yacc/sysy.y"
                    { yylhs.value.as < LAndExp* > () = yystack_[2].value.as < LAndExp* > (); yylhs.value.as < LAndExp* > ()->pushBack(SY_AND); yylhs.value.as < LAndExp* > ()->pushBack(yystack_[0].value.as < EqExp* > ()); }
#line 1655 "src/yacc/Bison.cpp"
    break;

  case 92: // LOrExp: LAndExp
#line 308 "src/yacc/sysy.y"
          { yylhs.value.as < LOrExp* > () = new LOrExp(yystack_[0].value.as < LAndExp* > ()); }
#line 1661 "src/yacc/Bison.cpp"
    break;

  case 93: // LOrExp: LOrExp OR LAndExp
#line 309 "src/yacc/sysy.y"
                    { yylhs.value.as < LOrExp* > () = yystack_[2].value.as < LOrExp* > (); yylhs.value.as < LOrExp* > ()->pushBack(SY_OR); yylhs.value.as < LOrExp* > ()->pushBack(yystack_[0].value.as < LAndExp* > ()); }
#line 1667 "src/yacc/Bison.cpp"
    break;

  case 94: // Type: VOID
#line 314 "src/yacc/sysy.y"
        { yylhs.value.as < ASTType > () = ASTType::makeScalar(ASTScalarKind::Void); }
#line 1673 "src/yacc/Bison.cpp"
    break;

  case 95: // Type: INT
#line 315 "src/yacc/sysy.y"
       { yylhs.value.as < ASTType > () = ASTType::makeScalar(ASTScalarKind::Int32); }
#line 1679 "src/yacc/Bison.cpp"
    break;

  case 96: // Type: FLOAT
#line 316 "src/yacc/sysy.y"
         { yylhs.value.as < ASTType > () = ASTType::makeScalar(ASTScalarKind::Float32); }
#line 1685 "src/yacc/Bison.cpp"
    break;

  case 97: // Type: TENSOR INT
#line 317 "src/yacc/sysy.y"
              { yylhs.value.as < ASTType > () = ASTType::makeTensor(ASTScalarKind::Int32); }
#line 1691 "src/yacc/Bison.cpp"
    break;

  case 98: // Type: TENSOR FLOAT
#line 318 "src/yacc/sysy.y"
                { yylhs.value.as < ASTType > () = ASTType::makeTensor(ASTScalarKind::Float32); }
#line 1697 "src/yacc/Bison.cpp"
    break;


#line 1701 "src/yacc/Bison.cpp"

            default:
              break;
            }
        }
#if YY_EXCEPTIONS
      catch (const syntax_error& yyexc)
        {
          YYCDEBUG << "Caught exception: " << yyexc.what() << '\n';
          error (yyexc);
          YYERROR;
        }
#endif // YY_EXCEPTIONS
      YY_SYMBOL_PRINT ("-> $$ =", yylhs);
      yypop_ (yylen);
      yylen = 0;

      // Shift the result of the reduction.
      yypush_ (YY_NULLPTR, YY_MOVE (yylhs));
    }
    goto yynewstate;


  /*--------------------------------------.
  | yyerrlab -- here on detecting error.  |
  `--------------------------------------*/
  yyerrlab:
    // If not already recovering from an error, report this error.
    if (!yyerrstatus_)
      {
        ++yynerrs_;
        std::string msg = YY_("syntax error");
        error (YY_MOVE (msg));
      }


    if (yyerrstatus_ == 3)
      {
        /* If just tried and failed to reuse lookahead token after an
           error, discard it.  */

        // Return failure if at end of input.
        if (yyla.kind () == symbol_kind::S_YYEOF)
          YYABORT;
        else if (!yyla.empty ())
          {
            yy_destroy_ ("Error: discarding", yyla);
            yyla.clear ();
          }
      }

    // Else will try to reuse lookahead token after shifting the error token.
    goto yyerrlab1;


  /*---------------------------------------------------.
  | yyerrorlab -- error raised explicitly by YYERROR.  |
  `---------------------------------------------------*/
  yyerrorlab:
    /* Pacify compilers when the user code never invokes YYERROR and
       the label yyerrorlab therefore never appears in user code.  */
    if (false)
      YYERROR;

    /* Do not reclaim the symbols of the rule whose action triggered
       this YYERROR.  */
    yypop_ (yylen);
    yylen = 0;
    YY_STACK_PRINT ();
    goto yyerrlab1;


  /*-------------------------------------------------------------.
  | yyerrlab1 -- common code for both syntax error and YYERROR.  |
  `-------------------------------------------------------------*/
  yyerrlab1:
    yyerrstatus_ = 3;   // Each real token shifted decrements this.
    // Pop stack until we find a state that shifts the error token.
    for (;;)
      {
        yyn = yypact_[+yystack_[0].state];
        if (!yy_pact_value_is_default_ (yyn))
          {
            yyn += symbol_kind::S_YYerror;
            if (0 <= yyn && yyn <= yylast_
                && yycheck_[yyn] == symbol_kind::S_YYerror)
              {
                yyn = yytable_[yyn];
                if (0 < yyn)
                  break;
              }
          }

        // Pop the current state because it cannot handle the error token.
        if (yystack_.size () == 1)
          YYABORT;

        yy_destroy_ ("Error: popping", yystack_[0]);
        yypop_ ();
        YY_STACK_PRINT ();
      }
    {
      stack_symbol_type error_token;


      // Shift the error token.
      error_token.state = state_type (yyn);
      yypush_ ("Shifting", YY_MOVE (error_token));
    }
    goto yynewstate;


  /*-------------------------------------.
  | yyacceptlab -- YYACCEPT comes here.  |
  `-------------------------------------*/
  yyacceptlab:
    yyresult = 0;
    goto yyreturn;


  /*-----------------------------------.
  | yyabortlab -- YYABORT comes here.  |
  `-----------------------------------*/
  yyabortlab:
    yyresult = 1;
    goto yyreturn;


  /*-----------------------------------------------------.
  | yyreturn -- parsing is finished, return the result.  |
  `-----------------------------------------------------*/
  yyreturn:
    if (!yyla.empty ())
      yy_destroy_ ("Cleanup: discarding lookahead", yyla);

    /* Do not reclaim the symbols of the rule whose action triggered
       this YYABORT or YYACCEPT.  */
    yypop_ (yylen);
    YY_STACK_PRINT ();
    while (1 < yystack_.size ())
      {
        yy_destroy_ ("Cleanup: popping", yystack_[0]);
        yypop_ ();
      }

    return yyresult;
  }
#if YY_EXCEPTIONS
    catch (...)
      {
        YYCDEBUG << "Exception caught: cleaning lookahead and stack\n";
        // Do not try to display the values of the reclaimed symbols,
        // as their printers might throw an exception.
        if (!yyla.empty ())
          yy_destroy_ (YY_NULLPTR, yyla);

        while (1 < yystack_.size ())
          {
            yy_destroy_ (YY_NULLPTR, yystack_[0]);
            yypop_ ();
          }
        throw;
      }
#endif // YY_EXCEPTIONS
  }

  void
  parser::error (const syntax_error& yyexc)
  {
    error (yyexc.what ());
  }

#if YYDEBUG || 0
  const char *
  parser::symbol_name (symbol_kind_type yysymbol)
  {
    return yytname_[yysymbol];
  }
#endif // #if YYDEBUG || 0









  const signed char parser::yypact_ninf_ = -81;

  const signed char parser::yytable_ninf_ = -1;

  const short
  parser::yypact_[] =
  {
     275,   268,   -81,   -81,   -81,   180,     4,   275,   -81,   -81,
     -81,   -81,    10,    64,   -81,   -81,   -81,   -81,   -81,    55,
     168,   -81,    -7,   179,   -81,   218,    49,    95,    -4,   -81,
      90,   232,   100,   -81,    64,   -81,   -81,    18,    95,    95,
      95,    95,   192,   -81,   -81,   -81,   -81,   233,   206,   -12,
      71,   -81,   101,    24,   218,    95,   114,   -81,   212,   -81,
     206,   232,   -81,   258,    95,    75,   -81,   -81,   -81,    53,
     -81,   -81,   125,    95,    95,    95,    95,    95,    95,   115,
     -81,   -12,   268,    77,   -81,   -81,    42,   -81,   -81,   132,
     -81,   -81,    73,   206,    50,    95,   -81,   -81,   218,   -81,
     -81,   -81,   -81,   233,   233,    63,    94,   108,    16,   105,
     -81,   -81,   -81,   -81,   152,   -81,   -81,   124,     2,    90,
     -81,   -81,   238,    75,   -81,   -81,   232,   -81,    95,   -81,
      72,   -81,   -81,    13,    95,    95,   -81,   -81,   -81,   -81,
      95,   -81,    99,   -81,   206,   -81,   -81,   206,   269,   251,
     151,    -2,     6,    22,    75,    95,    95,    95,    95,    95,
      95,    95,    95,   172,   172,   -81,   206,   206,   206,   206,
     269,   269,   251,   151,   138,   -81,   172,   -81
  };

  const signed char
  parser::yydefact_[] =
  {
       0,     0,    95,    96,    94,     0,     0,     2,     5,     7,
       8,     6,     0,     0,    97,    98,     1,     3,     4,    24,
       0,    22,     0,     0,    11,     0,     0,     0,    26,    21,
       0,     0,     0,     9,     0,    64,    65,    58,     0,     0,
       0,     0,     0,    25,    63,    68,    74,    79,    28,     0,
       0,    35,     0,     0,     0,     0,    24,    23,     0,    12,
      16,     0,    10,     0,     0,    59,    71,    69,    70,     0,
      29,    31,     0,     0,     0,     0,     0,     0,     0,     0,
      33,     0,     0,    37,    15,    27,     0,    17,    19,     0,
      13,    66,     0,    72,     0,     0,    62,    30,     0,    78,
      75,    76,    77,    80,    81,     0,     0,     0,     0,     0,
      41,    48,    45,    50,     0,    43,    46,    63,     0,     0,
      34,    36,     0,    39,    14,    18,     0,    67,     0,    60,
       0,    32,    56,     0,     0,     0,    54,    55,    42,    44,
       0,    49,    38,    20,    73,    61,    57,    82,    87,    90,
      92,     0,     0,     0,    40,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,    47,    83,    84,    85,    86,
      88,    89,    91,    93,    52,    53,     0,    51
  };

  const short
  parser::yypgoto_[] =
  {
     -81,   -81,   -81,    20,   -81,   -81,   137,   156,   -50,   -81,
     -81,   -81,   162,   -33,   -81,   191,   -81,   119,   -44,   -81,
      96,     5,   -72,   -80,   -81,   -26,   -81,   170,   -25,   134,
      51,    58,    89,     0
  };

  const unsigned char
  parser::yydefgoto_[] =
  {
       0,     6,     7,   112,     9,    23,    24,    28,    59,    89,
      10,    20,    21,    43,    72,    11,    50,    51,   113,   114,
     115,   116,    44,    65,    45,    46,    92,    47,   147,   148,
     149,   150,   151,    12
  };

  const unsigned char
  parser::yytable_[] =
  {
      48,    13,    53,   123,    16,    80,    60,   117,    88,    71,
     162,    90,    66,    67,    68,    19,    69,    48,   162,    31,
       8,    85,    54,    77,    78,    79,    52,    17,    27,    48,
      86,    55,   163,    60,    77,    78,    60,   120,    93,    94,
     164,   141,   117,    77,    78,    77,    78,    99,   100,   101,
     102,    63,   146,    64,   118,   136,     2,     3,     4,     5,
      84,   165,   154,    77,    78,   131,    35,    36,    37,    22,
     130,    77,    78,    48,    77,    78,   143,    38,   124,   119,
     133,    25,    52,    49,    39,    40,   129,    96,    26,   118,
      27,   117,   117,    77,    78,    56,    41,    94,    35,    36,
      37,    60,   132,   144,   117,    81,    83,   127,   145,    38,
      95,    82,   122,   128,   119,   153,    39,    40,    35,    36,
      37,     1,     2,     3,     4,     5,    61,   134,    41,    38,
     166,   167,   168,   169,    64,    55,    39,    40,   118,   118,
      25,   135,   105,   106,   137,   107,   108,   109,    41,    27,
     140,   118,    79,   110,   111,    35,    36,    37,     1,     2,
       3,     4,     5,    97,   161,    98,    38,   176,   174,   175,
     125,    62,   126,    39,    40,    35,    36,    37,    32,   105,
     106,   177,   107,   108,   109,    41,    38,    14,    15,    79,
     138,   111,    57,    39,    40,    35,    36,    37,    18,   105,
     106,   121,   107,   108,   109,    41,    38,    29,    30,    79,
     139,   111,   172,    39,    40,    35,    36,    37,    33,    34,
     173,    35,    36,    37,   152,    41,    38,    77,    78,    42,
      70,     0,    38,    39,    40,    35,    36,    37,     0,    39,
      40,    35,    36,    37,    73,    41,    38,   103,   104,    58,
      87,    41,    38,    39,    40,    42,    74,    75,    76,    39,
      40,    35,    36,    37,     0,    41,   159,   160,     0,    58,
       0,    41,    38,     0,   142,     2,     3,     4,     5,    39,
      40,     1,     2,     3,     4,     5,   155,   156,   157,   158,
       0,    41,    91,   170,   171
  };

  const short
  parser::yycheck_[] =
  {
      25,     1,    27,    83,     0,    49,    31,    79,    58,    42,
      12,    61,    38,    39,    40,     5,    41,    42,    12,    26,
       0,    54,    26,    21,    22,    37,    26,     7,    35,    54,
      55,    35,    34,    58,    21,    22,    61,    81,    63,    64,
      34,    39,   114,    21,    22,    21,    22,    73,    74,    75,
      76,    33,    39,    35,    79,    39,     7,     8,     9,    10,
      36,    39,   142,    21,    22,    98,     3,     4,     5,     5,
      95,    21,    22,    98,    21,    22,   126,    14,    36,    79,
     105,    26,    82,    34,    21,    22,    36,    34,    33,   114,
      35,   163,   164,    21,    22,     5,    33,   122,     3,     4,
       5,   126,    39,   128,   176,    34,     5,    34,    36,    14,
      35,    40,    35,    40,   114,   140,    21,    22,     3,     4,
       5,     6,     7,     8,     9,    10,    26,    33,    33,    14,
     155,   156,   157,   158,    35,    35,    21,    22,   163,   164,
      26,    33,    27,    28,    39,    30,    31,    32,    33,    35,
      26,   176,    37,    38,    39,     3,     4,     5,     6,     7,
       8,     9,    10,    38,    13,    40,    14,    29,   163,   164,
      38,    34,    40,    21,    22,     3,     4,     5,    22,    27,
      28,   176,    30,    31,    32,    33,    14,     7,     8,    37,
      38,    39,    30,    21,    22,     3,     4,     5,     7,    27,
      28,    82,    30,    31,    32,    33,    14,    39,    40,    37,
     114,    39,   161,    21,    22,     3,     4,     5,    39,    40,
     162,     3,     4,     5,   135,    33,    14,    21,    22,    37,
      38,    -1,    14,    21,    22,     3,     4,     5,    -1,    21,
      22,     3,     4,     5,    11,    33,    14,    77,    78,    37,
      38,    33,    14,    21,    22,    37,    23,    24,    25,    21,
      22,     3,     4,     5,    -1,    33,    15,    16,    -1,    37,
      -1,    33,    14,    -1,    36,     7,     8,     9,    10,    21,
      22,     6,     7,     8,     9,    10,    17,    18,    19,    20,
      -1,    33,    34,   159,   160
  };

  const signed char
  parser::yystos_[] =
  {
       0,     6,     7,     8,     9,    10,    43,    44,    45,    46,
      52,    57,    75,    75,     7,     8,     0,    45,    57,     5,
      53,    54,     5,    47,    48,    26,    33,    35,    49,    39,
      40,    26,    49,    39,    40,     3,     4,     5,    14,    21,
      22,    33,    37,    55,    64,    66,    67,    69,    70,    34,
      58,    59,    75,    70,    26,    35,     5,    54,    37,    50,
      70,    26,    48,    33,    35,    65,    67,    67,    67,    70,
      38,    55,    56,    11,    23,    24,    25,    21,    22,    37,
      60,    34,    40,     5,    36,    55,    70,    38,    50,    51,
      50,    34,    68,    70,    70,    35,    34,    38,    40,    67,
      67,    67,    67,    69,    69,    27,    28,    30,    31,    32,
      38,    39,    45,    60,    61,    62,    63,    64,    70,    75,
      60,    59,    35,    65,    36,    38,    40,    34,    40,    36,
      70,    55,    39,    70,    33,    33,    39,    39,    38,    62,
      26,    39,    36,    50,    70,    36,    39,    70,    71,    72,
      73,    74,    74,    70,    65,    17,    18,    19,    20,    15,
      16,    13,    12,    34,    34,    39,    70,    70,    70,    70,
      71,    71,    72,    73,    63,    63,    29,    63
  };

  const signed char
  parser::yyr1_[] =
  {
       0,    42,    43,    44,    44,    44,    44,    45,    45,    46,
      47,    47,    48,    48,    49,    49,    50,    50,    50,    51,
      51,    52,    53,    53,    54,    54,    54,    54,    55,    55,
      55,    56,    56,    57,    57,    58,    58,    59,    59,    59,
      59,    60,    60,    61,    61,    62,    62,    63,    63,    63,
      63,    63,    63,    63,    63,    63,    63,    63,    64,    64,
      65,    65,    66,    66,    66,    66,    66,    66,    67,    67,
      67,    67,    68,    68,    69,    69,    69,    69,    69,    70,
      70,    70,    71,    71,    71,    71,    71,    72,    72,    72,
      73,    73,    74,    74,    75,    75,    75,    75,    75
  };

  const signed char
  parser::yyr2_[] =
  {
       0,     2,     1,     2,     2,     1,     1,     1,     1,     4,
       3,     1,     3,     4,     4,     3,     1,     2,     3,     1,
       3,     3,     1,     3,     1,     3,     2,     4,     1,     2,
       3,     1,     3,     5,     6,     1,     3,     2,     4,     3,
       5,     2,     3,     1,     2,     1,     1,     4,     1,     2,
       1,     7,     5,     5,     2,     2,     2,     3,     1,     2,
       3,     4,     3,     1,     1,     1,     3,     4,     1,     2,
       2,     2,     1,     3,     1,     3,     3,     3,     3,     1,
       3,     3,     1,     3,     3,     3,     3,     1,     3,     3,
       1,     3,     1,     3,     1,     1,     1,     2,     2
  };


#if YYDEBUG
  // YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
  // First, the terminals, then, starting at \a YYNTOKENS, nonterminals.
  const char*
  const parser::yytname_[] =
  {
  "\"end of file\"", "error", "\"invalid token\"", "INTCONST",
  "FLOATCONST", "IDENT", "CONST", "INT", "FLOAT", "VOID", "TENSOR", "GEMM",
  "OR", "AND", "NOT", "EQ", "NOTEQ", "LESS", "GREAT", "LESSEQ", "GREATEQ",
  "ADD", "SUB", "MUL", "DIV", "MOD", "ASSIGN", "RETURN", "IF", "ELSE",
  "WHILE", "BREAK", "CONTINUE", "L_SMALL_BRACKET", "R_SMALL_BRACKET",
  "L_MIDDLE_BRACKET", "R_MIDDLE_BRACKET", "L_BIG_BRACKET", "R_BIG_BRACKET",
  "SEMICOLON", "COMMA", "END", "$accept", "Start", "CompUnit", "Decl",
  "ConstDecl", "ConstDefList", "ConstDef", "ConstExpList", "ConstInitVal",
  "ConstInitValList", "VarDecl", "VarDefList", "VarDef", "InitVal",
  "InitValList", "FuncDef", "FuncFParamList", "FuncFParam", "Block",
  "BlockItemList", "BlockItem", "Stmt", "LVal", "ExpList", "PrimaryExp",
  "UnaryExp", "FuncRParamList", "MulExp", "AddExp", "RelExp", "EqExp",
  "LAndExp", "LOrExp", "Type", YY_NULLPTR
  };
#endif


#if YYDEBUG
  const short
  parser::yyrline_[] =
  {
       0,   118,   118,   122,   123,   124,   125,   130,   131,   136,
     140,   141,   145,   146,   150,   151,   155,   156,   157,   161,
     162,   166,   170,   171,   175,   176,   177,   178,   182,   183,
     184,   188,   189,   193,   194,   198,   199,   203,   204,   205,
     206,   210,   211,   215,   216,   220,   221,   226,   227,   228,
     229,   230,   231,   232,   233,   234,   235,   236,   241,   242,
     246,   247,   251,   252,   253,   254,   255,   256,   262,   263,
     264,   265,   270,   271,   275,   276,   277,   278,   279,   283,
     284,   285,   289,   290,   291,   292,   293,   297,   298,   299,
     303,   304,   308,   309,   314,   315,   316,   317,   318
  };

  void
  parser::yy_stack_print_ () const
  {
    *yycdebug_ << "Stack now";
    for (stack_type::const_iterator
           i = yystack_.begin (),
           i_end = yystack_.end ();
         i != i_end; ++i)
      *yycdebug_ << ' ' << int (i->state);
    *yycdebug_ << '\n';
  }

  void
  parser::yy_reduce_print_ (int yyrule) const
  {
    int yylno = yyrline_[yyrule];
    int yynrhs = yyr2_[yyrule];
    // Print the symbols being reduced, and their result.
    *yycdebug_ << "Reducing stack by rule " << yyrule - 1
               << " (line " << yylno << "):\n";
    // The symbols being reduced.
    for (int yyi = 0; yyi < yynrhs; yyi++)
      YY_SYMBOL_PRINT ("   $" << yyi + 1 << " =",
                       yystack_[(yynrhs) - (yyi + 1)]);
  }
#endif // YYDEBUG


} // yy
#line 2150 "src/yacc/Bison.cpp"

#line 321 "src/yacc/sysy.y"
