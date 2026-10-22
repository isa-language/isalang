#pragma once
#include "isac/storage.hpp"

namespace isac::carriers {
using CarrierId = uint32_t;

struct Identity {
  CarrierId id;
  uint32_t point = None;
  SymbolId parameter = None;
  hir::ReferencePath path;
  bool operator==(const Identity &) const = default;
};
struct Binding {
  std::set<CarrierId> instances;
  std::set<SymbolId> origins;
  std::set<RegionId> regions;
  Access capability = Access::Shared;
  SymbolId parent = None;

  bool present = false;
  bool operator==(const Binding &) const = default;
};
using State = std::map<storage::Location, Binding>;

State extract(const State &, const storage::Location &);
void erase(State &, const storage::Location &);
void store(State &, const storage::Location &, const State &, bool weak = false);
void join(State &, const State &);

enum class EndKind { Transfer, Consume, Drop, Cleanup, Return, DeadValue, BorrowEnd };
struct End {
  storage::Location position;
  Binding binding;
  EndKind kind;
  bool operator==(const End &) const = default;
};
struct Transition {
  State before, after;
  std::vector<End> ends;
  bool operator==(const Transition &) const = default;
};
struct SlotAttachment {
  storage::Location carrier;
  storage::SlotWitness slot;
  bool after;
  bool operator==(const SlotAttachment &) const = default;
};
struct ReferentSlot {
  storage::Location carrier;
  uint32_t loan;
  storage::SlotWitness slot;
  bool after;
  bool operator==(const ReferentSlot &) const = default;
};
struct Facts {
  std::vector<Identity> identities;
  std::map<uint32_t, Transition> transitions;

  std::map<uint32_t, std::vector<SlotAttachment>> slots;
  std::map<uint32_t, std::vector<ReferentSlot>> referents;
  bool operator==(const Facts &) const = default;
};

void infer(core::Program &);
void validate(const core::Program &);
void print(const Facts &, std::ostream &);
}
