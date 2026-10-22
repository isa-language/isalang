#include "region_inference.hpp"
#include "storage_paths.hpp"
#include <deque>

namespace isac::regions::detail {
namespace {
using Location = storage::Location;
using State = carriers::State;
using Op = FieldRegionOperation;
State pack(const hir::RegionArgs &args) {
  State result;
  for (const auto &[path, regions] : args) {
    carriers::Binding binding;
    binding.regions = regions;
    result.emplace(Location{Location::Value, None, path}, std::move(binding));
  }
  return result;
}
hir::RegionArgs unpack(const State &state) {
  hir::RegionArgs result;
  for (const auto &[position, binding] : state)
    result[position.path] = binding.regions;
  return result;
}
}
std::map<uint32_t, Op> fieldRegionOperations(const core::Program &p, const FunctionGraph &graph) {
  const auto &function = *std::find_if(p.functions.begin(), p.functions.end(),
                                       [&](const auto &f) { return f.id == graph.function; });
  storage::detail::Places places(p, graph, function);
  std::map<uint32_t, Op> result;
  for (auto *i : places.instructions) {
    if (!i || !graph.blocks[i->semanticPoint].reachable)
      continue;
    using I = core::Inst;
    Op op{};
    auto managed = [&](TypeId t) {
      return p.semantics.types[t].kind == hir::Type::Structure && p.semantics.localCarrierType(t);
    };
    op.type = i->type;
    op.result = i->result;
    if (i->kind == I::Bind && managed(i->type)) {
      op.kind = Op::Bind;
      op.place = {Location::Symbol, i->slot};
      op.source = i->args.at(0);
    } else if (i->kind == I::Move && managed(i->type)) {
      op.kind = Op::Move;
      op.place = {Location::Symbol, i->slot};
    } else if ((i->kind == I::FieldTake || i->kind == I::FieldStore || i->kind == I::FieldDrop) &&
               p.semantics.containsReferences(i->type)) {
      auto base = places.get(i->slot);
      if (!base)
        fail(i->loc, DiagnosticCode::InvalidCore, "field region flow has no semantic place");
      op.kind = i->kind == I::FieldTake    ? Op::Take
                : i->kind == I::FieldStore ? Op::Store
                                           : Op::Drop;
      op.place = storage::detail::field(*base, i->fieldIndex);
      if (i->kind == I::FieldStore)
        op.source = i->args.at(0);
    } else if (i->kind == I::Drop && managed(p.semantics.symbols.at(i->slot).type)) {
      op.kind = Op::Drop;
      op.place = {Location::Symbol, i->slot};
    } else if (i->kind == I::ScopeEnd) {
      op.kind = Op::Scope;
      op.locals = i->locals;
    } else if (i->kind == I::Return)
      op.kind = Op::Return;
    else
      continue;
    result.emplace(i->semanticPoint, std::move(op));
  }

  bool fieldInputs = std::any_of(
      graph.contract.inputs.begin(), graph.contract.inputs.end(), [](const auto &input) {
        return std::any_of(input.second.begin(), input.second.end(), [](const auto &position) {
          return !position.first.empty() &&
                 position.first[0].kind == hir::ReferenceProjectionKind::Field;
        });
      });
  if (!fieldInputs && graph.fieldRegionOperations.empty() &&
      std::none_of(result.begin(), result.end(), [](const auto &entry) {
        return entry.second.kind != Op::Scope && entry.second.kind != Op::Return;
      }))
    result.clear();
  return result;
}
FieldRegionFlow fieldArguments(const FunctionGraph &graph) {
  FieldRegionFlow facts;
  if (graph.fieldRegionOperations.empty())
    return facts;
  auto args = [&](ValueId id) {
    auto found = graph.valueRegions.find(id);
    return found == graph.valueRegions.end() ? State{} : pack(found->second);
  };
  auto transfer = [&](State state, uint32_t point) {
    auto found = graph.fieldRegionOperations.find(point);
    if (found == graph.fieldRegionOperations.end())
      return state;
    const auto &op = found->second;
    if (op.kind == Op::Bind || op.kind == Op::Store)
      carriers::store(state, op.place, args(op.source));
    else if (op.kind == Op::Move || op.kind == Op::Take || op.kind == Op::Drop)
      carriers::erase(state, op.place);
    else if (op.kind == Op::Scope)
      for (auto id : op.locals)
        carriers::erase(state, {Location::Symbol, id});
    if (op.kind == Op::Return)
      state.clear();
    return state;
  };
  std::vector<State> in(graph.blocks.size()), out(graph.blocks.size());
  std::vector<bool> reached(graph.blocks.size()), queued(graph.blocks.size());
  std::vector<std::vector<uint32_t>> predecessors(graph.blocks.size());
  for (const auto &block : graph.blocks)
    if (block.reachable)
      for (auto next : block.successors)
        predecessors[next].push_back(block.id);
  std::deque<uint32_t> work{graph.entry};
  queued[graph.entry] = true;
  while (!work.empty()) {
    auto point = work.front();
    work.pop_front();
    queued[point] = false;
    std::optional<State> incoming;
    if (point == graph.entry) {
      incoming = State{};
      for (const auto &[symbol, args] : graph.contract.inputs)
        if (std::any_of(args.begin(), args.end(), [](const auto &entry) {
              return !entry.first.empty() &&
                     entry.first[0].kind == hir::ReferenceProjectionKind::Field;
            }))
          carriers::store(*incoming, {Location::Symbol, symbol}, pack(args));
    }
    for (auto previous : predecessors[point])
      if (reached[previous]) {
        if (!incoming)
          incoming = out[previous];
        else
          carriers::join(*incoming, out[previous]);
      }
    if (!incoming)
      continue;
    in[point] = std::move(*incoming);
    auto next = transfer(in[point], point);
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
  for (const auto &block : graph.blocks)
    if (reached[block.id]) {
      for (const auto &[position, binding] : in[block.id]) {
        auto &args = facts.before[block.id][position.id][position.path];
        args.insert(binding.regions.begin(), binding.regions.end());
        auto &summary = facts.summaries[position.id][position.path];
        summary.insert(binding.regions.begin(), binding.regions.end());
      }
      auto found = graph.fieldRegionOperations.find(block.id);
      if (found != graph.fieldRegionOperations.end()) {
        const auto &op = found->second;
        if (op.kind == Op::Move || op.kind == Op::Take)
          facts.values[op.result] = unpack(carriers::extract(in[block.id], op.place));
      }
    }
  return facts;
}
void validateFieldArguments(const core::Program &p) {
  for (const auto &graph : p.regionGraphs) {
    auto active = fieldRegionOperations(p, graph);
    for (const auto &[point, op] : active)
      if (!graph.fieldRegionOperations.contains(point) ||
          graph.fieldRegionOperations.at(point) != op)
        fail(graph.blocks.at(point).origin, DiagnosticCode::InvalidCore,
             "typed field region operation was altered");
    for (const auto &[point, op] : graph.fieldRegionOperations) {
      if (point >= graph.blocks.size() || !graph.blocks[point].reachable ||
          (graph.resourcePoints.contains(point) && !active.contains(point)))
        fail({}, DiagnosticCode::InvalidCore, "invalid original field region operation");
      if (op.source != None && !graph.blocks[point].valueUses.contains(op.source))
        fail({}, DiagnosticCode::InvalidCore, "field region source is not a typed CFG operand");
      if (op.result != None && !graph.blocks[point].valueDefinitions.contains(op.result))
        fail({}, DiagnosticCode::InvalidCore, "field region result is not a typed CFG definition");
    }
    auto flow = fieldArguments(graph);
    if (flow.before != graph.fieldRegions)
      fail({}, DiagnosticCode::InvalidCore, "field RegionArgs contradict the original typed CFG");
    for (const auto &[value, args] : flow.values) {
      auto found = graph.valueRegions.find(value);
      if ((found == graph.valueRegions.end() && !args.empty()) ||
          (found != graph.valueRegions.end() && found->second != args))
        fail({}, DiagnosticCode::InvalidCore, "field result RegionArgs lost a positional origin");
    }
  }
}
}
