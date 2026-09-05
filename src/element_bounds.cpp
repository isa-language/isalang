#include "storage_paths.hpp"
#include <charconv>
#include <deque>
#include <functional>

namespace isac::storage::detail {
namespace {
using I = core::Inst;
using Lengths = std::map<Location, uint64_t>;
Location symbol(SymbolId id) {
  return {Location::Symbol, id};
}
Location value(ValueId id) {
  return {Location::Value, id};
}
void erase(Lengths &state, const Location &base) {
  for (auto it = state.begin(); it != state.end();)
    if (within(it->first, base))
      it = state.erase(it);
    else
      ++it;
}
Lengths subtree(const Lengths &state, const Location &base) {
  Lengths result;
  for (const auto &[location, count] : state)
    if (within(location, base)) {
      auto relative = location;
      relative.path.erase(relative.path.begin(), relative.path.begin() + base.path.size());
      relative.id = None;
      relative.kind = Location::Value;
      result.emplace(std::move(relative), count);
    }
  return result;
}
void install(Lengths &state, const Location &base, const Lengths &relative) {
  erase(state, base);
  for (const auto &[position, count] : relative)
    state[append(base, position.path)] = count;
}

void meet(Lengths &a, const Lengths &b) {
  for (auto it = a.begin(); it != a.end();)
    if (auto other = b.find(it->first); other == b.end() || other->second != it->second)
      it = a.erase(it);
    else
      ++it;
}
struct Spec {
  ElementSelection selection;
  ElementGuard guard;
};
std::optional<Spec> spec(IntrinsicId id) {
  using S = ElementSelection;
  using G = ElementGuard;
  switch (id) {
  case IntrinsicId::Vec_get:
  case IntrinsicId::Vec_get_mut:
  case IntrinsicId::Slice_get:
  case IntrinsicId::Slice_get_mut:
    return Spec{S::Index, G::OptionSome};
  case IntrinsicId::Vec_first:
  case IntrinsicId::Vec_first_mut:
  case IntrinsicId::Slice_first:
  case IntrinsicId::Slice_first_mut:
    return Spec{S::First, G::OptionSome};
  case IntrinsicId::Vec_last:
  case IntrinsicId::Vec_last_mut:
  case IntrinsicId::Slice_last:
  case IntrinsicId::Slice_last_mut:
  case IntrinsicId::Vec_pop:
    return Spec{S::Last, G::OptionSome};
  case IntrinsicId::Vec_remove:
  case IntrinsicId::Vec_replace:
  case IntrinsicId::Vec_swap:
    return Spec{S::Index, G::BoundsPanic};
  case IntrinsicId::Vec_insert:
  case IntrinsicId::Vec_push:
    return Spec{S::Insertion, G::BoundsPanic};
  case IntrinsicId::Vec_as_slice:
  case IntrinsicId::Vec_as_mut_slice:
    return Spec{S::Whole, G::WholeView};
  case IntrinsicId::Box_get:
  case IntrinsicId::Box_get_mut:
  case IntrinsicId::Box_into_inner:
    return Spec{S::BoxValue, G::InitializedBox};
  default:
    return {};
  }
}
class Bounds {
  const core::Program &p;
  const regions::FunctionGraph &graph;
  const core::Function &function;
  Places places;
  std::map<ValueId, SymbolId> references;
  std::map<ValueId, uint64_t> constants;
  std::vector<Lengths> in, out;
  std::vector<bool> reached;
  Lengths fixed(TypeId type, Location base, bool initialized = true) const {
    Lengths result;
    const auto &shape = p.semantics.types.at(type);
    if (shape.kind == hir::Type::Array)
      result[base] = shape.count;
    else if (shape.representation == hir::Representation::GenericBox) {
      if (initialized)
        result[base] = 1;
    } else if (!p.semantics.heap(type) && shape.kind == hir::Type::Structure)
      for (uint32_t n = 0; n < shape.fields.size(); ++n) {
        auto children = fixed(shape.fields[n].type, field(base, n), initialized);
        result.insert(children.begin(), children.end());
      }
    return result;
  }
  Lengths transfer(Lengths state, const I &i) const {
    auto destination = value(i.result);
    if (i.kind == I::Move)
      install(state, destination, subtree(state, symbol(i.slot)));
    else if (i.kind == I::FieldTake) {
      if (auto base = places.get(i.slot))
        install(state, destination, subtree(state, field(*base, i.fieldIndex)));
    } else if (i.kind == I::PayloadTake) {
      if (auto base = places.get(i.slot))
        install(state, destination, subtree(state, append(*base, i.payloadPath)));
    } else if (i.kind == I::StructInit || i.kind == I::EnumConstruct) {
      for (size_t n = 0; n < i.args.size(); ++n)
        install(state,
                i.kind == I::StructInit ? field(destination, i.fieldPath[n])
                                        : append(destination, hir::payloadPosition(i.offset, n)),
                subtree(state, value(i.args[n])));
    } else if (i.kind == I::ContainerCall && i.intrinsicId == IntrinsicId::Box_new_generic) {
      state[destination] = 1;
      install(state, element(destination, 0), subtree(state, value(i.args.at(0))));
    } else if (i.kind == I::ContainerCall && i.intrinsicId == IntrinsicId::Box_into_inner) {
      if (auto base = places.get(i.slot))
        install(state, destination, subtree(state, element(*base, 0)));
    } else if (i.kind == I::ContainerCall &&
               (i.intrinsicId == IntrinsicId::Vec_pop || i.intrinsicId == IntrinsicId::Vec_remove ||
                i.intrinsicId == IntrinsicId::Vec_replace)) {
      if (auto base = places.get(i.slot)) {
        auto child = subtree(state, element(*base));
        if (i.intrinsicId == IntrinsicId::Vec_pop && state.contains(*base) && state.at(*base) == 0)
          child.clear();
        install(state,
                i.intrinsicId == IntrinsicId::Vec_pop
                    ? append(destination, hir::payloadPosition(0, 0))
                    : destination,
                child);
      }
    } else if (i.kind == I::Create || i.kind == I::Array || i.kind == I::ArrayRepeat ||
               ((i.kind == I::Call || i.kind == I::ContainerCall) &&
                !p.semantics.functions.at(i.function).returnsReference())) {
      auto known = fixed(i.type, destination, i.kind != I::Create);
      state.insert(known.begin(), known.end());
    }
    if (i.kind == I::ContainerCall) {
      if (i.intrinsicId == IntrinsicId::Vec_new || i.intrinsicId == IntrinsicId::Vec_with_capacity)
        state[destination] = 0;
      if (i.slot != None)
        if (auto base = places.get(i.slot)) {
          auto old = state.find(*base);
          std::optional<uint64_t> oldLength;
          if (old != state.end())
            oldLength = old->second;
          if (i.intrinsicId == IntrinsicId::Vec_push || i.intrinsicId == IntrinsicId::Vec_insert ||
              i.intrinsicId == IntrinsicId::Vec_replace) {
            auto child = subtree(state, value(i.args.back()));
            if (oldLength == 0 || (oldLength == 1 && i.intrinsicId == IntrinsicId::Vec_replace))
              install(state, element(*base), child);
            else {
              auto existing = subtree(state, element(*base));
              meet(existing, child);
              install(state, element(*base), existing);
            }
          }
          if (i.intrinsicId == IntrinsicId::Vec_clear) {
            erase(state, *base);
            state[*base] = 0;
          }
          if (i.intrinsicId == IntrinsicId::Vec_push || i.intrinsicId == IntrinsicId::Vec_insert) {
            auto found = state.find(*base);
            if (found != state.end()) {
              if (found->second == UINT64_MAX)
                state.erase(found);
              else
                ++found->second;
            }
          }
          if (i.intrinsicId == IntrinsicId::Vec_pop || i.intrinsicId == IntrinsicId::Vec_remove) {
            auto found = state.find(*base);
            if (found != state.end() && found->second)
              --found->second;
            if (oldLength && *oldLength <= 1)
              erase(state, element(*base));
          }
          if (i.intrinsicId == IntrinsicId::Box_into_inner)
            erase(state, *base);
        }
    }
    if (i.kind == I::Call || i.kind == I::ProtocolTransition) {
      const auto &target = p.semantics.functions.at(i.function);
      for (size_t n = 0; n < target.params.size(); ++n) {
        const auto &param = p.semantics.symbols.at(target.params[n]);
        if (!(param.reference || param.receiver) || param.access == Access::Shared)
          continue;
        SymbolId source = None;
        if (n == 0 && i.slot != None)
          source = i.slot;
        else if (auto ref = references.find(i.args.at(n - (i.slot != None)));
                 ref != references.end())
          source = ref->second;
        auto base = source == None ? std::nullopt : places.get(source);
        if (base && places.type(*base) == param.type) {
          erase(state, *base);
          auto known = fixed(param.type, *base);
          state.insert(known.begin(), known.end());
        }
      }
    }
    if (i.kind == I::Bind) {
      install(state, symbol(i.slot), subtree(state, value(i.args.at(0))));
      erase(state, value(i.args.at(0)));
    }
    if (i.kind == I::FieldStore || i.kind == I::FieldTake || i.kind == I::FieldDrop)
      if (auto base = places.get(i.slot)) {
        auto target = field(*base, i.fieldIndex);
        if (i.kind == I::FieldStore)
          install(state, target, subtree(state, value(i.args.at(0))));
        else
          erase(state, target);
      }
    if (i.kind == I::PayloadTake)
      if (auto base = places.get(i.slot))
        erase(state, append(*base, i.payloadPath));
    if (i.kind == I::Move || i.kind == I::Drop)
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
    return state;
  }

public:
  Bounds(const core::Program &program, const regions::FunctionGraph &g, const core::Function &f)
      : p(program), graph(g), function(f), places(p, g, f), in(g.blocks.size()),
        out(g.blocks.size()), reached(g.blocks.size()) {
    for (auto *i : places.instructions) {
      if (!i)
        continue;
      if (i->kind == I::ReferenceArgument)
        references[i->result] = i->slot;
      if (i->kind == I::Constant && i->type == USize) {
        uint64_t number;
        auto parsed = std::from_chars(i->text.data(), i->text.data() + i->text.size(), number);
        if (parsed.ec == std::errc{} && parsed.ptr == i->text.data() + i->text.size())
          constants[i->result] = number;
      }
    }
  }
  void run(Facts &facts) {
    Lengths entry;
    for (auto param : p.semantics.functions.at(function.id).params) {
      auto known = fixed(p.semantics.symbols.at(param).type, symbol(param));
      entry.insert(known.begin(), known.end());
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
      std::optional<Lengths> incoming;
      if (point == graph.entry)
        incoming = entry;
      for (auto before : predecessors[point])
        if (reached[before]) {
          if (!incoming)
            incoming = out[before];
          else
            meet(*incoming, out[before]);
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
    for (auto *i : places.instructions) {
      if (!i || !reached[i->semanticPoint] || i->kind != I::ContainerCall || i->slot == None)
        continue;
      auto access = spec(i->intrinsicId);
      auto owner = places.get(i->slot);
      if (!access || !owner)
        continue;
      auto count = in[i->semanticPoint].find(*owner);
      std::optional<uint64_t> length;
      if (count != in[i->semanticPoint].end())
        length = count->second;
      auto add = [&](size_t argument = 0) {
        ElementBound bound{i->semanticPoint, *owner, i->intrinsicId, access->selection,
                           access->guard};
        bound.length = length;
        if (i->intrinsicId == IntrinsicId::Vec_push)
          bound.index = length;
        else if (access->selection == ElementSelection::Index ||
                 access->selection == ElementSelection::Insertion) {
          bound.indexValue = i->args.at(argument);
          if (constants.contains(bound.indexValue))
            bound.index = constants.at(bound.indexValue);
        } else if (access->selection == ElementSelection::First ||
                   access->selection == ElementSelection::BoxValue)
          bound.index = 0;
        else if (access->selection == ElementSelection::Last && length && *length)
          bound.index = *length - 1;
        if (length && access->selection != ElementSelection::Whole) {
          if (bound.index)
            bound.inBounds = access->selection == ElementSelection::Insertion
                                 ? *bound.index <= *length
                                 : *bound.index < *length;
          else if (!*length)
            bound.inBounds = false;
        }
        bound.borrowed = containerBorrowIntrinsic(i->intrinsicId);
        if (bound.borrowed)
          bound.capability =
              containerMutableBorrowIntrinsic(i->intrinsicId) ? Access::Unique : Access::Shared;
        if (access->guard == ElementGuard::OptionSome)
          bound.resultPath = hir::payloadPosition(0, 0);
        bound.resultValue = i->result;
        if (bound.borrowed)
          if (auto regions = graph.valueRegions.find(i->result);
              regions != graph.valueRegions.end())
            bound.resultRegions = regions->second;
        facts.elements.push_back(std::move(bound));
      };
      add();
      if (i->intrinsicId == IntrinsicId::Vec_swap)
        add(1);
    }
  }
};
}
void elementBounds(const core::Program &p, const regions::FunctionGraph &g, const core::Function &f,
                   Facts &facts) {
  Bounds(p, g, f).run(facts);
}
}
