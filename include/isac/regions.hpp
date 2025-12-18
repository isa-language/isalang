#pragma once
#include "isac/hir.hpp"
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
enum class RegionKind { Local, Parameter, Return, Static };
enum class ConstraintKind { ContainsPoint, Subregion };
enum class ConstraintReason { BorrowCreation, LiveCarrier, Reborrow, FunctionResult };
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
struct FunctionGraph {
  FunctionId function;
  BasicBlockId entry;
  std::vector<BasicBlock> blocks;
  std::vector<Loan> loans;
  std::map<RegionId, Region> regions;
  std::vector<Constraint> constraints;
};

void infer(core::Program &);
void print(const core::Program &, std::ostream &);
void printConstraints(const core::Program &, std::ostream &);
}
