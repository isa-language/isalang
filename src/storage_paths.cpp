#include "storage_paths.hpp"
#include <algorithm>
#include <functional>

namespace isac::storage::detail {
Location append(Location base, const Path &path) {
  base.path.insert(base.path.end(), path.begin(), path.end());
  return base;
}
Location field(Location base, uint32_t index) {
  return append(base, {{hir::ReferenceProjectionKind::Field, index}});
}
Location fieldOperation(Location base, const core::Inst &i) {
  for (auto index : i.fieldPath)
    base = field(std::move(base), index);
  return field(std::move(base), i.fieldIndex);
}
Location element(Location base, uint32_t index) {
  return append(base, {{hir::ReferenceProjectionKind::Element, index}});
}
bool within(const Location &location, const Location &base) {
  return location.kind == base.kind && location.id == base.id &&
         location.path.size() >= base.path.size() &&
         std::equal(base.path.begin(), base.path.end(), location.path.begin());
}
bool overlaps(const Location &a, const Location &b) {
  if (a.kind != b.kind || a.id != b.id)
    return false;
  for (size_t n = 0; n < std::min(a.path.size(), b.path.size()); ++n) {
    const auto &x = a.path[n], &y = b.path[n];
    if (x.kind != y.kind)
      return false;
    if (x.kind == hir::ReferenceProjectionKind::Element) {

      if (x.index != None && y.index != None && x.index != y.index)
        return false;
    } else if (x != y)
      return false;
  }
  return true;
}
bool covers(const Location &invalidation, const Location &borrowedSlot) {

  return invalidation.path.size() <= borrowedSlot.path.size() &&
         overlaps(invalidation, borrowedSlot);
}
void erase(State &state, const Location &base) {
  for (auto it = state.begin(); it != state.end();)
    if (within(it->first, base))
      it = state.erase(it);
    else
      ++it;
}
State subtree(const State &state, const Location &base) {
  State result;
  for (const auto &[position, candidates] : state)
    if (within(position, base)) {
      auto relative = position;
      relative.path.erase(relative.path.begin(), relative.path.begin() + base.path.size());
      relative.kind = Location::Value;
      relative.id = None;
      result.emplace(std::move(relative), candidates);
    }
  return result;
}
void install(State &state, const Location &base, const State &relative, bool weak) {
  if (!weak)
    erase(state, base);
  for (const auto &[position, candidates] : relative) {
    auto destination = append(base, position.path);
    if (!weak)
      state[destination] = candidates;
    else {
      auto &target = state[destination];
      target.versions.insert(candidates.versions.begin(), candidates.versions.end());
      target.unknown |= candidates.unknown;
    }
  }
}
std::vector<Leaf> leaves(const hir::Program &program, TypeId type) {
  std::vector<Leaf> result;
  std::set<TypeId> active;
  Path path;
  std::function<void(TypeId)> visit = [&](TypeId id) {
    if (program.heap(id)) {
      result.push_back({path, id});
      return;
    }
    const auto &shape = program.types.at(id);
    if (!active.insert(id).second)
      return;
    auto descend = [&](TypeId child, hir::ReferenceProjection projection) {
      path.push_back(projection);
      visit(child);
      path.pop_back();
    };
    if (shape.kind == hir::Type::Structure)
      for (uint32_t n = 0; n < shape.fields.size(); ++n)
        descend(shape.fields[n].type, {hir::ReferenceProjectionKind::Field, n});
    else if (shape.kind == hir::Type::Enum)
      for (const auto &variant : shape.variants)
        for (uint32_t n = 0; n < variant.payload.size(); ++n)
          descend(variant.payload[n],
                  {hir::ReferenceProjectionKind::VariantPayload, n, variant.id});
    active.erase(id);
  };
  visit(type);
  return result;
}
void printPath(const Path &path, std::ostream &out) {
  for (const auto &part : path) {
    if (part.kind == hir::ReferenceProjectionKind::Field)
      out << ".field[" << part.index << "]";
    else if (part.kind == hir::ReferenceProjectionKind::VariantPayload)
      out << ".variant[" << part.variant << "].payload[" << part.index << "]";
    else {
      out << ".element[";
      if (part.index == None)
        out << "*";
      else
        out << part.index;
      out << "]";
    }
  }
}
void Places::collect(const std::vector<core::Inst> &body) {
  for (const auto &i : body) {
    if (i.semanticPoint == None || i.semanticPoint >= instructions.size())
      fail(i.loc, DiagnosticCode::InvalidCore, "storage operation has no semantic CFG point");
    instructions[i.semanticPoint] = &i;
    if (i.result != None)
      producers[i.result] = &i;
    if (i.kind == core::Inst::BorrowShared || i.kind == core::Inst::BorrowUnique)
      aliases[i.offset] = &i;
    collect(i.body);
    collect(i.otherwise);
    collect(i.condition);
    collect(i.latch);
  }
}
Places::Places(const core::Program &p, const regions::FunctionGraph &g, const core::Function &f)
    : program(p), instructions(g.blocks.size()) {
  collect(f.body);
}
const core::Inst *Places::producer(ValueId value) const {
  auto it = producers.find(value);
  return it == producers.end() ? nullptr : it->second;
}
std::optional<Location> Places::resolve(SymbolId symbol, std::set<SymbolId> &seen) const {
  if (!seen.insert(symbol).second)
    return {};
  auto found = aliases.find(symbol);
  if (found == aliases.end())
    return Location{Location::Symbol, symbol};
  const auto &i = *found->second;
  auto base = resolve(i.slot, seen);
  if (!base)
    return {};
  auto leaf = program.semantics.referencePositionType(program.semantics.symbols.at(i.slot).type,
                                                      i.payloadPath);
  if (i.storedPayloadReference || (i.placeProjection == core::Inst::PlaceProjection::None &&
                                   i.args.empty() && leaf && program.semantics.reference(*leaf)))
    return {};
  if (!i.payloadPath.empty()) {
    return append(*base, i.payloadPath);
  }
  if (i.placeProjection == core::Inst::PlaceProjection::Field) {
    for (auto index : i.fieldPath)
      *base = field(*base, index);
    return base;
  }
  if (i.placeProjection == core::Inst::PlaceProjection::Index)
    return element(*base);
  if (!i.args.empty()) {
    const auto *call = producer(i.args[0]);
    if (!call || call->kind != core::Inst::ContainerCall ||
        !containerBorrowIntrinsic(call->intrinsicId))
      return {};

    if (containerSliceIntrinsic(call->intrinsicId))
      return base;
    if (call->intrinsicId == IntrinsicId::Box_get || call->intrinsicId == IntrinsicId::Box_get_mut)
      return element(*base, 0);
    return {};
  }
  return base;
}
std::optional<Location> Places::get(SymbolId symbol) const {
  std::set<SymbolId> seen;
  return resolve(symbol, seen);
}
std::optional<TypeId> Places::type(const Location &location) const {
  TypeId current;
  if (location.kind == Location::Symbol)
    current = program.semantics.symbols.at(location.id).type;
  else {
    auto *definition = producer(location.id);
    if (!definition)
      return {};
    current = definition->type;
  }
  for (const auto &part : location.path) {
    const auto &shape = program.semantics.types.at(current);
    if (part.kind == hir::ReferenceProjectionKind::Field) {
      if (shape.kind != hir::Type::Structure || program.semantics.heap(current) ||
          part.index >= shape.fields.size())
        return {};
      current = shape.fields[part.index].type;
    } else if (part.kind == hir::ReferenceProjectionKind::Element) {
      if (!program.semantics.dynamicContainer(current) && shape.kind != hir::Type::Array &&
          shape.kind != hir::Type::Slice)
        return {};
      current = shape.element;
    } else {
      if (shape.kind != hir::Type::Enum || part.variant >= shape.variants.size() ||
          part.index >= shape.variants[part.variant].payload.size())
        return {};
      current = shape.variants[part.variant].payload[part.index];
    }
  }
  return current;
}
}
