#pragma once
#include "isac/frontend.hpp"
#include "isac/target.hpp"
#include <limits>
namespace isac {
using SymbolId = uint32_t;
using TypeId = uint32_t;
using ProtocolId = uint32_t;
using StateId = uint32_t;
using OperationId = uint32_t;
using FunctionId = uint32_t;
using ValueId = uint32_t;
inline constexpr uint32_t None = std::numeric_limits<uint32_t>::max();
inline constexpr TypeId Unit = 0, I32 = 1, Bool = 2, USize = 3, RawPtr = 4;
namespace hir {
struct Field {
  std::string name;
  TypeId type;
  uint32_t offset;
};
enum class Representation { Stack, BoxPointer, BufferPointer };
enum class DropGlue { Trivial, FreeAllocation };
struct Type {
  enum Kind { UnitType, Integer, Boolean, Structure, Array, Size, Pointer } kind;
  std::string name;
  ProtocolId protocol = None;
  TypeId element = None;
  uint32_t count = 0, width = 1;
  std::vector<Field> fields;
  Representation representation = Representation::Stack;
  DropGlue dropGlue = DropGlue::Trivial;
};
struct Operation {
  OperationId id;
  std::string name;
  Access access;
  StateId from, to;
};
struct Protocol {
  ProtocolId id;
  std::string name;
  StateId initial;
  std::vector<StateId> finals;
  std::vector<std::string> states;
  std::vector<Operation> operations;
};
struct Symbol {
  SymbolId id;
  std::string name;
  TypeId type;
  Access access = Access::Own;
  bool parameter = false, receiver = false;
  Loc loc;
};
struct Expr {
  enum Kind {
    Constant,
    Variable,
    Binary,
    Unary,
    Call,
    Method,
    Field,
    Index,
    Array,
    Assign,
    Move,
    Drop,
    Construct,
    Print,
    UnitValue
  } kind;
  Loc loc;
  TypeId type = Unit;
  uint32_t ref = None;
  std::string text;
  std::vector<Expr> args;
};
struct Stmt {
  enum Kind { Let, Return, Expression, If, Borrow } kind;
  Loc loc;
  SymbolId symbol = None;
  Access access = Access::Own;
  Expr expr;
  std::vector<Stmt> body, otherwise;
};
struct Function {
  bool trusted = false, external = false, releases = false, pub = false;
  std::string intrinsic;
  FunctionId id;
  std::string name;
  TypeId owner = None, result = Unit;
  Access access = Access::Own;
  std::vector<SymbolId> params;
  std::vector<OperationId> operations;
  std::vector<Stmt> body;
  Loc loc;
};
struct Program {
  std::vector<Type> types;
  std::vector<Protocol> protocols;
  std::vector<Symbol> symbols;
  std::vector<Function> functions;
  bool heap(TypeId t) const {
    return types[t].representation != Representation::Stack;
  }
  bool linear(TypeId t) const {
    return types[t].kind == Type::Structure || types[t].kind == Type::Array;
  }
};
}
hir::Program resolve(const ast::Program &);
void printHIR(const hir::Program &, std::ostream &);
}
