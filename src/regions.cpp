#include "isac/regions.hpp"
#include "isac/core.hpp"
#include "region_inference.hpp"
#include "storage_paths.hpp"
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
    if (i.result != None) {
      b.valueDefinitions.insert(i.result);
      bool reference = i.kind == I::ReferenceArgument ||
                       ((core::functionCall(i) || i.kind == I::ContainerCall) &&
                        program.semantics.functions.at(i.function).returnsReference());
      graph.valueDefinitions.emplace(i.result, SemanticValueDefinition{i.type, reference, {id, 0}});
    }
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
        loan.place.projections.push_back({position.kind == hir::ReferenceProjectionKind::Field
                                              ? ProjectionKind::Field
                                              : ProjectionKind::VariantPayload,
                                          position.index, position.variant});
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
        if (!i || (!core::functionCall(*i) && i->kind != I::ContainerCall) ||
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

    graph.parentOperations = detail::parentOperations(program, graph);
    auto parentFlow = detail::parentObligations(graph);
    graph.parentStates = std::move(parentFlow.before);
    graph.parentUses = std::move(parentFlow.uses);
    graph.loanParents = std::move(parentFlow.aliases);
    graph.storedResultParents = std::move(parentFlow.summaries);
    for (const auto &[point, required] : graph.parentUses)
      graph.blocks[point].uses.insert(required.begin(), required.end());
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
      auto sources = graph.loanParents.contains(loan.carrier) ? graph.loanParents.at(loan.carrier)
                                                              : ParentSet{};
      if (sources.empty())
        sources.insert(parents.at(loan.carrier));
      for (auto parent : sources) {
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
    }

    std::map<SymbolId, std::vector<Projection>> paths;
    for (const auto &loan : graph.loans)
      paths[loan.carrier] = loan.place.projections;
    for (auto &loan : graph.loans) {
      if (auto source = graph.loanParents.find(loan.carrier);
          source != graph.loanParents.end() && source->second.size() == 1) {
        loan.place.root = *source->second.begin();
        loan.place.projections.clear();
      }
      std::set<SymbolId> seen;
      while (parents.contains(loan.place.root) && seen.insert(loan.place.root).second) {
        const auto &prefix = paths[loan.place.root];
        loan.place.projections.insert(loan.place.projections.begin(), prefix.begin(), prefix.end());
        loan.place.root = parents.at(loan.place.root);
      }
    }
    detail::storageLimits(program, graph, instructions, nextRegion);
    detail::projectionLimits(program, graph);

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
        if (graph.loanParents.contains(carrier))
          pending.insert(pending.end(), graph.loanParents.at(carrier).begin(),
                         graph.loanParents.at(carrier).end());
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
  RegionId nextRegion = detail::freshRegionStart(program);
  const auto signatures = detail::contracts(program, nextRegion);
  for (auto &function : program.functions)
    program.regionGraphs.push_back(Builder(program, function, signatures, nextRegion).run());
}
void validateBounds(const core::Program &program) {
  detail::validateArguments(program);
  detail::validateFieldArguments(program);
  detail::validateParentObligations(program);
  detail::validateProjections(program);
  storage::validate(program);
  carriers::validate(program);
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
    storage::print(graph.storage, out);
    carriers::print(graph.carriers, out);
    for (const auto &[point, symbols] : graph.fieldRegions)
      for (const auto &[symbol, arguments] : symbols) {
        out << "  field RegionArgs bb" << point << ":0 #v" << symbol;
        printArgs(arguments, out);
        out << "\n";
      }
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
    for (const auto &[id, bound] : graph.projectionBounds) {
      out << "  projection storage bound #P" << id << " loans={";
      for (auto loan : bound.loans)
        out << " #L" << loan;
      out << " } owner=" << p.semantics.symbols.at(bound.owner).name << "#v" << bound.owner;
      for (auto field : bound.fields)
        out << ".field[" << field << "]";
      out << " initialized resource points:";
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
      out << "  parent summary " << p.semantics.symbols[carrier].name << "#v" << carrier
          << " (debug only):";
      for (auto parent : parents)
        out << " " << p.semantics.symbols[parent].name << "#v" << parent;
      out << "\n";
    }
    for (const auto &[point, op] : graph.parentOperations)
      if (op.kind == ParentOperation::Forward)
        for (const auto &[path, source] : op.transfers) {
          out << "  parent.forward bb" << point << ":0 %" << source.id;
          storage::detail::printPath(source.path, out);
          out << " -> %" << op.destination.id;
          storage::detail::printPath(path, out);
          out << " [transfer existing obligation]\n";
        }
    for (const auto &[point, state] : graph.parentStates)
      for (const auto &[position, parents] : state) {
        out << "  parent obligation bb" << point << ":0 "
            << (position.kind == storage::Location::Symbol ? "#v" : "%") << position.id;
        storage::detail::printPath(position.path, out);
        out << " keeps input alive={";
        for (auto parent : parents)
          out << " #v" << parent;
        out << " }\n";
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
    for (const auto &[id, bound] : graph.projectionBounds)
      for (auto loan : bound.loans) {
        out << "  #R" << graph.loans.at(loan).region << " <= initialized(#v" << bound.owner;
        for (auto field : bound.fields)
          out << ".field[" << field << "]";
        out << ") reason=ProjectionStorage\n";
      }
  }
}
}
