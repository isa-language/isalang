#include "isac/regions.hpp"
#include "isac/core.hpp"
#include "region_inference.hpp"
#include <algorithm>
#include <functional>
#include <map>

namespace isac::regions {
namespace {
using I = core::Inst;
using detail::Contracts;
class Builder {
  core::Program &program;
  const Contracts &signatures;
  RegionId &nextRegion;
  FunctionGraph graph;
  struct Loop {
    BasicBlockId again, exit;
  };
  std::map<uint32_t, Loop> loops;
  std::map<ValueId, SymbolId> referenceArguments;
  std::map<SymbolId, SymbolId> parents;
  std::map<BasicBlockId, BasicBlockId> loopExits;
  std::vector<I *> instructions;
  BasicBlockId node(I *instruction = nullptr) {
    BasicBlock b{};
    b.id = graph.blocks.size();
    if (instruction) {
      b.origin = instruction->loc;
      instruction->semanticPoint = b.id;
      instruction->inferredEnds.clear();
      instruction->exitEnds.clear();
    }
    graph.blocks.push_back(std::move(b));
    instructions.push_back(instruction);
    return graph.blocks.back().id;
  }
  void facts(I &i, BasicBlockId id) {
    auto &b = graph.blocks[id];
    b.valueUses.insert(i.args.begin(), i.args.end());
    if (i.result != None)
      b.valueDefinitions.insert(i.result);
    if (i.slot != None && i.kind != I::Bind && i.kind != I::EndBorrow)
      b.uses.insert(i.slot);
    if (i.kind == I::Bind)
      b.definitions.insert(i.slot);
    if (i.kind == I::BorrowShared || i.kind == I::BorrowUnique) {
      b.definitions.insert(i.offset);
      parents[i.offset] = i.slot;
      Loan loan{};
      loan.id = graph.loans.size();
      loan.region = i.region;
      loan.carrier = i.offset;
      loan.place.root = i.slot;
      if (i.placeProjection == I::PlaceProjection::Field)
        for (auto field : i.fieldPath)
          loan.place.projections.push_back({ProjectionKind::Field, field});
      else if (i.placeProjection == I::PlaceProjection::Index)
        loan.place.projections.push_back({ProjectionKind::Index, None});
      for (const auto &position : i.payloadPath)
        loan.place.projections.push_back(
            {ProjectionKind::VariantPayload, position.index, position.variant});
      loan.capability = i.kind == I::BorrowShared ? BorrowKind::Shared : BorrowKind::Unique;
      loan.created = {id, 0};
      loan.origin = i.loc;
      loan.inferred = program.semantics.symbols.at(i.offset).inferredRegion;
      graph.loans.push_back(std::move(loan));
    }
    if (i.kind == I::ReferenceArgument)
      referenceArguments[i.result] = i.slot;

    if (i.kind == I::EndBorrow && !program.semantics.symbols.at(i.slot).inferredRegion)
      b.uses.insert(i.slot);
  }
  BasicBlockId sequence(std::vector<I> &body, BasicBlockId next) {
    for (auto it = body.rbegin(); it != body.rend(); ++it) {
      auto &i = *it;
      auto id = node(&i);
      facts(i, id);
      if (i.kind == I::UnsafeScope) {
        graph.blocks[id].successors = {sequence(i.body, next)};
      } else if (i.kind == I::If) {
        auto yes = sequence(i.body, next);
        auto no = sequence(i.otherwise, next);
        graph.blocks[id].terminator = Terminator::CondBranch;
        graph.blocks[id].successors = {yes, no};
      } else if (i.kind == I::Loop) {
        loopExits[id] = next;
        auto header = sequence(i.condition, id);
        auto backedge = node();
        graph.blocks[backedge].terminator = Terminator::LoopBackedge;
        graph.blocks[backedge].successors = {header};
        auto latch = sequence(i.latch, backedge);
        loops[i.loop] = {latch, next};
        auto entry = sequence(i.body, latch);
        loops.erase(i.loop);
        graph.blocks[id].terminator = i.unconditional ? Terminator::Branch : Terminator::CondBranch;
        graph.blocks[id].successors = {entry};
        if (!i.unconditional)
          graph.blocks[id].successors.push_back(next);
        next = header;
        continue;
      } else if (i.kind == I::Return) {
        graph.blocks[id].terminator = Terminator::Return;
      } else if (i.kind == I::Break || i.kind == I::Continue) {
        const auto loop = loops.at(i.loop);
        graph.blocks[id].successors = {i.kind == I::Break ? loop.exit : loop.again};
      } else
        graph.blocks[id].successors = {next};
      next = id;
    }
    return next;
  }
  template <class Id>
  static std::set<Id> incoming(const std::set<Id> &uses, const std::set<Id> &defs,
                               const std::set<Id> &out) {
    auto in = uses;
    for (auto id : out)
      if (!defs.contains(id))
        in.insert(id);
    return in;
  }

public:
  Builder(core::Program &p, core::Function &f, const Contracts &contracts, RegionId &fresh)
      : program(p), signatures(contracts), nextRegion(fresh) {
    graph.function = f.id;
    auto exit = node();
    graph.blocks[exit].terminator = Terminator::Unreachable;
    graph.entry = sequence(f.body, exit);
  }
  FunctionGraph run() {

    bool originsChanged = true;
    while (originsChanged) {
      originsChanged = false;
      for (auto *i : instructions) {
        if (!i || (i->kind != I::Call && i->kind != I::ContainerCall) ||
            referenceArguments.contains(i->result))
          continue;
        const auto &target = program.semantics.functions[i->function];
        if (!target.returnsReference())
          continue;
        auto index = std::find(target.params.begin(), target.params.end(), target.resultOrigin) -
                     target.params.begin();
        if (i->slot != None && index == 0) {
          referenceArguments[i->result] = i->slot;
          originsChanged = true;
          continue;
        }
        auto argument = i->args.at(index - (i->slot != None));
        if (referenceArguments.contains(argument)) {
          referenceArguments[i->result] = referenceArguments.at(argument);
          originsChanged = true;
        }
      }
    }

    for (auto &b : graph.blocks)
      for (auto value : b.valueUses)
        if (referenceArguments.contains(value))
          b.uses.insert(referenceArguments.at(value));
    std::vector<BasicBlockId> work{graph.entry};
    while (!work.empty()) {
      auto id = work.back();
      work.pop_back();
      if (graph.blocks[id].reachable)
        continue;
      graph.blocks[id].reachable = true;
      for (auto next : graph.blocks[id].successors)
        work.push_back(next);
    }
    detail::arguments(program, graph, instructions, parents, signatures, nextRegion);
    detail::cleanupUses(program, graph, instructions, parents);

    std::map<ValueId, std::set<SymbolId>> resultParents;
    std::map<SymbolId, std::set<SymbolId>> carrierParents;
    std::set<SymbolId> storedCarriers;
    bool carriersChanged = true;
    while (carriersChanged) {
      carriersChanged = false;
      auto merge = [&](auto &target, const auto &source) {
        auto before = target.size();
        target.insert(source.begin(), source.end());
        carriersChanged |= before != target.size();
      };
      for (auto *i : instructions) {
        if (!i)
          continue;
        if (i->kind == I::ContainerCall || i->kind == I::Call) {
          const auto &target = program.semantics.functions[i->function];
          if (target.resultBorrowedContainer && target.resultAccess == Access::Unique) {
            auto index =
                std::find(target.params.begin(), target.params.end(), target.resultOrigin) -
                target.params.begin();
            SymbolId input = None;
            if (i->slot != None && index == 0)
              input = i->slot;
            else {
              auto argument = i->args.at(index - (i->slot != None));
              if (referenceArguments.contains(argument))
                input = referenceArguments.at(argument);
            }
            if (input != None && program.semantics.symbols[input].reference)
              merge(resultParents[i->result], std::set<SymbolId>{input});
          }
        }
        if (i->kind == I::Move && carrierParents.contains(i->slot))
          merge(resultParents[i->result], carrierParents.at(i->slot));
        if (i->kind == I::Bind && resultParents.contains(i->args.at(0))) {
          merge(carrierParents[i->slot], resultParents.at(i->args[0]));
          storedCarriers.insert(i->slot);
          if (carrierParents[i->slot].size() == 1)
            parents[i->slot] = *carrierParents[i->slot].begin();
        }
        if ((i->kind == I::BorrowShared || i->kind == I::BorrowUnique) &&
            carrierParents.contains(i->slot))
          merge(carrierParents[i->offset], carrierParents.at(i->slot));
      }
    }
    if (!storedCarriers.empty()) {
      for (auto carrier : storedCarriers)
        graph.storedResultParents[carrier] = carrierParents.at(carrier);

      std::vector<std::set<SymbolId>> activeIn(graph.blocks.size()), activeOut(graph.blocks.size());
      std::map<uint32_t, std::set<SymbolId>> loopCarriers;
      std::function<void(const std::vector<I> &, std::set<SymbolId> &)> localCarriers;
      localCarriers = [&](const std::vector<I> &ops, std::set<SymbolId> &locals) {
        for (const auto &op : ops) {
          if (op.kind == I::Bind && storedCarriers.contains(op.slot))
            locals.insert(op.slot);
          localCarriers(op.body, locals);
          localCarriers(op.otherwise, locals);
          localCarriers(op.latch, locals);
          localCarriers(op.condition, locals);
        }
      };
      for (auto *i : instructions)
        if (i && i->kind == I::Loop) {
          localCarriers(i->body, loopCarriers[i->loop]);
          localCarriers(i->latch, loopCarriers[i->loop]);
          localCarriers(i->condition, loopCarriers[i->loop]);
        }
      std::vector<std::vector<BasicBlockId>> predecessors(graph.blocks.size());
      for (const auto &b : graph.blocks)
        for (auto successor : b.successors)
          predecessors[successor].push_back(b.id);
      bool activeChanged = true;
      while (activeChanged) {
        activeChanged = false;
        for (const auto &b : graph.blocks) {
          if (!b.reachable)
            continue;
          std::set<SymbolId> in;
          for (auto predecessor : predecessors[b.id])
            if (graph.blocks[predecessor].reachable)
              in.insert(activeOut[predecessor].begin(), activeOut[predecessor].end());
          auto out = in;
          if (auto *i = instructions[b.id]) {
            if (i->kind == I::Bind && storedCarriers.contains(i->slot))
              out.insert(i->slot);
            if (i->kind == I::Move || i->kind == I::Drop)
              out.erase(i->slot);
            if (i->kind == I::ScopeEnd)
              for (auto local : i->locals)
                out.erase(local);
            if (i->kind == I::Break || i->kind == I::Continue)
              for (auto local : loopCarriers[i->loop])
                out.erase(local);
            if (i->kind == I::Return)
              out.clear();
          }
          activeChanged |= in != activeIn[b.id] || out != activeOut[b.id];
          activeIn[b.id] = std::move(in);
          activeOut[b.id] = std::move(out);
        }
      }
      for (auto &b : graph.blocks) {
        auto *i = instructions[b.id];
        if (i && i->kind == I::ScopeEnd)
          for (auto local : i->locals)
            if (activeIn[b.id].contains(local))
              b.uses.insert(carrierParents.at(local).begin(), carrierParents.at(local).end());
        if (i && (i->kind == I::Return || i->kind == I::Break || i->kind == I::Continue))
          for (auto carrier : activeIn[b.id])
            b.uses.insert(carrierParents.at(carrier).begin(), carrierParents.at(carrier).end());
        auto uses = b.uses;
        for (auto symbol : uses)
          if (carrierParents.contains(symbol))
            b.uses.insert(carrierParents.at(symbol).begin(), carrierParents.at(symbol).end());
      }
    }

    for (auto &b : graph.blocks)
      for (auto value : b.valueUses)
        if (resultParents.contains(value))
          b.uses.insert(resultParents.at(value).begin(), resultParents.at(value).end());
    ValueId first = None, end = 0;
    for (const auto &b : graph.blocks) {
      for (auto value : b.valueUses) {
        first = std::min(first, value);
        end = std::max(end, value + 1);
      }
      for (auto value : b.valueDefinitions) {
        first = std::min(first, value);
        end = std::max(end, value + 1);
      }
    }
    if (first == None)
      first = end = 0;
    for (auto &b : graph.blocks) {
      b.valuesIn = ValueLivenessSet(first, end);
      b.valuesOut = ValueLivenessSet(first, end);
    }
    bool changed = true;
    while (changed) {
      changed = false;
      for (auto &b : graph.blocks) {
        if (!b.reachable)
          continue;
        std::set<SymbolId> out;
        ValueLivenessSet values(first, end);
        for (auto next : b.successors) {
          const auto &s = graph.blocks[next];
          out.insert(s.liveIn.begin(), s.liveIn.end());
          values.merge(s.valuesIn);
        }
        auto in = incoming(b.uses, b.definitions, out);
        auto vin = values;
        for (auto value : b.valueDefinitions)
          vin.erase(value);
        for (auto value : b.valueUses)
          vin.insert(value);
        changed |= in != b.liveIn || out != b.liveOut || vin != b.valuesIn || values != b.valuesOut;
        b.liveIn = std::move(in);
        b.liveOut = std::move(out);
        b.valuesIn = std::move(vin);
        b.valuesOut = std::move(values);
      }
    }
    for (const auto &parameter : program.semantics.functions[graph.function].parameterRegions)
      graph.regions.emplace(parameter.region, Region{parameter.region, RegionKind::Parameter, {}});
    for (const auto &parameter :
         program.semantics.functions[graph.function].payloadParameterRegions)
      graph.regions.emplace(parameter.region, Region{parameter.region, RegionKind::Parameter, {}});
    for (const auto &[parameter, arguments] : graph.contract.inputs)
      for (const auto &[path, regions] : arguments)
        for (auto region : regions)
          graph.regions.emplace(region, Region{region, RegionKind::Parameter, {}});
    for (const auto &[region, sources] : graph.contract.resultSources) {
      graph.regions.emplace(region, Region{region, RegionKind::Return, {}});
      for (auto source : sources)
        graph.constraints.push_back({ConstraintKind::Subregion,
                                     region,
                                     source,
                                     {},
                                     ConstraintReason::FunctionResult,
                                     program.semantics.functions[graph.function].loc});
      for (const auto &b : graph.blocks)
        if (b.reachable && b.terminator == Terminator::Return)
          graph.constraints.push_back({ConstraintKind::ContainsPoint,
                                       region,
                                       None,
                                       {b.id, 0},
                                       ConstraintReason::FunctionResult,
                                       b.origin});
    }
    for (const auto &loan : graph.loans) {
      graph.regions.emplace(loan.region, Region{loan.region, RegionKind::Local, {}});
      graph.constraints.push_back({ConstraintKind::ContainsPoint, loan.region, None, loan.created,
                                   ConstraintReason::BorrowCreation, loan.origin});
      for (const auto &b : graph.blocks)
        if (b.reachable && b.liveIn.contains(loan.carrier))
          graph.constraints.push_back({ConstraintKind::ContainsPoint,
                                       loan.region,
                                       None,
                                       {b.id, 0},
                                       ConstraintReason::LiveCarrier,
                                       b.origin});
      auto parent = parents.at(loan.carrier);
      if (carrierParents.contains(loan.carrier) && carrierParents.at(loan.carrier).size() == 1)
        parent = *carrierParents.at(loan.carrier).begin();
      const auto &source = program.semantics.symbols.at(parent);
      if (auto reference = program.semantics.symbolReference(parent);
          source.reference && reference && reference->region != None) {
        graph.regions.emplace(reference->region,
                              Region{reference->region,
                                     source.parameter ? RegionKind::Parameter : RegionKind::Local,
                                     {}});
        graph.constraints.push_back({ConstraintKind::Subregion,
                                     loan.region,
                                     reference->region,
                                     {},
                                     ConstraintReason::Reborrow,
                                     loan.origin});
      }
    }
    const auto &signature = program.semantics.functions[graph.function];
    if (signature.resultOrigin != None) {
      graph.regions.emplace(signature.resultRegion,
                            Region{signature.resultRegion, RegionKind::Return, {}});
      graph.constraints.push_back({ConstraintKind::Subregion,
                                   signature.resultRegion,
                                   program.semantics.symbols[signature.resultOrigin].region,
                                   {},
                                   ConstraintReason::FunctionResult,
                                   signature.loc});
      for (auto origin : signature.additionalResultOrigins)
        graph.constraints.push_back({ConstraintKind::Subregion,
                                     signature.resultRegion,
                                     program.semantics.symbols[origin].region,
                                     {},
                                     ConstraintReason::FunctionResult,
                                     signature.loc});
      for (const auto &b : graph.blocks)
        if (b.reachable && b.terminator == Terminator::Return)
          graph.constraints.push_back({ConstraintKind::ContainsPoint,
                                       signature.resultRegion,
                                       None,
                                       {b.id, 0},
                                       ConstraintReason::FunctionResult,
                                       b.origin});
    }
    std::map<SymbolId, std::vector<Projection>> paths;
    for (const auto &loan : graph.loans)
      paths[loan.carrier] = loan.place.projections;
    for (auto &loan : graph.loans) {
      std::set<SymbolId> seen;
      while (parents.contains(loan.place.root) && seen.insert(loan.place.root).second) {
        const auto &prefix = paths[loan.place.root];
        loan.place.projections.insert(loan.place.projections.begin(), prefix.begin(), prefix.end());
        loan.place.root = parents.at(loan.place.root);
      }
    }
    detail::storageLimits(program, graph, instructions, nextRegion);

    for (const auto &constraint : graph.constraints)
      if (constraint.kind == ConstraintKind::ContainsPoint)
        graph.regions.at(constraint.region).points.insert(constraint.point.block);
    changed = true;
    while (changed) {
      changed = false;
      for (const auto &constraint : graph.constraints) {
        if (constraint.kind != ConstraintKind::Subregion)
          continue;
        const auto &lower = graph.regions.at(constraint.region).points;
        auto &upper = graph.regions.at(constraint.upper).points;
        auto size = upper.size();
        upper.insert(lower.begin(), lower.end());
        changed |= size != upper.size();
      }
    }

    std::map<SymbolId, Loc> lastUses;
    for (const auto &b : graph.blocks) {
      if (!b.reachable)
        continue;
      std::vector<SymbolId> pending(b.uses.begin(), b.uses.end());
      std::set<SymbolId> seen;
      while (!pending.empty()) {
        auto carrier = pending.back();
        pending.pop_back();
        if (!seen.insert(carrier).second)
          continue;
        auto &last = lastUses[carrier];
        if (b.origin.span.file != NoFile &&
            (last.span.file == NoFile ||
             (b.origin.span.file == last.span.file && b.origin.span.start >= last.span.start)))
          last = b.origin;
        if (parents.contains(carrier))
          pending.push_back(parents.at(carrier));
        if (carrierParents.contains(carrier))
          pending.insert(pending.end(), carrierParents.at(carrier).begin(),
                         carrierParents.at(carrier).end());
      }
    }
    for (auto &loan : graph.loans) {
      loan.points = graph.regions.at(loan.region).points;
      Loc lastUse = lastUses.contains(loan.carrier) ? lastUses.at(loan.carrier) : loan.origin;
      if (loan.inferred)
        program.semantics.symbols[loan.carrier].lastUse = lastUse;
      for (const auto &b : graph.blocks) {
        if (!b.reachable)
          continue;
        auto *instruction = instructions[b.id];
        if (!loan.inferred || !instruction || loan.points.contains(b.id))
          continue;
        instruction->inferredEnds.push_back(loan.carrier);
      }
      if (loan.inferred)
        for (auto [loop, exit] : loopExits)
          if (!loan.points.contains(exit))
            instructions[loop]->exitEnds.push_back(loan.carrier);
    }
    return std::move(graph);
  }
};
}
void infer(core::Program &program) {
  program.regionGraphs.clear();
  RegionId nextRegion = 0;
  auto reserve = [&](RegionId region) {
    if (region != None)
      nextRegion = std::max(nextRegion, region + 1);
  };
  for (const auto &symbol : program.semantics.symbols)
    reserve(symbol.region);
  for (const auto &function : program.semantics.functions)
    reserve(function.resultRegion);
  const auto signatures = detail::contracts(program, nextRegion);
  for (auto &function : program.functions)
    program.regionGraphs.push_back(Builder(program, function, signatures, nextRegion).run());
}
void validateBounds(const core::Program &program) {
  detail::validateArguments(program);
  for (const auto &graph : program.regionGraphs)
    for (const auto &[region, owners] : graph.ownerBounds)
      for (auto owner : owners) {
        const auto &bound = graph.storageBounds.at(owner);
        for (auto point : graph.regions.at(region).points)
          if (!bound.points.contains(point)) {
            const auto &block = graph.blocks.at(point);
            DiagnosticEngine()
                .error(DiagnosticCode::BorrowConflict, block.origin,
                       "reference requires storage outside the lifetime of `" +
                           program.semantics.symbols[owner].name + "`")
                .label(block.origin, "reference must remain valid here")
                .secondary(bound.origin, "owner storage is initialized here")
                .note(
                    "owner/storage bounds are computed from initialization and cleanup on the CFG")
                .help("keep the owner alive until all references derived from it have ended")
                .emit();
          }
      }
}
namespace {
void printPath(const hir::ReferencePath &path, std::ostream &out) {
  if (path.empty())
    out << "self";
  for (const auto &projection : path) {
    if (projection.kind == hir::ReferenceProjectionKind::Field)
      out << ".field[" << projection.index << "]";
    else if (projection.kind == hir::ReferenceProjectionKind::Element)
      out << ".element";
    else
      out << "#V" << projection.variant << ".payload[" << projection.index << "]";
  }
}
void printArgs(const hir::RegionArgs &arguments, std::ostream &out) {
  for (const auto &[path, regions] : arguments) {
    out << " path=";
    printPath(path, out);
    out << " regions={";
    for (auto region : regions)
      out << " #R" << region;
    out << " }";
  }
}
const char *reasonName(ConstraintReason reason) {
  switch (reason) {
  case ConstraintReason::BorrowCreation:
    return "BorrowCreation";
  case ConstraintReason::LiveCarrier:
    return "LiveCarrier";
  case ConstraintReason::Reborrow:
    return "Reborrow";
  case ConstraintReason::FunctionResult:
    return "FunctionResult";
  case ConstraintReason::StoredReference:
    return "StoredReference";
  }
  return "Invalid";
}
}
void print(const core::Program &p, std::ostream &out) {
  out << "Semantic CFG regions (before erasure)\n";
  for (const auto &graph : p.regionGraphs) {
    out << "fn #F" << graph.function << " " << p.semantics.functions[graph.function].name << "\n";
    for (const auto &[parameter, arguments] : graph.contract.inputs) {
      out << "  RegionArgs input #v" << parameter;
      printArgs(arguments, out);
      out << "\n";
    }
    if (!graph.contract.result.empty()) {
      out << "  RegionArgs return";
      printArgs(graph.contract.result, out);
      out << "\n";
    }
    for (const auto &[symbol, arguments] : graph.symbolRegions)
      if (!arguments.empty()) {
        out << "  RegionArgs symbol #v" << symbol;
        printArgs(arguments, out);
        out << "\n";
      }
    for (const auto &[value, arguments] : graph.valueRegions)
      if (!arguments.empty()) {
        out << "  RegionArgs value %" << value;
        printArgs(arguments, out);
        out << "\n";
      }
    for (const auto &call : graph.calls) {
      out << "  region substitution bb" << call.point.block << ":0 call #F" << call.target;
      for (const auto &[formal, actual] : call.substitution) {
        out << " #R" << formal << "={";
        for (auto region : actual)
          out << " #R" << region;
        out << " }";
      }
      printArgs(call.result, out);
      out << "\n";
    }
    for (const auto &[owner, bound] : graph.storageBounds) {
      out << "  storage bound owner=" << p.semantics.symbols.at(owner).name << "#v" << owner
          << " must-initialized points:";
      for (auto point : bound.points)
        out << " bb" << point << ":0";
      out << "\n";
    }
    for (const auto &b : graph.blocks) {
      if (!b.reachable)
        continue;
      out << "  bb" << b.id << ":0 liveIn={";
      for (auto symbol : b.liveIn)
        out << " #v" << symbol;
      out << " } liveOut={";
      for (auto symbol : b.liveOut)
        out << " #v" << symbol;
      out << " } successors={";
      for (auto next : b.successors)
        out << " bb" << next;
      out << " }\n";
    }
    for (const auto &loan : graph.loans) {
      out << "  loan #L" << loan.id << " region #R" << loan.region << " kind=Local "
          << (loan.capability == BorrowKind::Shared ? "Shared" : "Unique")
          << " owner=" << p.semantics.symbols.at(loan.place.root).name << "#v" << loan.place.root
          << " carrier=#v" << loan.carrier << (loan.inferred ? " inferred" : " lexical") << "\n";
      for (const auto &projection : loan.place.projections)
        out << "    place projection="
            << (projection.kind == ProjectionKind::Field   ? "Field"
                : projection.kind == ProjectionKind::Index ? "Index"
                                                           : "VariantPayload")
            << " #" << projection.index
            << (projection.variant == None ? ""
                                           : " variant=#V" + std::to_string(projection.variant))
            << "\n";
      out << "    live points:";
      for (auto point : loan.points)
        out << " bb" << point << ":0";
      out << "\n";
    }
    for (const auto &[carrier, parents] : graph.storedResultParents) {
      out << "  stored result " << p.semantics.symbols[carrier].name << "#v" << carrier
          << " keeps input alive:";
      for (auto parent : parents)
        out << " " << p.semantics.symbols[parent].name << "#v" << parent;
      out << "\n";
    }
  }
}
void printConstraints(const core::Program &p, std::ostream &out) {
  out << "Region constraints (semantic Core, least fixed point)\n";
  for (const auto &graph : p.regionGraphs) {
    if (graph.constraints.empty())
      continue;
    out << "fn #F" << graph.function << " " << p.semantics.functions[graph.function].name << "\n";
    for (const auto &constraint : graph.constraints) {
      out << "  #R" << constraint.region;
      if (constraint.kind == ConstraintKind::Subregion)
        out << " <= #R" << constraint.upper << " reason=" << reasonName(constraint.reason);
      else
        out << " contains bb" << constraint.point.block
            << ":0 reason=" << reasonName(constraint.reason);
      out << " at " << constraint.origin.line << ":" << constraint.origin.column << "\n";
    }
    for (const auto &[region, owners] : graph.ownerBounds)
      for (auto owner : owners)
        out << "  #R" << region << " <= storage(#v" << owner << ") reason=OwnerStorage\n";
  }
}
}
