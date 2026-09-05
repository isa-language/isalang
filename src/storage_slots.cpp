#include "storage_paths.hpp"
#include <algorithm>
#include <deque>

namespace isac::storage::detail {
namespace {
using I = core::Inst;
Location symbol(SymbolId id) {
  return {Location::Symbol, id};
}
Location value(ValueId id) {
  return {Location::Value, id};
}

void erase(SlotState &state, const Location &base, bool childrenOnly = false) {
  for (auto it = state.begin(); it != state.end();)
    if (within(it->first, base) && (!childrenOnly || it->first.path.size() > base.path.size()))
      it = state.erase(it);
    else
      ++it;
}
SlotState subtree(const SlotState &state, const Location &base, bool childrenOnly = false) {
  SlotState result;
  for (const auto &[place, candidates] : state)
    if (within(place, base) && (!childrenOnly || place.path.size() > base.path.size())) {
      auto path = place.path;
      path.erase(path.begin(), path.begin() + base.path.size());
      result.emplace(Location{Location::Value, None, std::move(path)}, candidates);
    }
  return result;
}
void install(SlotState &state, const Location &base, const SlotState &relative) {
  erase(state, base);
  for (const auto &[place, candidates] : relative)
    state[append(base, place.path)] = candidates;
}
void merge(SlotCandidates &to, const SlotCandidates &from) {
  to.versions.insert(from.versions.begin(), from.versions.end());
  to.unknown |= from.unknown;
  to.initialized &= from.initialized;
}

void join(SlotState &to, const SlotState &from) {
  for (auto &[place, candidates] : to)
    if (!from.contains(place))
      candidates.initialized = false;
  for (const auto &[place, candidates] : from) {
    auto [position, added] = to.emplace(place, candidates);
    if (added)
      position->second.initialized = false;
    else
      merge(position->second, candidates);
  }
}
void renew(SlotCandidates &candidates, uint32_t point) {
  std::set<SlotVersion> next;
  for (const auto &version : candidates.versions)
    next.insert({version.content, point});
  candidates.versions = std::move(next);
}

class Slots {
  const core::Program &p;
  const regions::FunctionGraph &graph;
  const core::Function &function;
  Places places;
  Facts &facts;
  std::map<uint32_t, SlotId> births;
  std::map<uint32_t, std::vector<const ElementBound *>> bounds;
  std::map<uint32_t, const Epoch *> epochs;
  std::map<ValueId, uint64_t> indices;
  std::set<uint32_t> queriedIndices;
  std::vector<SlotState> in, out;
  std::vector<bool> reached;

  SlotState normalize(SlotState state) const {
    SlotState result;
    for (auto &[position, candidates] : state) {
      Location destination{position.kind, position.id};
      bool summarized = false;
      for (auto projection : position.path) {
        auto type = places.type(destination);
        if (projection.kind == hir::ReferenceProjectionKind::Element && type &&
            p.semantics.types.at(*type).representation == hir::Representation::GenericVec &&
            (projection.index == None || !queriedIndices.contains(projection.index))) {
          projection.index = None;
          summarized = true;
        }
        destination.path.push_back(projection);
      }
      if (summarized) {

        candidates.versions.clear();
        candidates.unknown = true;
        candidates.initialized = false;
      }
      auto [it, added] = result.emplace(std::move(destination), candidates);
      if (!added)
        merge(it->second, candidates);
    }
    return result;
  }

  std::optional<uint32_t> index(const I &i, size_t n = 0) const {
    if (n >= i.args.size())
      return {};
    auto it = indices.find(i.args[n]);
    if (it == indices.end() || it->second >= None)
      return {};
    return static_cast<uint32_t>(it->second);
  }
  std::optional<uint64_t> length(uint32_t point) const {
    auto found = bounds.find(point);
    if (found == bounds.end() || found->second.empty())
      return {};
    return found->second.front()->length;
  }
  std::optional<uint32_t> selected(const I &i) const {
    auto found = bounds.find(i.semanticPoint);
    if (found == bounds.end() || found->second.empty())
      return {};
    auto n = found->second.front()->index;
    return n && *n < None ? std::optional<uint32_t>(*n) : std::nullopt;
  }
  SlotCandidates fresh(const I &i) const {
    return {{{births.at(i.semanticPoint), i.semanticPoint}}, false, true};
  }

  void opaque(SlotState &state, TypeId type, const Location &base) const {
    for (const auto &leaf : leaves(p.semantics, type)) {
      const auto &shape = p.semantics.types.at(leaf.type);
      auto parent = append(base, leaf.path);
      if (shape.representation == hir::Representation::GenericBox) {
        bool conditional = std::any_of(leaf.path.begin(), leaf.path.end(), [](const auto &part) {
          return part.kind == hir::ReferenceProjectionKind::VariantPayload;
        });
        state[element(parent, 0)] = {{}, true, !conditional};
      } else if (shape.representation == hir::Representation::GenericVec)
        state[element(parent)] = {{}, true, false};
    }
  }

  void collapse(SlotState &state, const Location &base, uint32_t point) const {
    auto children = subtree(state, base, true);
    erase(state, base, true);
    for (auto &[relative, candidates] : children) {
      if (relative.path.front().kind != hir::ReferenceProjectionKind::Element)
        continue;
      auto destination = relative;
      destination.path.front().index = None;
      candidates.unknown = true;
      candidates.initialized = false;
      if (relative.path.size() == 1)
        renew(candidates, point);
      auto [it, added] = state.emplace(append(base, destination.path), candidates);
      if (!added)
        merge(it->second, candidates);
    }
    auto &summary = state[element(base)];
    summary.unknown = true;
    summary.initialized = false;
  }

  void shift(SlotState &state, const Location &base, uint32_t start, int delta,
             uint32_t point) const {
    auto children = subtree(state, base, true);
    erase(state, base, true);
    for (auto &[relative, candidates] : children) {
      auto path = relative.path;
      auto &part = path.front();
      if (part.kind == hir::ReferenceProjectionKind::Element && part.index >= start) {
        if (part.index == None || (delta > 0 && part.index == None - 1)) {
          part.index = None;
          candidates.unknown = true;
          candidates.initialized = false;
        } else
          part.index = static_cast<uint32_t>(int64_t(part.index) + delta);
        if (relative.path.size() == 1)
          renew(candidates, point);
      }
      auto [it, added] = state.emplace(append(base, path), candidates);
      if (!added)
        merge(it->second, candidates);
    }
  }
  void put(SlotState &state, const Location &slot, const I &i, const SlotState &child) const {
    install(state, slot, child);
    state[slot] = fresh(i);
  }
  SlotState take(const SlotState &state, const Location &base, std::optional<uint32_t> n) const {
    if (n) {
      auto result = subtree(state, element(base, *n), true);

      auto summarized = subtree(state, element(base), true);
      if (!summarized.empty())
        join(result, summarized);
      return result;
    }
    SlotState result;
    for (const auto &[relative, candidates] : subtree(state, base, true)) {
      auto path = relative.path;
      if (path.front().kind != hir::ReferenceProjectionKind::Element || path.size() == 1)
        continue;
      path.erase(path.begin());
      auto possible = candidates;
      possible.unknown = true;
      possible.initialized = false;
      auto [it, added] = result.emplace(Location{Location::Value, None, std::move(path)}, possible);
      if (!added)
        merge(it->second, possible);
    }
    return result;
  }
  std::set<Location> invalidations(const I &i) const {
    std::set<Location> result;
    auto base = i.slot == None ? std::nullopt : places.get(i.slot);
    if ((i.kind == I::FieldStore || i.kind == I::FieldTake || i.kind == I::FieldDrop) && base)
      result.insert(field(*base, i.fieldIndex));
    if (i.kind == I::PayloadTake && base)
      result.insert(append(*base, i.payloadPath));
    if (i.kind == I::Move || i.kind == I::Drop)
      result.insert(symbol(i.slot));
    if (i.kind == I::ContainerCall && base) {
      auto id = i.intrinsicId;
      if (id == IntrinsicId::Box_into_inner)
        result.insert(element(*base, 0));
      else if (id == IntrinsicId::Vec_clear || id == IntrinsicId::Vec_remove ||
               id == IntrinsicId::Vec_insert)
        result.insert(element(*base));
      else if (id == IntrinsicId::Vec_pop || id == IntrinsicId::Vec_replace) {
        if (!(id == IntrinsicId::Vec_pop && length(i.semanticPoint) == 0))
          result.insert(element(*base, selected(i).value_or(None)));
      } else if (id == IntrinsicId::Vec_swap && (!index(i) || index(i) != index(i, 1))) {
        result.insert(element(*base, index(i).value_or(None)));
        result.insert(element(*base, index(i, 1).value_or(None)));
      }
    }
    if (auto epoch = epochs.find(i.semanticPoint); epoch != epochs.end())
      if (epoch->second->mutation == Mutation::Unknown ||
          epoch->second->mutation == Mutation::Release ||
          epoch->second->mutation == Mutation::Reallocate)
        for (const auto &target : epoch->second->places) {
          auto type = places.type(target);
          if (!type || !p.semantics.dynamicContainer(*type))
            continue;

          auto box = p.semantics.types.at(*type).representation == hir::Representation::GenericBox;
          result.insert(element(target, box ? 0 : None));
        }
    if ((i.kind == I::Call || i.kind == I::ProtocolTransition || i.kind == I::ContainerCall) &&
        p.semantics.functions.at(i.function).consumesSelf && base)
      result.insert(*base);
    for (auto local : i.cleanup)
      result.insert(symbol(local));
    if (i.kind == I::ScopeEnd)
      for (auto local : i.locals)
        result.insert(symbol(local));
    return result;
  }
  SlotState transfer(SlotState state, const I &i) const {
    auto destination = value(i.result);
    auto base = i.slot == None ? std::nullopt : places.get(i.slot);
    if (i.kind == I::Move)
      install(state, destination, subtree(state, symbol(i.slot)));
    else if (i.kind == I::FieldTake && base)
      install(state, destination, subtree(state, field(*base, i.fieldIndex)));
    else if (i.kind == I::PayloadTake && base)
      install(state, destination, subtree(state, append(*base, i.payloadPath)));
    else if (i.kind == I::StructInit || i.kind == I::EnumConstruct) {
      for (size_t n = 0; n < i.args.size(); ++n)
        install(state,
                i.kind == I::StructInit ? field(destination, i.fieldPath[n])
                                        : append(destination, hir::payloadPosition(i.offset, n)),
                subtree(state, value(i.args[n])));
    } else if (i.kind == I::ContainerCall && i.intrinsicId == IntrinsicId::Box_new_generic)
      put(state, element(destination, 0), i, subtree(state, value(i.args.at(0))));
    else if (i.kind == I::ContainerCall && base && i.intrinsicId == IntrinsicId::Box_into_inner) {
      install(state, destination, subtree(state, element(*base, 0), true));
      erase(state, *base, true);
    } else if (i.kind == I::ContainerCall && base) {
      auto id = i.intrinsicId;
      auto point = i.semanticPoint;
      auto count = length(point);
      if (id == IntrinsicId::Vec_pop || id == IntrinsicId::Vec_remove ||
          id == IntrinsicId::Vec_replace) {
        auto result = count == 0 && id == IntrinsicId::Vec_pop ? SlotState{}
                                                               : take(state, *base, selected(i));
        install(state,
                id == IntrinsicId::Vec_pop ? append(destination, hir::payloadPosition(0, 0))
                                           : destination,
                result);
      }
      if (id == IntrinsicId::Vec_push || id == IntrinsicId::Vec_insert ||
          id == IntrinsicId::Vec_replace) {
        auto n = id == IntrinsicId::Vec_push
                     ? (count && *count < None ? std::optional<uint32_t>(*count) : std::nullopt)
                     : index(i);
        auto child = subtree(state, value(i.args.back()));
        if (!n || !count) {
          collapse(state, *base, point);
          auto incoming = fresh(i);
          incoming.unknown = true;
          incoming.initialized = false;
          merge(state[element(*base)], incoming);
          for (auto &[relative, candidates] : child) {
            candidates.unknown = true;
            candidates.initialized = false;
            auto [it, added] = state.emplace(append(element(*base), relative.path), candidates);
            if (!added)
              merge(it->second, candidates);
          }
        } else {
          if (id == IntrinsicId::Vec_insert)
            shift(state, *base, *n, 1, point);
          put(state, element(*base, *n), i, child);
        }
      } else if (id == IntrinsicId::Vec_pop || id == IntrinsicId::Vec_remove) {
        auto n = selected(i);
        if (n && count) {
          erase(state, element(*base, *n));
          if (id == IntrinsicId::Vec_remove)
            shift(state, *base, *n + 1, -1, point);
          if (*count <= 1)
            erase(state, *base, true);
        } else if (count != 0)
          collapse(state, *base, point);
      } else if (id == IntrinsicId::Vec_clear)
        erase(state, *base, true);
      else if (id == IntrinsicId::Vec_swap) {
        auto a = index(i), b = index(i, 1);
        if (a && b && count) {
          if (*a != *b) {
            auto left = subtree(state, element(*base, *a)),
                 right = subtree(state, element(*base, *b));
            if (left.contains(value(None)))
              renew(left.at(value(None)), point);
            if (right.contains(value(None)))
              renew(right.at(value(None)), point);
            install(state, element(*base, *a), right);
            install(state, element(*base, *b), left);
          }
        } else
          collapse(state, *base, point);
      }
    } else if ((i.kind == I::Call || i.kind == I::ProtocolTransition) && i.result != None &&
               !p.semantics.functions.at(i.function).returnsReference())
      opaque(state, i.type, destination);

    if (auto epoch = epochs.find(i.semanticPoint); epoch != epochs.end())
      for (const auto &target : epoch->second->places) {
        if (epoch->second->mutation != Mutation::Unknown &&
            epoch->second->mutation != Mutation::Release)
          continue;
        erase(state, target, true);
        if (epoch->second->mutation == Mutation::Unknown)
          if (auto type = places.type(target))
            opaque(state, *type, target);
      }
    if (i.kind == I::Bind) {
      install(state, symbol(i.slot), subtree(state, value(i.args.at(0))));
      erase(state, value(i.args.at(0)));
    }
    if ((i.kind == I::FieldStore || i.kind == I::FieldTake || i.kind == I::FieldDrop) && base) {
      auto target = field(*base, i.fieldIndex);
      if (i.kind == I::FieldStore)
        install(state, target, subtree(state, value(i.args.at(0))));
      else
        erase(state, target);
    }
    if (i.kind == I::PayloadTake && base)
      erase(state, append(*base, i.payloadPath));
    bool consumes = i.kind == I::Move || i.kind == I::Drop;
    if (i.kind == I::Call || i.kind == I::ProtocolTransition || i.kind == I::ContainerCall)
      consumes |= p.semantics.functions.at(i.function).consumesSelf;
    if (consumes)
      erase(state, symbol(i.slot));
    for (auto local : i.cleanup)
      erase(state, symbol(local));
    if (i.kind == I::ScopeEnd)
      for (auto local : i.locals)
        erase(state, symbol(local));
    for (auto it = state.begin(); it != state.end();)
      if (it->first.kind == Location::Value &&
          !graph.blocks[i.semanticPoint].valuesOut.contains(it->first.id))
        it = state.erase(it);
      else
        ++it;
    if (i.kind == I::Return)
      state.clear();
    return normalize(std::move(state));
  }

public:
  Slots(const core::Program &program, const regions::FunctionGraph &g, const core::Function &f,
        Facts &result)
      : p(program), graph(g), function(f), places(p, g, f), facts(result), in(g.blocks.size()),
        out(g.blocks.size()), reached(g.blocks.size()) {
    for (const auto &bound : facts.elements) {
      bounds[bound.point].push_back(&bound);
      if (bound.indexValue != None && bound.index)
        indices[bound.indexValue] = *bound.index;
      if (bound.selection != ElementSelection::Insertion && bound.index && *bound.index < None)
        queriedIndices.insert(*bound.index);
    }
    for (const auto &epoch : facts.epochs)
      epochs[epoch.point] = &epoch;
  }
  void run() {
    SlotState entry;
    for (auto parameter : p.semantics.functions.at(function.id).params) {
      opaque(entry, p.semantics.symbols.at(parameter).type, symbol(parameter));
      for (auto &[place, candidates] : entry) {
        if (place.id != parameter || !candidates.initialized)
          continue;
        SlotId id = facts.slots.size();
        facts.slots.push_back({id, None, parameter, place.path});
        candidates = {{{id, None}}, false, true};
      }
    }
    for (auto *i : places.instructions) {
      if (!i || !graph.blocks[i->semanticPoint].reachable || i->kind != I::ContainerCall)
        continue;
      if (i->intrinsicId != IntrinsicId::Box_new_generic &&
          i->intrinsicId != IntrinsicId::Vec_push && i->intrinsicId != IntrinsicId::Vec_insert &&
          i->intrinsicId != IntrinsicId::Vec_replace)
        continue;
      SlotId id = facts.slots.size();
      facts.slots.push_back({id, i->semanticPoint});
      births[i->semanticPoint] = id;
    }

    std::vector<std::vector<uint32_t>> predecessors(graph.blocks.size());
    for (const auto &block : graph.blocks)
      if (block.reachable)
        for (auto next : block.successors)
          predecessors[next].push_back(block.id);
    std::vector<bool> queued(graph.blocks.size());
    std::deque<uint32_t> work{graph.entry};
    queued[graph.entry] = true;
    while (!work.empty()) {
      auto point = work.front();
      work.pop_front();
      queued[point] = false;
      std::optional<SlotState> incoming;
      if (point == graph.entry)
        incoming = entry;
      for (auto previous : predecessors[point])
        if (reached[previous]) {
          if (!incoming)
            incoming = out[previous];
          else
            join(*incoming, out[previous]);
        }
      if (!incoming)
        continue;
      in[point] = std::move(*incoming);
      auto *i = places.instructions[point];
      auto next = i ? transfer(in[point], *i) : in[point];
      if (reached[point] && out[point] == next)
        continue;
      out[point] = std::move(next);
      reached[point] = true;
      for (auto successor : graph.blocks[point].successors)
        if (!queued[successor]) {
          queued[successor] = true;
          work.push_back(successor);
        }
    }
    for (auto &bound : facts.elements) {
      if (bound.selection == ElementSelection::Whole ||
          bound.selection == ElementSelection::Insertion)
        continue;
      auto place =
          element(bound.owner, bound.index && *bound.index < None ? uint32_t(*bound.index) : None);
      SlotCandidates candidates{{}, true, false};
      if (bound.inBounds == false)
        candidates = {};
      else {
        auto found = in[bound.point].find(place);
        if (found != in[bound.point].end())
          candidates = found->second;
        auto summary = in[bound.point].find(element(bound.owner));
        if (summary != in[bound.point].end() && summary->first != place)
          merge(candidates, summary->second);
        if (!bound.index || bound.inBounds != true)
          candidates.initialized = false;
      }
      bound.slot = SlotWitness{place, std::move(candidates)};
    }
    for (const auto &loan : graph.loans) {
      auto *creation = places.instructions[loan.created.block];
      if (!creation || creation->args.empty() || !facts.loans.at(loan.id).backing)
        continue;
      for (const auto &bound : facts.elements)
        if (bound.borrowed && bound.resultValue == creation->args[0] && bound.slot)
          facts.loans.at(loan.id).slot = bound.slot;
    }
    for (const auto &block : graph.blocks) {
      if (!reached[block.id])
        continue;
      if (auto *i = places.instructions[block.id]) {
        auto invalidated = invalidations(*i);
        std::erase_if(invalidated, [&](const auto &target) {
          return std::none_of(in[block.id].begin(), in[block.id].end(),
                              [&](const auto &entry) { return overlaps(target, entry.first); });
        });
        if (!invalidated.empty())
          facts.slotInvalidations.emplace(block.id, std::move(invalidated));
      }
      if (facts.transitions.contains(block.id) || bounds.contains(block.id))
        if (!in[block.id].empty() || !out[block.id].empty())
          facts.slotTransitions.emplace(block.id, SlotTransition{in[block.id], out[block.id]});
    }
  }
};
}
void slotGenerations(const core::Program &p, const regions::FunctionGraph &g,
                     const core::Function &f, Facts &facts) {
  Slots(p, g, f, facts).run();
}
}
