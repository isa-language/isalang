#include "region_inference.hpp"
#include "storage_paths.hpp"
#include <deque>
#include <functional>

namespace isac::regions::detail {
namespace {
using I = core::Inst;
using Op = ParentOperation;
using Location = storage::Location;
Location symbol(SymbolId id) {
  return {Location::Symbol, id};
}
Location value(ValueId id) {
  return {Location::Value, id};
}
void merge(ParentSet &to, const ParentSet &from) {
  to.insert(from.begin(), from.end());
}
void join(ParentState &to, const ParentState &from) {
  for (const auto &[position, parents] : from)
    merge(to[position], parents);
}
ParentState extract(const ParentState &state, const Location &base) {
  ParentState result;
  for (const auto &[position, parents] : state)
    if (storage::detail::within(position, base)) {
      auto path = position.path;
      path.erase(path.begin(), path.begin() + base.path.size());
      result.emplace(Location{Location::Value, None, std::move(path)}, parents);
    }
  return result;
}
void erase(ParentState &state, const Location &base) {
  for (const auto &part : base.path)
    if (part.kind == hir::ReferenceProjectionKind::Element && part.index == None)
      fail({}, DiagnosticCode::InvalidCore,
           "summary element cannot end a suspended parent obligation");
  std::erase_if(state,
                [&](const auto &entry) { return storage::detail::within(entry.first, base); });
}
void store(ParentState &state, const Location &base, const ParentState &relative) {
  erase(state, base);
  for (const auto &[position, parents] : relative)
    if (!parents.empty())
      state.emplace(storage::detail::append(base, position.path), parents);
}
ParentSet dependencies(const ParentState &state, const Location &base) {
  ParentSet result;
  for (const auto &[position, parents] : state)
    if (storage::detail::within(position, base))
      merge(result, parents);
  return result;
}
std::set<Location> reads(const core::Program &p, const storage::detail::Places &places,
                         const std::set<SymbolId> &symbols) {
  std::set<Location> result;
  for (auto id : symbols) {
    result.insert(symbol(id));
    if (p.semantics.containsReferences(p.semantics.symbols.at(id).type))
      if (auto base = places.get(id))
        result.insert(*base);
  }
  return result;
}
ParentState transfer(ParentState state, const Op &op) {
  ParentState result;
  if (op.kind == Op::Seed) {
    for (const auto &[path, parents] : op.seeds)
      result[{Location::Value, None, path}] = parents;
  } else if (op.kind == Op::Bind || op.kind == Op::Store)
    result = extract(state, value(op.inputs.at(0)));
  else if (op.kind == Op::Move || op.kind == Op::Take || op.kind == Op::Alias ||
           op.kind == Op::Reborrow)
    result = extract(state, op.place);
  else if (op.kind == Op::Forward) {
    for (const auto &[path, source] : op.transfers) {
      auto destination = value(None);
      destination.path = path;
      store(result, destination, extract(state, source));
    }
  } else if (op.kind == Op::Construct)
    for (size_t n = 0; n < op.inputs.size(); ++n) {
      auto base = value(None);
      base.path = op.paths[n];
      store(result, base, extract(state, value(op.inputs[n])));
    }
  if (op.kind == Op::Reborrow) {

    auto &parents = result[value(None)];
    merge(parents, op.seeds.at({}));
  }
  if (op.kind == Op::Move || op.kind == Op::Take || op.kind == Op::Alias || op.kind == Op::Drop ||
      op.kind == Op::EndAlias)
    erase(state, op.place);
  for (auto input : op.inputs)
    erase(state, value(input));
  if (op.kind == Op::Scope || op.kind == Op::Return)
    for (auto local : op.locals)
      erase(state, symbol(local));
  if (op.kind == Op::Return)
    state.clear();
  if (op.destination.id != None)
    store(state, op.destination, result);
  return state;
}
}

std::map<uint32_t, ParentOperation> parentOperations(const core::Program &p,
                                                     const FunctionGraph &graph) {
  const auto &function = *std::find_if(p.functions.begin(), p.functions.end(),
                                       [&](const auto &f) { return f.id == graph.function; });
  storage::detail::Places places(p, graph, function);
  std::function<SymbolId(ValueId, std::set<ValueId> &)> referenceSource;
  referenceSource = [&](ValueId id, std::set<ValueId> &seen) -> SymbolId {
    if (!seen.insert(id).second)
      return None;
    const auto *i = places.producer(id);
    if (!i)
      return None;
    if (i->kind == I::ReferenceArgument)
      return i->slot;
    if (!core::functionCall(*i) && i->kind != I::ContainerCall)
      return None;
    const auto &target = p.semantics.functions.at(i->function);
    if (!target.returnsReference())
      return None;
    auto index = std::find(target.params.begin(), target.params.end(), target.resultOrigin) -
                 target.params.begin();
    if (i->slot != None && index == 0)
      return i->slot;
    return referenceSource(i->args.at(index - (i->slot != None)), seen);
  };
  std::map<uint32_t, ParentSet> seeds;
  bool forwarding = false;
  for (auto *i : places.instructions) {
    if (!i || !graph.blocks[i->semanticPoint].reachable)
      continue;
    if (core::storedFieldReborrow(p.semantics, *i)) {
      seeds[i->semanticPoint].insert(i->slot);
      continue;
    }
    if (i->kind == I::StoreReference && i->access == Access::Unique) {
      seeds[i->semanticPoint].insert(i->slot);
      continue;
    }
    if (!core::functionCall(*i) && i->kind != I::ContainerCall)
      continue;
    const auto &target = p.semantics.functions.at(i->function);
    if (!target.resultBorrowedContainer || target.resultAccess != Access::Unique)
      continue;
    if (target.resultPayloadSource) {
      forwarding = true;
      continue;
    }
    auto index = std::find(target.params.begin(), target.params.end(), target.resultOrigin) -
                 target.params.begin();
    std::set<ValueId> seen;
    SymbolId parent = i->slot != None && index == 0
                          ? i->slot
                          : referenceSource(i->args.at(index - (i->slot != None)), seen);
    if (parent != None && p.semantics.symbols.at(parent).reference)
      seeds[i->semanticPoint].insert(parent);
  }

  if (seeds.empty() && !forwarding && graph.parentOperations.empty())
    return {};
  std::vector<SymbolId> locals;
  std::map<uint32_t, std::vector<SymbolId>> loopLocals;
  std::function<void(const std::vector<I> &, std::vector<SymbolId> &)> collect;
  collect = [&](const auto &body, auto &out) {
    for (const auto &i : body) {
      if (i.kind == I::Bind && !p.semantics.symbols.at(i.slot).reference)
        out.push_back(i.slot);
      collect(i.body, out);
      collect(i.otherwise, out);
      collect(i.condition, out);
      collect(i.latch, out);
    }
  };
  std::set<SymbolId> ownedLocals;
  for (const auto &block : graph.blocks)
    for (auto id : block.definitions)
      if (!p.semantics.symbols.at(id).reference)
        ownedLocals.insert(id);
  locals.assign(ownedLocals.begin(), ownedLocals.end());
  for (auto *i : places.instructions)
    if (i && i->kind == I::Loop) {
      collect(i->body, loopLocals[i->loop]);
      collect(i->condition, loopLocals[i->loop]);
      collect(i->latch, loopLocals[i->loop]);
    }
  std::map<uint32_t, Op> operations;
  for (auto *i : places.instructions) {
    if (!i || !graph.blocks[i->semanticPoint].reachable)
      continue;
    Op op{};
    op.kind = Op::Consume;
    op.reads = reads(p, places, graph.blocks[i->semanticPoint].uses);
    auto base = i->slot == None ? std::optional<Location>{} : places.get(i->slot);
    if (i->kind == I::Bind) {
      op.kind = Op::Bind;
      op.destination = symbol(i->slot);
      op.inputs = i->args;
    } else if (i->kind == I::Move) {
      op.kind = Op::Move;
      op.place = symbol(i->slot);
      op.destination = value(i->result);
    } else if (i->kind == I::FieldTake || i->kind == I::FieldStore || i->kind == I::FieldDrop) {
      if (!base)
        fail(i->loc, DiagnosticCode::InvalidCore, "parent obligation has no field place");
      op.place = storage::detail::fieldOperation(*base, *i);
      if (i->kind == I::FieldTake) {
        op.kind = Op::Take;
        op.destination = value(i->result);
      } else if (i->kind == I::FieldStore) {
        op.kind = Op::Store;
        op.destination = op.place;
        op.inputs = i->args;
      } else
        op.kind = Op::Drop;
    } else if (i->kind == I::StructInit || i->kind == I::EnumConstruct) {
      op.kind = Op::Construct;
      op.destination = value(i->result);
      op.inputs = i->args;
      for (size_t n = 0; n < i->args.size(); ++n)
        op.paths.push_back(
            i->kind == I::StructInit
                ? hir::ReferencePath{{hir::ReferenceProjectionKind::Field, i->fieldPath[n]}}
                : hir::payloadPosition(i->offset, n));
    } else if (core::storedFieldReborrow(p.semantics, *i)) {
      if (!base)
        fail(i->loc, DiagnosticCode::InvalidCore, "stored reborrow has no field place");
      op.kind = Op::Reborrow;
      op.place = storage::detail::append(*base, i->payloadPath);
      op.destination = symbol(i->offset);
      op.seeds[{}].insert(i->slot);
    } else if (i->kind == I::BorrowUnique &&
               (!i->payloadPath.empty() ||
                p.semantics.reference(p.semantics.symbols.at(i->slot).type))) {
      auto leaf =
          p.semantics.referencePositionType(p.semantics.symbols.at(i->slot).type, i->payloadPath);
      if (leaf && p.semantics.reference(*leaf) &&
          p.semantics.types[*leaf].referenceAccess == Access::Unique) {
        op.kind = Op::Alias;
        op.place = storage::detail::append(symbol(i->slot), i->payloadPath);
        op.destination = symbol(i->offset);
      }
    } else if (i->kind == I::StoreReference && i->access == Access::Unique) {
      op.kind = Op::Seed;
      op.destination = value(i->result);
      op.seeds[{}] = seeds.at(i->semanticPoint);
    } else if (i->kind == I::Drop || i->kind == I::EndBorrow) {
      op.kind = i->kind == I::Drop ? Op::Drop : Op::EndAlias;
      op.place = symbol(i->slot);
    } else if (i->kind == I::ScopeEnd || i->kind == I::Break || i->kind == I::Continue) {
      op.kind = Op::Scope;
      op.locals = i->kind == I::ScopeEnd ? i->locals : loopLocals[i->loop];
      std::erase_if(op.locals, [&](auto id) { return p.semantics.symbols.at(id).reference; });
    } else if (i->kind == I::Return) {
      op.kind = Op::Return;
      op.locals = locals;
    } else if (i->kind == I::Discard)
      op.inputs = i->args;
    else if (core::functionCall(*i) || i->kind == I::ContainerCall ||
             i->kind == I::ProtocolTransition) {
      const auto &target = p.semantics.functions.at(i->function);
      for (size_t n = 0; n < i->args.size(); ++n)
        if (!p.semantics.symbols.at(target.params.at(n + (i->slot != None))).reference)
          op.inputs.push_back(i->args[n]);
      if (auto seed = seeds.find(i->semanticPoint); seed != seeds.end()) {
        op.kind = Op::Seed;
        op.destination = value(i->result);
        for (const auto &leaf : p.semantics.borrowedReferences(i->type))
          op.seeds[leaf.path] = seed->second;
      }
      if (target.resultBorrowedContainer && target.resultAccess == Access::Unique &&
          target.resultPayloadSource) {
        op.kind = Op::Forward;
        op.destination = value(i->result);
        for (const auto &[path, sources] : p.semantics.referenceTransfers(target)) {
          const auto &source = *sources.begin();
          auto index = std::find(target.params.begin(), target.params.end(), source.parameter) -
                       target.params.begin();
          op.transfers[path] =
              storage::detail::append(value(i->args.at(index - (i->slot != None))), source.path);
        }
      }
    }
    operations.emplace(i->semanticPoint, std::move(op));
  }
  return operations;
}

ParentFlow parentObligations(const FunctionGraph &graph) {
  ParentFlow result;
  if (graph.parentOperations.empty())
    return result;
  std::vector<ParentState> in(graph.blocks.size()), out(graph.blocks.size());
  std::vector<bool> reached(graph.blocks.size()), queued(graph.blocks.size());
  std::vector<std::vector<uint32_t>> predecessors(graph.blocks.size());
  for (const auto &b : graph.blocks)
    if (b.reachable)
      for (auto next : b.successors)
        predecessors[next].push_back(b.id);
  std::deque<uint32_t> work{graph.entry};
  queued[graph.entry] = true;
  while (!work.empty()) {
    auto point = work.front();
    work.pop_front();
    queued[point] = false;
    std::optional<ParentState> incoming;
    if (point == graph.entry)
      incoming = ParentState{};
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
    auto op = graph.parentOperations.find(point);
    auto next = op == graph.parentOperations.end() ? in[point] : transfer(in[point], op->second);
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
  for (const auto &b : graph.blocks)
    if (reached[b.id]) {
      const auto &state = in[b.id];
      if (!state.empty())
        result.before[b.id] = state;
      for (const auto &[position, parents] : state)
        if (position.kind == Location::Symbol)
          merge(result.summaries[position.id], parents);
      auto found = graph.parentOperations.find(b.id);
      if (found == graph.parentOperations.end())
        continue;
      const auto &op = found->second;
      ParentSet used;
      for (const auto &read : op.reads)
        merge(used, dependencies(state, read));
      for (auto input : b.valueUses)
        merge(used, dependencies(state, value(input)));
      if (op.kind == Op::Scope || op.kind == Op::Return)
        for (auto local : op.locals)
          merge(used, dependencies(state, symbol(local)));
      if (op.kind == Op::Return)
        for (const auto &[position, parents] : state)
          if (position.kind == Location::Value)
            merge(used, parents);
      if (op.kind == Op::Alias || op.kind == Op::Reborrow) {
        merge(result.aliases[op.destination.id], dependencies(state, op.place));
        if (op.kind == Op::Reborrow)
          merge(result.aliases[op.destination.id], op.seeds.at({}));
      }
      ParentSet pending = used;
      while (!pending.empty()) {
        auto parent = *pending.begin();
        pending.erase(pending.begin());
        auto more = dependencies(state, symbol(parent));
        for (auto next : more)
          if (used.insert(next).second)
            pending.insert(next);
      }
      if (!used.empty())
        result.uses[b.id] = std::move(used);
    }
  return result;
}

void validateParentObligations(const core::Program &p) {
  for (const auto &graph : p.regionGraphs) {
    const auto &function = *std::find_if(p.functions.begin(), p.functions.end(),
                                         [&](const auto &f) { return f.id == graph.function; });
    storage::detail::Places places(p, graph, function);
    auto active = parentOperations(p, graph);
    for (auto &[point, op] : active) {

      auto original = graph.parentOperations.find(point);
      if (original == graph.parentOperations.end())
        fail({}, DiagnosticCode::InvalidCore, "missing typed parent obligation");
      for (const auto &read : original->second.reads)
        if (!op.reads.contains(read))
          fail({}, DiagnosticCode::InvalidCore, "untyped parent dependency read");
      const auto inferred = graph.parentUses.contains(point)
                                ? reads(p, places, graph.parentUses.at(point))
                                : std::set<Location>{};
      for (const auto &read : op.reads)
        if (!original->second.reads.contains(read) && !inferred.contains(read))
          fail({}, DiagnosticCode::InvalidCore, "missing original parent dependency read");
      if (op.kind == Op::Scope && op.locals != original->second.locals) {

        for (auto id : op.locals)
          if (std::find(original->second.locals.begin(), original->second.locals.end(), id) ==
              original->second.locals.end())
            fail({}, DiagnosticCode::InvalidCore, "untyped parent cleanup binding");
        for (auto id : original->second.locals)
          if (std::find(op.locals.begin(), op.locals.end(), id) == op.locals.end()) {
            bool defined = false;
            for (const auto &block : graph.blocks)
              if (block.definitions.contains(id)) {
                defined = true;
                if (graph.resourcePoints.contains(block.id))
                  fail({}, DiagnosticCode::InvalidCore, "live parent cleanup binding was omitted");
              }
            if (!defined)
              fail({}, DiagnosticCode::InvalidCore, "unknown original parent cleanup binding");
          }
        op.locals = original->second.locals;
      }
      op.reads = original->second.reads;
      if (op != original->second)
        fail({}, DiagnosticCode::InvalidCore, "typed parent obligation was altered");
    }
    for (const auto &[point, op] : graph.parentOperations) {
      if (point >= graph.blocks.size() || !graph.blocks[point].reachable ||
          (graph.resourcePoints.contains(point) && !active.contains(point)))
        fail({}, DiagnosticCode::InvalidCore, "invalid original parent operation");
      for (auto input : op.inputs)
        if (!graph.blocks[point].valueUses.contains(input))
          fail({}, DiagnosticCode::InvalidCore, "parent transfer has no typed value operand");
      if (op.destination.id != None &&
          (op.destination.kind == Location::Value
               ? !graph.blocks[point].valueDefinitions.contains(op.destination.id)
               : op.kind != Op::Store &&
                     !graph.blocks[point].definitions.contains(op.destination.id)))
        fail({}, DiagnosticCode::InvalidCore, "parent transfer has no typed destination");
    }
    auto facts = parentObligations(graph);
    if (facts.before != graph.parentStates || facts.uses != graph.parentUses ||
        facts.aliases != graph.loanParents || facts.summaries != graph.storedResultParents)
      fail({}, DiagnosticCode::InvalidCore, "parent obligations contradict the typed CFG");
    for (const auto &[point, parents] : facts.uses)
      for (auto parent : parents)
        if (!graph.blocks.at(point).uses.contains(parent))
          fail({}, DiagnosticCode::InvalidCore,
               "suspended parent is missing from semantic liveness");
  }
}
}
