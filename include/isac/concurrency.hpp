#pragma once
#include "isac/core.hpp"

namespace isac::concurrency {
enum class Capability { Send, Sync };
enum class Requirement {
  AllocatorContract,
  RuntimeTypeContract,
  ProtocolContract,
  ForeignContract,
  RecursiveContract,
  UnsupportedType
};

struct TypeObligation {
  Requirement requirement;
  TypeId type;
  Capability capability;
  std::vector<regions::Projection> path;
};
struct Assessment {
  bool structural = true;
  bool needsRegion = false;
  std::set<Requirement> contracts;
  std::vector<regions::Projection> witness;
  std::vector<TypeObligation> obligations;
  void merge(const Assessment &);
};

constexpr Capability referentCapability(Access reference, Capability requested) {
  return requested == Capability::Send && reference == Access::Unique ? Capability::Send
                                                                      : Capability::Sync;
}
struct LiveValue {
  ValueId value;
  TypeId type;
  bool reference;
  hir::RegionArgs regions;
};
enum class Side { Before, After };
struct PointFacts {
  FunctionId function;
  regions::ProgramPoint point;
  Side side;
  Loc origin;
  std::set<SymbolId> liveSymbols;
  std::vector<LiveValue> liveValues;

  std::vector<regions::LoanId> loanDemands;

  bool carriersObserved = false;
  carriers::State carriers;
};

struct SpanFacts {
  SymbolId view;
  TypeId element;
  Access capability;
  std::set<SymbolId> origins;
  hir::RegionArgs regions;
  bool callerOwned;
  std::vector<regions::LoanId> loans;
  std::map<regions::LoanId, storage::LoanStorage> storage;
};

enum class CaptureOwnerRelation { External, PossibleInternal };
struct CaptureOwnerDependency {
  SymbolId owner;
  bool origin = false;
  std::set<RegionId> upperRegions;
  std::optional<regions::StorageBound> bound;
  CaptureOwnerRelation relation = CaptureOwnerRelation::External;
  std::set<storage::Location> proposedTransfers;
};
struct CaptureLoanDependency {
  regions::Loan loan;

  bool demanded = false;
  std::optional<storage::LoanStorage> storage;
  std::vector<regions::ProjectionStorageBound> projections;
};
struct CaptureReferenceDependency {
  storage::Location position;
  Access capability = Access::Shared;
  bool callerOwned = false, originsObserved = false, regionsObserved = false;
  std::set<SymbolId> origins;

  std::optional<SymbolId> parent;

  std::set<SymbolId> parentDependencies;

  hir::RegionArgs regions;
  std::optional<carriers::Binding> carrier;
  std::vector<CaptureOwnerDependency> owners;
  std::vector<CaptureLoanDependency> loans;
};
struct CaptureRegionDependencies {
  FunctionId function;
  regions::ProgramPoint point;
  core::StateSide side;
  StateId entryState;

  std::set<storage::Location> proposedTransfers;

  std::optional<carriers::State> carriers;
  std::optional<storage::State> storage;
  std::optional<storage::SlotState> slots;
  std::vector<CaptureReferenceDependency> references;
};
class Analysis {
  const core::Verified &verified;
  std::map<std::pair<TypeId, Capability>, Assessment> cache;
  std::set<std::pair<TypeId, Capability>> visiting;
  Assessment assess(TypeId, Capability);

public:
  explicit Analysis(const core::Verified &p) : verified(p) {}
  Analysis(core::Verified &&) = delete;
  Analysis(const core::Verified &&) = delete;
  Assessment type(TypeId id, Capability capability) {
    return assess(id, capability);
  }
  Assessment symbol(SymbolId, Capability);
  PointFacts point(FunctionId, regions::ProgramPoint, Side = Side::Before) const;
  std::vector<const core::CaptureState *> states(FunctionId, regions::ProgramPoint,
                                                 Side = Side::Before) const;
  SpanFacts span(const core::CaptureState &, SymbolId) const;

  CaptureRegionDependencies
  captureRegions(const core::CaptureState &,
                 const std::set<storage::Location> &proposedTransfers = {}) const;
};
void print(const core::Verified &, std::ostream &);
void printCaptures(const core::Verified &, std::ostream &);
}
