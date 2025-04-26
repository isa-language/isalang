#pragma once
#include "isac/frontend.hpp"
#include "isac/intrinsics.hpp"
#include "isac/target.hpp"
#include <limits>
namespace isac {
using EnumId = uint32_t;
using VariantId = uint32_t;
using LoopId = uint32_t;
using SymbolId = uint32_t;
using TypeId = uint32_t;
using ProtocolId = uint32_t;
using StateId = uint32_t;
using OperationId = uint32_t;
using FunctionId = uint32_t;
using ValueId = uint32_t;
inline constexpr uint32_t None = std::numeric_limits<uint32_t>::max();
inline constexpr TypeId Unit = 0, I32 = 1, Bool = 2, USize = 3, RawPtr = 4, I8 = 5, I16 = 6,
                        I64 = 7, U8 = 8, U16 = 9, U32 = 10, U64 = 11, ISize = 12, F32 = 13,
                        F64 = 14, Char = 15, Str = 16;
bool integer(TypeId);
bool floating(TypeId);
bool numeric(TypeId);
bool scalar(TypeId);
bool signedType(TypeId);
unsigned bitWidth(TypeId, unsigned pointerBits = 64);
void validateLiteral(TypeId, const std::string &, Loc, unsigned pointerBits = 64);
namespace hir {
struct Field {
  std::string name;
  TypeId type;
  uint32_t offset;
};
enum class Representation { Stack, BoxPointer, BufferPointer, StringRecord, InputRecord };
enum class DropGlue { Trivial, FreeAllocation, StringAllocation, InputAllocation };
struct Variant {
  VariantId id;
  std::string name;
  std::vector<TypeId> payload;
};
struct Type {
  enum Kind {
    UnitType,
    Integer,
    Boolean,
    Structure,
    Array,
    Size,
    Pointer,
    Float,
    Character,
    Enum
  } kind;
  std::string name;
  ProtocolId protocol = None;
  TypeId element = None;
  uint32_t count = 0, width = 1;
  std::vector<Field> fields = {};
  Representation representation = Representation::Stack;
  DropGlue dropGlue = DropGlue::Trivial;
  std::vector<Variant> variants = {};
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
struct Stmt;
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
    UnitValue,
    Cast,
    EnumConstruct,
    StaticStr,
    Block
  } kind;
  Loc loc;
  bool inferredLiteral = false;
  enum class OperatorClass {
    Other,
    Arithmetic,
    Logical,
    Bitwise,
    Comparison
  } operatorClass = OperatorClass::Other;
  TypeId type = Unit;
  uint32_t ref = None;
  std::string text;
  std::vector<Expr> args;
  std::vector<Stmt> body;
};
struct Stmt {
  enum Kind { Let, Return, Expression, If, Borrow, Loop, Break, Continue, Scope, Match } kind;
  Loc loc;
  SymbolId symbol = None;
  uint32_t loop = None;
  bool unconditional = false;
  VariantId variant = None;
  bool catchall = false;
  Access access = Access::Own;
  Expr expr;
  std::vector<Stmt> body, otherwise, latch;
};
struct Function {
  FunctionKind kind = FunctionKind::Normal;
  FunctionEffects effects;
  IntrinsicId intrinsicId = IntrinsicId::None;
  bool trusted = false, external = false, releases = false, pub = false;
  std::string intrinsic;
  FunctionId id;
  std::string name;
  TypeId owner = None, result = Unit;
  Access access = Access::Own;
  std::vector<SymbolId> params;
  std::vector<OperationId> operations;
  std::vector<Stmt> body = {};
  Loc loc;
};
struct Program {
  TargetInfo target;
  std::vector<Type> types;
  std::vector<Protocol> protocols;
  std::vector<Symbol> symbols;
  std::vector<Function> functions;
  bool heap(TypeId t) const {
    return types[t].representation != Representation::Stack;
  }
  bool linear(TypeId t) const {
    return types[t].kind == Type::Structure || types[t].kind == Type::Array ||
           types[t].kind == Type::Enum;
  }
};
}
hir::Program resolve(const ast::Program &, TargetInfo target = {});
void printHIR(const hir::Program &, std::ostream &);
}
