#pragma once
#include "isac/diagnostics.hpp"
#include "isac/effects.hpp"
#include <cstdint>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>
namespace isac {
struct Token {
  std::string text;
  Loc loc;
  enum Kind { Identifier, Number, Punctuation, String, Character, End } kind;
};
std::vector<Token> lex(const std::string &, FileId file = NoFile);
struct NumericLiteral {
  std::string value, suffix;
  bool floating = false;
};

NumericLiteral parseNumericLiteral(const std::string &, Loc);
struct TypeSyntax {
  std::string name = "unit";
  int count = 0;
  Loc loc;
  bool reference = false, mutableReference = false;
  std::string countName = {};
  std::vector<TypeSyntax> arguments = {};
  bool rawPointer = false, mutablePointer = false;
  bool functionPointer = false;
  std::vector<std::pair<int, std::string>> dimensions = {};
};
enum class Access { Own, Shared, Unique };
const char *accessName(Access);
namespace ast {
struct Stmt;
struct Expr {
  enum Kind {
    Int,
    Bool,
    Name,
    Binary,
    Call,
    Method,
    Member,
    Index,
    Array,
    Assign,
    Unary,
    Cast,
    Char,
    String,
    Block,
    AtCall,
    Borrow,
    Match,
    Unsafe,
    Dereference,
    ArrayRepeat,
    Invoke,
    BraceInit
  } kind;
  Loc loc;
  std::string text;
  std::vector<Expr> args;
  std::vector<Stmt> body;
  std::vector<TypeSyntax> typeArguments;

  std::vector<std::string> initializerNames;
  Expr();
  Expr(Kind, Loc, std::string, std::vector<Expr>);
};
struct Pattern {
  enum Kind { Wildcard, Binding, Integer, Boolean, Variant, Character } kind;
  Loc loc;
  std::string name;
  std::vector<std::string> bindings;
};
struct Stmt {
  enum Kind {
    Let,
    Return,
    Expression,
    If,
    Borrow,
    While,
    Loop,
    For,
    Break,
    Continue,
    Match,
    Const
  } kind;
  Loc loc;
  std::string name;
  TypeSyntax annotation;
  bool annotated = false;
  bool expressionMatch = false;
  bool conditionalPattern = false;
  bool collectionIteration = false;
  bool classicFor = false;
  Access access = Access::Own;
  Expr expr, limit;
  Pattern pattern;
  std::vector<Stmt> body, otherwise;
  std::vector<Stmt> headerInit;
};
inline Expr::Expr() : kind(Int) {}
inline Expr::Expr(Kind k, Loc l, std::string text, std::vector<Expr> args)
    : kind(k), loc(std::move(l)), text(std::move(text)), args(std::move(args)) {}
struct Param {
  std::string name;
  TypeSyntax type;
  Access access = Access::Own;
  Loc loc;
};
struct Function {
  FunctionKind kind = FunctionKind::Normal;
  FunctionEffects effects;
  bool trusted = false, external = false, releases = false;
  std::string intrinsic;
  std::string name, owner;
  Loc loc;
  bool pub = false;
  std::vector<Param> params;
  TypeSyntax result;
  std::vector<Stmt> body = {};
  bool foreignC = false, unsafeFunction = false;
  std::string cLinkName = {};
  bool variadic = false;
};
struct Field {
  std::string name;
  TypeSyntax type;
  Loc loc;
  bool pub = false;
};
struct Struct {
  std::vector<std::string> parameters;
  bool trusted = false;
  std::string representation;
  std::string name, protocol;
  Loc loc;
  bool pub = false;
  std::vector<Field> fields;
  bool foreignC = false;
  std::string cName = {};
  bool foreignUnion = false;

  std::vector<std::string> initializerMethods;
  std::vector<std::string> iterationMethods;
};
struct ForeignType {
  std::string name;
  Loc loc;
  bool alias = false;
  TypeSyntax target;
  std::string cName = {};
};
struct Operation {
  std::string name, target;
  Access access;
  Loc loc;
};
struct State {
  std::string name;
  Loc loc;
  std::vector<Operation> operations;
};
struct Protocol {
  std::string name, initial;
  Loc loc;
  bool pub = false;
  std::vector<std::string> finals;
  std::vector<State> states;
};
struct Impl {
  std::string owner;
  Loc loc;
  std::vector<uint32_t> methods;
  std::vector<std::string> parameters;
  TypeSyntax target;
};
struct EnumVariant {
  std::string name;
  Loc loc;
  std::vector<TypeSyntax> payload;
};
struct Enum {
  std::vector<std::string> parameters;
  std::string name;
  Loc loc;
  std::vector<EnumVariant> variants;
};
struct Program {
  std::vector<Stmt> constants;
  std::vector<Enum> enums;
  std::vector<std::string> imports;
  std::vector<Loc> importOrigins;
  std::vector<std::string> importAliases;
  std::vector<Impl> impls;
  std::vector<Struct> structs;
  std::vector<Protocol> protocols;
  std::vector<Function> functions;
  std::vector<ForeignType> foreignTypes;
  bool library = false;
};
}
ast::Program parse(const std::vector<Token> &, bool trusted = false);
bool parseFragment(const std::vector<Token> &, const std::string &);
ast::Program loadImports(ast::Program, SourceManager *sources = nullptr);
void printAST(const ast::Program &, std::ostream &);
}
