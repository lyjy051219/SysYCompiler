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
namespace yy {
    void parser::error(const std::string &err)
    {
        std::cerr << "Bison error: " << err << '\n';
    }
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
      case symbol_kind::S_AddExp: // AddExp
        value.YY_MOVE_OR_COPY< AddExp* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_ConstExpList: // ConstExpList
      case symbol_kind::S_ExpList: // ExpList
        value.YY_MOVE_OR_COPY< ArrayList* > (YY_MOVE (that.value));
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

      case symbol_kind::S_PrimaryExp: // PrimaryExp
        value.YY_MOVE_OR_COPY< HasOperand* > (YY_MOVE (that.value));
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

      case symbol_kind::S_Decl: // Decl
      case symbol_kind::S_BlockItem: // BlockItem
      case symbol_kind::S_Stmt: // Stmt
        value.YY_MOVE_OR_COPY< Stmt* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_Type: // Type
        value.YY_MOVE_OR_COPY< Type > (YY_MOVE (that.value));
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

      case symbol_kind::S_STRING_LITERAL: // STRING_LITERAL
        value.YY_MOVE_OR_COPY< std::string > (YY_MOVE (that.value));
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
      case symbol_kind::S_AddExp: // AddExp
        value.move< AddExp* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_ConstExpList: // ConstExpList
      case symbol_kind::S_ExpList: // ExpList
        value.move< ArrayList* > (YY_MOVE (that.value));
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

      case symbol_kind::S_PrimaryExp: // PrimaryExp
        value.move< HasOperand* > (YY_MOVE (that.value));
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

      case symbol_kind::S_Decl: // Decl
      case symbol_kind::S_BlockItem: // BlockItem
      case symbol_kind::S_Stmt: // Stmt
        value.move< Stmt* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_Type: // Type
        value.move< Type > (YY_MOVE (that.value));
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

      case symbol_kind::S_STRING_LITERAL: // STRING_LITERAL
        value.move< std::string > (YY_MOVE (that.value));
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
      case symbol_kind::S_AddExp: // AddExp
        value.copy< AddExp* > (that.value);
        break;

      case symbol_kind::S_ConstExpList: // ConstExpList
      case symbol_kind::S_ExpList: // ExpList
        value.copy< ArrayList* > (that.value);
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

      case symbol_kind::S_PrimaryExp: // PrimaryExp
        value.copy< HasOperand* > (that.value);
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

      case symbol_kind::S_Decl: // Decl
      case symbol_kind::S_BlockItem: // BlockItem
      case symbol_kind::S_Stmt: // Stmt
        value.copy< Stmt* > (that.value);
        break;

      case symbol_kind::S_Type: // Type
        value.copy< Type > (that.value);
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

      case symbol_kind::S_STRING_LITERAL: // STRING_LITERAL
        value.copy< std::string > (that.value);
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
      case symbol_kind::S_AddExp: // AddExp
        value.move< AddExp* > (that.value);
        break;

      case symbol_kind::S_ConstExpList: // ConstExpList
      case symbol_kind::S_ExpList: // ExpList
        value.move< ArrayList* > (that.value);
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

      case symbol_kind::S_PrimaryExp: // PrimaryExp
        value.move< HasOperand* > (that.value);
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

      case symbol_kind::S_Decl: // Decl
      case symbol_kind::S_BlockItem: // BlockItem
      case symbol_kind::S_Stmt: // Stmt
        value.move< Stmt* > (that.value);
        break;

      case symbol_kind::S_Type: // Type
        value.move< Type > (that.value);
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

      case symbol_kind::S_STRING_LITERAL: // STRING_LITERAL
        value.move< std::string > (that.value);
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
      case symbol_kind::S_AddExp: // AddExp
        yylhs.value.emplace< AddExp* > ();
        break;

      case symbol_kind::S_ConstExpList: // ConstExpList
      case symbol_kind::S_ExpList: // ExpList
        yylhs.value.emplace< ArrayList* > ();
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

      case symbol_kind::S_PrimaryExp: // PrimaryExp
        yylhs.value.emplace< HasOperand* > ();
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

      case symbol_kind::S_Decl: // Decl
      case symbol_kind::S_BlockItem: // BlockItem
      case symbol_kind::S_Stmt: // Stmt
        yylhs.value.emplace< Stmt* > ();
        break;

      case symbol_kind::S_Type: // Type
        yylhs.value.emplace< Type > ();
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

      case symbol_kind::S_STRING_LITERAL: // STRING_LITERAL
        yylhs.value.emplace< std::string > ();
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
#line 117 "src/yacc/sysy.y"
                { ASTRoot = std::unique_ptr<CompUnit>(yystack_[0].value.as < CompUnit* > ()); }
#line 1156 "src/yacc/Bison.cpp"
    break;

  case 3: // CompUnit: CompUnit Decl
#line 121 "src/yacc/sysy.y"
                 {yylhs.value.as < CompUnit* > () = yystack_[1].value.as < CompUnit* > (); yylhs.value.as < CompUnit* > ()->pushBack((BaseAST*)yystack_[0].value.as < Stmt* > ());}
#line 1162 "src/yacc/Bison.cpp"
    break;

  case 4: // CompUnit: CompUnit FuncDef
#line 122 "src/yacc/sysy.y"
                    {yylhs.value.as < CompUnit* > () = yystack_[1].value.as < CompUnit* > (); yylhs.value.as < CompUnit* > ()->pushBack((BaseAST*)yystack_[0].value.as < FuncDef* > ());}
#line 1168 "src/yacc/Bison.cpp"
    break;

  case 5: // CompUnit: Decl
#line 123 "src/yacc/sysy.y"
        {yylhs.value.as < CompUnit* > () = new CompUnit((BaseAST*)yystack_[0].value.as < Stmt* > ());}
#line 1174 "src/yacc/Bison.cpp"
    break;

  case 6: // CompUnit: FuncDef
#line 124 "src/yacc/sysy.y"
            {yylhs.value.as < CompUnit* > () = new CompUnit((BaseAST*)yystack_[0].value.as < FuncDef* > ());}
#line 1180 "src/yacc/Bison.cpp"
    break;

  case 7: // Decl: ConstDecl
#line 129 "src/yacc/sysy.y"
              { yylhs.value.as < Stmt* > () = (Stmt*)yystack_[0].value.as < ConstDecl* > (); }
#line 1186 "src/yacc/Bison.cpp"
    break;

  case 8: // Decl: VarDecl
#line 130 "src/yacc/sysy.y"
            { yylhs.value.as < Stmt* > () = (Stmt*)yystack_[0].value.as < VarDecl* > (); }
#line 1192 "src/yacc/Bison.cpp"
    break;

  case 9: // ConstDecl: CONST Type ConstDefList SEMICOLON
#line 135 "src/yacc/sysy.y"
                                     { yylhs.value.as < ConstDecl* > () = new ConstDecl(yystack_[2].value.as < Type > (),yystack_[1].value.as < ConstDefList* > ()); }
#line 1198 "src/yacc/Bison.cpp"
    break;

  case 10: // ConstDefList: ConstDefList COMMA ConstDef
#line 139 "src/yacc/sysy.y"
                               { yylhs.value.as < ConstDefList* > () = yystack_[2].value.as < ConstDefList* > (); yystack_[2].value.as < ConstDefList* > ()->pushBack(yystack_[0].value.as < ConstDef* > ()); }
#line 1204 "src/yacc/Bison.cpp"
    break;

  case 11: // ConstDefList: ConstDef
#line 140 "src/yacc/sysy.y"
            { yylhs.value.as < ConstDefList* > () = new ConstDefList(yystack_[0].value.as < ConstDef* > ()); }
#line 1210 "src/yacc/Bison.cpp"
    break;

  case 12: // ConstDef: IDENT ASSIGN ConstInitVal
#line 144 "src/yacc/sysy.y"
                             { yylhs.value.as < ConstDef* > () = new ConstDef(yystack_[2].value.as < LocatedIdentifier > ().text,nullptr,yystack_[0].value.as < InitVal* > ()); }
#line 1216 "src/yacc/Bison.cpp"
    break;

  case 13: // ConstDef: IDENT ConstExpList ASSIGN ConstInitVal
#line 145 "src/yacc/sysy.y"
                                          { yylhs.value.as < ConstDef* > () = new ConstDef(yystack_[3].value.as < LocatedIdentifier > ().text,yystack_[2].value.as < ArrayList* > (),yystack_[0].value.as < InitVal* > ()); }
#line 1222 "src/yacc/Bison.cpp"
    break;

  case 14: // ConstExpList: ConstExpList L_MIDDLE_BRACKET AddExp R_MIDDLE_BRACKET
#line 149 "src/yacc/sysy.y"
                                                        { yylhs.value.as < ArrayList* > () = yystack_[3].value.as < ArrayList* > (); yylhs.value.as < ArrayList* > ()->pushBack(yystack_[1].value.as < AddExp* > ()); }
#line 1228 "src/yacc/Bison.cpp"
    break;

  case 15: // ConstExpList: L_MIDDLE_BRACKET AddExp R_MIDDLE_BRACKET
#line 150 "src/yacc/sysy.y"
                                            { yylhs.value.as < ArrayList* > () = new ArrayList(yystack_[1].value.as < AddExp* > ()); }
#line 1234 "src/yacc/Bison.cpp"
    break;

  case 16: // ConstInitVal: AddExp
#line 154 "src/yacc/sysy.y"
         { yylhs.value.as < InitVal* > () = new InitVal((BaseAST*)yystack_[0].value.as < AddExp* > ()); }
#line 1240 "src/yacc/Bison.cpp"
    break;

  case 17: // ConstInitVal: L_BIG_BRACKET R_BIG_BRACKET
#line 155 "src/yacc/sysy.y"
                              { yylhs.value.as < InitVal* > () = new InitVal(); }
#line 1246 "src/yacc/Bison.cpp"
    break;

  case 18: // ConstInitVal: L_BIG_BRACKET ConstInitValList R_BIG_BRACKET
#line 156 "src/yacc/sysy.y"
                                               { yylhs.value.as < InitVal* > () = new InitVal((BaseAST*)yystack_[1].value.as < InitValList* > ()); }
#line 1252 "src/yacc/Bison.cpp"
    break;

  case 19: // ConstInitValList: ConstInitVal
#line 160 "src/yacc/sysy.y"
               { yylhs.value.as < InitValList* > () = new InitValList(yystack_[0].value.as < InitVal* > ()); }
#line 1258 "src/yacc/Bison.cpp"
    break;

  case 20: // ConstInitValList: ConstInitValList COMMA ConstInitVal
#line 161 "src/yacc/sysy.y"
                                      { yylhs.value.as < InitValList* > () = yystack_[2].value.as < InitValList* > (); yylhs.value.as < InitValList* > ()->pushBack(yystack_[0].value.as < InitVal* > ()); }
#line 1264 "src/yacc/Bison.cpp"
    break;

  case 21: // VarDecl: Type VarDefList SEMICOLON
#line 165 "src/yacc/sysy.y"
                            { yylhs.value.as < VarDecl* > () = new VarDecl(yystack_[2].value.as < Type > (),yystack_[1].value.as < VarDefList* > ()); }
#line 1270 "src/yacc/Bison.cpp"
    break;

  case 22: // VarDefList: VarDef
#line 169 "src/yacc/sysy.y"
         { yylhs.value.as < VarDefList* > () = new VarDefList(yystack_[0].value.as < VarDef* > ()); }
#line 1276 "src/yacc/Bison.cpp"
    break;

  case 23: // VarDefList: VarDefList COMMA VarDef
#line 170 "src/yacc/sysy.y"
                          { yylhs.value.as < VarDefList* > () = yystack_[2].value.as < VarDefList* > (); yylhs.value.as < VarDefList* > ()->pushBack(yystack_[0].value.as < VarDef* > ()); }
#line 1282 "src/yacc/Bison.cpp"
    break;

  case 24: // VarDef: IDENT
#line 174 "src/yacc/sysy.y"
         { yylhs.value.as < VarDef* > () = new VarDef(yystack_[0].value.as < LocatedIdentifier > ().text); }
#line 1288 "src/yacc/Bison.cpp"
    break;

  case 25: // VarDef: IDENT ASSIGN InitVal
#line 175 "src/yacc/sysy.y"
                       { yylhs.value.as < VarDef* > () = new VarDef(yystack_[2].value.as < LocatedIdentifier > ().text,nullptr,yystack_[0].value.as < InitVal* > ()); }
#line 1294 "src/yacc/Bison.cpp"
    break;

  case 26: // VarDef: IDENT ConstExpList
#line 176 "src/yacc/sysy.y"
                      { yylhs.value.as < VarDef* > () = new VarDef(yystack_[1].value.as < LocatedIdentifier > ().text,yystack_[0].value.as < ArrayList* > (),nullptr); }
#line 1300 "src/yacc/Bison.cpp"
    break;

  case 27: // VarDef: IDENT ConstExpList ASSIGN InitVal
#line 177 "src/yacc/sysy.y"
                                     { yylhs.value.as < VarDef* > () = new VarDef(yystack_[3].value.as < LocatedIdentifier > ().text,yystack_[2].value.as < ArrayList* > (),yystack_[0].value.as < InitVal* > ()); }
#line 1306 "src/yacc/Bison.cpp"
    break;

  case 28: // InitVal: AddExp
#line 181 "src/yacc/sysy.y"
          { yylhs.value.as < InitVal* > () = new InitVal((BaseAST*)yystack_[0].value.as < AddExp* > ()); }
#line 1312 "src/yacc/Bison.cpp"
    break;

  case 29: // InitVal: L_BIG_BRACKET R_BIG_BRACKET
#line 182 "src/yacc/sysy.y"
                               { yylhs.value.as < InitVal* > () = new InitVal(nullptr); }
#line 1318 "src/yacc/Bison.cpp"
    break;

  case 30: // InitVal: L_BIG_BRACKET InitValList R_BIG_BRACKET
#line 183 "src/yacc/sysy.y"
                                           { yylhs.value.as < InitVal* > () = new InitVal((BaseAST*)yystack_[1].value.as < InitValList* > ()); }
#line 1324 "src/yacc/Bison.cpp"
    break;

  case 31: // InitValList: InitVal
#line 187 "src/yacc/sysy.y"
           { yylhs.value.as < InitValList* > () = new InitValList(yystack_[0].value.as < InitVal* > ()); }
#line 1330 "src/yacc/Bison.cpp"
    break;

  case 32: // InitValList: InitValList COMMA InitVal
#line 188 "src/yacc/sysy.y"
                             { yylhs.value.as < InitValList* > () = yystack_[2].value.as < InitValList* > (); yylhs.value.as < InitValList* > ()->pushBack(yystack_[0].value.as < InitVal* > ()); }
#line 1336 "src/yacc/Bison.cpp"
    break;

  case 33: // FuncDef: Type IDENT L_SMALL_BRACKET R_SMALL_BRACKET Block
#line 192 "src/yacc/sysy.y"
                                                   { yylhs.value.as < FuncDef* > () = new FuncDef(yystack_[4].value.as < Type > (), yystack_[3].value.as < LocatedIdentifier > ().text, nullptr, yystack_[0].value.as < Block* > ()); }
#line 1342 "src/yacc/Bison.cpp"
    break;

  case 34: // FuncDef: Type IDENT L_SMALL_BRACKET FuncFParamList R_SMALL_BRACKET Block
#line 193 "src/yacc/sysy.y"
                                                                  { yylhs.value.as < FuncDef* > () = new FuncDef(yystack_[5].value.as < Type > (), yystack_[4].value.as < LocatedIdentifier > ().text, yystack_[2].value.as < FuncParamList* > (), yystack_[0].value.as < Block* > ()); }
#line 1348 "src/yacc/Bison.cpp"
    break;

  case 35: // FuncFParamList: FuncFParam
#line 197 "src/yacc/sysy.y"
              { yylhs.value.as < FuncParamList* > () = new FuncParamList(yystack_[0].value.as < FuncParam* > ()); }
#line 1354 "src/yacc/Bison.cpp"
    break;

  case 36: // FuncFParamList: FuncFParamList COMMA FuncFParam
#line 198 "src/yacc/sysy.y"
                                   { yylhs.value.as < FuncParamList* > () = yystack_[2].value.as < FuncParamList* > (); yylhs.value.as < FuncParamList* > ()->pushBack(yystack_[0].value.as < FuncParam* > ()); }
#line 1360 "src/yacc/Bison.cpp"
    break;

  case 37: // FuncFParam: Type IDENT
#line 202 "src/yacc/sysy.y"
              { yylhs.value.as < FuncParam* > () = new FuncParam(yystack_[1].value.as < Type > (),yystack_[0].value.as < LocatedIdentifier > ().text); }
#line 1366 "src/yacc/Bison.cpp"
    break;

  case 38: // FuncFParam: Type IDENT L_MIDDLE_BRACKET R_MIDDLE_BRACKET
#line 203 "src/yacc/sysy.y"
                                                { yylhs.value.as < FuncParam* > () = new FuncParam(yystack_[3].value.as < Type > (),yystack_[2].value.as < LocatedIdentifier > ().text,true); }
#line 1372 "src/yacc/Bison.cpp"
    break;

  case 39: // FuncFParam: Type IDENT ExpList
#line 204 "src/yacc/sysy.y"
                      { yylhs.value.as < FuncParam* > () = new FuncParam(yystack_[2].value.as < Type > (),yystack_[1].value.as < LocatedIdentifier > ().text,false,yystack_[0].value.as < ArrayList* > ()); }
#line 1378 "src/yacc/Bison.cpp"
    break;

  case 40: // FuncFParam: Type IDENT L_MIDDLE_BRACKET R_MIDDLE_BRACKET ExpList
#line 205 "src/yacc/sysy.y"
                                                        { yylhs.value.as < FuncParam* > () = new FuncParam(yystack_[4].value.as < Type > (),yystack_[3].value.as < LocatedIdentifier > ().text,true,yystack_[0].value.as < ArrayList* > ()); }
#line 1384 "src/yacc/Bison.cpp"
    break;

  case 41: // Block: L_BIG_BRACKET R_BIG_BRACKET
#line 209 "src/yacc/sysy.y"
                              { yylhs.value.as < Block* > () = new Block(nullptr); }
#line 1390 "src/yacc/Bison.cpp"
    break;

  case 42: // Block: L_BIG_BRACKET BlockItemList R_BIG_BRACKET
#line 210 "src/yacc/sysy.y"
                                             { yylhs.value.as < Block* > () = new Block(yystack_[1].value.as < BlockItemList* > ()); }
#line 1396 "src/yacc/Bison.cpp"
    break;

  case 43: // BlockItemList: BlockItem
#line 214 "src/yacc/sysy.y"
             { yylhs.value.as < BlockItemList* > () = new BlockItemList(yystack_[0].value.as < Stmt* > ()); }
#line 1402 "src/yacc/Bison.cpp"
    break;

  case 44: // BlockItemList: BlockItemList BlockItem
#line 215 "src/yacc/sysy.y"
                           { yylhs.value.as < BlockItemList* > () = yystack_[1].value.as < BlockItemList* > (); yylhs.value.as < BlockItemList* > ()->pushBack(yystack_[0].value.as < Stmt* > ()); }
#line 1408 "src/yacc/Bison.cpp"
    break;

  case 45: // BlockItem: Decl
#line 219 "src/yacc/sysy.y"
       { yylhs.value.as < Stmt* > () = (Stmt*)yystack_[0].value.as < Stmt* > (); }
#line 1414 "src/yacc/Bison.cpp"
    break;

  case 46: // BlockItem: Stmt
#line 220 "src/yacc/sysy.y"
       { yylhs.value.as < Stmt* > () = (Stmt*)yystack_[0].value.as < Stmt* > (); }
#line 1420 "src/yacc/Bison.cpp"
    break;

  case 47: // Stmt: LVal ASSIGN AddExp SEMICOLON
#line 225 "src/yacc/sysy.y"
                               { yylhs.value.as < Stmt* > () = new AssignStmt(yystack_[3].value.as < LVal* > (),yystack_[1].value.as < AddExp* > ()); }
#line 1426 "src/yacc/Bison.cpp"
    break;

  case 48: // Stmt: SEMICOLON
#line 226 "src/yacc/sysy.y"
             { yylhs.value.as < Stmt* > () = new ExpStmt(nullptr); }
#line 1432 "src/yacc/Bison.cpp"
    break;

  case 49: // Stmt: AddExp SEMICOLON
#line 227 "src/yacc/sysy.y"
                    { yylhs.value.as < Stmt* > () = new ExpStmt(yystack_[1].value.as < AddExp* > ()); }
#line 1438 "src/yacc/Bison.cpp"
    break;

  case 50: // Stmt: Block
#line 228 "src/yacc/sysy.y"
         { yylhs.value.as < Stmt* > () = yystack_[0].value.as < Block* > (); }
#line 1444 "src/yacc/Bison.cpp"
    break;

  case 51: // Stmt: IF L_SMALL_BRACKET LOrExp R_SMALL_BRACKET Stmt ELSE Stmt
#line 229 "src/yacc/sysy.y"
                                                            { yylhs.value.as < Stmt* > () = new IfStmt(yystack_[4].value.as < LOrExp* > (),yystack_[2].value.as < Stmt* > (),yystack_[0].value.as < Stmt* > ()); }
#line 1450 "src/yacc/Bison.cpp"
    break;

  case 52: // Stmt: IF L_SMALL_BRACKET LOrExp R_SMALL_BRACKET Stmt
#line 230 "src/yacc/sysy.y"
                                                  { yylhs.value.as < Stmt* > () = new IfStmt(yystack_[2].value.as < LOrExp* > (),yystack_[0].value.as < Stmt* > ()); }
#line 1456 "src/yacc/Bison.cpp"
    break;

  case 53: // Stmt: WHILE L_SMALL_BRACKET LOrExp R_SMALL_BRACKET Stmt
#line 231 "src/yacc/sysy.y"
                                                     { yylhs.value.as < Stmt* > () = new WhileStmt(yystack_[2].value.as < LOrExp* > (),yystack_[0].value.as < Stmt* > ()); }
#line 1462 "src/yacc/Bison.cpp"
    break;

  case 54: // Stmt: BREAK SEMICOLON
#line 232 "src/yacc/sysy.y"
                   { yylhs.value.as < Stmt* > () = new BreakStmt(); }
#line 1468 "src/yacc/Bison.cpp"
    break;

  case 55: // Stmt: CONTINUE SEMICOLON
#line 233 "src/yacc/sysy.y"
                      { yylhs.value.as < Stmt* > () = new ContinueStmt(); }
#line 1474 "src/yacc/Bison.cpp"
    break;

  case 56: // Stmt: RETURN SEMICOLON
#line 234 "src/yacc/sysy.y"
                    { yylhs.value.as < Stmt* > () = new ReturnStmt(); }
#line 1480 "src/yacc/Bison.cpp"
    break;

  case 57: // Stmt: RETURN AddExp SEMICOLON
#line 235 "src/yacc/sysy.y"
                           { yylhs.value.as < Stmt* > () = new ReturnStmt(yystack_[1].value.as < AddExp* > ()); }
#line 1486 "src/yacc/Bison.cpp"
    break;

  case 58: // LVal: IDENT
#line 240 "src/yacc/sysy.y"
        { yylhs.value.as < LVal* > () = new LVal(yystack_[0].value.as < LocatedIdentifier > ().text); }
#line 1492 "src/yacc/Bison.cpp"
    break;

  case 59: // LVal: IDENT ExpList
#line 241 "src/yacc/sysy.y"
                 { yylhs.value.as < LVal* > () = new LVal(yystack_[1].value.as < LocatedIdentifier > ().text,yystack_[0].value.as < ArrayList* > ()); }
#line 1498 "src/yacc/Bison.cpp"
    break;

  case 60: // ExpList: L_MIDDLE_BRACKET AddExp R_MIDDLE_BRACKET
#line 245 "src/yacc/sysy.y"
                                           { yylhs.value.as < ArrayList* > () = new ArrayList(yystack_[1].value.as < AddExp* > ()); }
#line 1504 "src/yacc/Bison.cpp"
    break;

  case 61: // ExpList: ExpList L_MIDDLE_BRACKET AddExp R_MIDDLE_BRACKET
#line 246 "src/yacc/sysy.y"
                                                   { yylhs.value.as < ArrayList* > () = yystack_[3].value.as < ArrayList* > (); yylhs.value.as < ArrayList* > ()->pushBack(yystack_[1].value.as < AddExp* > ()); }
#line 1510 "src/yacc/Bison.cpp"
    break;

  case 62: // PrimaryExp: L_SMALL_BRACKET AddExp R_SMALL_BRACKET
#line 250 "src/yacc/sysy.y"
                                          { yylhs.value.as < HasOperand* > () = (HasOperand*)yystack_[1].value.as < AddExp* > (); }
#line 1516 "src/yacc/Bison.cpp"
    break;

  case 63: // PrimaryExp: LVal
#line 251 "src/yacc/sysy.y"
        { yylhs.value.as < HasOperand* > () = (HasOperand*)yystack_[0].value.as < LVal* > (); }
#line 1522 "src/yacc/Bison.cpp"
    break;

  case 64: // PrimaryExp: INTCONST
#line 252 "src/yacc/sysy.y"
           { yylhs.value.as < HasOperand* > () = (HasOperand*)(new ConValue<int>(yystack_[0].value.as < int > ())); }
#line 1528 "src/yacc/Bison.cpp"
    break;

  case 65: // PrimaryExp: FLOATCONST
#line 253 "src/yacc/sysy.y"
             { yylhs.value.as < HasOperand* > () = (HasOperand*)(new ConValue<float>(yystack_[0].value.as < float > ())); }
#line 1534 "src/yacc/Bison.cpp"
    break;

  case 66: // PrimaryExp: IDENT L_SMALL_BRACKET R_SMALL_BRACKET
#line 254 "src/yacc/sysy.y"
                                         {yylhs.value.as < HasOperand* > () = (HasOperand*)(new FuncCall(yystack_[2].value.as < LocatedIdentifier > ().text, yystack_[2].value.as < LocatedIdentifier > ().line));}
#line 1540 "src/yacc/Bison.cpp"
    break;

  case 67: // PrimaryExp: IDENT L_SMALL_BRACKET FuncRParamList R_SMALL_BRACKET
#line 255 "src/yacc/sysy.y"
                                                       {yylhs.value.as < HasOperand* > () = (HasOperand*)(new FuncCall(yystack_[3].value.as < LocatedIdentifier > ().text, yystack_[1].value.as < FuncRParamList* > (), yystack_[3].value.as < LocatedIdentifier > ().line));}
#line 1546 "src/yacc/Bison.cpp"
    break;

  case 68: // PrimaryExp: IDENT L_SMALL_BRACKET STRING_LITERAL R_SMALL_BRACKET
#line 256 "src/yacc/sysy.y"
                                                        {yylhs.value.as < HasOperand* > () = (HasOperand*)(new FuncCall(yystack_[3].value.as < LocatedIdentifier > ().text, yystack_[1].value.as < std::string > (), yystack_[3].value.as < LocatedIdentifier > ().line));}
#line 1552 "src/yacc/Bison.cpp"
    break;

  case 69: // PrimaryExp: IDENT L_SMALL_BRACKET STRING_LITERAL COMMA FuncRParamList R_SMALL_BRACKET
#line 257 "src/yacc/sysy.y"
                                                                             {yylhs.value.as < HasOperand* > () = (HasOperand*)(new FuncCall(yystack_[5].value.as < LocatedIdentifier > ().text, yystack_[3].value.as < std::string > (), yystack_[1].value.as < FuncRParamList* > (), yystack_[5].value.as < LocatedIdentifier > ().line));}
#line 1558 "src/yacc/Bison.cpp"
    break;

  case 70: // UnaryExp: PrimaryExp
#line 263 "src/yacc/sysy.y"
             { yylhs.value.as < UnaryExp* > () = new UnaryExp(yystack_[0].value.as < HasOperand* > ()); }
#line 1564 "src/yacc/Bison.cpp"
    break;

  case 71: // UnaryExp: ADD UnaryExp
#line 264 "src/yacc/sysy.y"
               { yylhs.value.as < UnaryExp* > () = yystack_[0].value.as < UnaryExp* > (); yylhs.value.as < UnaryExp* > ()->pushFront(SY_ADD); }
#line 1570 "src/yacc/Bison.cpp"
    break;

  case 72: // UnaryExp: SUB UnaryExp
#line 265 "src/yacc/sysy.y"
               { yylhs.value.as < UnaryExp* > () = yystack_[0].value.as < UnaryExp* > (); yylhs.value.as < UnaryExp* > ()->pushFront(SY_SUB); }
#line 1576 "src/yacc/Bison.cpp"
    break;

  case 73: // UnaryExp: NOT UnaryExp
#line 266 "src/yacc/sysy.y"
               { yylhs.value.as < UnaryExp* > () = yystack_[0].value.as < UnaryExp* > (); yylhs.value.as < UnaryExp* > ()->pushFront(SY_NOT); }
#line 1582 "src/yacc/Bison.cpp"
    break;

  case 74: // FuncRParamList: AddExp
#line 271 "src/yacc/sysy.y"
          { yylhs.value.as < FuncRParamList* > () = new FuncRParamList(yystack_[0].value.as < AddExp* > ()); }
#line 1588 "src/yacc/Bison.cpp"
    break;

  case 75: // FuncRParamList: FuncRParamList COMMA AddExp
#line 272 "src/yacc/sysy.y"
                              { yylhs.value.as < FuncRParamList* > () = yystack_[2].value.as < FuncRParamList* > (); yylhs.value.as < FuncRParamList* > ()->pushBack(yystack_[0].value.as < AddExp* > ()); }
#line 1594 "src/yacc/Bison.cpp"
    break;

  case 76: // MulExp: UnaryExp
#line 276 "src/yacc/sysy.y"
            { yylhs.value.as < MulExp* > () = new MulExp(yystack_[0].value.as < UnaryExp* > ()); }
#line 1600 "src/yacc/Bison.cpp"
    break;

  case 77: // MulExp: MulExp MUL UnaryExp
#line 277 "src/yacc/sysy.y"
                       { yylhs.value.as < MulExp* > () = yystack_[2].value.as < MulExp* > (); yylhs.value.as < MulExp* > ()->pushBack(SY_MUL); yylhs.value.as < MulExp* > ()->pushBack(yystack_[0].value.as < UnaryExp* > ()); }
#line 1606 "src/yacc/Bison.cpp"
    break;

  case 78: // MulExp: MulExp DIV UnaryExp
#line 278 "src/yacc/sysy.y"
                       { yylhs.value.as < MulExp* > () = yystack_[2].value.as < MulExp* > (); yylhs.value.as < MulExp* > ()->pushBack(SY_DIV); yylhs.value.as < MulExp* > ()->pushBack(yystack_[0].value.as < UnaryExp* > ()); }
#line 1612 "src/yacc/Bison.cpp"
    break;

  case 79: // MulExp: MulExp MOD UnaryExp
#line 279 "src/yacc/sysy.y"
                       { yylhs.value.as < MulExp* > () = yystack_[2].value.as < MulExp* > (); yylhs.value.as < MulExp* > ()->pushBack(SY_MOD); yylhs.value.as < MulExp* > ()->pushBack(yystack_[0].value.as < UnaryExp* > ()); }
#line 1618 "src/yacc/Bison.cpp"
    break;

  case 80: // AddExp: MulExp
#line 283 "src/yacc/sysy.y"
         { yylhs.value.as < AddExp* > () = new AddExp(yystack_[0].value.as < MulExp* > ()); }
#line 1624 "src/yacc/Bison.cpp"
    break;

  case 81: // AddExp: AddExp ADD MulExp
#line 284 "src/yacc/sysy.y"
                    { yylhs.value.as < AddExp* > () = yystack_[2].value.as < AddExp* > (); yylhs.value.as < AddExp* > ()->pushBack(SY_ADD); yylhs.value.as < AddExp* > ()->pushBack(yystack_[0].value.as < MulExp* > ()); }
#line 1630 "src/yacc/Bison.cpp"
    break;

  case 82: // AddExp: AddExp SUB MulExp
#line 285 "src/yacc/sysy.y"
                    { yylhs.value.as < AddExp* > () = yystack_[2].value.as < AddExp* > (); yylhs.value.as < AddExp* > ()->pushBack(SY_SUB); yylhs.value.as < AddExp* > ()->pushBack(yystack_[0].value.as < MulExp* > ()); }
#line 1636 "src/yacc/Bison.cpp"
    break;

  case 83: // RelExp: AddExp
#line 289 "src/yacc/sysy.y"
          {yylhs.value.as < RelExp* > () = new RelExp(yystack_[0].value.as < AddExp* > ());}
#line 1642 "src/yacc/Bison.cpp"
    break;

  case 84: // RelExp: RelExp LESS AddExp
#line 290 "src/yacc/sysy.y"
                     { yylhs.value.as < RelExp* > () = yystack_[2].value.as < RelExp* > (); yylhs.value.as < RelExp* > ()->pushBack(SY_LESS); yylhs.value.as < RelExp* > ()->pushBack(yystack_[0].value.as < AddExp* > ()); }
#line 1648 "src/yacc/Bison.cpp"
    break;

  case 85: // RelExp: RelExp GREAT AddExp
#line 291 "src/yacc/sysy.y"
                      { yylhs.value.as < RelExp* > () = yystack_[2].value.as < RelExp* > (); yylhs.value.as < RelExp* > ()->pushBack(SY_GREAT); yylhs.value.as < RelExp* > ()->pushBack(yystack_[0].value.as < AddExp* > ()); }
#line 1654 "src/yacc/Bison.cpp"
    break;

  case 86: // RelExp: RelExp LESSEQ AddExp
#line 292 "src/yacc/sysy.y"
                       { yylhs.value.as < RelExp* > () = yystack_[2].value.as < RelExp* > (); yylhs.value.as < RelExp* > ()->pushBack(SY_LESSEQ); yylhs.value.as < RelExp* > ()->pushBack(yystack_[0].value.as < AddExp* > ()); }
#line 1660 "src/yacc/Bison.cpp"
    break;

  case 87: // RelExp: RelExp GREATEQ AddExp
#line 293 "src/yacc/sysy.y"
                        { yylhs.value.as < RelExp* > () = yystack_[2].value.as < RelExp* > (); yylhs.value.as < RelExp* > ()->pushBack(SY_GREATEQ); yylhs.value.as < RelExp* > ()->pushBack(yystack_[0].value.as < AddExp* > ()); }
#line 1666 "src/yacc/Bison.cpp"
    break;

  case 88: // EqExp: RelExp
#line 297 "src/yacc/sysy.y"
         { yylhs.value.as < EqExp* > () = new EqExp(yystack_[0].value.as < RelExp* > ()); }
#line 1672 "src/yacc/Bison.cpp"
    break;

  case 89: // EqExp: EqExp EQ RelExp
#line 298 "src/yacc/sysy.y"
                  { yylhs.value.as < EqExp* > () = yystack_[2].value.as < EqExp* > (); yylhs.value.as < EqExp* > ()->pushBack(SY_EQ); yylhs.value.as < EqExp* > ()->pushBack(yystack_[0].value.as < RelExp* > ()); }
#line 1678 "src/yacc/Bison.cpp"
    break;

  case 90: // EqExp: EqExp NOTEQ RelExp
#line 299 "src/yacc/sysy.y"
                     { yylhs.value.as < EqExp* > () = yystack_[2].value.as < EqExp* > (); yylhs.value.as < EqExp* > ()->pushBack(SY_NOTEQ); yylhs.value.as < EqExp* > ()->pushBack(yystack_[0].value.as < RelExp* > ()); }
#line 1684 "src/yacc/Bison.cpp"
    break;

  case 91: // LAndExp: EqExp
#line 303 "src/yacc/sysy.y"
         { yylhs.value.as < LAndExp* > () = new LAndExp(yystack_[0].value.as < EqExp* > ()); }
#line 1690 "src/yacc/Bison.cpp"
    break;

  case 92: // LAndExp: LAndExp AND EqExp
#line 304 "src/yacc/sysy.y"
                    { yylhs.value.as < LAndExp* > () = yystack_[2].value.as < LAndExp* > (); yylhs.value.as < LAndExp* > ()->pushBack(SY_AND); yylhs.value.as < LAndExp* > ()->pushBack(yystack_[0].value.as < EqExp* > ()); }
#line 1696 "src/yacc/Bison.cpp"
    break;

  case 93: // LOrExp: LAndExp
#line 308 "src/yacc/sysy.y"
          { yylhs.value.as < LOrExp* > () = new LOrExp(yystack_[0].value.as < LAndExp* > ()); }
#line 1702 "src/yacc/Bison.cpp"
    break;

  case 94: // LOrExp: LOrExp OR LAndExp
#line 309 "src/yacc/sysy.y"
                    { yylhs.value.as < LOrExp* > () = yystack_[2].value.as < LOrExp* > (); yylhs.value.as < LOrExp* > ()->pushBack(SY_OR); yylhs.value.as < LOrExp* > ()->pushBack(yystack_[0].value.as < LAndExp* > ()); }
#line 1708 "src/yacc/Bison.cpp"
    break;

  case 95: // Type: VOID
#line 314 "src/yacc/sysy.y"
       {yylhs.value.as < Type > ()=SY_VOID;}
#line 1714 "src/yacc/Bison.cpp"
    break;

  case 96: // Type: INT
#line 315 "src/yacc/sysy.y"
       {yylhs.value.as < Type > ()=SY_INT;}
#line 1720 "src/yacc/Bison.cpp"
    break;

  case 97: // Type: FLOAT
#line 316 "src/yacc/sysy.y"
         {yylhs.value.as < Type > ()=SY_FLOAT;}
#line 1726 "src/yacc/Bison.cpp"
    break;


#line 1730 "src/yacc/Bison.cpp"

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









  const signed char parser::yypact_ninf_ = -77;

  const signed char parser::yytable_ninf_ = -1;

  const short
  parser::yypact_[] =
  {
     260,   164,   -77,   -77,   -77,    32,   260,   -77,   -77,   -77,
     -77,     3,    43,   -77,   -77,   -77,    27,    17,   -77,    -3,
     105,   -77,    63,    41,   245,    67,   -77,    64,   215,   113,
     -77,    43,   -77,   -77,    55,   245,   245,   245,   245,    77,
     -77,   -77,   -77,   -77,   252,   201,    76,   -19,   -77,    86,
      37,    63,   245,   169,   -77,   195,   -77,   201,   215,   -77,
     240,   245,   127,   -77,   -77,   -77,    97,   -77,   -77,   173,
     245,   245,   245,   245,   245,   119,   -77,    76,   164,   135,
     -77,   -77,    42,   -77,   -77,   200,   -77,   -15,   -77,    -5,
     201,   132,   245,   -77,   -77,    63,   -77,   -77,   -77,   252,
     252,    22,   134,   145,    93,   159,   -77,   -77,   -77,   -77,
     155,   -77,   -77,   184,    65,    64,   -77,   -77,   220,   127,
     -77,   -77,   215,   -77,   245,   -77,   245,   -77,   236,   -77,
     -77,    87,   245,   245,   -77,   -77,   -77,   -77,   245,   -77,
     180,   -77,    72,   201,   -77,   -77,   201,   262,   248,   205,
       4,     8,    99,   127,   -77,   245,   245,   245,   245,   245,
     245,   245,   245,   175,   175,   -77,   201,   201,   201,   201,
     262,   262,   248,   205,   198,   -77,   175,   -77
  };

  const signed char
  parser::yydefact_[] =
  {
       0,     0,    96,    97,    95,     0,     2,     5,     7,     8,
       6,     0,     0,     1,     3,     4,    24,     0,    22,     0,
       0,    11,     0,     0,     0,    26,    21,     0,     0,     0,
       9,     0,    64,    65,    58,     0,     0,     0,     0,     0,
      25,    63,    70,    76,    80,    28,     0,     0,    35,     0,
       0,     0,     0,    24,    23,     0,    12,    16,     0,    10,
       0,     0,    59,    73,    71,    72,     0,    29,    31,     0,
       0,     0,     0,     0,     0,     0,    33,     0,     0,    37,
      15,    27,     0,    17,    19,     0,    13,     0,    66,     0,
      74,     0,     0,    62,    30,     0,    77,    78,    79,    81,
      82,     0,     0,     0,     0,     0,    41,    48,    45,    50,
       0,    43,    46,    63,     0,     0,    34,    36,     0,    39,
      14,    18,     0,    68,     0,    67,     0,    60,     0,    32,
      56,     0,     0,     0,    54,    55,    42,    44,     0,    49,
      38,    20,     0,    75,    61,    57,    83,    88,    91,    93,
       0,     0,     0,    40,    69,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,    47,    84,    85,    86,    87,
      89,    90,    92,    94,    52,    53,     0,    51
  };

  const short
  parser::yypgoto_[] =
  {
     -77,   -77,   -77,   115,   -77,   -77,   203,   219,   -51,   -77,
     -77,   -77,   227,   -30,   -77,   253,   -77,   186,   -33,   -77,
     172,     7,   -70,   -76,   -77,   -25,   118,   116,   -22,    70,
     122,   123,   151,     0
  };

  const unsigned char
  parser::yydefgoto_[] =
  {
       0,     5,     6,   108,     8,    20,    21,    25,    56,    85,
       9,    17,    18,    40,    69,    10,    47,    48,   109,   110,
     111,   112,    41,    62,    42,    43,    89,    44,   146,   147,
     148,   149,   150,    11
  };

  const unsigned char
  parser::yytable_[] =
  {
      45,    12,    50,   119,    84,   113,    57,    86,    16,    68,
      63,    64,    65,    76,    77,   162,    66,    45,   123,   162,
      78,    81,    28,    49,   124,    32,    33,    34,   125,    45,
      82,    24,    13,    57,   126,    35,    57,   163,    90,    91,
     113,   164,    36,    37,   116,    96,    97,    98,    19,     2,
       3,     4,    22,   114,    38,    26,    27,    73,    74,    23,
     130,    24,    73,    74,   153,   129,    32,    33,    34,    53,
     128,   141,    80,    45,    46,   115,    35,   120,    49,   131,
      32,    33,    34,    36,    37,    73,    74,    60,   114,    61,
      35,    79,    51,   113,   113,    38,    91,    36,    37,    39,
      57,    52,    90,   139,   143,   154,   113,    73,    74,    38,
     115,   126,    75,    39,    67,     7,   152,    73,    74,    73,
      74,    14,    32,    33,    34,   145,     1,     2,     3,     4,
      93,   134,    35,   166,   167,   168,   169,   165,    58,    36,
      37,   114,   114,    30,    31,   101,   102,    52,   103,   104,
     105,    38,    73,    74,   114,    75,   106,   107,    32,    33,
      34,    92,     1,     2,     3,     4,   132,   127,    35,   118,
     174,   175,     2,     3,     4,    36,    37,   133,    32,    33,
      34,   101,   102,   177,   103,   104,   105,    38,    35,    99,
     100,    75,   136,   107,    22,    36,    37,   135,    32,    33,
      34,   101,   102,    24,   103,   104,   105,    38,    35,   138,
      94,    75,    95,   107,    61,    36,    37,   161,    32,    33,
      34,    73,    74,    32,    33,    34,   176,    38,    35,   170,
     171,    55,    83,    35,    59,    36,    37,   121,    29,   122,
      36,    37,   142,    32,    33,    34,    87,    38,    32,    33,
      34,    55,    38,    35,    54,   140,    73,    74,    35,    15,
      36,    37,   159,   160,   117,    36,    37,     1,     2,     3,
       4,   144,    38,    88,    70,    71,    72,    38,   155,   156,
     157,   158,   137,   172,   151,   173
  };

  const unsigned char
  parser::yycheck_[] =
  {
      22,     1,    24,    79,    55,    75,    28,    58,     5,    39,
      35,    36,    37,    46,    33,    11,    38,    39,    33,    11,
      39,    51,    25,    23,    39,     3,     4,     5,    33,    51,
      52,    34,     0,    55,    39,    13,    58,    33,    60,    61,
     110,    33,    20,    21,    77,    70,    71,    72,     5,     8,
       9,    10,    25,    75,    32,    38,    39,    20,    21,    32,
      38,    34,    20,    21,   140,    95,     3,     4,     5,     5,
      92,   122,    35,    95,    33,    75,    13,    35,    78,   101,
       3,     4,     5,    20,    21,    20,    21,    32,   110,    34,
      13,     5,    25,   163,   164,    32,   118,    20,    21,    36,
     122,    34,   124,    38,   126,    33,   176,    20,    21,    32,
     110,    39,    36,    36,    37,     0,   138,    20,    21,    20,
      21,     6,     3,     4,     5,    38,     7,     8,     9,    10,
      33,    38,    13,   155,   156,   157,   158,    38,    25,    20,
      21,   163,   164,    38,    39,    26,    27,    34,    29,    30,
      31,    32,    20,    21,   176,    36,    37,    38,     3,     4,
       5,    34,     7,     8,     9,    10,    32,    35,    13,    34,
     163,   164,     8,     9,    10,    20,    21,    32,     3,     4,
       5,    26,    27,   176,    29,    30,    31,    32,    13,    73,
      74,    36,    37,    38,    25,    20,    21,    38,     3,     4,
       5,    26,    27,    34,    29,    30,    31,    32,    13,    25,
      37,    36,    39,    38,    34,    20,    21,    12,     3,     4,
       5,    20,    21,     3,     4,     5,    28,    32,    13,   159,
     160,    36,    37,    13,    31,    20,    21,    37,    19,    39,
      20,    21,   124,     3,     4,     5,     6,    32,     3,     4,
       5,    36,    32,    13,    27,    35,    20,    21,    13,     6,
      20,    21,    14,    15,    78,    20,    21,     7,     8,     9,
      10,    35,    32,    33,    22,    23,    24,    32,    16,    17,
      18,    19,   110,   161,   133,   162
  };

  const signed char
  parser::yystos_[] =
  {
       0,     7,     8,     9,    10,    42,    43,    44,    45,    51,
      56,    74,    74,     0,    44,    56,     5,    52,    53,     5,
      46,    47,    25,    32,    34,    48,    38,    39,    25,    48,
      38,    39,     3,     4,     5,    13,    20,    21,    32,    36,
      54,    63,    65,    66,    68,    69,    33,    57,    58,    74,
      69,    25,    34,     5,    53,    36,    49,    69,    25,    47,
      32,    34,    64,    66,    66,    66,    69,    37,    54,    55,
      22,    23,    24,    20,    21,    36,    59,    33,    39,     5,
      35,    54,    69,    37,    49,    50,    49,     6,    33,    67,
      69,    69,    34,    33,    37,    39,    66,    66,    66,    68,
      68,    26,    27,    29,    30,    31,    37,    38,    44,    59,
      60,    61,    62,    63,    69,    74,    59,    58,    34,    64,
      35,    37,    39,    33,    39,    33,    39,    35,    69,    54,
      38,    69,    32,    32,    38,    38,    37,    61,    25,    38,
      35,    49,    67,    69,    35,    38,    69,    70,    71,    72,
      73,    73,    69,    64,    33,    16,    17,    18,    19,    14,
      15,    12,    11,    33,    33,    38,    69,    69,    69,    69,
      70,    70,    71,    72,    62,    62,    28,    62
  };

  const signed char
  parser::yyr1_[] =
  {
       0,    41,    42,    43,    43,    43,    43,    44,    44,    45,
      46,    46,    47,    47,    48,    48,    49,    49,    49,    50,
      50,    51,    52,    52,    53,    53,    53,    53,    54,    54,
      54,    55,    55,    56,    56,    57,    57,    58,    58,    58,
      58,    59,    59,    60,    60,    61,    61,    62,    62,    62,
      62,    62,    62,    62,    62,    62,    62,    62,    63,    63,
      64,    64,    65,    65,    65,    65,    65,    65,    65,    65,
      66,    66,    66,    66,    67,    67,    68,    68,    68,    68,
      69,    69,    69,    70,    70,    70,    70,    70,    71,    71,
      71,    72,    72,    73,    73,    74,    74,    74
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
       3,     4,     3,     1,     1,     1,     3,     4,     4,     6,
       1,     2,     2,     2,     1,     3,     1,     3,     3,     3,
       1,     3,     3,     1,     3,     3,     3,     3,     1,     3,
       3,     1,     3,     1,     3,     1,     1,     1
  };


#if YYDEBUG
  // YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
  // First, the terminals, then, starting at \a YYNTOKENS, nonterminals.
  const char*
  const parser::yytname_[] =
  {
  "\"end of file\"", "error", "\"invalid token\"", "INTCONST",
  "FLOATCONST", "IDENT", "STRING_LITERAL", "CONST", "INT", "FLOAT", "VOID",
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
       0,   117,   117,   121,   122,   123,   124,   129,   130,   135,
     139,   140,   144,   145,   149,   150,   154,   155,   156,   160,
     161,   165,   169,   170,   174,   175,   176,   177,   181,   182,
     183,   187,   188,   192,   193,   197,   198,   202,   203,   204,
     205,   209,   210,   214,   215,   219,   220,   225,   226,   227,
     228,   229,   230,   231,   232,   233,   234,   235,   240,   241,
     245,   246,   250,   251,   252,   253,   254,   255,   256,   257,
     263,   264,   265,   266,   271,   272,   276,   277,   278,   279,
     283,   284,   285,   289,   290,   291,   292,   293,   297,   298,
     299,   303,   304,   308,   309,   314,   315,   316
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
#line 2177 "src/yacc/Bison.cpp"

#line 319 "src/yacc/sysy.y"
