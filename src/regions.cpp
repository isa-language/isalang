#include "isac/regions.hpp"
#include "isac/core.hpp"
#include <algorithm>
#include <functional>
#include <map>

namespace isac::regions {
namespace {
using I = core::Inst;
class Builder {
  core::Program &program;
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
        loan.place.projections.push_back({ProjectionKind::Field, i.fieldIndex});
      else if (i.placeProjection == I::PlaceProjection::Index)
        loan.place.projections.push_back({ProjectionKind::Index, None});
      if (i.projection)
        loan.place.projections.push_back({ProjectionKind::VariantPayload, i.projection});
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
      if (i.kind == I::If) {
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
  Builder(core::Program &p, core::Function &f) : program(p) {
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
        if (!i || i->kind != I::Call || referenceArguments.contains(i->result))
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
    bool changed = true;
    while (changed) {
      changed = false;
      for (auto &b : graph.blocks) {
        if (!b.reachable)
          continue;
        std::set<SymbolId> out;
        std::set<ValueId> values;
        for (auto next : b.successors) {
          const auto &s = graph.blocks[next];
          out.insert(s.liveIn.begin(), s.liveIn.end());
          values.insert(s.valuesIn.begin(), s.valuesIn.end());
        }
        auto in = incoming(b.uses, b.definitions, out);
        auto vin = incoming(b.valueUses, b.valueDefinitions, values);
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
      const auto &source = program.semantics.symbols.at(parent);
      if (source.reference && source.region != None) {
        graph.regions.emplace(source.region,
                              Region{source.region,
                                     source.parameter ? RegionKind::Parameter : RegionKind::Local,
                                     {}});
        graph.constraints.push_back({ConstraintKind::Subregion,
                                     loan.region,
                                     source.region,
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
    for (auto &loan : graph.loans) {
      loan.points = graph.regions.at(loan.region).points;

      std::set<SymbolId> seen;
      while (parents.contains(loan.place.root) && seen.insert(loan.place.root).second)
        loan.place.root = parents.at(loan.place.root);
      Loc lastUse = loan.origin;
      for (const auto &b : graph.blocks)
        if (b.reachable && b.uses.contains(loan.carrier) &&
            b.origin.span.file == lastUse.span.file && b.origin.span.start >= lastUse.span.start)
          lastUse = b.origin;
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
  for (auto &function : program.functions)
    program.regionGraphs.push_back(Builder(program, function).run());
}
void print(const core::Program &p, std::ostream &out) {
  out << "Semantic CFG regions (before erasure)\n";
  for (const auto &graph : p.regionGraphs) {
    out << "fn #F" << graph.function << " " << p.semantics.functions[graph.function].name << "\n";
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
            << " #" << projection.index << "\n";
      out << "    live points:";
      for (auto point : loan.points)
        out << " bb" << point << ":0";
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
        out << " <= #R" << constraint.upper << " reason="
            << (constraint.reason == ConstraintReason::FunctionResult ? "FunctionResult"
                                                                      : "Reborrow");
      else
        out << " contains bb" << constraint.point.block << ":0 reason="
            << (constraint.reason == ConstraintReason::BorrowCreation   ? "BorrowCreation"
                : constraint.reason == ConstraintReason::FunctionResult ? "FunctionResult"
                                                                        : "LiveCarrier");
      out << " at " << constraint.origin.line << ":" << constraint.origin.column << "\n";
    }
  }
}
}
