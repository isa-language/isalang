#pragma once
#include "isac/hir.hpp"
#include "isac/regions.hpp"
#include <map>
namespace isac {
enum class Quantity { Zero, One, Omega };
enum class MemoryState { Own, Shared, Unique, Dead };
namespace core {
struct Merge {
  SymbolId slot;
  ValueId left, right, result;
};
struct FieldDropPlan {
  bool skip = false;
  std::map<uint32_t, FieldDropPlan> fields;
};

struct OwnedFieldState {
  StateId state = None;
  bool allocation = false;
  std::map<uint32_t, OwnedFieldState> fields;
  Loc origin = {};
  bool operator==(const OwnedFieldState &other) const {
    return state == other.state && allocation == other.allocation && fields == other.fields;
  }
};
struct FieldPostState {
  SymbolId parameter;
  std::map<uint32_t, OwnedFieldState> fields;
  Loc origin;
};
struct Inst {
  enum Kind {
    Constant,
    Copy,
    Read,
    Write,
    Create,
    Array,
    Move,
    Bind,
    Drop,
    BorrowShared,
    BorrowUnique,
    EndBorrow,
    Call,
    ProtocolTransition,
    Return,
    If,
    ScopeEnd,
    Binary,
    Unary,
    Print,
    Discard,
    HeapAlloc,
    HeapFree,
    HeapLoad,
    HeapStore,
    MemCopy,
    MemMove,
    Cast,
    Loop,
    Break,
    Continue,
    EnumConstruct,
    Inspect,
    StaticStr,
    StringCall,
    TextPrint,
    EndViews,
    InputCall,
    RefineVariant,
    PayloadTake,
    ReferenceArgument,
    StoreReference,
    Length,
    FieldStore,
    FieldTake,
    FieldDrop,
    ContainerCall,
    UnsafeScope,
    ForeignCall,
    RawAddress,
    PointerCast,
    RawLoad,
    RawStore,
    ArrayRepeat,
    FunctionAddress,
    IndirectForeignCall,
    StructInit
  } kind;
  Loc loc;
  uint32_t semanticPoint = None;
  std::vector<SymbolId> inferredEnds;
  std::vector<VariantId> possibleVariants;
  std::vector<SymbolId> exitEnds;
  RegionId region = None;
  ValueId result = None;
  TypeId type = Unit;
  SymbolId slot = None;
  uint32_t offset = 0;
  uint32_t projection = 0;
  bool storedPayloadReference = false;
  enum class PlaceProjection { None, Field, Index } placeProjection = PlaceProjection::None;
  uint32_t fieldIndex = None;
  std::vector<uint32_t> fieldPath;
  uint32_t loop = None;
  bool unconditional = false;
  FunctionId function = None;
  IntrinsicId intrinsicId = IntrinsicId::None;
  std::string text;
  hir::Expr::OperatorClass operatorClass = hir::Expr::OperatorClass::Other;
  std::vector<ValueId> args;
  std::vector<Inst> body, otherwise, condition, latch;
  std::vector<SymbolId> locals;
  ValueId resourceInput = None;
  std::vector<Merge> merges;
  std::vector<SymbolId> cleanup;
  std::map<SymbolId, std::vector<uint32_t>> movedPayloads;
  bool freeTemporary = false;
  bool replaceField = false;
  std::map<SymbolId, std::vector<uint32_t>> absentFields;
  std::vector<uint32_t> temporaryAbsentFields;
  std::map<SymbolId, FieldDropPlan> fieldCleanup;
  FieldDropPlan replacedFieldPlan, temporaryFieldPlan;
  Quantity quantity = Quantity::Omega;
  Access access = Access::Own;
  ProtocolId protocol = None;
  OperationId operation = None;
  StateId from = None, to = None;
};
struct Function {
  FunctionId id;
  std::vector<Inst> body;
  std::vector<ValueId> parameters;
  std::vector<FieldPostState> fieldPostStates = {};
};
struct Program {
  hir::Program semantics;
  std::vector<Function> functions;
  uint32_t values = 0;
  std::vector<regions::FunctionGraph> regionGraphs;
};
class Verified {
  Program program;
  explicit Verified(Program p) : program(std::move(p)) {}
  friend Verified verify(Program);

public:
  const Program &get() const {
    return program;
  }
};
Program elaborate(hir::Program);
Verified verify(Program);
void print(const Program &, std::ostream &);
}
}
