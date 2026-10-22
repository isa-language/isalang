#include "region_inference.hpp"
#include "storage_paths.hpp"
#include <algorithm>
#include <functional>

namespace isac::regions::detail {
using I = core::Inst;
RegionId freshRegionStart(const core::Program &program) {
  RegionId next = 0;
  auto reserve = [&](RegionId region) {
    if (region != None)
      next = std::max(next, region + 1);
  };
  for (const auto &symbol : program.semantics.symbols)
    reserve(symbol.region);
  for (const auto &function : program.semantics.functions)
    reserve(function.resultRegion);
  return next;
}
Contracts contracts(const core::Program &program, RegionId &nextRegion) {
  Contracts result;
  for (const auto &function : program.semantics.functions) {
    auto &contract = result[function.id];
    for (const auto &input : function.parameterRegions)
      contract.inputs[input.parameter][{}] = {input.region};
    for (const auto &input : function.payloadParameterRegions)
      contract.inputs[input.parameter][input.path] = {input.region};
    if (function.resultOrigin == None)
      continue;
    for (const auto &[path, sources] : program.semantics.referenceTransfers(function)) {
      auto region = function.returnsReference() ? function.resultRegion : nextRegion++;
      contract.result[path] = {region};
      for (const auto &source : sources) {
        const auto &input = contract.inputs.at(source.parameter).at(source.path);
        contract.resultSources[region].insert(input.begin(), input.end());
      }
      if (contract.resultSources[region].empty())
        fail(function.loc, DiagnosticCode::InvalidCore,
             "reference result position has no regional input contract");
    }
  }
  return result;
}
namespace {
const hir::RegionArgs &lookupArguments(const auto &entries, uint32_t id) {
  static const hir::RegionArgs empty;
  auto found = entries.find(id);
  return found == entries.end() ? empty : found->second;
}
hir::RegionSubstitution instantiate(const core::Program &program, const FunctionGraph &graph,
                                    const I &instruction, const FunctionRegionContract &contract) {
  const auto &function = program.semantics.functions.at(instruction.function);
  hir::RegionSubstitution substitution;
  for (const auto &[parameter, arguments] : contract.inputs) {
    auto index = std::find(function.params.begin(), function.params.end(), parameter) -
                 function.params.begin();
    const auto *actual =
        instruction.slot != None && index == 0
            ? &lookupArguments(graph.symbolRegions, instruction.slot)
            : &lookupArguments(graph.valueRegions,
                               instruction.args.at(index - (instruction.slot != None)));
    hir::RegionArgs projected;
    if (instruction.slot != None && index == 0 &&
        program.semantics.types[program.semantics.symbols[instruction.slot].type].kind ==
            hir::Type::Structure &&
        program.semantics.containsReferences(program.semantics.symbols[instruction.slot].type)) {
      const auto &definition = *std::find_if(program.functions.begin(), program.functions.end(),
                                             [&](const auto &f) { return f.id == graph.function; });
      storage::detail::Places places(program, graph, definition);
      if (auto base = places.get(instruction.slot)) {
        auto at = graph.fieldRegions.find(instruction.semanticPoint);
        if (at != graph.fieldRegions.end())
          for (const auto &[path, regions] : lookupArguments(at->second, base->id))
            if (path.size() >= base->path.size() &&
                std::equal(base->path.begin(), base->path.end(), path.begin()))
              projected[{path.begin() + base->path.size(), path.end()}] = regions;
      }
      actual = &projected;
    }
    for (const auto &[path, formalRegions] : arguments)
      for (auto formal : formalRegions) {
        auto &binding = substitution[formal];
        if (auto found = actual->find(path); found != actual->end())
          binding.insert(found->second.begin(), found->second.end());
        else if (instruction.slot != None && index == 0 && path.empty() &&
                 graph.storageRegions.contains(instruction.slot))
          binding.insert(graph.storageRegions.at(instruction.slot));
      }
  }
  for (const auto &[result, sources] : contract.resultSources)
    for (auto source : sources) {
      const auto &actual = substitution.at(source);
      substitution[result].insert(actual.begin(), actual.end());
    }
  return substitution;
}
class RegionFacts {
  core::Program &program;
  FunctionGraph &graph;
  const std::vector<I *> &instructions;
  const std::map<SymbolId, SymbolId> &parents;
  const Contracts &signatures;
  RegionId &nextRegion;
  static const hir::RegionArgs &lookup(const auto &entries, uint32_t id) {
    static const hir::RegionArgs empty;
    auto found = entries.find(id);
    return found == entries.end() ? empty : found->second;
  }
  hir::RegionSubstitution instantiate(const I &instruction) const {
    return detail::instantiate(program, graph, instruction, signatures.at(instruction.function));
  }
  static bool merge(hir::RegionArgs &target, const hir::RegionArgs &source) {
    bool changed = false;
    for (const auto &[path, regions] : source) {
      auto &entry = target[path];
      auto size = entry.size();
      entry.insert(regions.begin(), regions.end());
      changed |= size != entry.size();
    }
    return changed;
  }

public:
  RegionFacts(core::Program &p, FunctionGraph &g, const std::vector<I *> &ops,
              const std::map<SymbolId, SymbolId> &aliases, const Contracts &contracts,
              RegionId &fresh)
      : program(p), graph(g), instructions(ops), parents(aliases), signatures(contracts),
        nextRegion(fresh) {}
  void regionArguments() {
    graph.contract = signatures.at(graph.function);
    graph.symbolRegions = graph.contract.inputs;
    graph.fieldRegionOperations = fieldRegionOperations(program, graph);
    for (const auto &loan : graph.loans)
      graph.symbolRegions[loan.carrier][{}] = {loan.region};

    for (auto *i : instructions)
      if (i && (i->kind == I::Call || i->kind == I::ContainerCall) && i->slot != None &&
          graph.blocks[i->semanticPoint].reachable &&
          !program.semantics.symbols.at(i->slot).reference) {
        const auto &function = program.semantics.functions[i->function];
        const auto &contract = signatures.at(i->function);
        auto receiver = contract.inputs.find(function.params.at(0));
        if (receiver != contract.inputs.end() && receiver->second.contains({}) &&
            !graph.storageRegions.contains(i->slot)) {
          auto region = nextRegion++;
          graph.storageRegions[i->slot] = region;
          graph.regions.emplace(region, Region{region, RegionKind::Storage, {}});
        }
      }

    bool changed = true;
    while (changed) {
      changed = false;
      const auto fields = fieldArguments(graph);
      graph.fieldRegions = fields.before;
      for (const auto &[symbol, args] : fields.summaries)
        changed |= merge(graph.symbolRegions[symbol], args);
      for (auto *i : instructions) {
        if (!i || !graph.blocks[i->semanticPoint].reachable)
          continue;
        hir::RegionArgs value;
        if (i->kind == I::FieldTake ||
            (i->kind == I::Move && program.semantics.types[i->type].kind == hir::Type::Structure &&
             program.semantics.localCarrierType(i->type))) {
          auto found = fields.values.find(i->result);
          if (found != fields.values.end())
            value = found->second;
        } else if (i->kind == I::ReferenceArgument || i->kind == I::StoreReference ||
                   ((i->kind == I::Move || i->kind == I::Copy) &&
                    program.semantics.containsReferences(i->type)))
          value = lookup(graph.symbolRegions, i->slot);
        else if (i->kind == I::Bind) {
          const auto &input = lookup(graph.valueRegions, i->args.at(0));
          if (!input.empty())
            changed |= merge(graph.symbolRegions[i->slot], input);
        } else if (i->kind == I::EnumConstruct || i->kind == I::StructInit || i->kind == I::Array) {
          for (size_t n = 0; n < i->args.size(); ++n)
            for (const auto &[path, regions] : lookup(graph.valueRegions, i->args[n])) {
              hir::ReferencePath position;
              if (i->kind == I::EnumConstruct)
                position = hir::payloadPosition(i->offset, n);
              else
                position = {{i->kind == I::StructInit ? hir::ReferenceProjectionKind::Field
                                                      : hir::ReferenceProjectionKind::Element,
                             i->kind == I::StructInit ? i->fieldPath.at(n) : None}};
              position.insert(position.end(), path.begin(), path.end());
              value[position].insert(regions.begin(), regions.end());
            }
        } else if (i->kind == I::Call || i->kind == I::ContainerCall) {
          const auto &contract = signatures.at(i->function);
          if (!contract.result.empty())
            value = hir::substituteRegions(contract.result, instantiate(*i));
        }
        if (i->result != None && !value.empty())
          changed |= merge(graph.valueRegions[i->result], value);
        for (const auto &phi : i->merges) {
          for (auto input : {phi.left, phi.right}) {
            const auto &arguments = lookup(graph.valueRegions, input);
            if (!arguments.empty())
              changed |= merge(graph.valueRegions[phi.result], arguments);
          }
        }
      }
    }
    graph.fieldRegions = fieldArguments(graph).before;
    const auto &definition = *std::find_if(program.functions.begin(), program.functions.end(),
                                           [&](const auto &f) { return f.id == graph.function; });
    std::optional<storage::detail::Places> places;
    for (auto *i : instructions) {
      if (!i || !graph.blocks[i->semanticPoint].reachable)
        continue;
      if ((i->kind == I::BorrowShared || i->kind == I::BorrowUnique) &&
          (!i->payloadPath.empty() ||
           program.semantics.reference(program.semantics.symbols[i->slot].type))) {
        auto source = i->slot;
        std::set<SymbolId> seen;
        while (parents.contains(source) && seen.insert(source).second)
          source = parents.at(source);
        const auto *arguments = &lookup(graph.symbolRegions, source);
        hir::ReferencePath positionPath = i->payloadPath;
        if (program.semantics.types[program.semantics.symbols[source].type].kind ==
            hir::Type::Structure) {

          static const std::map<SymbolId, hir::RegionArgs> empty;
          auto at = graph.fieldRegions.find(i->semanticPoint);
          const auto &point = at == graph.fieldRegions.end() ? empty : at->second;
          if (!places)
            places.emplace(program, graph, definition);
          if (auto base = places->get(i->slot)) {
            source = base->id;
            positionPath = base->path;
            positionPath.insert(positionPath.end(), i->payloadPath.begin(), i->payloadPath.end());
          }
          arguments = &lookup(point, source);
        }
        if (auto position = arguments->find(positionPath); position != arguments->end())
          for (auto region : position->second)
            graph.constraints.push_back({ConstraintKind::Subregion,
                                         i->region,
                                         region,
                                         {},
                                         ConstraintReason::StoredReference,
                                         i->loc});
      }
      if ((i->kind == I::BorrowShared || i->kind == I::BorrowUnique) &&
          i->placeProjection == I::PlaceProjection::None && !i->args.empty())
        for (const auto &[path, regions] : lookup(graph.valueRegions, i->args[0]))
          for (auto region : regions)
            graph.constraints.push_back({ConstraintKind::Subregion,
                                         i->region,
                                         region,
                                         {},
                                         ConstraintReason::FunctionResult,
                                         i->loc});
      if (i->kind != I::Call && i->kind != I::ContainerCall)
        continue;
      const auto &contract = signatures.at(i->function);
      if (!contract.inputs.empty() || !contract.result.empty())
        graph.calls.push_back({{i->semanticPoint, 0},
                               i->function,
                               instantiate(*i),
                               lookup(graph.valueRegions, i->result)});
    }
  }
  void storageLimits() {
    std::set<SymbolId> owners, carriers, tracked;
    for (const auto &loan : graph.loans) {
      const auto &symbol = program.semantics.symbols.at(loan.place.root);

      if (symbol.reference || program.semantics.containsReferences(symbol.type))
        continue;
      owners.insert(symbol.id);
      graph.ownerBounds[loan.region].insert(symbol.id);
      graph.storageBounds.emplace(symbol.id, StorageBound{symbol.id, symbol.loc, {}});
    }
    for (const auto &[owner, region] : graph.storageRegions) {
      owners.insert(owner);
      graph.ownerBounds[region].insert(owner);
      graph.storageBounds.emplace(owner,
                                  StorageBound{owner, program.semantics.symbols.at(owner).loc, {}});
    }
    for (const auto &[symbol, arguments] : graph.symbolRegions)
      if (!arguments.empty() && !program.semantics.symbols.at(symbol).reference)
        carriers.insert(symbol);
    tracked = owners;
    tracked.insert(carriers.begin(), carriers.end());
    if (tracked.empty())
      return;
    std::map<uint32_t, std::set<SymbolId>> loopLocals;
    std::function<void(const std::vector<I> &, std::set<SymbolId> &)> collect;
    collect = [&](const auto &body, auto &locals) {
      for (const auto &i : body) {
        if (i.kind == I::Bind && tracked.contains(i.slot))
          locals.insert(i.slot);
        collect(i.body, locals);
        collect(i.otherwise, locals);
        collect(i.condition, locals);
        collect(i.latch, locals);
      }
    };
    for (auto *i : instructions)
      if (i && i->kind == I::Loop) {
        collect(i->body, loopLocals[i->loop]);
        collect(i->condition, loopLocals[i->loop]);
        collect(i->latch, loopLocals[i->loop]);
      }
    std::set<SymbolId> entry;
    for (auto parameter : program.semantics.functions[graph.function].params)
      if (tracked.contains(parameter))
        entry.insert(parameter);
    std::vector<std::vector<BasicBlockId>> predecessors(graph.blocks.size());
    for (const auto &b : graph.blocks)
      for (auto next : b.successors)
        if (b.reachable)
          predecessors[next].push_back(b.id);

    std::vector<std::set<SymbolId>> mustIn(graph.blocks.size(), tracked),
        mustOut(graph.blocks.size(), tracked), mayIn(graph.blocks.size()),
        mayOut(graph.blocks.size());
    auto transfer = [&](BasicBlockId id, std::set<SymbolId> values) {
      if (auto *i = instructions[id]) {
        if (i->kind == I::Bind && tracked.contains(i->slot))
          values.insert(i->slot);
        if (i->kind == I::Move || i->kind == I::Drop)
          values.erase(i->slot);
        if (i->kind == I::ScopeEnd)
          for (auto local : i->locals)
            values.erase(local);
        if (i->kind == I::Break || i->kind == I::Continue)
          for (auto local : loopLocals[i->loop])
            values.erase(local);
        if (i->kind == I::Return)
          values.clear();
      }
      return values;
    };
    bool changed = true;
    while (changed) {
      changed = false;
      for (const auto &b : graph.blocks) {
        if (!b.reachable)
          continue;
        auto must = b.id == graph.entry ? entry : tracked;
        auto may = b.id == graph.entry ? entry : std::set<SymbolId>{};
        for (auto previous : predecessors[b.id]) {
          std::erase_if(must, [&](auto symbol) { return !mustOut[previous].contains(symbol); });
          may.insert(mayOut[previous].begin(), mayOut[previous].end());
        }
        auto required = transfer(b.id, must), possible = transfer(b.id, may);
        changed |= mustIn[b.id] != must || mustOut[b.id] != required || mayIn[b.id] != may ||
                   mayOut[b.id] != possible;
        mustIn[b.id] = std::move(must);
        mustOut[b.id] = std::move(required);
        mayIn[b.id] = std::move(may);
        mayOut[b.id] = std::move(possible);
      }
    }
    auto demand = [&](const hir::RegionArgs &arguments, const BasicBlock &b) {
      for (const auto &[path, regions] : arguments)
        for (auto region : regions)
          graph.constraints.push_back({ConstraintKind::ContainsPoint,
                                       region,
                                       None,
                                       {b.id, 0},
                                       ConstraintReason::StoredReference,
                                       b.origin});
    };
    auto current = [&](SymbolId symbol, const BasicBlock &b) -> const hir::RegionArgs & {
      static const hir::RegionArgs empty;
      const auto &type = program.semantics.types[program.semantics.symbols[symbol].type];
      if (type.kind == hir::Type::Structure &&
          program.semantics.localCarrierType(program.semantics.symbols[symbol].type)) {
        auto point = graph.fieldRegions.find(b.id);
        if (point == graph.fieldRegions.end())
          return empty;
        auto found = point->second.find(symbol);
        return found == point->second.end() ? empty : found->second;
      }
      return graph.symbolRegions.at(symbol);
    };
    for (const auto &b : graph.blocks) {
      if (!b.reachable)
        continue;
      for (auto owner : owners)
        if (mustIn[b.id].contains(owner))
          graph.storageBounds.at(owner).points.insert(b.id);
      for (auto symbol : b.liveIn)
        if (carriers.contains(symbol) && mayIn[b.id].contains(symbol))
          demand(current(symbol, b), b);
      for (auto value : b.valueUses)
        if (graph.valueRegions.contains(value))
          demand(graph.valueRegions.at(value), b);
      if (auto *i = instructions[b.id]) {
        auto cleanup = i->kind == I::Return || i->kind == I::Break || i->kind == I::Continue;
        if (cleanup || i->kind == I::ScopeEnd)
          for (auto carrier : carriers)
            if (mayIn[b.id].contains(carrier) &&
                (cleanup ||
                 std::find(i->locals.begin(), i->locals.end(), carrier) != i->locals.end()))
              demand(current(carrier, b), b);
      }
    }
  }
};
}
void arguments(core::Program &p, FunctionGraph &g, const std::vector<I *> &ops,
               const std::map<SymbolId, SymbolId> &aliases, const Contracts &contracts,
               RegionId &fresh) {
  RegionFacts(p, g, ops, aliases, contracts, fresh).regionArguments();
}
void storageLimits(core::Program &p, FunctionGraph &g, const std::vector<I *> &ops,
                   RegionId &fresh) {
  RegionFacts(p, g, ops, {}, {}, fresh).storageLimits();
}
void cleanupUses(core::Program &program, FunctionGraph &graph, const std::vector<I *> &ops,
                 const std::map<SymbolId, SymbolId> &parents) {
  if (graph.loans.empty())
    return;
  std::map<SymbolId, SymbolId> roots;
  for (const auto &loan : graph.loans) {
    auto root = loan.carrier;
    std::set<SymbolId> seen;
    while (parents.contains(root) && seen.insert(root).second)
      root = parents.at(root);
    roots[loan.carrier] = root;
  }
  std::map<uint32_t, std::set<SymbolId>> loopAliases;
  std::function<void(const std::vector<I> &, std::set<SymbolId> &)> collect;
  collect = [&](const auto &body, auto &aliases) {
    for (const auto &i : body) {
      if (i.kind == I::BorrowShared || i.kind == I::BorrowUnique)
        aliases.insert(i.offset);
      collect(i.body, aliases);
      collect(i.otherwise, aliases);
      collect(i.condition, aliases);
      collect(i.latch, aliases);
    }
  };
  for (auto *i : ops)
    if (i && i->kind == I::Loop) {
      collect(i->body, loopAliases[i->loop]);
      collect(i->condition, loopAliases[i->loop]);
      collect(i->latch, loopAliases[i->loop]);
    }
  std::vector<std::vector<BasicBlockId>> predecessors(graph.blocks.size());
  for (const auto &block : graph.blocks)
    if (block.reachable)
      for (auto successor : block.successors)
        predecessors[successor].push_back(block.id);

  std::vector<std::set<SymbolId>> ordinaryIn(graph.blocks.size()), ordinaryOut(graph.blocks.size());
  bool ordinaryChanged = true;
  while (ordinaryChanged) {
    ordinaryChanged = false;
    for (const auto &block : graph.blocks) {
      if (!block.reachable)
        continue;
      std::set<SymbolId> outgoing;
      for (auto successor : block.successors)
        outgoing.insert(ordinaryIn[successor].begin(), ordinaryIn[successor].end());
      auto incoming = outgoing;
      for (auto symbol : block.definitions)
        incoming.erase(symbol);
      auto uses = block.uses;
      if (auto *i = ops[block.id]; i && i->kind == I::EndBorrow)
        uses.erase(i->slot);
      incoming.insert(uses.begin(), uses.end());
      ordinaryChanged |= ordinaryIn[block.id] != incoming || ordinaryOut[block.id] != outgoing;
      ordinaryIn[block.id] = std::move(incoming);
      ordinaryOut[block.id] = std::move(outgoing);
    }
  }
  std::vector<std::set<SymbolId>> in(graph.blocks.size()), out(graph.blocks.size());
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &block : graph.blocks) {
      if (!block.reachable)
        continue;
      std::set<SymbolId> incoming;
      for (auto predecessor : predecessors[block.id])
        incoming.insert(out[predecessor].begin(), out[predecessor].end());
      auto active = incoming;
      if (auto *i = ops[block.id]) {
        if (i->kind == I::BorrowShared || i->kind == I::BorrowUnique)
          active.insert(i->offset);
        if (i->kind == I::EndBorrow)
          std::erase_if(active, [&](auto carrier) {
            std::set<SymbolId> seen;
            while (carrier != i->slot && parents.contains(carrier) && seen.insert(carrier).second)
              carrier = parents.at(carrier);
            return carrier == i->slot;
          });
        if (i->kind == I::ScopeEnd)
          std::erase_if(active, [&](auto carrier) {
            return !ordinaryOut[block.id].contains(carrier) &&
                   (std::find(i->locals.begin(), i->locals.end(), roots.at(carrier)) !=
                        i->locals.end() ||
                    std::find(i->locals.begin(), i->locals.end(), carrier) != i->locals.end());
          });
        if (i->kind == I::Return)
          active.clear();
        if (i->kind == I::Break || i->kind == I::Continue)
          for (auto alias : loopAliases[i->loop])
            active.erase(alias);
      }
      changed |= in[block.id] != incoming || out[block.id] != active;
      in[block.id] = std::move(incoming);
      out[block.id] = std::move(active);
    }
  }

  for (auto &block : graph.blocks) {
    auto *i = ops[block.id];
    if (!i || !block.reachable)
      continue;
    if (i->kind == I::EndBorrow && !in[block.id].contains(i->slot))
      block.uses.erase(i->slot);
    if (i->kind == I::ScopeEnd)
      for (auto carrier : in[block.id])
        if (!out[block.id].contains(carrier)) {
          block.definitions.insert(carrier);
          if (!program.semantics.symbols[carrier].inferredRegion)
            block.uses.insert(carrier);
        }
    if (i->kind == I::Break || i->kind == I::Continue)
      block.definitions.insert(loopAliases[i->loop].begin(), loopAliases[i->loop].end());
  }
}
void validateArguments(const core::Program &program) {
  auto fresh = freshRegionStart(program);
  const auto signatures = contracts(program, fresh);

  for (const auto &graph : program.regionGraphs) {
    const auto &signature = program.semantics.functions[graph.function];
    const auto &expected = signatures.at(graph.function);
    if (graph.contract.inputs != expected.inputs || graph.contract.result != expected.result ||
        graph.contract.resultSources != expected.resultSources)
      fail(signature.loc, DiagnosticCode::InvalidCore,
           "function region contract contradicts positional reference transfers");
    auto regionKind = [&](const hir::RegionArgs &arguments, RegionKind kind) {
      for (const auto &[path, regions] : arguments)
        for (auto region : regions) {
          auto definition = graph.regions.find(region);
          if (definition == graph.regions.end() || definition->second.kind != kind)
            fail(signature.loc, DiagnosticCode::InvalidCore,
                 "region parameter has an incompatible semantic kind");
        }
    };
    for (const auto &[input, arguments] : expected.inputs)
      regionKind(arguments, RegionKind::Parameter);
    regionKind(expected.result, RegionKind::Return);
    std::map<TypeId, std::set<hir::ReferencePath>> schemas;
    auto check = [&](const hir::RegionArgs &arguments, TypeId type, bool reference, Loc origin) {
      if (type >= program.semantics.types.size())
        fail(origin, DiagnosticCode::InvalidCore, "semantic value has an undefined type");
      auto &positions = schemas[type];
      if (!reference && positions.empty())
        for (const auto &leaf : program.semantics.borrowedReferences(type))
          positions.insert(leaf.path);
      for (const auto &[path, regions] : arguments) {
        if ((reference ? !path.empty() : !positions.contains(path)) || regions.empty())
          fail(origin, DiagnosticCode::InvalidCore,
               "region arguments contradict the semantic reference positions");
        for (auto region : regions)
          if (region == None || !graph.regions.contains(region))
            fail(origin, DiagnosticCode::InvalidCore,
                 "region argument has no definition in the semantic CFG");
      }
    };
    for (const auto &[symbol, arguments] : graph.symbolRegions) {
      const auto &source = program.semantics.symbols.at(symbol);
      check(arguments, source.type, source.reference, source.loc);
    }
    for (const auto &[parameter, arguments] : graph.contract.inputs) {
      const auto &source = program.semantics.symbols.at(parameter);
      check(arguments, source.type, source.reference, source.loc);
    }
    check(graph.contract.result, signature.result, signature.returnsReference(), signature.loc);
    const auto &function = *std::find_if(program.functions.begin(), program.functions.end(),
                                         [&](const auto &f) { return f.id == graph.function; });
    std::set<ValueId> checked;
    std::map<uint32_t, const I *> calls;
    std::function<void(const std::vector<I> &)> values;
    values = [&](const auto &body) {
      for (const auto &i : body) {
        if (i.kind == I::Call || i.kind == I::ContainerCall)
          calls[i.semanticPoint] = &i;
        bool reference = i.kind == I::ReferenceArgument ||
                         ((i.kind == I::Call || i.kind == I::ContainerCall) &&
                          program.semantics.functions[i.function].returnsReference());
        if (i.result != None) {
          auto definition = graph.valueDefinitions.find(i.result);
          if (definition == graph.valueDefinitions.end() || definition->second.type != i.type ||
              definition->second.reference != reference ||
              definition->second.point.block != i.semanticPoint ||
              definition->second.point.statementIndex != 0)
            fail(i.loc, DiagnosticCode::InvalidCore,
                 "semantic value definition contradicts the verified instruction");
        }
        if (auto found = graph.valueRegions.find(i.result); found != graph.valueRegions.end()) {
          check(found->second, i.type, reference, i.loc);
          checked.insert(i.result);
        }
        for (const auto &phi : i.merges)
          if (auto found = graph.valueRegions.find(phi.result); found != graph.valueRegions.end()) {
            const auto &symbol = program.semantics.symbols.at(phi.slot);
            check(found->second, symbol.type, symbol.reference, i.loc);
            checked.insert(phi.result);
          }
        values(i.body);
        values(i.otherwise);
        values(i.condition);
        values(i.latch);
      }
    };
    values(function.body);
    for (const auto &[value, arguments] : graph.valueRegions)
      if (!checked.contains(value)) {
        auto definition = graph.valueDefinitions.find(value);
        if (definition == graph.valueDefinitions.end() ||
            definition->second.point.block >= graph.blocks.size() ||
            definition->second.point.statementIndex != 0 ||
            !graph.blocks.at(definition->second.point.block).valueDefinitions.contains(value) ||
            graph.resourcePoints.contains(definition->second.point.block))
          fail(signature.loc, DiagnosticCode::InvalidCore,
               "region arguments belong to an undefined semantic value");

        check(arguments, definition->second.type, definition->second.reference,
              graph.blocks.at(definition->second.point.block).origin);
      }
    std::map<uint32_t, size_t> callCounts;
    for (const auto &call : graph.calls) {
      if (call.point.block >= graph.blocks.size() || call.point.statementIndex != 0 ||
          call.target >= program.semantics.functions.size())
        fail(signature.loc, DiagnosticCode::InvalidCore,
             "call region contract has an invalid semantic point");
      if (auto actual = calls.find(call.point.block); actual != calls.end()) {
        ++callCounts[call.point.block];
        const auto &instruction = *actual->second;
        const auto &target = signatures.at(instruction.function);
        auto arguments = instantiate(program, graph, instruction, target);
        if (call.target != instruction.function || call.point.statementIndex != 0 ||
            call.substitution != arguments ||
            call.result != hir::substituteRegions(target.result, arguments))
          fail(instruction.loc, DiagnosticCode::InvalidCore,
               "call region substitution contradicts its input positions");
      }
      for (const auto &[formal, actual] : call.substitution)
        for (auto region : actual)
          if (!graph.regions.contains(region))
            fail(signature.loc, DiagnosticCode::InvalidCore,
                 "call substitution contains an undefined region argument");
    }
    for (const auto &[point, instruction] : calls) {
      const auto &target = signatures.at(instruction->function);
      if (graph.resourcePoints.contains(point) &&
          (!target.inputs.empty() || !target.result.empty()) && callCounts[point] != 1)
        fail(instruction->loc, DiagnosticCode::InvalidCore,
             "missing or duplicated call region instantiation");
    }
  }
}
}
