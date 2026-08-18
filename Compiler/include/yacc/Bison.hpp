// A Bison parser, made by GNU Bison 3.8.2.

// Skeleton interface for Bison LALR(1) parsers in C++

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


/**
 ** \file include/yacc/Bison.hpp
 ** Define the yy::parser class.
 */

// C++ LALR(1) parser skeleton written by Akim Demaille.

// DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
// especially those whose name start with YY_ or yy_.  They are
// private implementation details that can be changed or removed.

#ifndef YY_YY_INCLUDE_YACC_BISON_HPP_INCLUDED
# define YY_YY_INCLUDE_YACC_BISON_HPP_INCLUDED
// "%code requires" blocks.
#line 12 "src/yacc/sysy.y"

  #include "../lib/AST.hpp"
  #include <memory>
  #include <iostream>

  // 调用行号必须在词法器识别函数名时锁定，不能在右括号归约时读取。
  struct LocatedIdentifier {
      std::string text;
      int line;
  };

#line 61 "include/yacc/Bison.hpp"


# include <cstdlib> // std::abort
# include <iostream>
# include <stdexcept>
# include <string>
# include <vector>

#if defined __cplusplus
# define YY_CPLUSPLUS __cplusplus
#else
# define YY_CPLUSPLUS 199711L
#endif

// Support move semantics when possible.
#if 201103L <= YY_CPLUSPLUS
# define YY_MOVE           std::move
# define YY_MOVE_OR_COPY   move
# define YY_MOVE_REF(Type) Type&&
# define YY_RVREF(Type)    Type&&
# define YY_COPY(Type)     Type
#else
# define YY_MOVE
# define YY_MOVE_OR_COPY   copy
# define YY_MOVE_REF(Type) Type&
# define YY_RVREF(Type)    const Type&
# define YY_COPY(Type)     const Type&
#endif

// Support noexcept when possible.
#if 201103L <= YY_CPLUSPLUS
# define YY_NOEXCEPT noexcept
# define YY_NOTHROW
#else
# define YY_NOEXCEPT
# define YY_NOTHROW throw ()
#endif

// Support constexpr when possible.
#if 201703 <= YY_CPLUSPLUS
# define YY_CONSTEXPR constexpr
#else
# define YY_CONSTEXPR
#endif



#ifndef YY_ATTRIBUTE_PURE
# if defined __GNUC__ && 2 < __GNUC__ + (96 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_PURE __attribute__ ((__pure__))
# else
#  define YY_ATTRIBUTE_PURE
# endif
#endif

#ifndef YY_ATTRIBUTE_UNUSED
# if defined __GNUC__ && 2 < __GNUC__ + (7 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_UNUSED __attribute__ ((__unused__))
# else
#  define YY_ATTRIBUTE_UNUSED
# endif
#endif

/* Suppress unused-variable warnings by "using" E.  */
#if ! defined lint || defined __GNUC__
# define YY_USE(E) ((void) (E))
#else
# define YY_USE(E) /* empty */
#endif

/* Suppress an incorrect diagnostic about yylval being uninitialized.  */
#if defined __GNUC__ && ! defined __ICC && 406 <= __GNUC__ * 100 + __GNUC_MINOR__
# if __GNUC__ * 100 + __GNUC_MINOR__ < 407
#  define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                           \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")
# else
#  define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                           \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")              \
    _Pragma ("GCC diagnostic ignored \"-Wmaybe-uninitialized\"")
# endif
# define YY_IGNORE_MAYBE_UNINITIALIZED_END      \
    _Pragma ("GCC diagnostic pop")
#else
# define YY_INITIAL_VALUE(Value) Value
#endif
#ifndef YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_END
#endif
#ifndef YY_INITIAL_VALUE
# define YY_INITIAL_VALUE(Value) /* Nothing. */
#endif

#if defined __cplusplus && defined __GNUC__ && ! defined __ICC && 6 <= __GNUC__
# define YY_IGNORE_USELESS_CAST_BEGIN                          \
    _Pragma ("GCC diagnostic push")                            \
    _Pragma ("GCC diagnostic ignored \"-Wuseless-cast\"")
# define YY_IGNORE_USELESS_CAST_END            \
    _Pragma ("GCC diagnostic pop")
#endif
#ifndef YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_END
#endif

# ifndef YY_CAST
#  ifdef __cplusplus
#   define YY_CAST(Type, Val) static_cast<Type> (Val)
#   define YY_REINTERPRET_CAST(Type, Val) reinterpret_cast<Type> (Val)
#  else
#   define YY_CAST(Type, Val) ((Type) (Val))
#   define YY_REINTERPRET_CAST(Type, Val) ((Type) (Val))
#  endif
# endif
# ifndef YY_NULLPTR
#  if defined __cplusplus
#   if 201103L <= __cplusplus
#    define YY_NULLPTR nullptr
#   else
#    define YY_NULLPTR 0
#   endif
#  else
#   define YY_NULLPTR ((void*)0)
#  endif
# endif

/* Debug traces.  */
#ifndef YYDEBUG
# define YYDEBUG 0
#endif

namespace yy {
#line 196 "include/yacc/Bison.hpp"




  /// A Bison parser.
  class parser
  {
  public:
#ifdef YYSTYPE
# ifdef __GNUC__
#  pragma GCC message "bison: do not #define YYSTYPE in C++, use %define api.value.type"
# endif
    typedef YYSTYPE value_type;
#else
  /// A buffer to store and retrieve objects.
  ///
  /// Sort of a variant, but does not keep track of the nature
  /// of the stored data, since that knowledge is available
  /// via the current parser state.
  class value_type
  {
  public:
    /// Type of *this.
    typedef value_type self_type;

    /// Empty construction.
    value_type () YY_NOEXCEPT
      : yyraw_ ()
    {}

    /// Construct and fill.
    template <typename T>
    value_type (YY_RVREF (T) t)
    {
      new (yyas_<T> ()) T (YY_MOVE (t));
    }

#if 201103L <= YY_CPLUSPLUS
    /// Non copyable.
    value_type (const self_type&) = delete;
    /// Non copyable.
    self_type& operator= (const self_type&) = delete;
#endif

    /// Destruction, allowed only if empty.
    ~value_type () YY_NOEXCEPT
    {}

# if 201103L <= YY_CPLUSPLUS
    /// Instantiate a \a T in here from \a t.
    template <typename T, typename... U>
    T&
    emplace (U&&... u)
    {
      return *new (yyas_<T> ()) T (std::forward <U>(u)...);
    }
# else
    /// Instantiate an empty \a T in here.
    template <typename T>
    T&
    emplace ()
    {
      return *new (yyas_<T> ()) T ();
    }

    /// Instantiate a \a T in here from \a t.
    template <typename T>
    T&
    emplace (const T& t)
    {
      return *new (yyas_<T> ()) T (t);
    }
# endif

    /// Instantiate an empty \a T in here.
    /// Obsolete, use emplace.
    template <typename T>
    T&
    build ()
    {
      return emplace<T> ();
    }

    /// Instantiate a \a T in here from \a t.
    /// Obsolete, use emplace.
    template <typename T>
    T&
    build (const T& t)
    {
      return emplace<T> (t);
    }

    /// Accessor to a built \a T.
    template <typename T>
    T&
    as () YY_NOEXCEPT
    {
      return *yyas_<T> ();
    }

    /// Const accessor to a built \a T (for %printer).
    template <typename T>
    const T&
    as () const YY_NOEXCEPT
    {
      return *yyas_<T> ();
    }

    /// Swap the content with \a that, of same type.
    ///
    /// Both variants must be built beforehand, because swapping the actual
    /// data requires reading it (with as()), and this is not possible on
    /// unconstructed variants: it would require some dynamic testing, which
    /// should not be the variant's responsibility.
    /// Swapping between built and (possibly) non-built is done with
    /// self_type::move ().
    template <typename T>
    void
    swap (self_type& that) YY_NOEXCEPT
    {
      std::swap (as<T> (), that.as<T> ());
    }

    /// Move the content of \a that to this.
    ///
    /// Destroys \a that.
    template <typename T>
    void
    move (self_type& that)
    {
# if 201103L <= YY_CPLUSPLUS
      emplace<T> (std::move (that.as<T> ()));
# else
      emplace<T> ();
      swap<T> (that);
# endif
      that.destroy<T> ();
    }

# if 201103L <= YY_CPLUSPLUS
    /// Move the content of \a that to this.
    template <typename T>
    void
    move (self_type&& that)
    {
      emplace<T> (std::move (that.as<T> ()));
      that.destroy<T> ();
    }
#endif

    /// Copy the content of \a that to this.
    template <typename T>
    void
    copy (const self_type& that)
    {
      emplace<T> (that.as<T> ());
    }

    /// Destroy the stored \a T.
    template <typename T>
    void
    destroy ()
    {
      as<T> ().~T ();
    }

  private:
#if YY_CPLUSPLUS < 201103L
    /// Non copyable.
    value_type (const self_type&);
    /// Non copyable.
    self_type& operator= (const self_type&);
#endif

    /// Accessor to raw memory as \a T.
    template <typename T>
    T*
    yyas_ () YY_NOEXCEPT
    {
      void *yyp = yyraw_;
      return static_cast<T*> (yyp);
     }

    /// Const accessor to raw memory as \a T.
    template <typename T>
    const T*
    yyas_ () const YY_NOEXCEPT
    {
      const void *yyp = yyraw_;
      return static_cast<const T*> (yyp);
     }

    /// An auxiliary type to compute the largest semantic type.
    union union_type
    {
      // Type
      char dummy1[sizeof (ASTType)];

      // AddExp
      char dummy2[sizeof (AddExp*)];

      // ConstExpList
      // ExpList
      char dummy3[sizeof (ArrayList*)];

      // Decl
      // BlockItem
      // Stmt
      // PrimaryExp
      char dummy4[sizeof (BaseAST*)];

      // Block
      char dummy5[sizeof (Block*)];

      // BlockItemList
      char dummy6[sizeof (BlockItemList*)];

      // CompUnit
      char dummy7[sizeof (CompUnit*)];

      // ConstDecl
      char dummy8[sizeof (ConstDecl*)];

      // ConstDef
      char dummy9[sizeof (ConstDef*)];

      // ConstDefList
      char dummy10[sizeof (ConstDefList*)];

      // EqExp
      char dummy11[sizeof (EqExp*)];

      // FuncDef
      char dummy12[sizeof (FuncDef*)];

      // FuncFParam
      char dummy13[sizeof (FuncParam*)];

      // FuncFParamList
      char dummy14[sizeof (FuncParamList*)];

      // FuncRParamList
      char dummy15[sizeof (FuncRParamList*)];

      // ConstInitVal
      // InitVal
      char dummy16[sizeof (InitVal*)];

      // ConstInitValList
      // InitValList
      char dummy17[sizeof (InitValList*)];

      // LAndExp
      char dummy18[sizeof (LAndExp*)];

      // LOrExp
      char dummy19[sizeof (LOrExp*)];

      // LVal
      char dummy20[sizeof (LVal*)];

      // IDENT
      char dummy21[sizeof (LocatedIdentifier)];

      // MulExp
      char dummy22[sizeof (MulExp*)];

      // RelExp
      char dummy23[sizeof (RelExp*)];

      // UnaryExp
      char dummy24[sizeof (UnaryExp*)];

      // VarDecl
      char dummy25[sizeof (VarDecl*)];

      // VarDef
      char dummy26[sizeof (VarDef*)];

      // VarDefList
      char dummy27[sizeof (VarDefList*)];

      // FLOATCONST
      char dummy28[sizeof (float)];

      // INTCONST
      char dummy29[sizeof (int)];
    };

    /// The size of the largest semantic type.
    enum { size = sizeof (union_type) };

    /// A buffer to store semantic values.
    union
    {
      /// Strongest alignment constraints.
      long double yyalign_me_;
      /// A buffer large enough to store any of the semantic values.
      char yyraw_[size];
    };
  };

#endif
    /// Backward compatibility (Bison 3.8).
    typedef value_type semantic_type;


    /// Syntax errors thrown from user actions.
    struct syntax_error : std::runtime_error
    {
      syntax_error (const std::string& m)
        : std::runtime_error (m)
      {}

      syntax_error (const syntax_error& s)
        : std::runtime_error (s.what ())
      {}

      ~syntax_error () YY_NOEXCEPT YY_NOTHROW;
    };

    /// Token kinds.
    struct token
    {
      enum token_kind_type
      {
        YYEMPTY = -2,
    YYEOF = 0,                     // "end of file"
    YYerror = 256,                 // error
    YYUNDEF = 257,                 // "invalid token"
    INTCONST = 258,                // INTCONST
    FLOATCONST = 259,              // FLOATCONST
    IDENT = 260,                   // IDENT
    CONST = 261,                   // CONST
    INT = 262,                     // INT
    FLOAT = 263,                   // FLOAT
    VOID = 264,                    // VOID
    TENSOR = 265,                  // TENSOR
    GEMM = 266,                    // GEMM
    OR = 267,                      // OR
    AND = 268,                     // AND
    NOT = 269,                     // NOT
    EQ = 270,                      // EQ
    NOTEQ = 271,                   // NOTEQ
    LESS = 272,                    // LESS
    GREAT = 273,                   // GREAT
    LESSEQ = 274,                  // LESSEQ
    GREATEQ = 275,                 // GREATEQ
    ADD = 276,                     // ADD
    SUB = 277,                     // SUB
    MUL = 278,                     // MUL
    DIV = 279,                     // DIV
    MOD = 280,                     // MOD
    ASSIGN = 281,                  // ASSIGN
    RETURN = 282,                  // RETURN
    IF = 283,                      // IF
    ELSE = 284,                    // ELSE
    WHILE = 285,                   // WHILE
    BREAK = 286,                   // BREAK
    CONTINUE = 287,                // CONTINUE
    L_SMALL_BRACKET = 288,         // L_SMALL_BRACKET
    R_SMALL_BRACKET = 289,         // R_SMALL_BRACKET
    L_MIDDLE_BRACKET = 290,        // L_MIDDLE_BRACKET
    R_MIDDLE_BRACKET = 291,        // R_MIDDLE_BRACKET
    L_BIG_BRACKET = 292,           // L_BIG_BRACKET
    R_BIG_BRACKET = 293,           // R_BIG_BRACKET
    SEMICOLON = 294,               // SEMICOLON
    COMMA = 295,                   // COMMA
    END = 296                      // END
      };
      /// Backward compatibility alias (Bison 3.6).
      typedef token_kind_type yytokentype;
    };

    /// Token kind, as returned by yylex.
    typedef token::token_kind_type token_kind_type;

    /// Backward compatibility alias (Bison 3.6).
    typedef token_kind_type token_type;

    /// Symbol kinds.
    struct symbol_kind
    {
      enum symbol_kind_type
      {
        YYNTOKENS = 42, ///< Number of tokens.
        S_YYEMPTY = -2,
        S_YYEOF = 0,                             // "end of file"
        S_YYerror = 1,                           // error
        S_YYUNDEF = 2,                           // "invalid token"
        S_INTCONST = 3,                          // INTCONST
        S_FLOATCONST = 4,                        // FLOATCONST
        S_IDENT = 5,                             // IDENT
        S_CONST = 6,                             // CONST
        S_INT = 7,                               // INT
        S_FLOAT = 8,                             // FLOAT
        S_VOID = 9,                              // VOID
        S_TENSOR = 10,                           // TENSOR
        S_GEMM = 11,                             // GEMM
        S_OR = 12,                               // OR
        S_AND = 13,                              // AND
        S_NOT = 14,                              // NOT
        S_EQ = 15,                               // EQ
        S_NOTEQ = 16,                            // NOTEQ
        S_LESS = 17,                             // LESS
        S_GREAT = 18,                            // GREAT
        S_LESSEQ = 19,                           // LESSEQ
        S_GREATEQ = 20,                          // GREATEQ
        S_ADD = 21,                              // ADD
        S_SUB = 22,                              // SUB
        S_MUL = 23,                              // MUL
        S_DIV = 24,                              // DIV
        S_MOD = 25,                              // MOD
        S_ASSIGN = 26,                           // ASSIGN
        S_RETURN = 27,                           // RETURN
        S_IF = 28,                               // IF
        S_ELSE = 29,                             // ELSE
        S_WHILE = 30,                            // WHILE
        S_BREAK = 31,                            // BREAK
        S_CONTINUE = 32,                         // CONTINUE
        S_L_SMALL_BRACKET = 33,                  // L_SMALL_BRACKET
        S_R_SMALL_BRACKET = 34,                  // R_SMALL_BRACKET
        S_L_MIDDLE_BRACKET = 35,                 // L_MIDDLE_BRACKET
        S_R_MIDDLE_BRACKET = 36,                 // R_MIDDLE_BRACKET
        S_L_BIG_BRACKET = 37,                    // L_BIG_BRACKET
        S_R_BIG_BRACKET = 38,                    // R_BIG_BRACKET
        S_SEMICOLON = 39,                        // SEMICOLON
        S_COMMA = 40,                            // COMMA
        S_END = 41,                              // END
        S_YYACCEPT = 42,                         // $accept
        S_Start = 43,                            // Start
        S_CompUnit = 44,                         // CompUnit
        S_Decl = 45,                             // Decl
        S_ConstDecl = 46,                        // ConstDecl
        S_ConstDefList = 47,                     // ConstDefList
        S_ConstDef = 48,                         // ConstDef
        S_ConstExpList = 49,                     // ConstExpList
        S_ConstInitVal = 50,                     // ConstInitVal
        S_ConstInitValList = 51,                 // ConstInitValList
        S_VarDecl = 52,                          // VarDecl
        S_VarDefList = 53,                       // VarDefList
        S_VarDef = 54,                           // VarDef
        S_InitVal = 55,                          // InitVal
        S_InitValList = 56,                      // InitValList
        S_FuncDef = 57,                          // FuncDef
        S_FuncFParamList = 58,                   // FuncFParamList
        S_FuncFParam = 59,                       // FuncFParam
        S_Block = 60,                            // Block
        S_BlockItemList = 61,                    // BlockItemList
        S_BlockItem = 62,                        // BlockItem
        S_Stmt = 63,                             // Stmt
        S_LVal = 64,                             // LVal
        S_ExpList = 65,                          // ExpList
        S_PrimaryExp = 66,                       // PrimaryExp
        S_UnaryExp = 67,                         // UnaryExp
        S_FuncRParamList = 68,                   // FuncRParamList
        S_MulExp = 69,                           // MulExp
        S_AddExp = 70,                           // AddExp
        S_RelExp = 71,                           // RelExp
        S_EqExp = 72,                            // EqExp
        S_LAndExp = 73,                          // LAndExp
        S_LOrExp = 74,                           // LOrExp
        S_Type = 75                              // Type
      };
    };

    /// (Internal) symbol kind.
    typedef symbol_kind::symbol_kind_type symbol_kind_type;

    /// The number of tokens.
    static const symbol_kind_type YYNTOKENS = symbol_kind::YYNTOKENS;

    /// A complete symbol.
    ///
    /// Expects its Base type to provide access to the symbol kind
    /// via kind ().
    ///
    /// Provide access to semantic value.
    template <typename Base>
    struct basic_symbol : Base
    {
      /// Alias to Base.
      typedef Base super_type;

      /// Default constructor.
      basic_symbol () YY_NOEXCEPT
        : value ()
      {}

#if 201103L <= YY_CPLUSPLUS
      /// Move constructor.
      basic_symbol (basic_symbol&& that)
        : Base (std::move (that))
        , value ()
      {
        switch (this->kind ())
    {
      case symbol_kind::S_Type: // Type
        value.move< ASTType > (std::move (that.value));
        break;

      case symbol_kind::S_AddExp: // AddExp
        value.move< AddExp* > (std::move (that.value));
        break;

      case symbol_kind::S_ConstExpList: // ConstExpList
      case symbol_kind::S_ExpList: // ExpList
        value.move< ArrayList* > (std::move (that.value));
        break;

      case symbol_kind::S_Decl: // Decl
      case symbol_kind::S_BlockItem: // BlockItem
      case symbol_kind::S_Stmt: // Stmt
      case symbol_kind::S_PrimaryExp: // PrimaryExp
        value.move< BaseAST* > (std::move (that.value));
        break;

      case symbol_kind::S_Block: // Block
        value.move< Block* > (std::move (that.value));
        break;

      case symbol_kind::S_BlockItemList: // BlockItemList
        value.move< BlockItemList* > (std::move (that.value));
        break;

      case symbol_kind::S_CompUnit: // CompUnit
        value.move< CompUnit* > (std::move (that.value));
        break;

      case symbol_kind::S_ConstDecl: // ConstDecl
        value.move< ConstDecl* > (std::move (that.value));
        break;

      case symbol_kind::S_ConstDef: // ConstDef
        value.move< ConstDef* > (std::move (that.value));
        break;

      case symbol_kind::S_ConstDefList: // ConstDefList
        value.move< ConstDefList* > (std::move (that.value));
        break;

      case symbol_kind::S_EqExp: // EqExp
        value.move< EqExp* > (std::move (that.value));
        break;

      case symbol_kind::S_FuncDef: // FuncDef
        value.move< FuncDef* > (std::move (that.value));
        break;

      case symbol_kind::S_FuncFParam: // FuncFParam
        value.move< FuncParam* > (std::move (that.value));
        break;

      case symbol_kind::S_FuncFParamList: // FuncFParamList
        value.move< FuncParamList* > (std::move (that.value));
        break;

      case symbol_kind::S_FuncRParamList: // FuncRParamList
        value.move< FuncRParamList* > (std::move (that.value));
        break;

      case symbol_kind::S_ConstInitVal: // ConstInitVal
      case symbol_kind::S_InitVal: // InitVal
        value.move< InitVal* > (std::move (that.value));
        break;

      case symbol_kind::S_ConstInitValList: // ConstInitValList
      case symbol_kind::S_InitValList: // InitValList
        value.move< InitValList* > (std::move (that.value));
        break;

      case symbol_kind::S_LAndExp: // LAndExp
        value.move< LAndExp* > (std::move (that.value));
        break;

      case symbol_kind::S_LOrExp: // LOrExp
        value.move< LOrExp* > (std::move (that.value));
        break;

      case symbol_kind::S_LVal: // LVal
        value.move< LVal* > (std::move (that.value));
        break;

      case symbol_kind::S_IDENT: // IDENT
        value.move< LocatedIdentifier > (std::move (that.value));
        break;

      case symbol_kind::S_MulExp: // MulExp
        value.move< MulExp* > (std::move (that.value));
        break;

      case symbol_kind::S_RelExp: // RelExp
        value.move< RelExp* > (std::move (that.value));
        break;

      case symbol_kind::S_UnaryExp: // UnaryExp
        value.move< UnaryExp* > (std::move (that.value));
        break;

      case symbol_kind::S_VarDecl: // VarDecl
        value.move< VarDecl* > (std::move (that.value));
        break;

      case symbol_kind::S_VarDef: // VarDef
        value.move< VarDef* > (std::move (that.value));
        break;

      case symbol_kind::S_VarDefList: // VarDefList
        value.move< VarDefList* > (std::move (that.value));
        break;

      case symbol_kind::S_FLOATCONST: // FLOATCONST
        value.move< float > (std::move (that.value));
        break;

      case symbol_kind::S_INTCONST: // INTCONST
        value.move< int > (std::move (that.value));
        break;

      default:
        break;
    }

      }
#endif

      /// Copy constructor.
      basic_symbol (const basic_symbol& that);

      /// Constructors for typed symbols.
#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t)
        : Base (t)
      {}
#else
      basic_symbol (typename Base::kind_type t)
        : Base (t)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, ASTType&& v)
        : Base (t)
        , value (std::move (v))
      {}
#else
      basic_symbol (typename Base::kind_type t, const ASTType& v)
        : Base (t)
        , value (v)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, AddExp*&& v)
        : Base (t)
        , value (std::move (v))
      {}
#else
      basic_symbol (typename Base::kind_type t, const AddExp*& v)
        : Base (t)
        , value (v)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, ArrayList*&& v)
        : Base (t)
        , value (std::move (v))
      {}
#else
      basic_symbol (typename Base::kind_type t, const ArrayList*& v)
        : Base (t)
        , value (v)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, BaseAST*&& v)
        : Base (t)
        , value (std::move (v))
      {}
#else
      basic_symbol (typename Base::kind_type t, const BaseAST*& v)
        : Base (t)
        , value (v)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, Block*&& v)
        : Base (t)
        , value (std::move (v))
      {}
#else
      basic_symbol (typename Base::kind_type t, const Block*& v)
        : Base (t)
        , value (v)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, BlockItemList*&& v)
        : Base (t)
        , value (std::move (v))
      {}
#else
      basic_symbol (typename Base::kind_type t, const BlockItemList*& v)
        : Base (t)
        , value (v)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, CompUnit*&& v)
        : Base (t)
        , value (std::move (v))
      {}
#else
      basic_symbol (typename Base::kind_type t, const CompUnit*& v)
        : Base (t)
        , value (v)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, ConstDecl*&& v)
        : Base (t)
        , value (std::move (v))
      {}
#else
      basic_symbol (typename Base::kind_type t, const ConstDecl*& v)
        : Base (t)
        , value (v)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, ConstDef*&& v)
        : Base (t)
        , value (std::move (v))
      {}
#else
      basic_symbol (typename Base::kind_type t, const ConstDef*& v)
        : Base (t)
        , value (v)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, ConstDefList*&& v)
        : Base (t)
        , value (std::move (v))
      {}
#else
      basic_symbol (typename Base::kind_type t, const ConstDefList*& v)
        : Base (t)
        , value (v)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, EqExp*&& v)
        : Base (t)
        , value (std::move (v))
      {}
#else
      basic_symbol (typename Base::kind_type t, const EqExp*& v)
        : Base (t)
        , value (v)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, FuncDef*&& v)
        : Base (t)
        , value (std::move (v))
      {}
#else
      basic_symbol (typename Base::kind_type t, const FuncDef*& v)
        : Base (t)
        , value (v)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, FuncParam*&& v)
        : Base (t)
        , value (std::move (v))
      {}
#else
      basic_symbol (typename Base::kind_type t, const FuncParam*& v)
        : Base (t)
        , value (v)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, FuncParamList*&& v)
        : Base (t)
        , value (std::move (v))
      {}
#else
      basic_symbol (typename Base::kind_type t, const FuncParamList*& v)
        : Base (t)
        , value (v)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, FuncRParamList*&& v)
        : Base (t)
        , value (std::move (v))
      {}
#else
      basic_symbol (typename Base::kind_type t, const FuncRParamList*& v)
        : Base (t)
        , value (v)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, InitVal*&& v)
        : Base (t)
        , value (std::move (v))
      {}
#else
      basic_symbol (typename Base::kind_type t, const InitVal*& v)
        : Base (t)
        , value (v)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, InitValList*&& v)
        : Base (t)
        , value (std::move (v))
      {}
#else
      basic_symbol (typename Base::kind_type t, const InitValList*& v)
        : Base (t)
        , value (v)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, LAndExp*&& v)
        : Base (t)
        , value (std::move (v))
      {}
#else
      basic_symbol (typename Base::kind_type t, const LAndExp*& v)
        : Base (t)
        , value (v)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, LOrExp*&& v)
        : Base (t)
        , value (std::move (v))
      {}
#else
      basic_symbol (typename Base::kind_type t, const LOrExp*& v)
        : Base (t)
        , value (v)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, LVal*&& v)
        : Base (t)
        , value (std::move (v))
      {}
#else
      basic_symbol (typename Base::kind_type t, const LVal*& v)
        : Base (t)
        , value (v)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, LocatedIdentifier&& v)
        : Base (t)
        , value (std::move (v))
      {}
#else
      basic_symbol (typename Base::kind_type t, const LocatedIdentifier& v)
        : Base (t)
        , value (v)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, MulExp*&& v)
        : Base (t)
        , value (std::move (v))
      {}
#else
      basic_symbol (typename Base::kind_type t, const MulExp*& v)
        : Base (t)
        , value (v)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, RelExp*&& v)
        : Base (t)
        , value (std::move (v))
      {}
#else
      basic_symbol (typename Base::kind_type t, const RelExp*& v)
        : Base (t)
        , value (v)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, UnaryExp*&& v)
        : Base (t)
        , value (std::move (v))
      {}
#else
      basic_symbol (typename Base::kind_type t, const UnaryExp*& v)
        : Base (t)
        , value (v)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, VarDecl*&& v)
        : Base (t)
        , value (std::move (v))
      {}
#else
      basic_symbol (typename Base::kind_type t, const VarDecl*& v)
        : Base (t)
        , value (v)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, VarDef*&& v)
        : Base (t)
        , value (std::move (v))
      {}
#else
      basic_symbol (typename Base::kind_type t, const VarDef*& v)
        : Base (t)
        , value (v)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, VarDefList*&& v)
        : Base (t)
        , value (std::move (v))
      {}
#else
      basic_symbol (typename Base::kind_type t, const VarDefList*& v)
        : Base (t)
        , value (v)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, float&& v)
        : Base (t)
        , value (std::move (v))
      {}
#else
      basic_symbol (typename Base::kind_type t, const float& v)
        : Base (t)
        , value (v)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, int&& v)
        : Base (t)
        , value (std::move (v))
      {}
#else
      basic_symbol (typename Base::kind_type t, const int& v)
        : Base (t)
        , value (v)
      {}
#endif

      /// Destroy the symbol.
      ~basic_symbol ()
      {
        clear ();
      }



      /// Destroy contents, and record that is empty.
      void clear () YY_NOEXCEPT
      {
        // User destructor.
        symbol_kind_type yykind = this->kind ();
        basic_symbol<Base>& yysym = *this;
        (void) yysym;
        switch (yykind)
        {
       default:
          break;
        }

        // Value type destructor.
switch (yykind)
    {
      case symbol_kind::S_Type: // Type
        value.template destroy< ASTType > ();
        break;

      case symbol_kind::S_AddExp: // AddExp
        value.template destroy< AddExp* > ();
        break;

      case symbol_kind::S_ConstExpList: // ConstExpList
      case symbol_kind::S_ExpList: // ExpList
        value.template destroy< ArrayList* > ();
        break;

      case symbol_kind::S_Decl: // Decl
      case symbol_kind::S_BlockItem: // BlockItem
      case symbol_kind::S_Stmt: // Stmt
      case symbol_kind::S_PrimaryExp: // PrimaryExp
        value.template destroy< BaseAST* > ();
        break;

      case symbol_kind::S_Block: // Block
        value.template destroy< Block* > ();
        break;

      case symbol_kind::S_BlockItemList: // BlockItemList
        value.template destroy< BlockItemList* > ();
        break;

      case symbol_kind::S_CompUnit: // CompUnit
        value.template destroy< CompUnit* > ();
        break;

      case symbol_kind::S_ConstDecl: // ConstDecl
        value.template destroy< ConstDecl* > ();
        break;

      case symbol_kind::S_ConstDef: // ConstDef
        value.template destroy< ConstDef* > ();
        break;

      case symbol_kind::S_ConstDefList: // ConstDefList
        value.template destroy< ConstDefList* > ();
        break;

      case symbol_kind::S_EqExp: // EqExp
        value.template destroy< EqExp* > ();
        break;

      case symbol_kind::S_FuncDef: // FuncDef
        value.template destroy< FuncDef* > ();
        break;

      case symbol_kind::S_FuncFParam: // FuncFParam
        value.template destroy< FuncParam* > ();
        break;

      case symbol_kind::S_FuncFParamList: // FuncFParamList
        value.template destroy< FuncParamList* > ();
        break;

      case symbol_kind::S_FuncRParamList: // FuncRParamList
        value.template destroy< FuncRParamList* > ();
        break;

      case symbol_kind::S_ConstInitVal: // ConstInitVal
      case symbol_kind::S_InitVal: // InitVal
        value.template destroy< InitVal* > ();
        break;

      case symbol_kind::S_ConstInitValList: // ConstInitValList
      case symbol_kind::S_InitValList: // InitValList
        value.template destroy< InitValList* > ();
        break;

      case symbol_kind::S_LAndExp: // LAndExp
        value.template destroy< LAndExp* > ();
        break;

      case symbol_kind::S_LOrExp: // LOrExp
        value.template destroy< LOrExp* > ();
        break;

      case symbol_kind::S_LVal: // LVal
        value.template destroy< LVal* > ();
        break;

      case symbol_kind::S_IDENT: // IDENT
        value.template destroy< LocatedIdentifier > ();
        break;

      case symbol_kind::S_MulExp: // MulExp
        value.template destroy< MulExp* > ();
        break;

      case symbol_kind::S_RelExp: // RelExp
        value.template destroy< RelExp* > ();
        break;

      case symbol_kind::S_UnaryExp: // UnaryExp
        value.template destroy< UnaryExp* > ();
        break;

      case symbol_kind::S_VarDecl: // VarDecl
        value.template destroy< VarDecl* > ();
        break;

      case symbol_kind::S_VarDef: // VarDef
        value.template destroy< VarDef* > ();
        break;

      case symbol_kind::S_VarDefList: // VarDefList
        value.template destroy< VarDefList* > ();
        break;

      case symbol_kind::S_FLOATCONST: // FLOATCONST
        value.template destroy< float > ();
        break;

      case symbol_kind::S_INTCONST: // INTCONST
        value.template destroy< int > ();
        break;

      default:
        break;
    }

        Base::clear ();
      }

#if YYDEBUG || 0
      /// The user-facing name of this symbol.
      const char *name () const YY_NOEXCEPT
      {
        return parser::symbol_name (this->kind ());
      }
#endif // #if YYDEBUG || 0


      /// Backward compatibility (Bison 3.6).
      symbol_kind_type type_get () const YY_NOEXCEPT;

      /// Whether empty.
      bool empty () const YY_NOEXCEPT;

      /// Destructive move, \a s is emptied into this.
      void move (basic_symbol& s);

      /// The semantic value.
      value_type value;

    private:
#if YY_CPLUSPLUS < 201103L
      /// Assignment operator.
      basic_symbol& operator= (const basic_symbol& that);
#endif
    };

    /// Type access provider for token (enum) based symbols.
    struct by_kind
    {
      /// The symbol kind as needed by the constructor.
      typedef token_kind_type kind_type;

      /// Default constructor.
      by_kind () YY_NOEXCEPT;

#if 201103L <= YY_CPLUSPLUS
      /// Move constructor.
      by_kind (by_kind&& that) YY_NOEXCEPT;
#endif

      /// Copy constructor.
      by_kind (const by_kind& that) YY_NOEXCEPT;

      /// Constructor from (external) token numbers.
      by_kind (kind_type t) YY_NOEXCEPT;



      /// Record that this symbol is empty.
      void clear () YY_NOEXCEPT;

      /// Steal the symbol kind from \a that.
      void move (by_kind& that);

      /// The (internal) type number (corresponding to \a type).
      /// \a empty when empty.
      symbol_kind_type kind () const YY_NOEXCEPT;

      /// Backward compatibility (Bison 3.6).
      symbol_kind_type type_get () const YY_NOEXCEPT;

      /// The symbol kind.
      /// \a S_YYEMPTY when empty.
      symbol_kind_type kind_;
    };

    /// Backward compatibility for a private implementation detail (Bison 3.6).
    typedef by_kind by_type;

    /// "External" symbols: returned by the scanner.
    struct symbol_type : basic_symbol<by_kind>
    {
      /// Superclass.
      typedef basic_symbol<by_kind> super_type;

      /// Empty symbol.
      symbol_type () YY_NOEXCEPT {}

      /// Constructor for valueless symbols, and symbols from each type.
#if 201103L <= YY_CPLUSPLUS
      symbol_type (int tok)
        : super_type (token_kind_type (tok))
#else
      symbol_type (int tok)
        : super_type (token_kind_type (tok))
#endif
      {}
#if 201103L <= YY_CPLUSPLUS
      symbol_type (int tok, LocatedIdentifier v)
        : super_type (token_kind_type (tok), std::move (v))
#else
      symbol_type (int tok, const LocatedIdentifier& v)
        : super_type (token_kind_type (tok), v)
#endif
      {}
#if 201103L <= YY_CPLUSPLUS
      symbol_type (int tok, float v)
        : super_type (token_kind_type (tok), std::move (v))
#else
      symbol_type (int tok, const float& v)
        : super_type (token_kind_type (tok), v)
#endif
      {}
#if 201103L <= YY_CPLUSPLUS
      symbol_type (int tok, int v)
        : super_type (token_kind_type (tok), std::move (v))
#else
      symbol_type (int tok, const int& v)
        : super_type (token_kind_type (tok), v)
#endif
      {}
    };

    /// Build a parser object.
    parser ();
    virtual ~parser ();

#if 201103L <= YY_CPLUSPLUS
    /// Non copyable.
    parser (const parser&) = delete;
    /// Non copyable.
    parser& operator= (const parser&) = delete;
#endif

    /// Parse.  An alias for parse ().
    /// \returns  0 iff parsing succeeded.
    int operator() ();

    /// Parse.
    /// \returns  0 iff parsing succeeded.
    virtual int parse ();

#if YYDEBUG
    /// The current debugging stream.
    std::ostream& debug_stream () const YY_ATTRIBUTE_PURE;
    /// Set the current debugging stream.
    void set_debug_stream (std::ostream &);

    /// Type for debugging levels.
    typedef int debug_level_type;
    /// The current debugging level.
    debug_level_type debug_level () const YY_ATTRIBUTE_PURE;
    /// Set the current debugging level.
    void set_debug_level (debug_level_type l);
#endif

    /// Report a syntax error.
    /// \param msg    a description of the syntax error.
    virtual void error (const std::string& msg);

    /// Report a syntax error.
    void error (const syntax_error& err);

#if YYDEBUG || 0
    /// The user-facing name of the symbol whose (internal) number is
    /// YYSYMBOL.  No bounds checking.
    static const char *symbol_name (symbol_kind_type yysymbol);
#endif // #if YYDEBUG || 0


    // Implementation of make_symbol for each token kind.
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_YYEOF ()
      {
        return symbol_type (token::YYEOF);
      }
#else
      static
      symbol_type
      make_YYEOF ()
      {
        return symbol_type (token::YYEOF);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_YYerror ()
      {
        return symbol_type (token::YYerror);
      }
#else
      static
      symbol_type
      make_YYerror ()
      {
        return symbol_type (token::YYerror);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_YYUNDEF ()
      {
        return symbol_type (token::YYUNDEF);
      }
#else
      static
      symbol_type
      make_YYUNDEF ()
      {
        return symbol_type (token::YYUNDEF);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_INTCONST (int v)
      {
        return symbol_type (token::INTCONST, std::move (v));
      }
#else
      static
      symbol_type
      make_INTCONST (const int& v)
      {
        return symbol_type (token::INTCONST, v);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_FLOATCONST (float v)
      {
        return symbol_type (token::FLOATCONST, std::move (v));
      }
#else
      static
      symbol_type
      make_FLOATCONST (const float& v)
      {
        return symbol_type (token::FLOATCONST, v);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_IDENT (LocatedIdentifier v)
      {
        return symbol_type (token::IDENT, std::move (v));
      }
#else
      static
      symbol_type
      make_IDENT (const LocatedIdentifier& v)
      {
        return symbol_type (token::IDENT, v);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_CONST ()
      {
        return symbol_type (token::CONST);
      }
#else
      static
      symbol_type
      make_CONST ()
      {
        return symbol_type (token::CONST);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_INT ()
      {
        return symbol_type (token::INT);
      }
#else
      static
      symbol_type
      make_INT ()
      {
        return symbol_type (token::INT);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_FLOAT ()
      {
        return symbol_type (token::FLOAT);
      }
#else
      static
      symbol_type
      make_FLOAT ()
      {
        return symbol_type (token::FLOAT);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_VOID ()
      {
        return symbol_type (token::VOID);
      }
#else
      static
      symbol_type
      make_VOID ()
      {
        return symbol_type (token::VOID);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_TENSOR ()
      {
        return symbol_type (token::TENSOR);
      }
#else
      static
      symbol_type
      make_TENSOR ()
      {
        return symbol_type (token::TENSOR);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_GEMM ()
      {
        return symbol_type (token::GEMM);
      }
#else
      static
      symbol_type
      make_GEMM ()
      {
        return symbol_type (token::GEMM);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_OR ()
      {
        return symbol_type (token::OR);
      }
#else
      static
      symbol_type
      make_OR ()
      {
        return symbol_type (token::OR);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_AND ()
      {
        return symbol_type (token::AND);
      }
#else
      static
      symbol_type
      make_AND ()
      {
        return symbol_type (token::AND);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_NOT ()
      {
        return symbol_type (token::NOT);
      }
#else
      static
      symbol_type
      make_NOT ()
      {
        return symbol_type (token::NOT);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_EQ ()
      {
        return symbol_type (token::EQ);
      }
#else
      static
      symbol_type
      make_EQ ()
      {
        return symbol_type (token::EQ);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_NOTEQ ()
      {
        return symbol_type (token::NOTEQ);
      }
#else
      static
      symbol_type
      make_NOTEQ ()
      {
        return symbol_type (token::NOTEQ);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_LESS ()
      {
        return symbol_type (token::LESS);
      }
#else
      static
      symbol_type
      make_LESS ()
      {
        return symbol_type (token::LESS);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_GREAT ()
      {
        return symbol_type (token::GREAT);
      }
#else
      static
      symbol_type
      make_GREAT ()
      {
        return symbol_type (token::GREAT);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_LESSEQ ()
      {
        return symbol_type (token::LESSEQ);
      }
#else
      static
      symbol_type
      make_LESSEQ ()
      {
        return symbol_type (token::LESSEQ);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_GREATEQ ()
      {
        return symbol_type (token::GREATEQ);
      }
#else
      static
      symbol_type
      make_GREATEQ ()
      {
        return symbol_type (token::GREATEQ);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_ADD ()
      {
        return symbol_type (token::ADD);
      }
#else
      static
      symbol_type
      make_ADD ()
      {
        return symbol_type (token::ADD);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_SUB ()
      {
        return symbol_type (token::SUB);
      }
#else
      static
      symbol_type
      make_SUB ()
      {
        return symbol_type (token::SUB);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_MUL ()
      {
        return symbol_type (token::MUL);
      }
#else
      static
      symbol_type
      make_MUL ()
      {
        return symbol_type (token::MUL);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_DIV ()
      {
        return symbol_type (token::DIV);
      }
#else
      static
      symbol_type
      make_DIV ()
      {
        return symbol_type (token::DIV);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_MOD ()
      {
        return symbol_type (token::MOD);
      }
#else
      static
      symbol_type
      make_MOD ()
      {
        return symbol_type (token::MOD);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_ASSIGN ()
      {
        return symbol_type (token::ASSIGN);
      }
#else
      static
      symbol_type
      make_ASSIGN ()
      {
        return symbol_type (token::ASSIGN);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_RETURN ()
      {
        return symbol_type (token::RETURN);
      }
#else
      static
      symbol_type
      make_RETURN ()
      {
        return symbol_type (token::RETURN);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_IF ()
      {
        return symbol_type (token::IF);
      }
#else
      static
      symbol_type
      make_IF ()
      {
        return symbol_type (token::IF);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_ELSE ()
      {
        return symbol_type (token::ELSE);
      }
#else
      static
      symbol_type
      make_ELSE ()
      {
        return symbol_type (token::ELSE);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_WHILE ()
      {
        return symbol_type (token::WHILE);
      }
#else
      static
      symbol_type
      make_WHILE ()
      {
        return symbol_type (token::WHILE);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_BREAK ()
      {
        return symbol_type (token::BREAK);
      }
#else
      static
      symbol_type
      make_BREAK ()
      {
        return symbol_type (token::BREAK);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_CONTINUE ()
      {
        return symbol_type (token::CONTINUE);
      }
#else
      static
      symbol_type
      make_CONTINUE ()
      {
        return symbol_type (token::CONTINUE);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_L_SMALL_BRACKET ()
      {
        return symbol_type (token::L_SMALL_BRACKET);
      }
#else
      static
      symbol_type
      make_L_SMALL_BRACKET ()
      {
        return symbol_type (token::L_SMALL_BRACKET);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_R_SMALL_BRACKET ()
      {
        return symbol_type (token::R_SMALL_BRACKET);
      }
#else
      static
      symbol_type
      make_R_SMALL_BRACKET ()
      {
        return symbol_type (token::R_SMALL_BRACKET);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_L_MIDDLE_BRACKET ()
      {
        return symbol_type (token::L_MIDDLE_BRACKET);
      }
#else
      static
      symbol_type
      make_L_MIDDLE_BRACKET ()
      {
        return symbol_type (token::L_MIDDLE_BRACKET);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_R_MIDDLE_BRACKET ()
      {
        return symbol_type (token::R_MIDDLE_BRACKET);
      }
#else
      static
      symbol_type
      make_R_MIDDLE_BRACKET ()
      {
        return symbol_type (token::R_MIDDLE_BRACKET);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_L_BIG_BRACKET ()
      {
        return symbol_type (token::L_BIG_BRACKET);
      }
#else
      static
      symbol_type
      make_L_BIG_BRACKET ()
      {
        return symbol_type (token::L_BIG_BRACKET);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_R_BIG_BRACKET ()
      {
        return symbol_type (token::R_BIG_BRACKET);
      }
#else
      static
      symbol_type
      make_R_BIG_BRACKET ()
      {
        return symbol_type (token::R_BIG_BRACKET);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_SEMICOLON ()
      {
        return symbol_type (token::SEMICOLON);
      }
#else
      static
      symbol_type
      make_SEMICOLON ()
      {
        return symbol_type (token::SEMICOLON);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_COMMA ()
      {
        return symbol_type (token::COMMA);
      }
#else
      static
      symbol_type
      make_COMMA ()
      {
        return symbol_type (token::COMMA);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_END ()
      {
        return symbol_type (token::END);
      }
#else
      static
      symbol_type
      make_END ()
      {
        return symbol_type (token::END);
      }
#endif


  private:
#if YY_CPLUSPLUS < 201103L
    /// Non copyable.
    parser (const parser&);
    /// Non copyable.
    parser& operator= (const parser&);
#endif


    /// Stored state numbers (used for stacks).
    typedef unsigned char state_type;

    /// Compute post-reduction state.
    /// \param yystate   the current state
    /// \param yysym     the nonterminal to push on the stack
    static state_type yy_lr_goto_state_ (state_type yystate, int yysym);

    /// Whether the given \c yypact_ value indicates a defaulted state.
    /// \param yyvalue   the value to check
    static bool yy_pact_value_is_default_ (int yyvalue) YY_NOEXCEPT;

    /// Whether the given \c yytable_ value indicates a syntax error.
    /// \param yyvalue   the value to check
    static bool yy_table_value_is_error_ (int yyvalue) YY_NOEXCEPT;

    static const signed char yypact_ninf_;
    static const signed char yytable_ninf_;

    /// Convert a scanner token kind \a t to a symbol kind.
    /// In theory \a t should be a token_kind_type, but character literals
    /// are valid, yet not members of the token_kind_type enum.
    static symbol_kind_type yytranslate_ (int t) YY_NOEXCEPT;

#if YYDEBUG || 0
    /// For a symbol, its name in clear.
    static const char* const yytname_[];
#endif // #if YYDEBUG || 0


    // Tables.
    // YYPACT[STATE-NUM] -- Index in YYTABLE of the portion describing
    // STATE-NUM.
    static const short yypact_[];

    // YYDEFACT[STATE-NUM] -- Default reduction number in state STATE-NUM.
    // Performed when YYTABLE does not specify something else to do.  Zero
    // means the default is an error.
    static const signed char yydefact_[];

    // YYPGOTO[NTERM-NUM].
    static const short yypgoto_[];

    // YYDEFGOTO[NTERM-NUM].
    static const unsigned char yydefgoto_[];

    // YYTABLE[YYPACT[STATE-NUM]] -- What to do in state STATE-NUM.  If
    // positive, shift that token.  If negative, reduce the rule whose
    // number is the opposite.  If YYTABLE_NINF, syntax error.
    static const unsigned char yytable_[];

    static const short yycheck_[];

    // YYSTOS[STATE-NUM] -- The symbol kind of the accessing symbol of
    // state STATE-NUM.
    static const signed char yystos_[];

    // YYR1[RULE-NUM] -- Symbol kind of the left-hand side of rule RULE-NUM.
    static const signed char yyr1_[];

    // YYR2[RULE-NUM] -- Number of symbols on the right-hand side of rule RULE-NUM.
    static const signed char yyr2_[];


#if YYDEBUG
    // YYRLINE[YYN] -- Source line where rule number YYN was defined.
    static const short yyrline_[];
    /// Report on the debug stream that the rule \a r is going to be reduced.
    virtual void yy_reduce_print_ (int r) const;
    /// Print the state stack on the debug stream.
    virtual void yy_stack_print_ () const;

    /// Debugging level.
    int yydebug_;
    /// Debug stream.
    std::ostream* yycdebug_;

    /// \brief Display a symbol kind, value and location.
    /// \param yyo    The output stream.
    /// \param yysym  The symbol.
    template <typename Base>
    void yy_print_ (std::ostream& yyo, const basic_symbol<Base>& yysym) const;
#endif

    /// \brief Reclaim the memory associated to a symbol.
    /// \param yymsg     Why this token is reclaimed.
    ///                  If null, print nothing.
    /// \param yysym     The symbol.
    template <typename Base>
    void yy_destroy_ (const char* yymsg, basic_symbol<Base>& yysym) const;

  private:
    /// Type access provider for state based symbols.
    struct by_state
    {
      /// Default constructor.
      by_state () YY_NOEXCEPT;

      /// The symbol kind as needed by the constructor.
      typedef state_type kind_type;

      /// Constructor.
      by_state (kind_type s) YY_NOEXCEPT;

      /// Copy constructor.
      by_state (const by_state& that) YY_NOEXCEPT;

      /// Record that this symbol is empty.
      void clear () YY_NOEXCEPT;

      /// Steal the symbol kind from \a that.
      void move (by_state& that);

      /// The symbol kind (corresponding to \a state).
      /// \a symbol_kind::S_YYEMPTY when empty.
      symbol_kind_type kind () const YY_NOEXCEPT;

      /// The state number used to denote an empty symbol.
      /// We use the initial state, as it does not have a value.
      enum { empty_state = 0 };

      /// The state.
      /// \a empty when empty.
      state_type state;
    };

    /// "Internal" symbol: element of the stack.
    struct stack_symbol_type : basic_symbol<by_state>
    {
      /// Superclass.
      typedef basic_symbol<by_state> super_type;
      /// Construct an empty symbol.
      stack_symbol_type ();
      /// Move or copy construction.
      stack_symbol_type (YY_RVREF (stack_symbol_type) that);
      /// Steal the contents from \a sym to build this.
      stack_symbol_type (state_type s, YY_MOVE_REF (symbol_type) sym);
#if YY_CPLUSPLUS < 201103L
      /// Assignment, needed by push_back by some old implementations.
      /// Moves the contents of that.
      stack_symbol_type& operator= (stack_symbol_type& that);

      /// Assignment, needed by push_back by other implementations.
      /// Needed by some other old implementations.
      stack_symbol_type& operator= (const stack_symbol_type& that);
#endif
    };

    /// A stack with random access from its top.
    template <typename T, typename S = std::vector<T> >
    class stack
    {
    public:
      // Hide our reversed order.
      typedef typename S::iterator iterator;
      typedef typename S::const_iterator const_iterator;
      typedef typename S::size_type size_type;
      typedef typename std::ptrdiff_t index_type;

      stack (size_type n = 200) YY_NOEXCEPT
        : seq_ (n)
      {}

#if 201103L <= YY_CPLUSPLUS
      /// Non copyable.
      stack (const stack&) = delete;
      /// Non copyable.
      stack& operator= (const stack&) = delete;
#endif

      /// Random access.
      ///
      /// Index 0 returns the topmost element.
      const T&
      operator[] (index_type i) const
      {
        return seq_[size_type (size () - 1 - i)];
      }

      /// Random access.
      ///
      /// Index 0 returns the topmost element.
      T&
      operator[] (index_type i)
      {
        return seq_[size_type (size () - 1 - i)];
      }

      /// Steal the contents of \a t.
      ///
      /// Close to move-semantics.
      void
      push (YY_MOVE_REF (T) t)
      {
        seq_.push_back (T ());
        operator[] (0).move (t);
      }

      /// Pop elements from the stack.
      void
      pop (std::ptrdiff_t n = 1) YY_NOEXCEPT
      {
        for (; 0 < n; --n)
          seq_.pop_back ();
      }

      /// Pop all elements from the stack.
      void
      clear () YY_NOEXCEPT
      {
        seq_.clear ();
      }

      /// Number of elements on the stack.
      index_type
      size () const YY_NOEXCEPT
      {
        return index_type (seq_.size ());
      }

      /// Iterator on top of the stack (going downwards).
      const_iterator
      begin () const YY_NOEXCEPT
      {
        return seq_.begin ();
      }

      /// Bottom of the stack.
      const_iterator
      end () const YY_NOEXCEPT
      {
        return seq_.end ();
      }

      /// Present a slice of the top of a stack.
      class slice
      {
      public:
        slice (const stack& stack, index_type range) YY_NOEXCEPT
          : stack_ (stack)
          , range_ (range)
        {}

        const T&
        operator[] (index_type i) const
        {
          return stack_[range_ - i];
        }

      private:
        const stack& stack_;
        index_type range_;
      };

    private:
#if YY_CPLUSPLUS < 201103L
      /// Non copyable.
      stack (const stack&);
      /// Non copyable.
      stack& operator= (const stack&);
#endif
      /// The wrapped container.
      S seq_;
    };


    /// Stack type.
    typedef stack<stack_symbol_type> stack_type;

    /// The stack.
    stack_type yystack_;

    /// Push a new state on the stack.
    /// \param m    a debug message to display
    ///             if null, no trace is output.
    /// \param sym  the symbol
    /// \warning the contents of \a s.value is stolen.
    void yypush_ (const char* m, YY_MOVE_REF (stack_symbol_type) sym);

    /// Push a new look ahead token on the state on the stack.
    /// \param m    a debug message to display
    ///             if null, no trace is output.
    /// \param s    the state
    /// \param sym  the symbol (for its value and location).
    /// \warning the contents of \a sym.value is stolen.
    void yypush_ (const char* m, state_type s, YY_MOVE_REF (symbol_type) sym);

    /// Pop \a n symbols from the stack.
    void yypop_ (int n = 1) YY_NOEXCEPT;

    /// Constants.
    enum
    {
      yylast_ = 294,     ///< Last index in yytable_.
      yynnts_ = 34,  ///< Number of nonterminal symbols.
      yyfinal_ = 16 ///< Termination state number.
    };



  };

  inline
  parser::symbol_kind_type
  parser::yytranslate_ (int t) YY_NOEXCEPT
  {
    // YYTRANSLATE[TOKEN-NUM] -- Symbol number corresponding to
    // TOKEN-NUM as returned by yylex.
    static
    const signed char
    translate_table[] =
    {
       0,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     1,     2,     3,     4,
       5,     6,     7,     8,     9,    10,    11,    12,    13,    14,
      15,    16,    17,    18,    19,    20,    21,    22,    23,    24,
      25,    26,    27,    28,    29,    30,    31,    32,    33,    34,
      35,    36,    37,    38,    39,    40,    41
    };
    // Last valid token kind.
    const int code_max = 296;

    if (t <= 0)
      return symbol_kind::S_YYEOF;
    else if (t <= code_max)
      return static_cast <symbol_kind_type> (translate_table[t]);
    else
      return symbol_kind::S_YYUNDEF;
  }

  // basic_symbol.
  template <typename Base>
  parser::basic_symbol<Base>::basic_symbol (const basic_symbol& that)
    : Base (that)
    , value ()
  {
    switch (this->kind ())
    {
      case symbol_kind::S_Type: // Type
        value.copy< ASTType > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_AddExp: // AddExp
        value.copy< AddExp* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_ConstExpList: // ConstExpList
      case symbol_kind::S_ExpList: // ExpList
        value.copy< ArrayList* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_Decl: // Decl
      case symbol_kind::S_BlockItem: // BlockItem
      case symbol_kind::S_Stmt: // Stmt
      case symbol_kind::S_PrimaryExp: // PrimaryExp
        value.copy< BaseAST* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_Block: // Block
        value.copy< Block* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_BlockItemList: // BlockItemList
        value.copy< BlockItemList* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_CompUnit: // CompUnit
        value.copy< CompUnit* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_ConstDecl: // ConstDecl
        value.copy< ConstDecl* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_ConstDef: // ConstDef
        value.copy< ConstDef* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_ConstDefList: // ConstDefList
        value.copy< ConstDefList* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_EqExp: // EqExp
        value.copy< EqExp* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_FuncDef: // FuncDef
        value.copy< FuncDef* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_FuncFParam: // FuncFParam
        value.copy< FuncParam* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_FuncFParamList: // FuncFParamList
        value.copy< FuncParamList* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_FuncRParamList: // FuncRParamList
        value.copy< FuncRParamList* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_ConstInitVal: // ConstInitVal
      case symbol_kind::S_InitVal: // InitVal
        value.copy< InitVal* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_ConstInitValList: // ConstInitValList
      case symbol_kind::S_InitValList: // InitValList
        value.copy< InitValList* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_LAndExp: // LAndExp
        value.copy< LAndExp* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_LOrExp: // LOrExp
        value.copy< LOrExp* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_LVal: // LVal
        value.copy< LVal* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_IDENT: // IDENT
        value.copy< LocatedIdentifier > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_MulExp: // MulExp
        value.copy< MulExp* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_RelExp: // RelExp
        value.copy< RelExp* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_UnaryExp: // UnaryExp
        value.copy< UnaryExp* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_VarDecl: // VarDecl
        value.copy< VarDecl* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_VarDef: // VarDef
        value.copy< VarDef* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_VarDefList: // VarDefList
        value.copy< VarDefList* > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_FLOATCONST: // FLOATCONST
        value.copy< float > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_INTCONST: // INTCONST
        value.copy< int > (YY_MOVE (that.value));
        break;

      default:
        break;
    }

  }




  template <typename Base>
  parser::symbol_kind_type
  parser::basic_symbol<Base>::type_get () const YY_NOEXCEPT
  {
    return this->kind ();
  }


  template <typename Base>
  bool
  parser::basic_symbol<Base>::empty () const YY_NOEXCEPT
  {
    return this->kind () == symbol_kind::S_YYEMPTY;
  }

  template <typename Base>
  void
  parser::basic_symbol<Base>::move (basic_symbol& s)
  {
    super_type::move (s);
    switch (this->kind ())
    {
      case symbol_kind::S_Type: // Type
        value.move< ASTType > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_AddExp: // AddExp
        value.move< AddExp* > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_ConstExpList: // ConstExpList
      case symbol_kind::S_ExpList: // ExpList
        value.move< ArrayList* > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_Decl: // Decl
      case symbol_kind::S_BlockItem: // BlockItem
      case symbol_kind::S_Stmt: // Stmt
      case symbol_kind::S_PrimaryExp: // PrimaryExp
        value.move< BaseAST* > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_Block: // Block
        value.move< Block* > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_BlockItemList: // BlockItemList
        value.move< BlockItemList* > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_CompUnit: // CompUnit
        value.move< CompUnit* > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_ConstDecl: // ConstDecl
        value.move< ConstDecl* > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_ConstDef: // ConstDef
        value.move< ConstDef* > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_ConstDefList: // ConstDefList
        value.move< ConstDefList* > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_EqExp: // EqExp
        value.move< EqExp* > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_FuncDef: // FuncDef
        value.move< FuncDef* > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_FuncFParam: // FuncFParam
        value.move< FuncParam* > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_FuncFParamList: // FuncFParamList
        value.move< FuncParamList* > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_FuncRParamList: // FuncRParamList
        value.move< FuncRParamList* > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_ConstInitVal: // ConstInitVal
      case symbol_kind::S_InitVal: // InitVal
        value.move< InitVal* > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_ConstInitValList: // ConstInitValList
      case symbol_kind::S_InitValList: // InitValList
        value.move< InitValList* > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_LAndExp: // LAndExp
        value.move< LAndExp* > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_LOrExp: // LOrExp
        value.move< LOrExp* > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_LVal: // LVal
        value.move< LVal* > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_IDENT: // IDENT
        value.move< LocatedIdentifier > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_MulExp: // MulExp
        value.move< MulExp* > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_RelExp: // RelExp
        value.move< RelExp* > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_UnaryExp: // UnaryExp
        value.move< UnaryExp* > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_VarDecl: // VarDecl
        value.move< VarDecl* > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_VarDef: // VarDef
        value.move< VarDef* > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_VarDefList: // VarDefList
        value.move< VarDefList* > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_FLOATCONST: // FLOATCONST
        value.move< float > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_INTCONST: // INTCONST
        value.move< int > (YY_MOVE (s.value));
        break;

      default:
        break;
    }

  }

  // by_kind.
  inline
  parser::by_kind::by_kind () YY_NOEXCEPT
    : kind_ (symbol_kind::S_YYEMPTY)
  {}

#if 201103L <= YY_CPLUSPLUS
  inline
  parser::by_kind::by_kind (by_kind&& that) YY_NOEXCEPT
    : kind_ (that.kind_)
  {
    that.clear ();
  }
#endif

  inline
  parser::by_kind::by_kind (const by_kind& that) YY_NOEXCEPT
    : kind_ (that.kind_)
  {}

  inline
  parser::by_kind::by_kind (token_kind_type t) YY_NOEXCEPT
    : kind_ (yytranslate_ (t))
  {}



  inline
  void
  parser::by_kind::clear () YY_NOEXCEPT
  {
    kind_ = symbol_kind::S_YYEMPTY;
  }

  inline
  void
  parser::by_kind::move (by_kind& that)
  {
    kind_ = that.kind_;
    that.clear ();
  }

  inline
  parser::symbol_kind_type
  parser::by_kind::kind () const YY_NOEXCEPT
  {
    return kind_;
  }


  inline
  parser::symbol_kind_type
  parser::by_kind::type_get () const YY_NOEXCEPT
  {
    return this->kind ();
  }


} // yy
#line 2844 "include/yacc/Bison.hpp"




#endif // !YY_YY_INCLUDE_YACC_BISON_HPP_INCLUDED
