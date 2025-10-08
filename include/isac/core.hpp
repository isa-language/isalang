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
    Length
  } kind;
  Loc loc;
  uint32_t semanticPoint = None;
  std::vector<SymbolId> inferredEnds;
  std::vector<SymbolId> exitEnds;
  RegionId region = None;
  ValueId result = None;
  TypeId type = Unit;
  SymbolId slot = None;
  uint32_t offset = 0;
  uint32_t projection = 0;
  enum class PlaceProjection { None, Field, Index } placeProjection = PlaceProjection::None;
  uint32_t fieldIndex = None;
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
