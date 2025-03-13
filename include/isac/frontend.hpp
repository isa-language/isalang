#pragma once
#include <cstdint>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>
namespace isac {
struct Loc {
  unsigned line = 1, column = 1;
  std::string file;
};
struct Error : std::runtime_error {
  Loc loc;
  std::string code;
  Error(Loc l, std::string c, std::string m) : std::runtime_error(m), loc(l), code(std::move(c)) {}
};
[[noreturn]] inline void fail(Loc l, std::string c, std::string m) {
  throw Error(l, std::move(c), std::move(m));
}
struct Token {
  std::string text;
  Loc loc;
  enum Kind { Identifier, Number, Punctuation, String, End } kind;
};
std::vector<Token> lex(const std::string &);
struct TypeSyntax {
  std::string name = "unit";
  int count = 0;
  Loc loc;
};
enum class Access { Own, Shared, Unique };
const char *accessName(Access);
namespace ast {
struct Expr {
  enum Kind { Int, Bool, Name, Binary, Call, Method, Member, Index, Array, Assign, Unary } kind;
  Loc loc;
  std::string text;
  std::vector<Expr> args;
};
struct Stmt {
  enum Kind { Let, Return, Expression, If, Borrow } kind;
  Loc loc;
  std::string name;
  TypeSyntax annotation;
  bool annotated = false;
  Access access = Access::Own;
  Expr expr;
  std::vector<Stmt> body, otherwise;
};
struct Param {
  std::string name;
  TypeSyntax type;
  Access access = Access::Own;
  Loc loc;
};
struct Function {
  bool trusted = false, external = false, releases = false;
  std::string intrinsic;
  std::string name, owner;
  Loc loc;
  bool pub = false;
  std::vector<Param> params;
  TypeSyntax result;
  std::vector<Stmt> body;
};
struct Field {
  std::string name;
  TypeSyntax type;
  Loc loc;
  bool pub = false;
};
struct Struct {
  bool trusted = false;
  std::string representation;
  std::string name, protocol;
  Loc loc;
  bool pub = false;
  std::vector<Field> fields;
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
};
struct Program {
  std::vector<std::string> imports;
  std::vector<Impl> impls;
  std::vector<Struct> structs;
  std::vector<Protocol> protocols;
  std::vector<Function> functions;
};
}
ast::Program parse(const std::vector<Token> &, bool trusted = false);
ast::Program loadImports(ast::Program);
void printAST(const ast::Program &, std::ostream &);
}
