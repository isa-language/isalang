#pragma once
#include "isac/hir.hpp"
#include "isac/storage.hpp"
#include "isac/carriers.hpp"
#include <map>
#include <set>

namespace isac::core {
struct Program;
}
namespace isac::regions {
using BasicBlockId = uint32_t;
using LoanId = uint32_t;
struct ProgramPoint {
  BasicBlockId block;
  uint32_t statementIndex = 0;
};
enum class RegionKind { Local, Parameter, Return, Static, Storage };
enum class ConstraintKind { ContainsPoint, Subregion };
enum class ConstraintReason {
  BorrowCreation,
  LiveCarrier,
  Reborrow,
  FunctionResult,
  StoredReference
};
struct Region {
  RegionId id;
  RegionKind kind;
  std::set<BasicBlockId> points;
};
struct Constraint {
  ConstraintKind kind;
  RegionId region;
  RegionId upper = None;
  ProgramPoint point{None, 0};
  ConstraintReason reason;
  Loc origin;
};
enum class Terminator { Branch, CondBranch, Return, LoopBackedge, Unreachable };
enum class ProjectionKind { Field, Index, Deref, VariantPayload };
struct Projection {
  ProjectionKind kind;
  uint32_t index;
  VariantId variant = None;
};
struct Place {
  SymbolId root;
  std::vector<Projection> projections;
};
struct Loan {
  LoanId id;
  RegionId region;
  RegionKind regionKind = RegionKind::Local;
  SymbolId carrier;
  Place place;
  BorrowKind capability;
  ProgramPoint created;
  Loc origin;
  bool inferred = false;
  std::set<BasicBlockId> points;
};

class ValueLivenessSet {
  ValueId base = 0;
  std::vector<uint64_t> words;

public:
  ValueLivenessSet() = default;
  ValueLivenessSet(ValueId first, ValueId end) : base(first), words((end - first + 63) / 64) {}
  bool contains(ValueId value) const {
    return value >= base && (value - base) / 64 < words.size() &&
           (words[(value - base) / 64] & (uint64_t{1} << ((value - base) % 64)));
  }
  void insert(ValueId value) {
    words.at((value - base) / 64) |= uint64_t{1} << ((value - base) % 64);
  }
  void erase(ValueId value) {
    words.at((value - base) / 64) &= ~(uint64_t{1} << ((value - base) % 64));
  }
  void merge(const ValueLivenessSet &other) {
    for (size_t n = 0; n < words.size(); ++n)
      words[n] |= other.words.at(n);
  }
  bool operator==(const ValueLivenessSet &) const = default;
};
struct BasicBlock {
  BasicBlockId id;
  Loc origin;
  Terminator terminator = Terminator::Branch;
  std::vector<BasicBlockId> successors;
  std::set<SymbolId> uses, definitions, liveIn, liveOut;
  std::set<ValueId> valueUses, valueDefinitions;
  ValueLivenessSet valuesIn, valuesOut;
  bool reachable = false;
};
struct FunctionRegionContract {
  std::map<SymbolId, hir::RegionArgs> inputs;
  hir::RegionArgs result;

  hir::RegionSubstitution resultSources;
};
struct CallRegionInstantiation {
  ProgramPoint point;
  FunctionId target;
  hir::RegionSubstitution substitution;
  hir::RegionArgs result;
};
struct StorageBound {
  SymbolId owner;
  Loc origin;

  std::set<BasicBlockId> points;
};

struct ProjectionStorageBound {
  SymbolId owner;
  std::vector<uint32_t> fields;
  TypeId type;
  Loc origin;
  std::set<LoanId> loans;
  std::set<BasicBlockId> points;
};
struct SemanticValueDefinition {
  TypeId type;
  bool reference;
  ProgramPoint point;
};

struct FieldRegionOperation {
  enum Kind { Bind, Move, Take, Store, Drop, Scope, Return } kind;
  storage::Location place{storage::Location::Symbol, None};
  TypeId type = Unit;
  ValueId source = None, result = None;
  std::vector<SymbolId> locals;
  bool operator==(const FieldRegionOperation &) const = default;
};

using ParentSet = std::set<SymbolId>;
using ParentState = std::map<storage::Location, ParentSet>;
struct ParentOperation {
  enum Kind {
    Seed,
    Bind,
    Move,
    Take,
    Store,
    Drop,
    Construct,
    Alias,
    Reborrow,
    EndAlias,
    Scope,
    Return,
    Consume,
    Forward
  } kind;
  storage::Location place{storage::Location::Symbol, None};
  storage::Location destination{storage::Location::Value, None};
  std::vector<ValueId> inputs;
  std::vector<hir::ReferencePath> paths;

  std::map<hir::ReferencePath, storage::Location> transfers;
  std::map<hir::ReferencePath, ParentSet> seeds;
  std::set<storage::Location> reads;
  std::vector<SymbolId> locals;
  bool operator==(const ParentOperation &) const = default;
};
struct FunctionGraph {
  FunctionId function;
  BasicBlockId entry;
  std::vector<BasicBlock> blocks;
  std::vector<Loan> loans;
  std::map<RegionId, Region> regions;
  std::vector<Constraint> constraints;
  std::map<SymbolId, std::set<SymbolId>> storedResultParents;
  std::map<uint32_t, ParentOperation> parentOperations;
  std::map<uint32_t, ParentState> parentStates;
  std::map<uint32_t, ParentSet> parentUses;
  std::map<SymbolId, ParentSet> loanParents;
  FunctionRegionContract contract;
  std::map<SymbolId, hir::RegionArgs> symbolRegions;
  std::map<ValueId, hir::RegionArgs> valueRegions;

  std::map<ValueId, SemanticValueDefinition> valueDefinitions;
  std::map<uint32_t, FieldRegionOperation> fieldRegionOperations;
  std::map<uint32_t, std::map<SymbolId, hir::RegionArgs>> fieldRegions;
  std::vector<CallRegionInstantiation> calls;
  std::map<SymbolId, StorageBound> storageBounds;
  std::map<SymbolId, RegionId> storageRegions;
  std::map<RegionId, std::set<SymbolId>> ownerBounds;

  std::map<uint32_t, ProjectionStorageBound> projectionBounds;

  std::set<BasicBlockId> resourcePoints;
  storage::Facts storage;
  carriers::Facts carriers;
};

void infer(core::Program &);

void validateBounds(const core::Program &);
void print(const core::Program &, std::ostream &);
void printConstraints(const core::Program &, std::ostream &);
}
