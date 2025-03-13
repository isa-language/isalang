#pragma once
#include "isac/hir.hpp"
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
    MemMove
  } kind;
  Loc loc;
  ValueId result = None;
  TypeId type = Unit;
  SymbolId slot = None;
  uint32_t offset = 0;
  FunctionId function = None;
  std::string text;
  std::vector<ValueId> args;
  std::vector<Inst> body, otherwise;
  std::vector<SymbolId> locals;
  ValueId resourceInput = None;
  std::vector<Merge> merges;
  std::vector<SymbolId> cleanup;
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
