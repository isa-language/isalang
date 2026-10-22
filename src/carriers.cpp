#include "isac/carriers.hpp"
#include "isac/core.hpp"
#include "storage_paths.hpp"
#include <algorithm>
#include <deque>

namespace isac::carriers {
namespace {
using Location = storage::Location;
using Path = hir::ReferencePath;
Location symbol(SymbolId id) {
  return {Location::Symbol, id};
}
Location value(ValueId id) {
  return {Location::Value, id};
}
void requireExact(const Location &base) {
  for (const auto &part : base.path)
    if (part.kind == hir::ReferenceProjectionKind::Element && part.index == None)
      fail({}, DiagnosticCode::InvalidCore,
           "summary carrier selector cannot justify a strong transfer");
}
void merge(Binding &to, const Binding &from) {
  if (to.capability != from.capability || to.parent != from.parent)
    fail({}, DiagnosticCode::InvalidCore, "carrier join has incompatible capabilities or parents");
  to.instances.insert(from.instances.begin(), from.instances.end());
  to.origins.insert(from.origins.begin(), from.origins.end());
  to.regions.insert(from.regions.begin(), from.regions.end());
  to.present &= from.present;
}
}
State extract(const State &state, const Location &base) {
  requireExact(base);
  State relative;
  for (const auto &[position, binding] : state)
    if (storage::detail::within(position, base)) {
      auto path = position.path;
      path.erase(path.begin(), path.begin() + base.path.size());
      relative.emplace(Location{Location::Value, None, std::move(path)}, binding);
    }
  return relative;
}
void erase(State &state, const Location &base) {
  requireExact(base);
  std::erase_if(state,
                [&](const auto &entry) { return storage::detail::within(entry.first, base); });
}
void store(State &state, const Location &base, const State &relative, bool weak) {
  if (!weak)
    erase(state, base);
  else
    for (auto &[position, binding] : state)
      if (storage::detail::overlaps(position, base))
        binding.present = false;
  for (const auto &[position, binding] : relative) {
    auto destination = storage::detail::append(base, position.path);
    auto [it, added] = state.emplace(destination, binding);
    if (!added)
      merge(it->second, binding);
    if (weak)
      it->second.present = false;
  }
}
void join(State &to, const State &from) {
  for (auto &[position, binding] : to)
    if (!from.contains(position))
      binding.present = false;
  for (const auto &[position, binding] : from) {
    auto [it, added] = to.emplace(position, binding);
    if (added)
      it->second.present = false;
    else
      merge(it->second, binding);
  }
}
namespace {
using I = core::Inst;
class Analysis {
  const core::Program &p;
  const regions::FunctionGraph &graph;
  const core::Function &function;
  storage::detail::Places places;
  Facts facts;
  std::map<std::pair<uint32_t, Path>, CarrierId> births;
  std::map<RegionId, std::set<regions::LoanId>> loanSources;

  const std::set<regions::LoanId> &loans(RegionId start) {
    auto [cached, added] = loanSources.try_emplace(start);
    if (!added)
      return cached->second;
    std::set<RegionId> required{start};
    std::deque<RegionId> work{start};
    while (!work.empty()) {
      auto region = work.front();
      work.pop_front();
      for (const auto &constraint : graph.constraints)
        if (constraint.kind == regions::ConstraintKind::Subregion && constraint.region == region &&
            required.insert(constraint.upper).second)
          work.push_back(constraint.upper);
    }
    for (const auto &loan : graph.loans)
      if (required.contains(loan.region))
        cached->second.insert(loan.id);
    return cached->second;
  }

  const hir::RegionArgs &regions(const Location &base) const {
    static const hir::RegionArgs empty;
    if (base.kind == Location::Symbol) {
      auto found = graph.symbolRegions.find(base.id);
      return found == graph.symbolRegions.end() ? empty : found->second;
    }
    auto found = graph.valueRegions.find(base.id);
    return found == graph.valueRegions.end() ? empty : found->second;
  }
  bool active(uint32_t point) const {
    if (!graph.blocks[point].reachable)
      return false;
    if (places.instructions[point])
      return graph.resourcePoints.contains(point);

    auto terminator = graph.blocks[point].terminator;
    return terminator == regions::Terminator::LoopBackedge ||
           terminator == regions::Terminator::Unreachable;
  }
  void end(State &state, const Location &base, EndKind kind, std::vector<End> *ends) const {
    if (ends)
      for (const auto &[position, binding] : state)
        if (storage::detail::within(position, base))
          ends->push_back({position, binding, kind});
    erase(state, base);
  }
  bool consumes(const I &i, size_t n) const {
    auto *producer = places.producer(i.args[n]);
    if (!producer || !p.semantics.linear(producer->type))
      return false;
    switch (i.kind) {
    case I::Bind:
    case I::EnumConstruct:
    case I::StructInit:
    case I::Array:
    case I::FieldStore:
    case I::Return:
    case I::Discard:
      return true;
    case I::Call:
    case I::ProtocolTransition:
    case I::ContainerCall: {
      const auto &target = p.semantics.functions.at(i.function);
      auto index = n + (i.slot != None);
      return index < target.params.size() &&
             !p.semantics.symbols.at(target.params[index]).reference;
    }
    default:
      return false;
    }
  }

  State derived(const State &state, const I &i) const {
    auto base = i.slot == None ? std::nullopt : places.get(i.slot);
    if (i.kind == I::Move)
      return extract(state, symbol(i.slot));

    if (i.kind == I::Bind)
      return extract(state, value(i.args.at(0)));
    if (i.kind == I::FieldTake && base)
      return extract(state, storage::detail::field(*base, i.fieldIndex));
    if (i.kind == I::PayloadTake && base)
      return extract(state, storage::detail::append(*base, i.payloadPath));
    State result;
    if (i.kind == I::EnumConstruct || i.kind == I::StructInit || i.kind == I::Array)
      for (size_t n = 0; n < i.args.size(); ++n) {
        auto destination = value(None);
        if (i.kind == I::EnumConstruct)
          destination.path = hir::payloadPosition(i.offset, n);
        else if (i.kind == I::StructInit)
          destination = storage::detail::field(destination, i.fieldPath.at(n));
        else
          destination = storage::detail::element(destination, n);
        store(result, destination, extract(state, value(i.args[n])));
      }
    return result;
  }
  State result(const State &state, const I &i) const {
    State next;
    auto prior = derived(state, i);
    auto base = i.kind == I::Bind ? symbol(i.slot) : value(i.result);
    const auto &arguments = regions(base);
    for (const auto &[path, provenance] : i.carriers) {
      Binding binding;
      auto found = prior.find(Location{Location::Value, None, path});
      if (found != prior.end() && found->second.origins == provenance.origins &&
          found->second.capability == provenance.capability &&
          found->second.parent == provenance.parent)
        binding = found->second;
      else
        binding.instances = {births.at({i.semanticPoint, path})};
      binding.origins = provenance.origins;
      binding.capability = provenance.capability;
      binding.parent = provenance.parent;
      if (auto position = arguments.find(path); position != arguments.end())
        binding.regions = position->second;

      if (i.kind == I::EnumConstruct || i.kind == I::StoreReference)
        binding.present = true;
      next.emplace(Location{Location::Value, None, path}, std::move(binding));
    }
    return next;
  }
  State transfer(State state, const I &i, std::vector<End> *ends = nullptr) const {
    for (auto alias : i.inferredEnds)
      end(state, symbol(alias), EndKind::BorrowEnd, ends);
    if (i.kind == I::EndBorrow)
      end(state, symbol(i.slot), EndKind::BorrowEnd, ends);
    auto produced = result(state, i);
    auto base = i.slot == None ? std::nullopt : places.get(i.slot);
    if ((i.kind == I::BorrowShared || i.kind == I::BorrowUnique) && i.storedPayloadReference) {
      if (!base)
        fail(i.loc, DiagnosticCode::InvalidCore, "stored payload carrier has no semantic location");
      auto position = storage::detail::append(*base, i.payloadPath);
      auto payload = extract(state, position);
      if (!payload.contains(value(None)))
        fail(i.loc, DiagnosticCode::InvalidCore, "stored payload alias has no carrier occurrence");
      auto &binding = payload.at(value(None));
      if (i.kind == I::BorrowShared) {
        binding.instances = {births.at({i.semanticPoint, {}})};
        binding.parent = None;
      } else
        end(state, position, EndKind::Transfer, ends);
      binding.regions = {i.region};
      binding.present = true;
      store(state, symbol(i.offset), payload);
    }
    if (i.kind == I::Move)
      end(state, symbol(i.slot), EndKind::Transfer, ends);
    if (i.kind == I::Drop)
      end(state, symbol(i.slot), EndKind::Drop, ends);
    if (i.kind == I::FieldTake || i.kind == I::FieldStore || i.kind == I::FieldDrop) {
      if (base) {
        auto destination = storage::detail::field(*base, i.fieldIndex);
        auto replacement = i.kind == I::FieldStore ? extract(state, value(i.args.at(0))) : State{};
        end(state, destination, i.kind == I::FieldTake ? EndKind::Transfer : EndKind::Drop, ends);
        if (i.kind == I::FieldStore)
          store(state, destination, replacement);
      }
    }
    if (i.kind == I::PayloadTake && base)
      end(state, storage::detail::append(*base, i.payloadPath), EndKind::Transfer, ends);
    for (size_t n = 0; n < i.args.size(); ++n)
      if (consumes(i, n))
        end(state, value(i.args[n]),
            i.kind == I::Return                                 ? EndKind::Return
            : i.kind == I::Discard                              ? EndKind::Drop
            : (i.kind == I::Call || i.kind == I::ContainerCall) ? EndKind::Consume
                                                                : EndKind::Transfer,
            ends);
    if ((i.kind == I::Call || i.kind == I::ProtocolTransition || i.kind == I::ContainerCall) &&
        i.slot != None && p.semantics.functions.at(i.function).consumesSelf)
      end(state, symbol(i.slot), EndKind::Consume, ends);
    if (i.kind == I::Bind) {
      end(state, symbol(i.slot), EndKind::Drop, ends);
      store(state, symbol(i.slot), produced);
    } else if (i.result != None)
      store(state, value(i.result), produced);
    for (auto local : i.cleanup)
      end(state, symbol(local), EndKind::Cleanup, ends);
    if (i.kind == I::ScopeEnd)
      for (auto local : i.locals)
        end(state, symbol(local), EndKind::Cleanup, ends);
    std::vector<Location> dead;
    for (const auto &[position, binding] : state)
      if (position.kind == Location::Value &&
          !graph.blocks[i.semanticPoint].valuesOut.contains(position.id))
        dead.push_back(position);
    for (const auto &position : dead)
      end(state, position, EndKind::DeadValue, ends);
    if (i.kind == I::Return) {
      if (ends)
        for (const auto &[position, binding] : state)
          ends->push_back({position, binding, EndKind::Cleanup});
      state.clear();
    }
    return state;
  }
  void attach(uint32_t point, const State &state, const storage::SlotState &slots, bool after) {
    for (const auto &[position, binding] : state)
      for (const auto &[slot, candidates] : slots)
        if (storage::detail::within(position, slot))
          facts.slots[point].push_back({position, {slot, candidates}, after});
  }
  void referents(uint32_t point, const State &state, bool after) {
    for (const auto &[position, binding] : state) {
      std::set<regions::LoanId> required;
      for (auto region : binding.regions) {
        const auto &source = loans(region);
        required.insert(source.begin(), source.end());
      }
      for (auto id : required) {
        const auto &backing = graph.storage.loans.at(id);
        if (backing.slot)
          facts.referents[point].push_back({position, id, *backing.slot, after});
      }
    }
  }

public:
  Analysis(const core::Program &p, const regions::FunctionGraph &g, const core::Function &f)
      : p(p), graph(g), function(f), places(p, g, f) {}
  Facts run() {
    State entry;
    const auto &signature = p.semantics.functions.at(function.id);
    for (const auto &input : signature.payloadParameterRegions) {
      auto id = static_cast<CarrierId>(facts.identities.size());
      facts.identities.push_back({id, None, input.parameter, input.path});
      entry[storage::detail::append(symbol(input.parameter), input.path)] = {
          {id}, {input.origin}, {input.region}, Access::Shared, None, false};
    }
    bool outputs = false;
    for (auto *i : places.instructions)
      if (i && active(i->semanticPoint))
        for (const auto &[path, provenance] : i->carriers) {
          auto id = static_cast<CarrierId>(facts.identities.size());
          facts.identities.push_back({id, i->semanticPoint, None, path});
          births[{i->semanticPoint, path}] = id;
          outputs = true;
        }
    for (auto *i : places.instructions)
      if (i && active(i->semanticPoint) && i->storedPayloadReference &&
          i->kind == I::BorrowShared) {
        auto id = static_cast<CarrierId>(facts.identities.size());
        facts.identities.push_back({id, i->semanticPoint, None, {}});
        births[{i->semanticPoint, {}}] = id;
        outputs = true;
      }
    if (entry.empty() && !outputs)
      return std::move(facts);
    std::vector<State> in(graph.blocks.size()), out(graph.blocks.size());
    std::vector<bool> reached(graph.blocks.size()), queued(graph.blocks.size());
    std::vector<std::vector<uint32_t>> predecessors(graph.blocks.size());
    for (const auto &block : graph.blocks)
      if (active(block.id))
        for (auto next : block.successors)
          if (active(next))
            predecessors[next].push_back(block.id);
    std::deque<uint32_t> work{graph.entry};
    queued[graph.entry] = true;
    while (!work.empty()) {
      auto point = work.front();
      work.pop_front();
      queued[point] = false;
      if (!active(point))
        continue;
      std::optional<State> incoming;
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
        if (active(successor) && !queued[successor]) {
          queued[successor] = true;
          work.push_back(successor);
        }
    }
    for (const auto &block : graph.blocks)
      if (reached[block.id]) {
        auto *i = places.instructions[block.id];
        if (!i)
          continue;
        std::vector<End> ends;
        auto after = transfer(in[block.id], *i, &ends);
        if (in[block.id] != after || !ends.empty() || !i->carriers.empty()) {
          facts.transitions.emplace(block.id,
                                    Transition{in[block.id], std::move(after), std::move(ends)});
          const auto &transition = facts.transitions.at(block.id);
          referents(block.id, transition.before, false);
          referents(block.id, transition.after, true);
          if (auto slots = graph.storage.slotTransitions.find(block.id);
              slots != graph.storage.slotTransitions.end()) {
            attach(block.id, transition.before, slots->second.before, false);
            attach(block.id, transition.after, slots->second.after, true);
          }
        }
      }
    return std::move(facts);
  }
};
const core::Function &definition(const core::Program &p, FunctionId id) {
  for (const auto &f : p.functions)
    if (f.id == id)
      return f;
  fail({}, DiagnosticCode::InvalidCore, "carrier graph has no function definition");
}
void printPosition(const Location &position, std::ostream &out) {
  out << (position.kind == Location::Symbol ? "#v" : "%") << position.id;
  storage::detail::printPath(position.path, out);
}
}
void infer(core::Program &p) {
  for (auto &graph : p.regionGraphs)
    graph.carriers = Analysis(p, graph, definition(p, graph.function)).run();
}
void validate(const core::Program &p) {
  for (const auto &graph : p.regionGraphs)
    if (graph.carriers != Analysis(p, graph, definition(p, graph.function)).run())
      fail(p.semantics.functions.at(graph.function).loc, DiagnosticCode::InvalidCore,
           "carrier instances/cleanup contradict the checked semantic CFG");
}
void print(const Facts &facts, std::ostream &out) {
  for (const auto &identity : facts.identities) {
    out << "  carrier instance #C" << identity.id;
    if (identity.parameter != None)
      out << " parameter=#v" << identity.parameter;
    else
      out << " site=bb" << identity.point << ":0";
    storage::detail::printPath(identity.path, out);
    out << " (occurrence-site abstraction)\n";
  }
  for (const auto &[point, transition] : facts.transitions) {
    out << "  carrier transition bb" << point << ":0\n";
    for (const auto &[position, binding] : transition.after) {
      out << "    ";
      printPosition(position, out);
      out << " instances={";
      for (auto id : binding.instances)
        out << " #C" << id;
      out << " } origins={";
      for (auto id : binding.origins)
        out << " #v" << id;
      out << " } RegionArgs={";
      for (auto id : binding.regions)
        out << " #R" << id;
      out << " } present=" << (binding.present ? "yes" : "conditional") << "\n";
    }
    static const char *names[] = {"transfer", "consume",    "drop",      "cleanup",
                                  "return",   "dead-value", "end-borrow"};
    for (const auto &end : transition.ends) {
      out << "    carrier end ";
      printPosition(end.position, out);
      out << " reason=" << names[static_cast<unsigned>(end.kind)] << "\n";
    }
  }
  for (const auto &[point, attachments] : facts.slots)
    for (const auto &attachment : attachments) {
      out << "  carrier slot bb" << point << ":0 " << (attachment.after ? "after " : "before ");
      printPosition(attachment.carrier, out);
      out << " within ";
      printPosition(attachment.slot.position, out);
      out << " initialized=" << (attachment.slot.candidates.initialized ? "yes" : "conditional")
          << "\n";
    }
  for (const auto &[point, attachments] : facts.referents)
    for (const auto &attachment : attachments) {
      out << "  carrier referent bb" << point << ":0 " << (attachment.after ? "after " : "before ");
      printPosition(attachment.carrier, out);
      out << " loan=#L" << attachment.loan << " slot=";
      printPosition(attachment.slot.position, out);
      out << " generations={";
      for (auto version : attachment.slot.candidates.versions)
        out << " #C" << version.content << "@" << version.generation;
      out << " }\n";
    }
}
}
