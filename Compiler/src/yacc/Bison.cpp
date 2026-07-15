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
#line 20 "src/yacc/sysy.y"

extern yy::parser::symbol_type yylex();
extern std::unique_ptr<CompUnit> ASTRoot;
extern int yylineno;
namespace yy {
    void parser::error(const std::string &err)
    {
        std::cerr << "Bison error: " << err << '\n';
    }
}

#line 58 "src/yacc/Bison.cpp"


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
#line 131 "src/yacc/Bison.cpp"

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

      case symbol_kind::S_IDENT: // IDENT
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

      case symbol_kind::S_IDENT: // IDENT
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

      case symbol_kind::S_IDENT: // IDENT
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

      case symbol_kind::S_IDENT: // IDENT
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

      case symbol_kind::S_IDENT: // IDENT
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
#line 112 "src/yacc/sysy.y"
                { ASTRoot = std::unique_ptr<CompUnit>(yystack_[0].value.as < CompUnit* > ()); }
#line 1137 "src/yacc/Bison.cpp"
    break;

  case 3: // CompUnit: CompUnit Decl
#line 116 "src/yacc/sysy.y"
                 {yylhs.value.as < CompUnit* > () = yystack_[1].value.as < CompUnit* > (); yylhs.value.as < CompUnit* > ()->pushBack((BaseAST*)yystack_[0].value.as < Stmt* > ());}
#line 1143 "src/yacc/Bison.cpp"
    break;

  case 4: // CompUnit: CompUnit FuncDef
#line 117 "src/yacc/sysy.y"
                    {yylhs.value.as < CompUnit* > () = yystack_[1].value.as < CompUnit* > (); yylhs.value.as < CompUnit* > ()->pushBack((BaseAST*)yystack_[0].value.as < FuncDef* > ());}
#line 1149 "src/yacc/Bison.cpp"
    break;

  case 5: // CompUnit: Decl
#line 118 "src/yacc/sysy.y"
        {yylhs.value.as < CompUnit* > () = new CompUnit((BaseAST*)yystack_[0].value.as < Stmt* > ());}
#line 1155 "src/yacc/Bison.cpp"
    break;

  case 6: // CompUnit: FuncDef
#line 119 "src/yacc/sysy.y"
            {yylhs.value.as < CompUnit* > () = new CompUnit((BaseAST*)yystack_[0].value.as < FuncDef* > ());}
#line 1161 "src/yacc/Bison.cpp"
    break;

  case 7: // Decl: ConstDecl
#line 124 "src/yacc/sysy.y"
              { yylhs.value.as < Stmt* > () = (Stmt*)yystack_[0].value.as < ConstDecl* > (); }
#line 1167 "src/yacc/Bison.cpp"
    break;

  case 8: // Decl: VarDecl
#line 125 "src/yacc/sysy.y"
            { yylhs.value.as < Stmt* > () = (Stmt*)yystack_[0].value.as < VarDecl* > (); }
#line 1173 "src/yacc/Bison.cpp"
    break;

  case 9: // ConstDecl: CONST Type ConstDefList SEMICOLON
#line 130 "src/yacc/sysy.y"
                                     { yylhs.value.as < ConstDecl* > () = new ConstDecl(yystack_[2].value.as < Type > (),yystack_[1].value.as < ConstDefList* > ()); }
#line 1179 "src/yacc/Bison.cpp"
    break;

  case 10: // ConstDefList: ConstDefList COMMA ConstDef
#line 134 "src/yacc/sysy.y"
                               { yylhs.value.as < ConstDefList* > () = yystack_[2].value.as < ConstDefList* > (); yystack_[2].value.as < ConstDefList* > ()->pushBack(yystack_[0].value.as < ConstDef* > ()); }
#line 1185 "src/yacc/Bison.cpp"
    break;

  case 11: // ConstDefList: ConstDef
#line 135 "src/yacc/sysy.y"
            { yylhs.value.as < ConstDefList* > () = new ConstDefList(yystack_[0].value.as < ConstDef* > ()); }
#line 1191 "src/yacc/Bison.cpp"
    break;

  case 12: // ConstDef: IDENT ASSIGN ConstInitVal
#line 139 "src/yacc/sysy.y"
                             { yylhs.value.as < ConstDef* > () = new ConstDef(yystack_[2].value.as < std::string > (),nullptr,yystack_[0].value.as < InitVal* > ()); }
#line 1197 "src/yacc/Bison.cpp"
    break;

  case 13: // ConstDef: IDENT ConstExpList ASSIGN ConstInitVal
#line 140 "src/yacc/sysy.y"
                                          { yylhs.value.as < ConstDef* > () = new ConstDef(yystack_[3].value.as < std::string > (),yystack_[2].value.as < ArrayList* > (),yystack_[0].value.as < InitVal* > ()); }
#line 1203 "src/yacc/Bison.cpp"
    break;

  case 14: // ConstExpList: ConstExpList L_MIDDLE_BRACKET AddExp R_MIDDLE_BRACKET
#line 144 "src/yacc/sysy.y"
                                                        { yylhs.value.as < ArrayList* > () = yystack_[3].value.as < ArrayList* > (); yylhs.value.as < ArrayList* > ()->pushBack(yystack_[1].value.as < AddExp* > ()); }
#line 1209 "src/yacc/Bison.cpp"
    break;

  case 15: // ConstExpList: L_MIDDLE_BRACKET AddExp R_MIDDLE_BRACKET
#line 145 "src/yacc/sysy.y"
                                            { yylhs.value.as < ArrayList* > () = new ArrayList(yystack_[1].value.as < AddExp* > ()); }
#line 1215 "src/yacc/Bison.cpp"
    break;

  case 16: // ConstInitVal: AddExp
#line 149 "src/yacc/sysy.y"
         { yylhs.value.as < InitVal* > () = new InitVal((BaseAST*)yystack_[0].value.as < AddExp* > ()); }
#line 1221 "src/yacc/Bison.cpp"
    break;

  case 17: // ConstInitVal: L_BIG_BRACKET R_BIG_BRACKET
#line 150 "src/yacc/sysy.y"
                              { yylhs.value.as < InitVal* > () = new InitVal(); }
#line 1227 "src/yacc/Bison.cpp"
    break;

  case 18: // ConstInitVal: L_BIG_BRACKET ConstInitValList R_BIG_BRACKET
#line 151 "src/yacc/sysy.y"
                                               { yylhs.value.as < InitVal* > () = new InitVal((BaseAST*)yystack_[1].value.as < InitValList* > ()); }
#line 1233 "src/yacc/Bison.cpp"
    break;

  case 19: // ConstInitValList: ConstInitVal
#line 155 "src/yacc/sysy.y"
               { yylhs.value.as < InitValList* > () = new InitValList(yystack_[0].value.as < InitVal* > ()); }
#line 1239 "src/yacc/Bison.cpp"
    break;

  case 20: // ConstInitValList: ConstInitValList COMMA ConstInitVal
#line 156 "src/yacc/sysy.y"
                                      { yylhs.value.as < InitValList* > () = yystack_[2].value.as < InitValList* > (); yylhs.value.as < InitValList* > ()->pushBack(yystack_[0].value.as < InitVal* > ()); }
#line 1245 "src/yacc/Bison.cpp"
    break;

  case 21: // VarDecl: Type VarDefList SEMICOLON
#line 160 "src/yacc/sysy.y"
                            { yylhs.value.as < VarDecl* > () = new VarDecl(yystack_[2].value.as < Type > (),yystack_[1].value.as < VarDefList* > ()); }
#line 1251 "src/yacc/Bison.cpp"
    break;

  case 22: // VarDefList: VarDef
#line 164 "src/yacc/sysy.y"
         { yylhs.value.as < VarDefList* > () = new VarDefList(yystack_[0].value.as < VarDef* > ()); }
#line 1257 "src/yacc/Bison.cpp"
    break;

  case 23: // VarDefList: VarDefList COMMA VarDef
#line 165 "src/yacc/sysy.y"
                          { yylhs.value.as < VarDefList* > () = yystack_[2].value.as < VarDefList* > (); yylhs.value.as < VarDefList* > ()->pushBack(yystack_[0].value.as < VarDef* > ()); }
#line 1263 "src/yacc/Bison.cpp"
    break;

  case 24: // VarDef: IDENT
#line 169 "src/yacc/sysy.y"
         { yylhs.value.as < VarDef* > () = new VarDef(yystack_[0].value.as < std::string > ()); }
#line 1269 "src/yacc/Bison.cpp"
    break;

  case 25: // VarDef: IDENT ASSIGN InitVal
#line 170 "src/yacc/sysy.y"
                       { yylhs.value.as < VarDef* > () = new VarDef(yystack_[2].value.as < std::string > (),nullptr,yystack_[0].value.as < InitVal* > ()); }
#line 1275 "src/yacc/Bison.cpp"
    break;

  case 26: // VarDef: IDENT ConstExpList
#line 171 "src/yacc/sysy.y"
                      { yylhs.value.as < VarDef* > () = new VarDef(yystack_[1].value.as < std::string > (),yystack_[0].value.as < ArrayList* > (),nullptr); }
#line 1281 "src/yacc/Bison.cpp"
    break;

  case 27: // VarDef: IDENT ConstExpList ASSIGN InitVal
#line 172 "src/yacc/sysy.y"
                                     { yylhs.value.as < VarDef* > () = new VarDef(yystack_[3].value.as < std::string > (),yystack_[2].value.as < ArrayList* > (),yystack_[0].value.as < InitVal* > ()); }
#line 1287 "src/yacc/Bison.cpp"
    break;

  case 28: // InitVal: AddExp
#line 176 "src/yacc/sysy.y"
          { yylhs.value.as < InitVal* > () = new InitVal((BaseAST*)yystack_[0].value.as < AddExp* > ()); }
#line 1293 "src/yacc/Bison.cpp"
    break;

  case 29: // InitVal: L_BIG_BRACKET R_BIG_BRACKET
#line 177 "src/yacc/sysy.y"
                               { yylhs.value.as < InitVal* > () = new InitVal(nullptr); }
#line 1299 "src/yacc/Bison.cpp"
    break;

  case 30: // InitVal: L_BIG_BRACKET InitValList R_BIG_BRACKET
#line 178 "src/yacc/sysy.y"
                                           { yylhs.value.as < InitVal* > () = new InitVal((BaseAST*)yystack_[1].value.as < InitValList* > ()); }
#line 1305 "src/yacc/Bison.cpp"
    break;

  case 31: // InitValList: InitVal
#line 182 "src/yacc/sysy.y"
           { yylhs.value.as < InitValList* > () = new InitValList(yystack_[0].value.as < InitVal* > ()); }
#line 1311 "src/yacc/Bison.cpp"
    break;

  case 32: // InitValList: InitValList COMMA InitVal
#line 183 "src/yacc/sysy.y"
                             { yylhs.value.as < InitValList* > () = yystack_[2].value.as < InitValList* > (); yylhs.value.as < InitValList* > ()->pushBack(yystack_[0].value.as < InitVal* > ()); }
#line 1317 "src/yacc/Bison.cpp"
    break;

  case 33: // FuncDef: Type IDENT L_SMALL_BRACKET R_SMALL_BRACKET Block
#line 187 "src/yacc/sysy.y"
                                                   { yylhs.value.as < FuncDef* > () = new FuncDef(yystack_[4].value.as < Type > (), yystack_[3].value.as < std::string > (), nullptr, yystack_[0].value.as < Block* > ()); }
#line 1323 "src/yacc/Bison.cpp"
    break;

  case 34: // FuncDef: Type IDENT L_SMALL_BRACKET FuncFParamList R_SMALL_BRACKET Block
#line 188 "src/yacc/sysy.y"
                                                                  { yylhs.value.as < FuncDef* > () = new FuncDef(yystack_[5].value.as < Type > (), yystack_[4].value.as < std::string > (), yystack_[2].value.as < FuncParamList* > (), yystack_[0].value.as < Block* > ()); }
#line 1329 "src/yacc/Bison.cpp"
    break;

  case 35: // FuncFParamList: FuncFParam
#line 192 "src/yacc/sysy.y"
              { yylhs.value.as < FuncParamList* > () = new FuncParamList(yystack_[0].value.as < FuncParam* > ()); }
#line 1335 "src/yacc/Bison.cpp"
    break;

  case 36: // FuncFParamList: FuncFParamList COMMA FuncFParam
#line 193 "src/yacc/sysy.y"
                                   { yylhs.value.as < FuncParamList* > () = yystack_[2].value.as < FuncParamList* > (); yylhs.value.as < FuncParamList* > ()->pushBack(yystack_[0].value.as < FuncParam* > ()); }
#line 1341 "src/yacc/Bison.cpp"
    break;

  case 37: // FuncFParam: Type IDENT
#line 197 "src/yacc/sysy.y"
              { yylhs.value.as < FuncParam* > () = new FuncParam(yystack_[1].value.as < Type > (),yystack_[0].value.as < std::string > ()); }
#line 1347 "src/yacc/Bison.cpp"
    break;

  case 38: // FuncFParam: Type IDENT L_MIDDLE_BRACKET R_MIDDLE_BRACKET
#line 198 "src/yacc/sysy.y"
                                                { yylhs.value.as < FuncParam* > () = new FuncParam(yystack_[3].value.as < Type > (),yystack_[2].value.as < std::string > (),true); }
#line 1353 "src/yacc/Bison.cpp"
    break;

  case 39: // FuncFParam: Type IDENT ExpList
#line 199 "src/yacc/sysy.y"
                      { yylhs.value.as < FuncParam* > () = new FuncParam(yystack_[2].value.as < Type > (),yystack_[1].value.as < std::string > (),false,yystack_[0].value.as < ArrayList* > ()); }
#line 1359 "src/yacc/Bison.cpp"
    break;

  case 40: // FuncFParam: Type IDENT L_MIDDLE_BRACKET R_MIDDLE_BRACKET ExpList
#line 200 "src/yacc/sysy.y"
                                                        { yylhs.value.as < FuncParam* > () = new FuncParam(yystack_[4].value.as < Type > (),yystack_[3].value.as < std::string > (),true,yystack_[0].value.as < ArrayList* > ()); }
#line 1365 "src/yacc/Bison.cpp"
    break;

  case 41: // Block: L_BIG_BRACKET R_BIG_BRACKET
#line 204 "src/yacc/sysy.y"
                              { yylhs.value.as < Block* > () = new Block(nullptr); }
#line 1371 "src/yacc/Bison.cpp"
    break;

  case 42: // Block: L_BIG_BRACKET BlockItemList R_BIG_BRACKET
#line 205 "src/yacc/sysy.y"
                                             { yylhs.value.as < Block* > () = new Block(yystack_[1].value.as < BlockItemList* > ()); }
#line 1377 "src/yacc/Bison.cpp"
    break;

  case 43: // BlockItemList: BlockItem
#line 209 "src/yacc/sysy.y"
             { yylhs.value.as < BlockItemList* > () = new BlockItemList(yystack_[0].value.as < Stmt* > ()); }
#line 1383 "src/yacc/Bison.cpp"
    break;

  case 44: // BlockItemList: BlockItemList BlockItem
#line 210 "src/yacc/sysy.y"
                           { yylhs.value.as < BlockItemList* > () = yystack_[1].value.as < BlockItemList* > (); yylhs.value.as < BlockItemList* > ()->pushBack(yystack_[0].value.as < Stmt* > ()); }
#line 1389 "src/yacc/Bison.cpp"
    break;

  case 45: // BlockItem: Decl
#line 214 "src/yacc/sysy.y"
       { yylhs.value.as < Stmt* > () = (Stmt*)yystack_[0].value.as < Stmt* > (); }
#line 1395 "src/yacc/Bison.cpp"
    break;

  case 46: // BlockItem: Stmt
#line 215 "src/yacc/sysy.y"
       { yylhs.value.as < Stmt* > () = (Stmt*)yystack_[0].value.as < Stmt* > (); }
#line 1401 "src/yacc/Bison.cpp"
    break;

  case 47: // Stmt: LVal ASSIGN AddExp SEMICOLON
#line 219 "src/yacc/sysy.y"
                               { yylhs.value.as < Stmt* > () = new AssignStmt(yystack_[3].value.as < LVal* > (),yystack_[1].value.as < AddExp* > ()); }
#line 1407 "src/yacc/Bison.cpp"
    break;

  case 48: // Stmt: SEMICOLON
#line 220 "src/yacc/sysy.y"
             { yylhs.value.as < Stmt* > () = new ExpStmt(nullptr); }
#line 1413 "src/yacc/Bison.cpp"
    break;

  case 49: // Stmt: AddExp SEMICOLON
#line 221 "src/yacc/sysy.y"
                    { yylhs.value.as < Stmt* > () = new ExpStmt(yystack_[1].value.as < AddExp* > ()); }
#line 1419 "src/yacc/Bison.cpp"
    break;

  case 50: // Stmt: Block
#line 222 "src/yacc/sysy.y"
         { yylhs.value.as < Stmt* > () = yystack_[0].value.as < Block* > (); }
#line 1425 "src/yacc/Bison.cpp"
    break;

  case 51: // Stmt: IF L_SMALL_BRACKET LOrExp R_SMALL_BRACKET Stmt ELSE Stmt
#line 223 "src/yacc/sysy.y"
                                                            { yylhs.value.as < Stmt* > () = new IfStmt(yystack_[4].value.as < LOrExp* > (),yystack_[2].value.as < Stmt* > (),yystack_[0].value.as < Stmt* > ()); }
#line 1431 "src/yacc/Bison.cpp"
    break;

  case 52: // Stmt: IF L_SMALL_BRACKET LOrExp R_SMALL_BRACKET Stmt
#line 224 "src/yacc/sysy.y"
                                                  { yylhs.value.as < Stmt* > () = new IfStmt(yystack_[2].value.as < LOrExp* > (),yystack_[0].value.as < Stmt* > ()); }
#line 1437 "src/yacc/Bison.cpp"
    break;

  case 53: // Stmt: WHILE L_SMALL_BRACKET LOrExp R_SMALL_BRACKET Stmt
#line 225 "src/yacc/sysy.y"
                                                     { yylhs.value.as < Stmt* > () = new WhileStmt(yystack_[2].value.as < LOrExp* > (),yystack_[0].value.as < Stmt* > ()); }
#line 1443 "src/yacc/Bison.cpp"
    break;

  case 54: // Stmt: BREAK SEMICOLON
#line 226 "src/yacc/sysy.y"
                   { yylhs.value.as < Stmt* > () = new BreakStmt(); }
#line 1449 "src/yacc/Bison.cpp"
    break;

  case 55: // Stmt: CONTINUE SEMICOLON
#line 227 "src/yacc/sysy.y"
                      { yylhs.value.as < Stmt* > () = new ContinueStmt(); }
#line 1455 "src/yacc/Bison.cpp"
    break;

  case 56: // Stmt: RETURN SEMICOLON
#line 228 "src/yacc/sysy.y"
                    { yylhs.value.as < Stmt* > () = new ReturnStmt(); }
#line 1461 "src/yacc/Bison.cpp"
    break;

  case 57: // Stmt: RETURN AddExp SEMICOLON
#line 229 "src/yacc/sysy.y"
                           { yylhs.value.as < Stmt* > () = new ReturnStmt(yystack_[1].value.as < AddExp* > ()); }
#line 1467 "src/yacc/Bison.cpp"
    break;

  case 58: // LVal: IDENT
#line 234 "src/yacc/sysy.y"
        { yylhs.value.as < LVal* > () = new LVal(yystack_[0].value.as < std::string > ()); }
#line 1473 "src/yacc/Bison.cpp"
    break;

  case 59: // LVal: IDENT ExpList
#line 235 "src/yacc/sysy.y"
                 { yylhs.value.as < LVal* > () = new LVal(yystack_[1].value.as < std::string > (),yystack_[0].value.as < ArrayList* > ()); }
#line 1479 "src/yacc/Bison.cpp"
    break;

  case 60: // ExpList: L_MIDDLE_BRACKET AddExp R_MIDDLE_BRACKET
#line 239 "src/yacc/sysy.y"
                                           { yylhs.value.as < ArrayList* > () = new ArrayList(yystack_[1].value.as < AddExp* > ()); }
#line 1485 "src/yacc/Bison.cpp"
    break;

  case 61: // ExpList: ExpList L_MIDDLE_BRACKET AddExp R_MIDDLE_BRACKET
#line 240 "src/yacc/sysy.y"
                                                   { yylhs.value.as < ArrayList* > () = yystack_[3].value.as < ArrayList* > (); yylhs.value.as < ArrayList* > ()->pushBack(yystack_[1].value.as < AddExp* > ()); }
#line 1491 "src/yacc/Bison.cpp"
    break;

  case 62: // PrimaryExp: L_SMALL_BRACKET AddExp R_SMALL_BRACKET
#line 244 "src/yacc/sysy.y"
                                          { yylhs.value.as < HasOperand* > () = (HasOperand*)yystack_[1].value.as < AddExp* > (); }
#line 1497 "src/yacc/Bison.cpp"
    break;

  case 63: // PrimaryExp: LVal
#line 245 "src/yacc/sysy.y"
        { yylhs.value.as < HasOperand* > () = (HasOperand*)yystack_[0].value.as < LVal* > (); }
#line 1503 "src/yacc/Bison.cpp"
    break;

  case 64: // PrimaryExp: INTCONST
#line 246 "src/yacc/sysy.y"
           { yylhs.value.as < HasOperand* > () = (HasOperand*)(new ConValue<int>(yystack_[0].value.as < int > ())); }
#line 1509 "src/yacc/Bison.cpp"
    break;

  case 65: // PrimaryExp: FLOATCONST
#line 247 "src/yacc/sysy.y"
             { yylhs.value.as < HasOperand* > () = (HasOperand*)(new ConValue<float>(yystack_[0].value.as < float > ())); }
#line 1515 "src/yacc/Bison.cpp"
    break;

  case 66: // PrimaryExp: IDENT L_SMALL_BRACKET R_SMALL_BRACKET
#line 248 "src/yacc/sysy.y"
                                         {yylhs.value.as < HasOperand* > () = (HasOperand*)(new FuncCall(yystack_[2].value.as < std::string > (), yylineno));}
#line 1521 "src/yacc/Bison.cpp"
    break;

  case 67: // PrimaryExp: IDENT L_SMALL_BRACKET FuncRParamList R_SMALL_BRACKET
#line 249 "src/yacc/sysy.y"
                                                       {yylhs.value.as < HasOperand* > () = (HasOperand*)(new FuncCall(yystack_[3].value.as < std::string > (), yystack_[1].value.as < FuncRParamList* > (), yylineno));}
#line 1527 "src/yacc/Bison.cpp"
    break;

  case 68: // UnaryExp: PrimaryExp
#line 255 "src/yacc/sysy.y"
             { yylhs.value.as < UnaryExp* > () = new UnaryExp(yystack_[0].value.as < HasOperand* > ()); }
#line 1533 "src/yacc/Bison.cpp"
    break;

  case 69: // UnaryExp: ADD UnaryExp
#line 256 "src/yacc/sysy.y"
               { yylhs.value.as < UnaryExp* > () = yystack_[0].value.as < UnaryExp* > (); yylhs.value.as < UnaryExp* > ()->pushFront(SY_ADD); }
#line 1539 "src/yacc/Bison.cpp"
    break;

  case 70: // UnaryExp: SUB UnaryExp
#line 257 "src/yacc/sysy.y"
               { yylhs.value.as < UnaryExp* > () = yystack_[0].value.as < UnaryExp* > (); yylhs.value.as < UnaryExp* > ()->pushFront(SY_SUB); }
#line 1545 "src/yacc/Bison.cpp"
    break;

  case 71: // UnaryExp: NOT UnaryExp
#line 258 "src/yacc/sysy.y"
               { yylhs.value.as < UnaryExp* > () = yystack_[0].value.as < UnaryExp* > (); yylhs.value.as < UnaryExp* > ()->pushFront(SY_NOT); }
#line 1551 "src/yacc/Bison.cpp"
    break;

  case 72: // FuncRParamList: AddExp
#line 263 "src/yacc/sysy.y"
          { yylhs.value.as < FuncRParamList* > () = new FuncRParamList(yystack_[0].value.as < AddExp* > ()); }
#line 1557 "src/yacc/Bison.cpp"
    break;

  case 73: // FuncRParamList: FuncRParamList COMMA AddExp
#line 264 "src/yacc/sysy.y"
                              { yylhs.value.as < FuncRParamList* > () = yystack_[2].value.as < FuncRParamList* > (); yylhs.value.as < FuncRParamList* > ()->pushBack(yystack_[0].value.as < AddExp* > ()); }
#line 1563 "src/yacc/Bison.cpp"
    break;

  case 74: // MulExp: UnaryExp
#line 268 "src/yacc/sysy.y"
            { yylhs.value.as < MulExp* > () = new MulExp(yystack_[0].value.as < UnaryExp* > ()); }
#line 1569 "src/yacc/Bison.cpp"
    break;

  case 75: // MulExp: MulExp MUL UnaryExp
#line 269 "src/yacc/sysy.y"
                       { yylhs.value.as < MulExp* > () = yystack_[2].value.as < MulExp* > (); yylhs.value.as < MulExp* > ()->pushBack(SY_MUL); yylhs.value.as < MulExp* > ()->pushBack(yystack_[0].value.as < UnaryExp* > ()); }
#line 1575 "src/yacc/Bison.cpp"
    break;

  case 76: // MulExp: MulExp DIV UnaryExp
#line 270 "src/yacc/sysy.y"
                       { yylhs.value.as < MulExp* > () = yystack_[2].value.as < MulExp* > (); yylhs.value.as < MulExp* > ()->pushBack(SY_DIV); yylhs.value.as < MulExp* > ()->pushBack(yystack_[0].value.as < UnaryExp* > ()); }
#line 1581 "src/yacc/Bison.cpp"
    break;

  case 77: // MulExp: MulExp MOD UnaryExp
#line 271 "src/yacc/sysy.y"
                       { yylhs.value.as < MulExp* > () = yystack_[2].value.as < MulExp* > (); yylhs.value.as < MulExp* > ()->pushBack(SY_MOD); yylhs.value.as < MulExp* > ()->pushBack(yystack_[0].value.as < UnaryExp* > ()); }
#line 1587 "src/yacc/Bison.cpp"
    break;

  case 78: // AddExp: MulExp
#line 275 "src/yacc/sysy.y"
         { yylhs.value.as < AddExp* > () = new AddExp(yystack_[0].value.as < MulExp* > ()); }
#line 1593 "src/yacc/Bison.cpp"
    break;

  case 79: // AddExp: AddExp ADD MulExp
#line 276 "src/yacc/sysy.y"
                    { yylhs.value.as < AddExp* > () = yystack_[2].value.as < AddExp* > (); yylhs.value.as < AddExp* > ()->pushBack(SY_ADD); yylhs.value.as < AddExp* > ()->pushBack(yystack_[0].value.as < MulExp* > ()); }
#line 1599 "src/yacc/Bison.cpp"
    break;

  case 80: // AddExp: AddExp SUB MulExp
#line 277 "src/yacc/sysy.y"
                    { yylhs.value.as < AddExp* > () = yystack_[2].value.as < AddExp* > (); yylhs.value.as < AddExp* > ()->pushBack(SY_SUB); yylhs.value.as < AddExp* > ()->pushBack(yystack_[0].value.as < MulExp* > ()); }
#line 1605 "src/yacc/Bison.cpp"
    break;

  case 81: // RelExp: AddExp
#line 281 "src/yacc/sysy.y"
          {yylhs.value.as < RelExp* > () = new RelExp(yystack_[0].value.as < AddExp* > ());}
#line 1611 "src/yacc/Bison.cpp"
    break;

  case 82: // RelExp: RelExp LESS AddExp
#line 282 "src/yacc/sysy.y"
                     { yylhs.value.as < RelExp* > () = yystack_[2].value.as < RelExp* > (); yylhs.value.as < RelExp* > ()->pushBack(SY_LESS); yylhs.value.as < RelExp* > ()->pushBack(yystack_[0].value.as < AddExp* > ()); }
#line 1617 "src/yacc/Bison.cpp"
    break;

  case 83: // RelExp: RelExp GREAT AddExp
#line 283 "src/yacc/sysy.y"
                      { yylhs.value.as < RelExp* > () = yystack_[2].value.as < RelExp* > (); yylhs.value.as < RelExp* > ()->pushBack(SY_GREAT); yylhs.value.as < RelExp* > ()->pushBack(yystack_[0].value.as < AddExp* > ()); }
#line 1623 "src/yacc/Bison.cpp"
    break;

  case 84: // RelExp: RelExp LESSEQ AddExp
#line 284 "src/yacc/sysy.y"
                       { yylhs.value.as < RelExp* > () = yystack_[2].value.as < RelExp* > (); yylhs.value.as < RelExp* > ()->pushBack(SY_LESSEQ); yylhs.value.as < RelExp* > ()->pushBack(yystack_[0].value.as < AddExp* > ()); }
#line 1629 "src/yacc/Bison.cpp"
    break;

  case 85: // RelExp: RelExp GREATEQ AddExp
#line 285 "src/yacc/sysy.y"
                        { yylhs.value.as < RelExp* > () = yystack_[2].value.as < RelExp* > (); yylhs.value.as < RelExp* > ()->pushBack(SY_GREATEQ); yylhs.value.as < RelExp* > ()->pushBack(yystack_[0].value.as < AddExp* > ()); }
#line 1635 "src/yacc/Bison.cpp"
    break;

  case 86: // EqExp: RelExp
#line 289 "src/yacc/sysy.y"
         { yylhs.value.as < EqExp* > () = new EqExp(yystack_[0].value.as < RelExp* > ()); }
#line 1641 "src/yacc/Bison.cpp"
    break;

  case 87: // EqExp: EqExp EQ RelExp
#line 290 "src/yacc/sysy.y"
                  { yylhs.value.as < EqExp* > () = yystack_[2].value.as < EqExp* > (); yylhs.value.as < EqExp* > ()->pushBack(SY_EQ); yylhs.value.as < EqExp* > ()->pushBack(yystack_[0].value.as < RelExp* > ()); }
#line 1647 "src/yacc/Bison.cpp"
    break;

  case 88: // EqExp: EqExp NOTEQ RelExp
#line 291 "src/yacc/sysy.y"
                     { yylhs.value.as < EqExp* > () = yystack_[2].value.as < EqExp* > (); yylhs.value.as < EqExp* > ()->pushBack(SY_NOTEQ); yylhs.value.as < EqExp* > ()->pushBack(yystack_[0].value.as < RelExp* > ()); }
#line 1653 "src/yacc/Bison.cpp"
    break;

  case 89: // LAndExp: EqExp
#line 295 "src/yacc/sysy.y"
         { yylhs.value.as < LAndExp* > () = new LAndExp(yystack_[0].value.as < EqExp* > ()); }
#line 1659 "src/yacc/Bison.cpp"
    break;

  case 90: // LAndExp: LAndExp AND EqExp
#line 296 "src/yacc/sysy.y"
                    { yylhs.value.as < LAndExp* > () = yystack_[2].value.as < LAndExp* > (); yylhs.value.as < LAndExp* > ()->pushBack(SY_AND); yylhs.value.as < LAndExp* > ()->pushBack(yystack_[0].value.as < EqExp* > ()); }
#line 1665 "src/yacc/Bison.cpp"
    break;

  case 91: // LOrExp: LAndExp
#line 300 "src/yacc/sysy.y"
          { yylhs.value.as < LOrExp* > () = new LOrExp(yystack_[0].value.as < LAndExp* > ()); }
#line 1671 "src/yacc/Bison.cpp"
    break;

  case 92: // LOrExp: LOrExp OR LAndExp
#line 301 "src/yacc/sysy.y"
                    { yylhs.value.as < LOrExp* > () = yystack_[2].value.as < LOrExp* > (); yylhs.value.as < LOrExp* > ()->pushBack(SY_OR); yylhs.value.as < LOrExp* > ()->pushBack(yystack_[0].value.as < LAndExp* > ()); }
#line 1677 "src/yacc/Bison.cpp"
    break;

  case 93: // Type: VOID
#line 306 "src/yacc/sysy.y"
       {yylhs.value.as < Type > ()=SY_VOID;}
#line 1683 "src/yacc/Bison.cpp"
    break;

  case 94: // Type: INT
#line 307 "src/yacc/sysy.y"
       {yylhs.value.as < Type > ()=SY_INT;}
#line 1689 "src/yacc/Bison.cpp"
    break;

  case 95: // Type: FLOAT
#line 308 "src/yacc/sysy.y"
         {yylhs.value.as < Type > ()=SY_FLOAT;}
#line 1695 "src/yacc/Bison.cpp"
    break;


#line 1699 "src/yacc/Bison.cpp"

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









  const signed char parser::yypact_ninf_ = -75;

  const signed char parser::yytable_ninf_ = -1;

  const short
  parser::yypact_[] =
  {
     158,   101,   -75,   -75,   -75,    24,   158,   -75,   -75,   -75,
     -75,    43,    47,   -75,   -75,   -75,   169,   123,   -75,   -10,
     152,   -75,   243,    11,    86,    -2,   -75,    55,   248,    92,
     -75,    47,   -75,   -75,    40,    86,    86,    86,    86,   208,
     -75,   -75,   -75,   -75,   173,   119,    28,    -6,   -75,    99,
      31,   243,    86,   139,   -75,   214,   -75,   119,   248,   -75,
     277,    86,    79,   -75,   -75,   -75,    35,   -75,   -75,    50,
      86,    86,    86,    86,    86,   115,   -75,    28,   101,    81,
     -75,   -75,    73,   -75,   -75,    58,   -75,   -75,    -4,   119,
     113,    86,   -75,   -75,   243,   -75,   -75,   -75,   173,   173,
      37,    95,   111,   131,   134,   -75,   -75,   -75,   -75,   150,
     -75,   -75,   124,    39,    55,   -75,   -75,   253,    79,   -75,
     -75,   248,   -75,    86,   -75,   154,   -75,   -75,    42,    86,
      86,   -75,   -75,   -75,   -75,    86,   -75,   144,   -75,   119,
     -75,   -75,   119,   206,   216,   181,    -5,     5,    63,    79,
      86,    86,    86,    86,    86,    86,    86,    86,   179,   179,
     -75,   119,   119,   119,   119,   206,   206,   216,   181,   170,
     -75,   179,   -75
  };

  const signed char
  parser::yydefact_[] =
  {
       0,     0,    94,    95,    93,     0,     2,     5,     7,     8,
       6,     0,     0,     1,     3,     4,    24,     0,    22,     0,
       0,    11,     0,     0,     0,    26,    21,     0,     0,     0,
       9,     0,    64,    65,    58,     0,     0,     0,     0,     0,
      25,    63,    68,    74,    78,    28,     0,     0,    35,     0,
       0,     0,     0,    24,    23,     0,    12,    16,     0,    10,
       0,     0,    59,    71,    69,    70,     0,    29,    31,     0,
       0,     0,     0,     0,     0,     0,    33,     0,     0,    37,
      15,    27,     0,    17,    19,     0,    13,    66,     0,    72,
       0,     0,    62,    30,     0,    75,    76,    77,    79,    80,
       0,     0,     0,     0,     0,    41,    48,    45,    50,     0,
      43,    46,    63,     0,     0,    34,    36,     0,    39,    14,
      18,     0,    67,     0,    60,     0,    32,    56,     0,     0,
       0,    54,    55,    42,    44,     0,    49,    38,    20,    73,
      61,    57,    81,    86,    89,    91,     0,     0,     0,    40,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
      47,    82,    83,    84,    85,    87,    88,    90,    92,    52,
      53,     0,    51
  };

  const short
  parser::yypgoto_[] =
  {
     -75,   -75,   -75,    75,   -75,   -75,   172,   182,   -51,   -75,
     -75,   -75,   188,   -30,   -75,   200,   -75,   147,   -33,   -75,
     128,   -56,   -74,   -71,   -75,   -25,   -75,   162,   -22,    77,
      82,    83,   112,     2
  };

  const unsigned char
  parser::yydefgoto_[] =
  {
       0,     5,     6,   107,     8,    20,    21,    25,    56,    85,
       9,    17,    18,    40,    69,    10,    47,    48,   108,   109,
     110,   111,    41,    62,    42,    43,    88,    44,   142,   143,
     144,   145,   146,    11
  };

  const unsigned char
  parser::yytable_[] =
  {
      45,   112,    50,    12,    84,   157,    57,    86,   118,    68,
      63,    64,    65,    76,    28,   157,    66,    45,     2,     3,
       4,    81,    51,    24,    13,    49,    77,   158,   122,    45,
      82,    52,    78,    57,   123,   112,    57,   159,    89,    90,
      32,    33,    34,    46,   115,    95,    96,    97,    16,    35,
      73,    74,    19,   113,    73,    74,    36,    37,    73,    74,
      53,    73,    74,    75,   126,    80,   149,    92,    38,   125,
     138,    60,    45,    61,   127,     7,   136,   114,   128,   141,
      49,    14,    73,    74,   112,   112,    93,   113,    94,    32,
      33,    34,    73,    74,   120,    90,   121,   112,    35,    57,
     160,   139,   169,   170,    79,    36,    37,   119,     2,     3,
       4,   114,    91,   148,   117,   172,    58,    38,    32,    33,
      34,     1,     2,     3,     4,    52,   129,    35,   161,   162,
     163,   164,    73,    74,    36,    37,   113,   113,    73,    74,
     100,   101,   130,   102,   103,   104,    38,   124,   135,   113,
      75,   105,   106,    32,    33,    34,     1,     2,     3,     4,
      26,    27,    35,    22,     1,     2,     3,     4,   131,    36,
      37,   132,    24,    73,    74,   100,   101,    61,   102,   103,
     104,    38,    32,    33,    34,    75,   133,   106,   140,    30,
      31,    35,   156,    22,    70,    71,    72,   171,    36,    37,
      23,    29,    24,    59,   100,   101,    15,   102,   103,   104,
      38,    32,    33,    34,    75,    54,   106,    32,    33,    34,
      35,   150,   151,   152,   153,   116,    35,    36,    37,   154,
     155,   165,   166,    36,    37,    98,    99,   134,   167,    38,
     168,     0,   147,    39,    67,    38,    32,    33,    34,    55,
      83,    32,    33,    34,     0,    35,    32,    33,    34,     0,
      35,     0,    36,    37,     0,    35,     0,    36,    37,     0,
       0,     0,    36,    37,    38,     0,     0,     0,    39,    38,
      32,    33,    34,    55,    38,     0,     0,   137,     0,    35,
       0,     0,     0,     0,     0,     0,    36,    37,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,    38,    87
  };

  const short
  parser::yycheck_[] =
  {
      22,    75,    24,     1,    55,    10,    28,    58,    79,    39,
      35,    36,    37,    46,    24,    10,    38,    39,     7,     8,
       9,    51,    24,    33,     0,    23,    32,    32,    32,    51,
      52,    33,    38,    55,    38,   109,    58,    32,    60,    61,
       3,     4,     5,    32,    77,    70,    71,    72,     5,    12,
      19,    20,     5,    75,    19,    20,    19,    20,    19,    20,
       5,    19,    20,    35,    94,    34,   137,    32,    31,    91,
     121,    31,    94,    33,    37,     0,    37,    75,   100,    37,
      78,     6,    19,    20,   158,   159,    36,   109,    38,     3,
       4,     5,    19,    20,    36,   117,    38,   171,    12,   121,
      37,   123,   158,   159,     5,    19,    20,    34,     7,     8,
       9,   109,    33,   135,    33,   171,    24,    31,     3,     4,
       5,     6,     7,     8,     9,    33,    31,    12,   150,   151,
     152,   153,    19,    20,    19,    20,   158,   159,    19,    20,
      25,    26,    31,    28,    29,    30,    31,    34,    24,   171,
      35,    36,    37,     3,     4,     5,     6,     7,     8,     9,
      37,    38,    12,    24,     6,     7,     8,     9,    37,    19,
      20,    37,    33,    19,    20,    25,    26,    33,    28,    29,
      30,    31,     3,     4,     5,    35,    36,    37,    34,    37,
      38,    12,    11,    24,    21,    22,    23,    27,    19,    20,
      31,    19,    33,    31,    25,    26,     6,    28,    29,    30,
      31,     3,     4,     5,    35,    27,    37,     3,     4,     5,
      12,    15,    16,    17,    18,    78,    12,    19,    20,    13,
      14,   154,   155,    19,    20,    73,    74,   109,   156,    31,
     157,    -1,   130,    35,    36,    31,     3,     4,     5,    35,
      36,     3,     4,     5,    -1,    12,     3,     4,     5,    -1,
      12,    -1,    19,    20,    -1,    12,    -1,    19,    20,    -1,
      -1,    -1,    19,    20,    31,    -1,    -1,    -1,    35,    31,
       3,     4,     5,    35,    31,    -1,    -1,    34,    -1,    12,
      -1,    -1,    -1,    -1,    -1,    -1,    19,    20,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    31,    32
  };

  const signed char
  parser::yystos_[] =
  {
       0,     6,     7,     8,     9,    41,    42,    43,    44,    50,
      55,    73,    73,     0,    43,    55,     5,    51,    52,     5,
      45,    46,    24,    31,    33,    47,    37,    38,    24,    47,
      37,    38,     3,     4,     5,    12,    19,    20,    31,    35,
      53,    62,    64,    65,    67,    68,    32,    56,    57,    73,
      68,    24,    33,     5,    52,    35,    48,    68,    24,    46,
      31,    33,    63,    65,    65,    65,    68,    36,    53,    54,
      21,    22,    23,    19,    20,    35,    58,    32,    38,     5,
      34,    53,    68,    36,    48,    49,    48,    32,    66,    68,
      68,    33,    32,    36,    38,    65,    65,    65,    67,    67,
      25,    26,    28,    29,    30,    36,    37,    43,    58,    59,
      60,    61,    62,    68,    73,    58,    57,    33,    63,    34,
      36,    38,    32,    38,    34,    68,    53,    37,    68,    31,
      31,    37,    37,    36,    60,    24,    37,    34,    48,    68,
      34,    37,    68,    69,    70,    71,    72,    72,    68,    63,
      15,    16,    17,    18,    13,    14,    11,    10,    32,    32,
      37,    68,    68,    68,    68,    69,    69,    70,    71,    61,
      61,    27,    61
  };

  const signed char
  parser::yyr1_[] =
  {
       0,    40,    41,    42,    42,    42,    42,    43,    43,    44,
      45,    45,    46,    46,    47,    47,    48,    48,    48,    49,
      49,    50,    51,    51,    52,    52,    52,    52,    53,    53,
      53,    54,    54,    55,    55,    56,    56,    57,    57,    57,
      57,    58,    58,    59,    59,    60,    60,    61,    61,    61,
      61,    61,    61,    61,    61,    61,    61,    61,    62,    62,
      63,    63,    64,    64,    64,    64,    64,    64,    65,    65,
      65,    65,    66,    66,    67,    67,    67,    67,    68,    68,
      68,    69,    69,    69,    69,    69,    70,    70,    70,    71,
      71,    72,    72,    73,    73,    73
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
       2,     2,     1,     3,     1,     3,     3,     3,     1,     3,
       3,     1,     3,     3,     3,     3,     1,     3,     3,     1,
       3,     1,     3,     1,     1,     1
  };


#if YYDEBUG
  // YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
  // First, the terminals, then, starting at \a YYNTOKENS, nonterminals.
  const char*
  const parser::yytname_[] =
  {
  "\"end of file\"", "error", "\"invalid token\"", "INTCONST",
  "FLOATCONST", "IDENT", "CONST", "INT", "FLOAT", "VOID", "OR", "AND",
  "NOT", "EQ", "NOTEQ", "LESS", "GREAT", "LESSEQ", "GREATEQ", "ADD", "SUB",
  "MUL", "DIV", "MOD", "ASSIGN", "RETURN", "IF", "ELSE", "WHILE", "BREAK",
  "CONTINUE", "L_SMALL_BRACKET", "R_SMALL_BRACKET", "L_MIDDLE_BRACKET",
  "R_MIDDLE_BRACKET", "L_BIG_BRACKET", "R_BIG_BRACKET", "SEMICOLON",
  "COMMA", "END", "$accept", "Start", "CompUnit", "Decl", "ConstDecl",
  "ConstDefList", "ConstDef", "ConstExpList", "ConstInitVal",
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
       0,   112,   112,   116,   117,   118,   119,   124,   125,   130,
     134,   135,   139,   140,   144,   145,   149,   150,   151,   155,
     156,   160,   164,   165,   169,   170,   171,   172,   176,   177,
     178,   182,   183,   187,   188,   192,   193,   197,   198,   199,
     200,   204,   205,   209,   210,   214,   215,   219,   220,   221,
     222,   223,   224,   225,   226,   227,   228,   229,   234,   235,
     239,   240,   244,   245,   246,   247,   248,   249,   255,   256,
     257,   258,   263,   264,   268,   269,   270,   271,   275,   276,
     277,   281,   282,   283,   284,   285,   289,   290,   291,   295,
     296,   300,   301,   306,   307,   308
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

#line 311 "src/yacc/sysy.y"
