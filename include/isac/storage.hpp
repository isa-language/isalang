#pragma once
#include "isac/hir.hpp"
#include <compare>
#include <map>
#include <ostream>
#include <optional>
#include <set>

namespace isac::core {
struct Program;
}
namespace isac::storage {
using StorageId = uint32_t;
using EpochId = uint32_t;

enum class OriginKind { Parameter, Local };
enum class Mutation { None, Elements, Reallocate, Unknown, Release };
struct IntrinsicContract {
  bool creates = false;
  Mutation mutation = Mutation::None;
  bool consumes = false;
};
IntrinsicContract intrinsicContract(IntrinsicId);
struct Identity {
  StorageId id;
  OriginKind kind;
  TypeId type;
  uint32_t point = None;
  SymbolId parameter = None;
  Loc origin;
  hir::ReferencePath path = {};
  bool operator==(const Identity &) const;
};
struct Version {
  StorageId storage;

  EpochId epoch = None;
  auto operator<=>(const Version &) const = default;
};
struct Candidates {
  std::set<Version> versions;

  bool unknown = false;
  bool operator==(const Candidates &) const = default;
};
struct Location {
  enum Kind { Symbol, Value } kind;
  uint32_t id;
  hir::ReferencePath path = {};
  auto operator<=>(const Location &) const = default;
};
using State = std::map<Location, Candidates>;
struct Epoch {
  EpochId id;
  uint32_t point;
  Mutation mutation;
  std::set<SymbolId> owners;
  Loc origin;
  std::set<Location> places = {};
  bool operator==(const Epoch &) const;
};
struct Transition {
  State before, after;
  bool operator==(const Transition &) const = default;
};
using SlotId = uint32_t;

struct SlotIdentity {
  SlotId id;
  uint32_t point = None;
  SymbolId parameter = None;
  hir::ReferencePath path = {};
  bool operator==(const SlotIdentity &) const = default;
};
struct SlotVersion {
  SlotId content;
  uint32_t generation = None;
  auto operator<=>(const SlotVersion &) const = default;
};
struct SlotCandidates {
  std::set<SlotVersion> versions;
  bool unknown = false;

  bool initialized = false;
  bool operator==(const SlotCandidates &) const = default;
};
using SlotState = std::map<Location, SlotCandidates>;
struct SlotTransition {
  SlotState before, after;
  bool operator==(const SlotTransition &) const = default;
};
struct SlotWitness {
  Location position;
  SlotCandidates candidates;
  bool operator==(const SlotWitness &) const = default;
};
struct LoanStorage {

  bool backing = false;
  Candidates candidates{{}, true};
  std::optional<Location> place;
  std::optional<SlotWitness> slot = {};
  bool operator==(const LoanStorage &) const = default;
};
enum class ElementSelection { Index, First, Last, Whole, BoxValue, Insertion };
enum class ElementGuard { OptionSome, BoundsPanic, WholeView, InitializedBox };
struct ElementBound {
  uint32_t point;
  Location owner;
  IntrinsicId intrinsic;
  ElementSelection selection;
  ElementGuard guard;
  ValueId indexValue = None;
  std::optional<uint64_t> index = {}, length = {};

  std::optional<bool> inBounds = {};
  bool borrowed = false;
  Access capability = Access::Own;
  hir::ReferencePath resultPath = {};
  ValueId resultValue = None;
  hir::RegionArgs resultRegions = {};
  std::optional<SlotWitness> slot = {};
  bool operator==(const ElementBound &) const = default;
};
struct Facts {
  std::vector<Identity> identities;
  std::vector<Epoch> epochs;

  std::map<uint32_t, Transition> transitions;
  std::map<uint32_t, LoanStorage> loans;
  std::vector<ElementBound> elements;
  std::vector<SlotIdentity> slots;
  std::map<uint32_t, SlotTransition> slotTransitions;

  std::map<uint32_t, std::set<Location>> slotInvalidations;
  bool operator==(const Facts &) const = default;
};

void infer(core::Program &);
void validate(const core::Program &);
void print(const Facts &, std::ostream &);
}
