#include "isac/concurrency.hpp"
#include <functional>
#include <ostream>

namespace isac::concurrency {
void Assessment::merge(const Assessment &other) {
  structural &= other.structural;
  needsRegion |= other.needsRegion;
  contracts.insert(other.contracts.begin(), other.contracts.end());
  if (witness.empty() && !other.structural)
    witness = other.witness;
  obligations.insert(obligations.end(), other.obligations.begin(), other.obligations.end());
}
namespace {
Assessment missing(Requirement contract, TypeId type, Capability capability) {
  return {false, false, {contract}, {}, {{contract, type, capability, {}}}};
}
const char *name(Requirement r) {
  switch (r) {
  case Requirement::AllocatorContract:
    return "allocator thread contract";
  case Requirement::RuntimeTypeContract:
    return "runtime type thread contract";
  case Requirement::ProtocolContract:
    return "protocol/drop thread contract";
  case Requirement::ForeignContract:
    return "foreign/raw thread contract";
  case Requirement::RecursiveContract:
    return "recursive type contract";
  case Requirement::UnsupportedType:
    return "unsupported type contract";
  }
  return "unknown contract";
}
void path(const std::vector<regions::Projection> &projections, std::ostream &out) {
  for (const auto &projection : projections) {
    if (projection.kind == regions::ProjectionKind::Field)
      out << ".field[" << projection.index << "]";
    else if (projection.kind == regions::ProjectionKind::VariantPayload)
      out << "#V" << projection.variant << ".payload[" << projection.index << "]";
    else if (projection.kind == regions::ProjectionKind::Deref)
      out << ".deref";
    else
      out << ".element";
  }
}
void show(const Assessment &a, std::ostream &out) {
  out << (a.structural ? "structural" : "requires contract");
  if (a.needsRegion)
    out << "; region bound required";
  for (auto contract : a.contracts)
    out << "; " << name(contract);
  path(a.witness, out);
  for (const auto &obligation : a.obligations) {
    out << "\n    require " << name(obligation.requirement) << " for "
        << (obligation.capability == Capability::Send ? "Send" : "Sync") << "<#T" << obligation.type
        << "> at self";
    path(obligation.path, out);
  }
}
}
Assessment Analysis::assess(TypeId id, Capability capability) {
  const auto &p = verified.get().semantics;
  if (id >= p.types.size())
    fail({}, DiagnosticCode::InvalidCore, "invalid concurrency type query");
  const auto key = std::pair{id, capability};
  if (auto cached = cache.find(key); cached != cache.end())
    return cached->second;
  if (!visiting.insert(key).second)
    return missing(Requirement::RecursiveContract, id, capability);
  const auto &shape = p.types.at(id);
  Assessment result;
  auto child = [&](TypeId type, Capability wanted, regions::Projection projection) {
    auto requirement = assess(type, wanted);
    if (!requirement.structural)
      requirement.witness.insert(requirement.witness.begin(), projection);
    for (auto &obligation : requirement.obligations)
      obligation.path.insert(obligation.path.begin(), projection);
    result.merge(requirement);
  };
  auto require = [&](Requirement contract) { result.merge(missing(contract, id, capability)); };
  if (shape.protocol != None)
    require(Requirement::ProtocolContract);
  if (p.heap(id)) {

    require(Requirement::AllocatorContract);
    require(Requirement::RuntimeTypeContract);
    if (p.dynamicContainer(id))
      child(shape.element, capability, {regions::ProjectionKind::Index, None});
  } else if (id == Str) {

    result.needsRegion = true;
  } else
    switch (shape.kind) {
    case hir::Type::UnitType:
    case hir::Type::Integer:
    case hir::Type::Boolean:
    case hir::Type::Size:
    case hir::Type::Float:
    case hir::Type::Character:
      break;
    case hir::Type::Reference:
      result.needsRegion = true;
      child(shape.element, referentCapability(shape.referenceAccess, capability),
            {regions::ProjectionKind::Deref, None});
      break;
    case hir::Type::Slice:
      result.needsRegion = true;
      child(shape.element, capability, {regions::ProjectionKind::Index, None});
      break;
    case hir::Type::Array:
      child(shape.element, capability, {regions::ProjectionKind::Index, None});
      break;
    case hir::Type::Structure:
      for (uint32_t n = 0; n < shape.fields.size(); ++n)
        child(shape.fields[n].type, capability, {regions::ProjectionKind::Field, n});
      break;
    case hir::Type::Enum:
      for (const auto &variant : shape.variants)
        for (uint32_t n = 0; n < variant.payload.size(); ++n)
          child(variant.payload[n], capability,
                {regions::ProjectionKind::VariantPayload, n, variant.id});
      break;
    case hir::Type::Pointer:
    case hir::Type::RawPointer:
    case hir::Type::FunctionPointer:
    case hir::Type::ForeignRecord:
    case hir::Type::ForeignOpaque:
      require(Requirement::ForeignContract);
      break;
    default:
      require(Requirement::UnsupportedType);
      break;
    }

  if (shape.kind == hir::Type::Structure || shape.kind == hir::Type::Enum)
    for (const auto &function : p.functions)
      if (function.owner == id &&
          (function.foreignC || function.unsafeFunction || function.effects.unsafeEffect))
        require(Requirement::ForeignContract);
  visiting.erase(key);
  cache.emplace(key, result);
  return result;
}
Assessment Analysis::symbol(SymbolId id, Capability capability) {
  const auto &p = verified.get().semantics;
  if (id >= p.symbols.size())
    fail({}, DiagnosticCode::InvalidCore, "invalid concurrency symbol query");
  const auto &symbol = p.symbols[id];
  if (!symbol.reference)
    return assess(symbol.type, capability);
  auto result = assess(symbol.type, referentCapability(symbol.access, capability));
  result.needsRegion = true;
  if (!result.structural)
    result.witness.insert(result.witness.begin(), {regions::ProjectionKind::Deref, None});
  for (auto &obligation : result.obligations)
    obligation.path.insert(obligation.path.begin(), {regions::ProjectionKind::Deref, None});
  return result;
}
PointFacts Analysis::point(FunctionId function, regions::ProgramPoint point, Side side) const {
  const auto &p = verified.get();
  auto graph = std::find_if(p.regionGraphs.begin(), p.regionGraphs.end(),
                            [&](const auto &g) { return g.function == function; });
  if (graph == p.regionGraphs.end() || point.statementIndex != 0 ||
      point.block >= graph->blocks.size() || !graph->blocks[point.block].reachable ||
      !graph->resourcePoints.contains(point.block))
    fail({}, DiagnosticCode::InvalidCore,
         "concurrency query requires a verified semantic instruction point");
  const auto &block = graph->blocks[point.block];
  PointFacts result{
      function, point, side, block.origin, side == Side::Before ? block.liveIn : block.liveOut, {},
      {},       false, {}};
  const auto &live = side == Side::Before ? block.valuesIn : block.valuesOut;
  for (const auto &[value, definition] : graph->valueDefinitions)
    if (live.contains(value)) {
      auto arguments = graph->valueRegions.find(value);
      result.liveValues.push_back(
          {value, definition.type, definition.reference,
           arguments == graph->valueRegions.end() ? hir::RegionArgs{} : arguments->second});
    }
  for (const auto &loan : graph->loans)
    if (loan.points.contains(point.block))
      result.loanDemands.push_back(loan.id);
  if (auto observation = graph->carriers.transitions.find(point.block);
      observation != graph->carriers.transitions.end()) {
    result.carriersObserved = true;
    result.carriers = side == Side::Before ? observation->second.before : observation->second.after;
  } else if (graph->carriers.identities.empty())
    result.carriersObserved = true;
  return result;
}
std::vector<const core::CaptureState *>
Analysis::states(FunctionId function, regions::ProgramPoint point, Side side) const {
  (void)this->point(function, point, side);
  std::vector<const core::CaptureState *> result;
  const auto selected = side == Side::Before ? core::StateSide::Before : core::StateSide::After;
  for (const auto &state : verified.get().captureStates)
    if (state.function == function && state.point.block == point.block && state.side == selected)
      result.push_back(&state);
  if (result.empty())
    fail({}, DiagnosticCode::InvalidCore,
         "capture query requires retained resource-state witnesses at this boundary");
  return result;
}
SpanFacts Analysis::span(const core::CaptureState &state, SymbolId id) const {
  const auto &p = verified.get();
  if (std::none_of(p.captureStates.begin(), p.captureStates.end(),
                   [&](const auto &s) { return &s == &state; }))
    fail({}, DiagnosticCode::InvalidCore,
         "Span query requires a witness belonging to this Verified Core");
  auto resource = std::find_if(state.resources.begin(), state.resources.end(), [&](const auto &r) {
    return r.location.kind == storage::Location::Symbol && r.location.id == id;
  });
  if (state.terminal || resource == state.resources.end() || id >= p.semantics.symbols.size() ||
      p.semantics.types.at(resource->type).kind != hir::Type::Slice ||
      (resource->memory != MemoryState::Shared && resource->memory != MemoryState::Unique))
    fail(state.origin, DiagnosticCode::InvalidCore,
         "Span query requires an initialized borrowed slice at this boundary");
  const auto &graph = *std::find_if(p.regionGraphs.begin(), p.regionGraphs.end(),
                                    [&](const auto &g) { return g.function == state.function; });
  const auto &symbol = p.semantics.symbols.at(id);
  SpanFacts result{id,
                   p.semantics.types[resource->type].element,
                   resource->memory == MemoryState::Unique ? Access::Unique : Access::Shared,
                   resource->additionalOrigins,
                   {},
                   symbol.parameter && symbol.reference,
                   {},
                   {}};
  if (resource->root != None)
    result.origins.insert(resource->root);
  if (auto arguments = graph.symbolRegions.find(id); arguments != graph.symbolRegions.end())
    result.regions = arguments->second;
  for (const auto &loan : graph.loans)
    if (loan.carrier == id) {
      result.loans.push_back(loan.id);
      result.regions[{}].insert(loan.region);
      if (auto storage = graph.storage.loans.find(loan.id); storage != graph.storage.loans.end())
        result.storage.emplace(loan.id, storage->second);
    }
  if (result.regions.empty())
    fail(state.origin, DiagnosticCode::InvalidCore,
         "borrowed slice has no retained region contract");
  return result;
}
CaptureRegionDependencies
Analysis::captureRegions(const core::CaptureState &state,
                         const std::set<storage::Location> &proposedTransfers) const {
  const auto &p = verified.get();
  if (std::none_of(p.captureStates.begin(), p.captureStates.end(),
                   [&](const auto &s) { return &s == &state; }))
    fail({}, DiagnosticCode::InvalidCore,
         "capture region query requires a witness belonging to this Verified Core");
  if (state.terminal)
    fail(state.origin, DiagnosticCode::InvalidCore,
         "terminal edge has no capture region continuation");
  const auto side = state.side == core::StateSide::Before ? Side::Before : Side::After;
  const auto pointFacts = point(state.function, state.point, side);
  const auto &graph = *std::find_if(p.regionGraphs.begin(), p.regionGraphs.end(),
                                    [&](const auto &g) { return g.function == state.function; });
  CaptureRegionDependencies result{
      state.function, state.point, state.side, state.entryState, proposedTransfers, {}, {}, {}, {}};
  if (pointFacts.carriersObserved)
    result.carriers = pointFacts.carriers;
  if (auto observation = graph.storage.transitions.find(state.point.block);
      observation != graph.storage.transitions.end())
    result.storage = side == Side::Before ? observation->second.before : observation->second.after;
  if (auto observation = graph.storage.slotTransitions.find(state.point.block);
      observation != graph.storage.slotTransitions.end())
    result.slots = side == Side::Before ? observation->second.before : observation->second.after;

  auto add = [&](const core::CaptureResource &resource, const hir::ReferencePath &path,
                 const core::BorrowProvenance *provenance) {
    CaptureReferenceDependency dependency;
    dependency.position = resource.location;
    dependency.position.path.insert(dependency.position.path.end(), path.begin(), path.end());
    dependency.capability = provenance                                   ? provenance->capability
                            : resource.mode == core::CaptureMode::Unique ? Access::Unique
                                                                         : Access::Shared;
    dependency.callerOwned = resource.callerOwned;
    if (path.empty() && resource.location.kind == storage::Location::Symbol)
      if (auto parents = graph.loanParents.find(resource.location.id);
          parents != graph.loanParents.end())
        dependency.parentDependencies = parents->second;
    if (provenance) {
      dependency.originsObserved = true;
      dependency.origins = provenance->origins;
      dependency.parent = provenance->parent;
    } else if (resource.location.kind == storage::Location::Symbol) {
      dependency.originsObserved = true;
      dependency.origins = resource.additionalOrigins;

      dependency.origins.insert(resource.root == None ? resource.location.id : resource.root);
    }
    const hir::RegionArgs *arguments = nullptr;
    if (resource.location.kind == storage::Location::Symbol) {
      if (auto found = graph.symbolRegions.find(resource.location.id);
          found != graph.symbolRegions.end())
        arguments = &found->second;
    } else if (auto found = graph.valueRegions.find(resource.location.id);
               found != graph.valueRegions.end())
      arguments = &found->second;
    if (arguments)
      if (auto argument = arguments->find(path); argument != arguments->end()) {
        dependency.regionsObserved = true;
        dependency.regions.emplace(*argument);
      }
    if (result.carriers)
      if (auto carrier = result.carriers->find(dependency.position);
          carrier != result.carriers->end()) {
        dependency.carrier = carrier->second;
        if (!dependency.originsObserved) {
          dependency.originsObserved = true;
          dependency.origins = carrier->second.origins;
        }
        if (!dependency.parent)
          dependency.parent = carrier->second.parent;
      }

    std::set<RegionId> sources;
    for (const auto &[position, regions] : dependency.regions) {
      (void)position;
      sources.insert(regions.begin(), regions.end());
    }
    if (dependency.carrier)
      sources.insert(dependency.carrier->regions.begin(), dependency.carrier->regions.end());
    if (path.empty() && resource.location.kind == storage::Location::Symbol)
      for (const auto &loan : graph.loans)
        if (loan.carrier == resource.location.id) {
          dependency.regionsObserved = true;
          dependency.regions[path].insert(loan.region);
          sources.insert(loan.region);
        }
    std::vector<RegionId> pending(sources.begin(), sources.end());
    while (!pending.empty()) {
      auto region = pending.back();
      pending.pop_back();
      for (const auto &constraint : graph.constraints)
        if (constraint.kind == regions::ConstraintKind::Subregion && constraint.region == region &&
            sources.insert(constraint.upper).second)
          pending.push_back(constraint.upper);
    }
    std::map<SymbolId, CaptureOwnerDependency> owners;
    auto owner = [&](SymbolId id) -> CaptureOwnerDependency & {
      auto [entry, added] = owners.try_emplace(id);
      if (added) {
        entry->second.owner = id;
        if (auto bound = graph.storageBounds.find(id); bound != graph.storageBounds.end())
          entry->second.bound = bound->second;
        for (const auto &transfer : proposedTransfers)
          if (transfer.kind == storage::Location::Symbol && transfer.id == id)
            entry->second.proposedTransfers.insert(transfer);
        if (!entry->second.proposedTransfers.empty())
          entry->second.relation = CaptureOwnerRelation::PossibleInternal;
      }
      return entry->second;
    };
    for (auto origin : dependency.origins)
      owner(origin).origin = true;
    for (auto region : sources)
      if (auto bounds = graph.ownerBounds.find(region); bounds != graph.ownerBounds.end())
        for (auto id : bounds->second)
          owner(id).upperRegions.insert(region);
    for (auto &[id, bound] : owners) {
      (void)id;
      dependency.owners.push_back(std::move(bound));
    }
    for (const auto &loan : graph.loans)
      if (sources.contains(loan.region)) {
        CaptureLoanDependency source{loan, loan.points.contains(state.point.block), {}, {}};
        if (auto observation = graph.storage.loans.find(loan.id);
            observation != graph.storage.loans.end())
          source.storage = observation->second;
        for (const auto &[id, bound] : graph.projectionBounds) {
          (void)id;
          if (bound.loans.contains(loan.id))
            source.projections.push_back(bound);
        }
        dependency.loans.push_back(std::move(source));
      }
    result.references.push_back(std::move(dependency));
  };
  for (const auto &resource : state.resources) {

    for (const auto &[path, provenance] : resource.carriers)
      add(resource, path, &provenance);
    if ((resource.mode == core::CaptureMode::Shared ||
         resource.mode == core::CaptureMode::Unique) &&
        !resource.carriers.contains(hir::ReferencePath{}))
      add(resource, {}, nullptr);
  }
  return result;
}
void printCaptures(const core::Verified &verified, std::ostream &out) {
  Analysis analysis(verified);
  const auto &p = verified.get();
  out << "Capture states: queries only; execution requires checked contracts\n"
         "Cleanup replay is not a transfer, Send/Sync or frame-storage permission\n";
  auto step = [&](const core::CaptureDrop &drop) {
    out << "    drop " << (drop.location.kind == storage::Location::Symbol ? "#S" : "%")
        << drop.location.id;
    for (const auto &part : drop.location.path)
      out << ".field[" << part.index << "]";
    out << " type=#T" << drop.type << " allocation=" << drop.allocation
        << " absent-fields=" << drop.absentFields.size()
        << " moved-payloads=" << drop.movedPayloads.size() << "\n";
  };
  for (const auto &state : p.captureStates) {
    out << "fn #F" << state.function << " " << p.semantics.functions.at(state.function).name
        << " bb" << state.point.block << ":0 "
        << (state.side == core::StateSide::Before ? "before" : "after")
        << " entry-state=" << state.entryState << (state.terminal ? " terminal" : "") << "\n";
    for (const auto &r : state.resources) {
      const bool variable = r.location.kind == storage::Location::Symbol;
      out << "  initialized " << (variable ? "#S" : "%") << r.location.id;
      if (variable)
        out << " " << p.semantics.symbols.at(r.location.id).name;
      out << " type=#T" << r.type << " memory="
          << (r.memory == MemoryState::Own      ? "Own"
              : r.memory == MemoryState::Shared ? "Shared"
                                                : "Unique")
          << " capture="
          << (r.mode == core::CaptureMode::Move     ? "Move"
              : r.mode == core::CaptureMode::Copy   ? "Copy"
              : r.mode == core::CaptureMode::Unique ? "Unique"
                                                    : "Shared")
          << " state=" << r.protocolState << " allocation=" << r.allocation
          << " execution-live=" << r.executionLive << " absent-fields=" << r.absentFields.size()
          << " moved-payloads=" << r.movedPayloads.size() << " carriers=" << r.carriers.size()
          << "\n";
      if (variable && !state.terminal && p.semantics.types[r.type].kind == hir::Type::Slice) {
        auto span = analysis.span(state, r.location.id);
        out << "    Span contract: borrowed slice; "
            << (span.capability == Access::Unique ? "Unique" : "Shared") << " element=#T"
            << span.element << " region-positions=" << span.regions.size()
            << " loans=" << span.loans.size() << " storage-witnesses=" << span.storage.size()
            << " caller-owned=" << span.callerOwned
            << "; frame/whole-result/storage contracts pending\n";
      }
    }
    out << "  cancellation-cleanup=" << (state.cleanupValid ? "checked" : "blocked") << "\n";
    if (state.blocker)
      out << "    " << diagnosticCode(state.blocker->code) << ": " << state.blocker->summary
          << "\n";
    for (const auto &drop : state.cleanup)
      step(drop);
    if (!state.completedCleanup.empty()) {
      out << "  completed-edge-cleanup:\n";
      for (const auto &drop : state.completedCleanup)
        step(drop);
    }
  }
}
void print(const core::Verified &verified, std::ostream &out) {
  Analysis analysis(verified);
  const auto &p = verified.get();
  out << "Concurrency foundations: queries only; execution requires checked contracts\n"
         "Structural capabilities do not prove capture regions, transfer, frame cleanup or cancellation\n";
  for (TypeId id = 0; id < p.semantics.types.size(); ++id) {
    out << "type #T" << id << " " << p.semantics.types[id].name << "\n  Send: ";
    show(analysis.type(id, Capability::Send), out);
    out << "\n  Sync: ";
    show(analysis.type(id, Capability::Sync), out);
    out << "\n";
  }
  for (const auto &function : p.functions) {
    auto graph = std::find_if(p.regionGraphs.begin(), p.regionGraphs.end(),
                              [&](const auto &g) { return g.function == function.id; });
    if (graph == p.regionGraphs.end())
      fail({}, DiagnosticCode::InvalidCore, "missing verified concurrency graph");
    out << "fn #F" << function.id << " " << p.semantics.functions[function.id].name
        << " semantic CFG\n";
    std::function<void(const std::vector<core::Inst> &)> walk = [&](const auto &body) {
      for (const auto &i : body) {
        if (i.semanticPoint != None && graph->resourcePoints.contains(i.semanticPoint) &&
            (core::functionCall(i) || i.kind == core::Inst::Return || i.kind == core::Inst::Loop ||
             i.kind == core::Inst::BorrowShared || i.kind == core::Inst::BorrowUnique)) {
          auto facts = analysis.point(function.id, {i.semanticPoint, 0});
          out << "  bb" << facts.point.block
              << ":0 before: live-symbols=" << facts.liveSymbols.size()
              << " live-values=" << facts.liveValues.size()
              << " loan-demands=" << facts.loanDemands.size() << " carriers=";
          if (facts.carriersObserved)
            out << facts.carriers.size();
          else
            out << "unobserved (sparse; not empty)";
          out << "\n";
        }
        walk(i.body);
        walk(i.otherwise);
        walk(i.condition);
        walk(i.latch);
      }
    };
    walk(function.body);
  }
}
}
