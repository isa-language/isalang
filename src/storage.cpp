#include "isac/storage.hpp"
#include "isac/core.hpp"
#include "storage_paths.hpp"
#include <deque>
#include <functional>

namespace isac::storage {
namespace {
bool sameOrigin(const Loc &a, const Loc &b) {
  return a.file == b.file && a.line == b.line && a.column == b.column &&
         a.span.file == b.span.file && a.span.start == b.span.start && a.span.end == b.span.end;
}
}
bool Identity::operator==(const Identity &b) const {
  return id == b.id && kind == b.kind && type == b.type && point == b.point &&
         parameter == b.parameter && path == b.path && sameOrigin(origin, b.origin);
}
bool Epoch::operator==(const Epoch &b) const {
  return id == b.id && point == b.point && mutation == b.mutation && owners == b.owners &&
         places == b.places && sameOrigin(origin, b.origin);
}
IntrinsicContract intrinsicContract(IntrinsicId id) {

  switch (id) {
  case IntrinsicId::String_new:
  case IntrinsicId::String_from:
  case IntrinsicId::box_new:
  case IntrinsicId::buffer_new:
  case IntrinsicId::Box_new_generic:
  case IntrinsicId::Vec_new:
  case IntrinsicId::Vec_with_capacity:
    return {true, Mutation::None, false};
  case IntrinsicId::Box_into_inner:
    return {false, Mutation::Elements, true};
  case IntrinsicId::Vec_push:
  case IntrinsicId::Vec_insert:
    return {false, Mutation::Reallocate, false};
  case IntrinsicId::Vec_clear:
  case IntrinsicId::Vec_pop:
  case IntrinsicId::Vec_remove:
  case IntrinsicId::Vec_replace:
  case IntrinsicId::Vec_swap:
  case IntrinsicId::String_clear:
    return {false, Mutation::Elements, false};
  case IntrinsicId::String_char_at:
  case IntrinsicId::String_len:
  case IntrinsicId::String_capacity:
  case IntrinsicId::String_is_empty:
  case IntrinsicId::String_as_str:
  case IntrinsicId::String_is_char_boundary:
    return {};
  default:
    if (containerBorrowIntrinsic(id) || id == IntrinsicId::Vec_len ||
        id == IntrinsicId::Vec_capacity)
      return {};

    if (intrinsicInfo(id).lowering == IntrinsicLowering::StringRecord)
      return {false, Mutation::Reallocate, false};
    if (intrinsicInfo(id).lowering == IntrinsicLowering::ContainerStorage)
      return {false, Mutation::Unknown, false};
    return {};
  }
}
namespace {
using I = core::Inst;
using Graph = regions::FunctionGraph;
Location symbol(SymbolId id) {
  return {Location::Symbol, id};
}
Location value(ValueId id) {
  return {Location::Value, id};
}
bool merge(Candidates &to, const Candidates &from) {
  auto size = to.versions.size();
  auto unknown = to.unknown;
  to.versions.insert(from.versions.begin(), from.versions.end());
  to.unknown |= from.unknown;
  return size != to.versions.size() || unknown != to.unknown;
}
bool merge(State &to, const State &from) {
  bool changed = false;
  for (const auto &[location, candidates] : from) {
    auto [position, added] = to.try_emplace(location);
    changed |= merge(position->second, candidates) || added;
  }
  return changed;
}
Candidates lookup(const State &state, Location location) {
  auto found = state.find(location);
  return found == state.end() ? Candidates{{}, true} : found->second;
}

class Analysis {
  const core::Program &p;
  const Graph &graph;
  const core::Function &function;
  detail::Places places;
  const std::vector<const I *> &instructions;
  mutable std::map<TypeId, std::vector<detail::Leaf>> schemas;
  std::map<ValueId, SymbolId> referenceArguments;
  std::set<ValueId> backingResults;
  std::map<uint32_t, StorageId> creations;
  std::map<uint32_t, EpochId> mutations;
  std::map<uint32_t, uint64_t> elementLengths;
  std::map<FunctionId, const core::Function *> definitions;
  Facts facts;

  bool heap(TypeId type) const {
    return p.semantics.heap(type);
  }
  const std::vector<detail::Leaf> &schema(TypeId type) const {
    auto [it, added] = schemas.try_emplace(type);
    if (added)
      it->second = detail::leaves(p.semantics, type);
    return it->second;
  }
  bool ownedResult(const I &i) const {
    if (i.result == None || schema(i.type).empty())
      return false;
    if (i.kind == I::Call || i.kind == I::ContainerCall)
      return !p.semantics.functions.at(i.function).returnsReference();
    return i.kind != I::ReferenceArgument && i.kind != I::Read && i.kind != I::Copy;
  }
  IntrinsicContract contract(const I &i) const {
    if (i.kind == I::ContainerCall || i.kind == I::StringCall || i.kind == I::InputCall)
      return intrinsicContract(i.intrinsicId);
    if (i.kind == I::Call || i.kind == I::ProtocolTransition) {
      const auto &target = p.semantics.functions.at(i.function);
      if (target.releases)
        return {false, Mutation::Release, false};
      if (target.intrinsicId != IntrinsicId::None)
        return intrinsicContract(target.intrinsicId);

      return {false, Mutation::Unknown, target.consumesSelf};
    }
    return {};
  }
  std::optional<SymbolId> argument(const I &i, size_t n) const {
    if (n == 0 && i.slot != None)
      return i.slot;
    auto input = i.args.at(n - (i.slot != None));
    auto found = referenceArguments.find(input);
    return found == referenceArguments.end() ? std::nullopt : std::optional(found->second);
  }
  void postState(State &state, const I &i) const {
    if ((i.kind != I::Call && i.kind != I::ProtocolTransition) || !definitions.contains(i.function))
      return;
    const auto &target = p.semantics.functions.at(i.function);
    for (const auto &post : definitions.at(i.function)->fieldPostStates) {
      auto parameter = std::find(target.params.begin(), target.params.end(), post.parameter);
      if (parameter == target.params.end())
        fail(i.loc, DiagnosticCode::InvalidCore, "storage post-state has no input parameter");
      auto source = argument(i, parameter - target.params.begin());
      auto base = source ? places.get(*source) : std::nullopt;
      if (!base)
        continue;
      for (const auto &leaf : schema(p.semantics.symbols.at(post.parameter).type)) {
        const std::map<uint32_t, core::OwnedFieldState> *fields = &post.fields;
        const core::OwnedFieldState *node = nullptr;
        for (const auto &part : leaf.path) {
          if (part.kind != hir::ReferenceProjectionKind::Field || !fields->contains(part.index)) {
            node = nullptr;
            break;
          }
          node = &fields->at(part.index);
          fields = &node->fields;
        }
        if (node && !node->allocation) {
          auto place = detail::append(*base, leaf.path);
          detail::erase(state, place);
          state[place] = {};
        }
      }
    }
  }
  std::set<Location> targets(const I &i) const {
    std::set<Location> result;
    auto add = [&](SymbolId source) {
      auto place = places.get(source);
      auto type = place ? places.type(*place) : std::nullopt;
      if (type && *type == p.semantics.symbols.at(source).type)
        for (const auto &leaf : schema(*type))
          result.insert(detail::append(*place, leaf.path));
    };
    if (i.slot != None && i.access == Access::Unique)
      add(i.slot);
    if (i.kind == I::Call || i.kind == I::ProtocolTransition || i.kind == I::ContainerCall) {
      const auto &target = p.semantics.functions.at(i.function);
      for (size_t n = 0; n < target.params.size(); ++n) {
        const auto &param = p.semantics.symbols.at(target.params[n]);
        if (!(param.reference || param.receiver) ||
            (param.access != Access::Unique && !(param.receiver && param.access == Access::Own)))
          continue;
        if (auto source = argument(i, n))
          add(*source);
      }
    }

    if (i.slot != None && (contract(i).consumes || contract(i).mutation == Mutation::Release))
      add(i.slot);
    return result;
  }
  bool observe(const I &i) const {
    return ownedResult(i) ||
           (i.slot != None && !schema(p.semantics.symbols.at(i.slot).type).empty()) ||
           mutations.contains(i.semanticPoint) || i.kind == I::BorrowShared ||
           i.kind == I::BorrowUnique || i.kind == I::FieldDrop;
  }
  State transfer(State state, const I &i) const {
    auto point = i.semanticPoint;
    if (mutations.contains(point)) {
      const auto &epoch = facts.epochs.at(mutations.at(point));
      for (const auto &place : epoch.places) {
        auto source = lookup(state, place);
        if (epoch.mutation == Mutation::Release) {

          detail::erase(state, place);
          state[place] = {};
          continue;
        }
        if (epoch.mutation == Mutation::Unknown) {
          detail::erase(state, place);
        }
        Candidates next{{}, source.unknown || epoch.mutation == Mutation::Unknown};
        for (const auto &version : source.versions)
          next.versions.insert({version.storage, epoch.id});
        state[place] = std::move(next);
      }
    }
    if (ownedResult(i)) {
      State result;
      if (creations.contains(point))
        result[value(None)].versions.insert({creations.at(point), None});
      else if (i.kind == I::Move)
        result = detail::subtree(state, symbol(i.slot));
      else if (i.kind == I::FieldTake) {
        if (auto base = places.get(i.slot))
          result = detail::subtree(state, detail::field(*base, i.fieldIndex));
      } else if (i.kind == I::PayloadTake) {
        if (auto base = places.get(i.slot))
          result = detail::subtree(state, detail::append(*base, i.payloadPath));
      } else if (i.kind == I::StructInit || i.kind == I::EnumConstruct) {
        for (size_t n = 0; n < i.args.size(); ++n) {
          auto destination = i.kind == I::StructInit
                                 ? detail::field(value(None), i.fieldPath[n])
                                 : detail::append(value(None), hir::payloadPosition(i.offset, n));
          detail::install(result, destination, detail::subtree(state, value(i.args[n])));
        }
      } else if (i.kind == I::ContainerCall && i.slot != None &&
                 (i.intrinsicId == IntrinsicId::Box_into_inner ||
                  i.intrinsicId == IntrinsicId::Vec_pop ||
                  i.intrinsicId == IntrinsicId::Vec_remove ||
                  i.intrinsicId == IntrinsicId::Vec_replace)) {
        if (auto base = places.get(i.slot)) {
          auto source =
              detail::element(*base, i.intrinsicId == IntrinsicId::Box_into_inner ? 0 : None);
          auto child = detail::subtree(state, source);
          if (i.intrinsicId == IntrinsicId::Vec_pop && elementLengths.contains(point) &&
              elementLengths.at(point) == 0)
            child.clear();
          if (i.intrinsicId == IntrinsicId::Vec_pop)
            detail::install(result, detail::append(value(None), hir::payloadPosition(0, 0)), child);
          else
            result = std::move(child);
        }
      } else if (i.kind != I::Create)
        for (const auto &leaf : schema(i.type))
          result[detail::append(value(None), leaf.path)].unknown = true;
      if (i.kind == I::ContainerCall && i.intrinsicId == IntrinsicId::Box_new_generic)
        detail::install(result, detail::element(value(None), 0),
                        detail::subtree(state, value(i.args.at(0))));
      detail::install(state, value(i.result), result);
    }
    if (i.kind == I::ContainerCall && i.slot != None) {
      if (auto base = places.get(i.slot)) {
        if (i.intrinsicId == IntrinsicId::Vec_push || i.intrinsicId == IntrinsicId::Vec_insert ||
            i.intrinsicId == IntrinsicId::Vec_replace)
          detail::install(state, detail::element(*base),
                          detail::subtree(state, value(i.args.back())),
                          !(i.intrinsicId == IntrinsicId::Vec_replace &&
                            elementLengths.contains(point) && elementLengths.at(point) == 1));
        if (i.intrinsicId == IntrinsicId::Vec_clear)
          detail::erase(state, detail::element(*base));
        if ((i.intrinsicId == IntrinsicId::Vec_pop || i.intrinsicId == IntrinsicId::Vec_remove) &&
            elementLengths.contains(point) && elementLengths.at(point) <= 1)
          detail::erase(state, detail::element(*base));
      }
    }
    if (i.kind == I::Bind && !schema(i.type).empty()) {
      detail::install(state, symbol(i.slot), detail::subtree(state, value(i.args.at(0))));
      detail::erase(state, value(i.args.at(0)));
    }
    if (i.kind == I::FieldStore || i.kind == I::FieldTake || i.kind == I::FieldDrop) {
      if (auto base = places.get(i.slot)) {
        auto field = detail::field(*base, i.fieldIndex);
        if (i.kind == I::FieldStore)
          detail::install(state, field, detail::subtree(state, value(i.args.at(0))));
        else
          detail::erase(state, field);
      }
    }
    if (i.kind == I::PayloadTake)
      if (auto base = places.get(i.slot))
        detail::erase(state, detail::append(*base, i.payloadPath));
    postState(state, i);
    if (i.kind == I::Move || i.kind == I::Drop || contract(i).consumes)
      detail::erase(state, symbol(i.slot));
    if (i.kind == I::ScopeEnd)
      for (auto local : i.locals)
        detail::erase(state, symbol(local));
    for (auto local : i.cleanup)
      detail::erase(state, symbol(local));

    const auto &block = graph.blocks.at(point);
    for (auto it = state.begin(); it != state.end();)
      if (it->first.kind == Location::Value && !block.valuesOut.contains(it->first.id))
        it = state.erase(it);
      else
        ++it;
    if (i.kind == I::Return)
      state.clear();
    return state;
  }

public:
  Analysis(const core::Program &program, const Graph &g, const core::Function &f)
      : p(program), graph(g), function(f), places(p, g, f), instructions(places.instructions) {
    for (const auto &body : p.functions)
      definitions[body.id] = &body;
    for (auto *i : instructions) {
      if (!i)
        continue;
      if (i->kind == I::ReferenceArgument)
        referenceArguments[i->result] = i->slot;
      if (i->kind == I::ContainerCall && containerBorrowIntrinsic(i->intrinsicId) &&
          !p.semantics.functions.at(i->function).resultBorrowedContainer)
        backingResults.insert(i->result);
    }
  }
  Facts run() {

    detail::elementBounds(p, graph, function, facts);
    for (const auto &bound : facts.elements)
      if (bound.length)
        elementLengths[bound.point] = *bound.length;
    State entry;
    for (auto param : p.semantics.functions.at(function.id).params) {
      const auto &s = p.semantics.symbols.at(param);
      for (const auto &leaf : schema(s.type)) {
        StorageId id = facts.identities.size();
        facts.identities.push_back(
            {id, OriginKind::Parameter, leaf.type, None, param, s.loc, leaf.path});
        entry[detail::append(symbol(param), leaf.path)].versions.insert({id, None});
      }
    }
    for (const auto &block : graph.blocks) {
      auto *i = instructions[block.id];
      if (!block.reachable || !i)
        continue;
      if (ownedResult(*i) && heap(i->type) &&
          (contract(*i).creates || i->kind == I::HeapAlloc || i->kind == I::Create ||
           i->kind == I::InputCall)) {
        StorageId id = facts.identities.size();
        facts.identities.push_back({id, OriginKind::Local, i->type, block.id, None, i->loc});
        creations[block.id] = id;
      }
      const auto effect = contract(*i).mutation;
      if (effect == Mutation::None)
        continue;
      auto owners = targets(*i);
      if (owners.empty())
        continue;
      EpochId id = facts.epochs.size();
      std::set<SymbolId> roots;
      for (const auto &place : owners)
        roots.insert(place.id);
      facts.epochs.push_back({id, block.id, effect, std::move(roots), i->loc, std::move(owners)});
      mutations[block.id] = id;
    }

    std::vector<State> in(graph.blocks.size()), out(graph.blocks.size());
    std::vector<bool> queued(graph.blocks.size()), reached(graph.blocks.size());
    std::deque<uint32_t> work{graph.entry};
    in[graph.entry] = entry;
    queued[graph.entry] = true;
    while (!work.empty()) {
      auto point = work.front();
      work.pop_front();
      queued[point] = false;
      auto *i = instructions[point];
      State next = i ? transfer(in[point], *i) : in[point];
      if (reached[point] && next == out[point])
        continue;
      reached[point] = true;
      out[point] = std::move(next);
      for (auto successor : graph.blocks[point].successors) {
        bool changed = merge(in[successor], out[point]);
        if ((!reached[successor] || changed) && !queued[successor]) {
          queued[successor] = true;
          work.push_back(successor);
        }
      }
    }
    for (const auto &block : graph.blocks) {
      auto *i = instructions[block.id];
      if (reached[block.id] && i && observe(*i))
        facts.transitions.emplace(block.id, Transition{in[block.id], out[block.id]});
    }
    for (const auto &loan : graph.loans) {

      const auto *creation = instructions.at(loan.created.block);
      auto source = creation ? places.get(creation->slot) : std::nullopt;
      bool backing = creation && !creation->args.empty() &&
                     backingResults.contains(creation->args[0]) && source && places.type(*source) &&
                     heap(*places.type(*source));
      facts.loans[loan.id] = {
          backing, backing ? lookup(in.at(loan.created.block), *source) : Candidates{{}, true},
          backing ? source : std::nullopt};
    }
    detail::slotGenerations(p, graph, function, facts);
    return std::move(facts);
  }
};
const core::Function &definition(const core::Program &p, FunctionId id) {
  for (const auto &function : p.functions)
    if (function.id == id)
      return function;
  fail({}, DiagnosticCode::InvalidCore, "storage graph has no function definition");
}
const char *mutationName(Mutation mutation) {
  switch (mutation) {
  case Mutation::None:
    return "preserve";
  case Mutation::Elements:
    return "invalidate-elements";
  case Mutation::Reallocate:
    return "may-reallocate";
  case Mutation::Unknown:
    return "unknown-mutable-call";
  case Mutation::Release:
    return "release-backing-storage";
  }
  return "invalid";
}
void printCandidates(const Candidates &candidates, std::ostream &out) {
  out << "{";
  for (auto version : candidates.versions) {
    out << " #S" << version.storage << "@";
    if (version.epoch == None)
      out << "initial";
    else
      out << "#E" << version.epoch;
  }
  if (candidates.unknown)
    out << " unknown";
  out << " }";
}
}
void infer(core::Program &p) {
  for (auto &graph : p.regionGraphs)
    graph.storage = Analysis(p, graph, definition(p, graph.function)).run();
}
void validate(const core::Program &p) {
  for (const auto &graph : p.regionGraphs) {
    if (graph.storage != Analysis(p, graph, definition(p, graph.function)).run())
      fail(p.semantics.functions.at(graph.function).loc, DiagnosticCode::InvalidCore,
           "storage identities/epochs contradict the typed semantic CFG");
    for (const auto &loan : graph.loans) {
      const auto &storage = graph.storage.loans.at(loan.id);
      if (storage.slot)
        for (const auto &[point, places] : graph.storage.slotInvalidations) {
          if (point == loan.created.block || !graph.regions.at(loan.region).points.contains(point))
            continue;
          if (!std::any_of(places.begin(), places.end(), [&](const auto &place) {
                return detail::covers(place, storage.slot->position);
              }))
            continue;
          DiagnosticEngine()
              .error(DiagnosticCode::BorrowConflict, graph.blocks.at(point).origin,
                     "operation may invalidate a slot while a reference to its content is live")
              .label(graph.blocks.at(point).origin, "slot may be replaced, moved or destroyed here")
              .secondary(loan.origin, "reference to the slot is created here")
              .note("keeping the backing allocation alive does not preserve the slot's contents")
              .help("end the reference before replacing or destroying its owner")
              .emit();
        }
      if (!storage.backing || storage.candidates.unknown)
        continue;
      for (const auto &epoch : graph.storage.epochs) {
        if (!storage.place || !epoch.places.contains(*storage.place) ||
            epoch.point == loan.created.block ||
            !graph.regions.at(loan.region).points.contains(epoch.point))
          continue;
        DiagnosticEngine()
            .error(DiagnosticCode::BorrowConflict, epoch.origin,
                   "operation may invalidate storage while a reference into it is live")
            .label(epoch.origin, "storage epoch may change here")
            .secondary(loan.origin, "reference into this storage is created here")
            .note("a live owner does not guarantee that its element storage is unchanged")
            .help("end references into the buffer before modifying its storage")
            .emit();
      }
    }
  }
}
void print(const Facts &facts, std::ostream &out) {
  for (const auto &identity : facts.identities) {
    out << "  storage identity #S" << identity.id << " type=#T" << identity.type;
    if (identity.kind == OriginKind::Parameter)
      out << " parameter=#v" << identity.parameter;
    else
      out << " site=bb" << identity.point << ":0";
    detail::printPath(identity.path, out);
    out << " (allocation-site abstraction)\n";
  }
  for (const auto &epoch : facts.epochs) {
    out << "  storage epoch #E" << epoch.id << " site=bb" << epoch.point << ":0 "
        << mutationName(epoch.mutation) << " owners={";
    for (auto owner : epoch.owners)
      out << " #v" << owner;
    out << " } places={";
    for (const auto &place : epoch.places) {
      out << " #v" << place.id;
      detail::printPath(place.path, out);
    }
    out << " }\n";
  }
  for (const auto &[point, transition] : facts.transitions) {
    out << "  storage transition bb" << point << ":0\n";
    for (const auto &[location, candidates] : transition.after) {
      out << "    " << (location.kind == Location::Symbol ? "#v" : "%") << location.id;
      detail::printPath(location.path, out);
      out << " = ";
      printCandidates(candidates, out);
      out << "\n";
    }
  }
  for (const auto &[loan, storage] : facts.loans) {
    out << "  loan storage #L" << loan << (storage.backing ? " backing = " : " unmodelled = ");
    printCandidates(storage.candidates, out);
    if (storage.slot) {
      out << " slot=";
      detail::printPath(storage.slot->position.path, out);
    }
    out << "\n";
  }
  for (const auto &bound : facts.elements) {
    out << "  element bound bb" << bound.point << ":0 owner=#v" << bound.owner.id;
    detail::printPath(bound.owner.path, out);
    out << " intrinsic=" << intrinsicInfo(bound.intrinsic).name << " length=";
    if (bound.length)
      out << *bound.length;
    else
      out << "unknown";
    out << " index=";
    if (bound.index)
      out << *bound.index;
    else
      out << "unknown";
    out << " in-bounds=";
    if (bound.inBounds)
      out << (*bound.inBounds ? "yes" : "no");
    else
      out << "conditional";
    out << " guard="
        << (bound.guard == ElementGuard::OptionSome    ? "Option::Some"
            : bound.guard == ElementGuard::BoundsPanic ? "bounds-panic"
            : bound.guard == ElementGuard::WholeView   ? "whole-view"
                                                       : "initialized-Box")
        << " access="
        << (bound.capability == Access::Shared   ? "Shared"
            : bound.capability == Access::Unique ? "Unique"
                                                 : "Own");
    detail::printPath(bound.resultPath, out);
    for (const auto &[path, arguments] : bound.resultRegions) {
      out << " RegionArgs";
      detail::printPath(path, out);
      out << "={";
      for (auto region : arguments)
        out << " #R" << region;
      out << " }";
    }
    if (bound.slot) {
      out << " slot=";
      detail::printPath(bound.slot->position.path, out);
      out << " initialized=" << (bound.slot->candidates.initialized ? "yes" : "unproven");
    }
    out << "\n";
  }
  for (const auto &identity : facts.slots) {
    out << "  slot identity #C" << identity.id;
    if (identity.parameter != None)
      out << " parameter=#v" << identity.parameter;
    else
      out << " site=bb" << identity.point << ":0";
    detail::printPath(identity.path, out);
    out << " (content-site abstraction)\n";
  }
  for (const auto &[point, transition] : facts.slotTransitions) {
    out << "  slot transition bb" << point << ":0\n";
    for (const auto &[position, candidates] : transition.after) {
      out << "    " << (position.kind == Location::Symbol ? "#v" : "%") << position.id;
      detail::printPath(position.path, out);
      out << " initialized=" << (candidates.initialized ? "yes" : "unproven") << " {";
      for (const auto &version : candidates.versions) {
        out << " #C" << version.content << "@";
        if (version.generation == None)
          out << "entry";
        else
          out << "bb" << version.generation;
      }
      if (candidates.unknown)
        out << " unknown";
      out << " }\n";
    }
  }
  for (const auto &[point, places] : facts.slotInvalidations) {
    out << "  slot invalidation bb" << point << ":0 {";
    for (const auto &place : places) {
      out << " " << (place.kind == Location::Symbol ? "#v" : "%") << place.id;
      detail::printPath(place.path, out);
    }
    out << " }\n";
  }
}
}
