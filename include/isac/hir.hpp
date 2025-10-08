#pragma once
#include "isac/frontend.hpp"
#include "isac/intrinsics.hpp"
#include "isac/target.hpp"
#include <limits>
#include <set>
namespace isac {
using GenericDeclId = uint32_t;
using TypeParamId = uint32_t;
using InstanceId = uint32_t;
using RegionId = uint32_t;
using ConstId = uint32_t;
using ExprId = uint32_t;
enum class BorrowKind { Shared, Unique };
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
    Enum,
    Reference,
    Slice
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
  bool reference = false;
  bool inferredRegion = false;
  RegionId region = None;
  Loc lastUse = {};
  bool temporary = false;
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
    Block,
    Borrow,
    Match,
    PayloadTake,
    StoreReference,
    Length
  } kind;
  Loc loc;
  bool inferredLiteral = false;
  ExprId id = None;
  BorrowKind borrowKind = BorrowKind::Shared;
  RegionId region = None;
  enum class OperatorClass {
    Other,
    Arithmetic,
    Logical,
    Bitwise,
    Comparison
  } operatorClass = OperatorClass::Other;
  TypeId type = Unit;
  uint32_t ref = None;
  ConstId constant = None;
  std::string text;
  std::vector<Expr> args;
  std::vector<Stmt> body;
};
struct Stmt {
  enum Kind {
    Let,
    Return,
    Expression,
    If,
    Borrow,
    Loop,
    Break,
    Continue,
    Scope,
    Match,
    BorrowStart,
    BorrowEnd
  } kind;
  Loc loc;
  SymbolId symbol = None;
  SymbolId matchResult = None;
  uint32_t loop = None;
  bool unconditional = false;
  VariantId variant = None;
  bool catchall = false;
  Access access = Access::Own;
  Expr expr;
  std::vector<Stmt> body, otherwise, latch;
};

struct ParameterRegion {
  SymbolId parameter;
  RegionId region;
  BorrowKind kind;
};
struct Function {
  FunctionKind kind = FunctionKind::Normal;
  FunctionEffects effects;
  IntrinsicId intrinsicId = IntrinsicId::None;
  bool trusted = false, external = false, releases = false, pub = false;
  bool consumesSelf = false;
  std::string intrinsic;
  FunctionId id;
  std::string name;
  TypeId owner = None, result = Unit;
  Access access = Access::Own;
  std::vector<SymbolId> params;
  std::vector<ParameterRegion> parameterRegions;
  SymbolId resultOrigin = None;
  bool resultBorrowedContainer = false;
  bool returnsReference() const {
    return resultOrigin != None && !resultBorrowedContainer;
  }
  RegionId resultRegion = None;
  std::vector<OperationId> operations;
  std::vector<Stmt> body = {};
  Loc loc;
};
struct ConstDecl {
  ConstId id;
  std::string name;
  TypeId type;
  Expr expression;
  Expr value;
  Loc loc;
};
struct GenericPayload {
  bool parameter;
  uint32_t id;
};
struct GenericVariant {
  std::string name;
  std::vector<GenericPayload> payload;
};
struct GenericEnum {
  GenericDeclId id;
  std::string name;
  std::vector<TypeParamId> parameters;
  std::vector<GenericVariant> variants;
};
struct GenericField {
  std::string name;
  GenericPayload type;
};
struct GenericStruct {
  GenericDeclId id;
  std::string name;
  std::vector<TypeParamId> parameters;
  std::vector<GenericField> fields;
};
struct Substitution {
  std::vector<std::pair<TypeParamId, TypeId>> types;
};
struct GenericInstance {
  InstanceId id;
  GenericDeclId declaration;
  Substitution substitution;
  TypeId type;
};
enum class MainSignatureKind { NoArgsI32, NoArgsUnit };
struct EntryPointInfo {
  FunctionId function = None;
  MainSignatureKind signature = MainSignatureKind::NoArgsI32;
};
struct Program {
  EntryPointInfo entryPoint;
  std::vector<GenericEnum> genericEnums;
  std::vector<GenericStruct> genericStructs;
  std::vector<GenericInstance> instances;
  std::vector<Diagnostic> warnings;
  std::vector<ConstDecl> constants;
  TargetInfo target;
  std::vector<Type> types;
  std::vector<Protocol> protocols;
  std::vector<Symbol> symbols;
  std::vector<Function> functions;
  uint32_t storageWidth(TypeId t) const {
    return heap(t) ? 1 : types[t].width;
  }
  uint32_t payloadOffset(TypeId t, VariantId variant, size_t index) const {
    uint32_t offset = 1;
    const auto &payload = types[t].variants.at(variant).payload;
    for (size_t n = 0; n < index; ++n)
      offset += storageWidth(payload.at(n));
    return offset;
  }

  bool borrowResultCompatible(TypeId input, TypeId result) const {
    if (input == result)
      return true;
    if (!scalar(result))
      return false;
    const auto &from = types.at(input);
    if (from.kind == Type::Array || from.kind == Type::Slice)
      return from.element == result;
    if (from.kind == Type::Structure && from.representation == Representation::Stack)
      for (const auto &field : from.fields)
        if (field.type == result)
          return true;
    return false;
  }
  bool borrowedPayloadResultCompatible(TypeId input, TypeId result) const {
    if (types.at(result).kind != Type::Enum || needsDrop(result))
      return false;
    bool found = false;
    for (const auto &variant : types[result].variants)
      for (auto payload : variant.payload) {
        if (reference(payload)) {
          if (types[payload].element != input)
            return false;
          found = true;
        } else if (containsReferences(payload))
          return false;
      }
    return found;
  }
  std::set<uint32_t> borrowedPayloadOffsets(TypeId result) const {
    std::set<uint32_t> offsets;
    for (const auto &variant : types.at(result).variants)
      for (size_t n = 0; n < variant.payload.size(); ++n)
        if (reference(variant.payload[n]))
          offsets.insert(payloadOffset(result, variant.id, n));
    return offsets;
  }
  bool reference(TypeId t) const {
    return types[t].kind == Type::Reference;
  }
  bool containsReferences(TypeId t) const {
    if (reference(t))
      return true;
    for (const auto &variant : types[t].variants)
      for (auto payload : variant.payload)
        if (containsReferences(payload))
          return true;
    return false;
  }
  bool needsDrop(TypeId t) const {
    if (heap(t))
      return true;
    for (const auto &variant : types[t].variants)
      for (auto payload : variant.payload)
        if (needsDrop(payload))
          return true;
    return false;
  }
  bool heap(TypeId t) const {
    return types[t].representation != Representation::Stack;
  }
  bool linear(TypeId t) const {
    return types[t].kind == Type::Structure || types[t].kind == Type::Array ||
           types[t].kind == Type::Enum || reference(t) || types[t].kind == Type::Slice;
  }
};
}
hir::Program resolve(const ast::Program &, TargetInfo target = {});
void printHIR(const hir::Program &, std::ostream &);
hir::Expr evaluateConstant(const hir::Expr &, unsigned pointerWidth);
}
