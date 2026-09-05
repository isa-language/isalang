#pragma once
#include "isac/core.hpp"

namespace isac::storage::detail {
using Path = hir::ReferencePath;
Location append(Location, const Path &);
Location field(Location, uint32_t);
Location element(Location, uint32_t = None);
bool within(const Location &, const Location &);
bool overlaps(const Location &, const Location &);
bool covers(const Location &invalidation, const Location &borrowedSlot);
void erase(State &, const Location &);
State subtree(const State &, const Location &);
void install(State &, const Location &, const State &, bool weak = false);
void printPath(const Path &, std::ostream &);

struct Leaf {
  Path path;
  TypeId type;
};

std::vector<Leaf> leaves(const hir::Program &, TypeId);
void elementBounds(const core::Program &, const regions::FunctionGraph &, const core::Function &,
                   Facts &);
void slotGenerations(const core::Program &, const regions::FunctionGraph &, const core::Function &,
                     Facts &);

class Places {
  const core::Program &program;
  std::map<SymbolId, const core::Inst *> aliases;
  std::map<ValueId, const core::Inst *> producers;
  std::optional<Location> resolve(SymbolId, std::set<SymbolId> &) const;
  void collect(const std::vector<core::Inst> &);

public:
  std::vector<const core::Inst *> instructions;
  Places(const core::Program &, const regions::FunctionGraph &, const core::Function &);
  std::optional<Location> get(SymbolId) const;
  std::optional<TypeId> type(const Location &) const;
  const core::Inst *producer(ValueId) const;
};
}
