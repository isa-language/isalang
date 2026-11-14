#include "frame_heap.hpp"
#include "frame_core_builder.hpp"
#include <functional>

namespace isac::frames {
namespace {
using I = core::Inst;
[[noreturn]] void unsupported(Loc loc, const char *message) {
  fail(loc, DiagnosticCode::FutureFeature, message);
}
bool complete(const hir::Program &p, TypeId type,
              const std::map<uint32_t, core::OwnedFieldState> &fields) {
  for (auto index : p.managedFields(type)) {
    auto found = fields.find(index);
    const auto child = p.types.at(type).fields.at(index).type;
    if (found == fields.end() || found->second.state != None ||
        found->second.allocation != p.needsDrop(child) || !complete(p, child, found->second.fields))
      return false;
  }
  return true;
}
void partialShape(const hir::Program &p, TypeId type,
                  const std::map<uint32_t, core::OwnedFieldState> &fields, Loc loc) {
  const auto &shape = p.types.at(type);
  if (shape.kind != hir::Type::Structure || p.heap(type) || shape.protocol != None)
    unsupported(loc, "partial projection transfer requires ordinary struct ancestors");
  for (uint32_t n = 0; n < shape.fields.size(); ++n) {
    const auto child = shape.fields[n].type;
    if (p.managedFieldType(child)) {
      if (auto found = fields.find(n); found != fields.end()) {
        if (found->second.state != None || found->second.allocation != p.needsDrop(child))
          unsupported(loc, "partial projection transfer requires an unchanged allocation contract");
        if (!complete(p, child, found->second.fields))
          partialShape(p, child, found->second.fields, loc);
      }
    } else if (!scalar(child))
      unsupported(loc, "projection capture currently copies only scalar non-owned fields");
  }
}
void remapUses(std::vector<I> &body, const std::map<ValueId, ValueId> &ids) {
  for (auto &i : body) {
    for (auto &arg : i.args)
      if (auto found = ids.find(arg); found != ids.end())
        arg = found->second;
    remapUses(i.condition, ids);
    remapUses(i.body, ids);
    remapUses(i.otherwise, ids);
    remapUses(i.latch, ids);
  }
}
}

size_t detail::capturePosition(const core::Verified &source, const core::CaptureState &state) {
  const auto &p = source.get();
  if (std::none_of(p.captureStates.begin(), p.captureStates.end(),
                   [&](const auto &s) { return &s == &state; }))
    fail(state.origin, DiagnosticCode::InvalidCore,
         "capture witness belongs to another Verified Core");
  const auto &signature = p.semantics.functions.at(state.function);
  if (state.side != core::StateSide::Before || state.entryState != None || state.terminal ||
      signature.owner != None || signature.resultOrigin != None ||
      signature.resultBorrowedContainer || !signature.parameterRegions.empty() ||
      !signature.payloadParameterRegions.empty())
    unsupported(
        state.origin,
        "frame handoff requires a normal top-level boundary without function region/receiver contracts");
  auto function = std::find_if(p.functions.begin(), p.functions.end(),
                               [&](const auto &f) { return f.id == state.function; });
  if (function == p.functions.end())
    fail(state.origin, DiagnosticCode::InvalidCore, "missing frame source function");
  auto instruction = std::find_if(function->body.begin(), function->body.end(), [&](const auto &i) {
    return i.semanticPoint == state.point.block;
  });
  if (instruction == function->body.end())
    unsupported(
        state.origin,
        "frame handoff inside a structured branch/loop requires an edge continuation transform");
  return instruction - function->body.begin();
}

const core::CaptureState &PreparedCapture::witness() const {
  const auto &p = source.get();
  auto functionBody = std::find_if(p.functions.begin(), p.functions.end(),
                                   [&](const auto &f) { return f.id == function; });
  if (functionBody == p.functions.end() || boundaryIndex >= functionBody->body.size())
    fail({}, DiagnosticCode::InvalidCore, "missing projected capture boundary");
  const auto point = functionBody->body[boundaryIndex].semanticPoint;
  for (const auto &s : p.captureStates)
    if (s.function == function && s.side == core::StateSide::Before && s.point.block == point)
      return s;
  fail({}, DiagnosticCode::InvalidCore, "projected capture has no reverified witness");
}

std::optional<PreparedCapture> projectCapture(const core::Verified &source,
                                              const core::CaptureState &state) {
  const auto position = detail::capturePosition(source, state);
  if (!state.cleanupValid) {
    if (state.blocker)
      throw Error(*state.blocker);
    fail(state.origin, DiagnosticCode::InvalidCore, "projection capture lacks a cleanup witness");
  }
  const auto &sem = source.get().semantics;

  if (std::none_of(state.resources.begin(), state.resources.end(), [&](const auto &r) {
        return sem.linear(r.type) && (!r.absentFields.empty() || !complete(sem, r.type, r.fields));
      }))
    return {};
  std::map<storage::Location, const core::CaptureResource *> owners;
  std::set<storage::Location> partial;
  for (const auto &r : state.resources)
    if (sem.linear(r.type)) {
      if (r.memory != MemoryState::Own || r.mode != core::CaptureMode::Move || r.callerOwned ||
          r.root != None || r.shared || r.unique || r.exclusive || !r.carriers.empty() ||
          sem.containsReferences(r.type) || !sem.containerElement(r.type) ||
          sem.containsProtocolFields(r.type))
        unsupported(state.origin,
                    "projection capture requires unborrowed, reference-free, protocol-free owners");
      if (!r.location.path.empty() || !r.movedPayloads.empty())
        unsupported(state.origin, "projection capture does not yet transfer partial enum payloads");
      if (!r.absentFields.empty() || !complete(sem, r.type, r.fields)) {
        if (r.location.kind != storage::Location::Symbol ||
            sem.types.at(r.type).kind != hir::Type::Structure || sem.heap(r.type))
          unsupported(state.origin,
                      "partial projection capture requires a named ordinary struct owner");
        partialShape(sem, r.type, r.fields, state.origin);
        partial.insert(r.location);
      } else if (!sem.managedFieldType(r.type))
        unsupported(state.origin, "projection capture requires a supported owned-field transfer");
      owners.emplace(r.location, &r);
    }
  if (partial.empty())
    return {};
  LoweredRoundTrip normalized{source.get(), None, None, {}, {}};
  auto &p = normalized.program;
  detail::CoreBuilder acquire{p, state.origin, {}}, restore{p, state.origin, {}};
  std::vector<ProjectionTransfer> transfers;
  std::vector<SymbolId> expectedDrop;
  std::map<ValueId, ValueId> restoredValues;
  auto stage = [&](storage::Location place, TypeId type, ValueId value) {
    hir::Symbol s{};
    s.id = static_cast<SymbolId>(p.semantics.symbols.size());
    s.name = "$frame.projection." + std::to_string(s.id);
    s.type = type;
    s.loc = state.origin;
    p.semantics.symbols.push_back(s);
    acquire.bind(s.id, value);
    transfers.push_back({std::move(place), type, s.id});
    if (p.semantics.linear(type))
      expectedDrop.push_back(s.id);
    return s.id;
  };
  std::set<storage::Location> visited;

  for (auto it = state.cleanup.rbegin(); it != state.cleanup.rend(); ++it) {
    if (!owners.contains(it->location))
      continue;
    if (!visited.insert(it->location).second)
      fail(state.origin, DiagnosticCode::InvalidCore, "duplicate projection source cleanup");
    const auto &r = *owners.at(it->location);
    if (it->type != r.type || it->allocation != r.allocation)
      fail(state.origin, DiagnosticCode::InvalidCore,
           "projection cleanup differs from source owner");
    if (!partial.contains(r.location)) {
      auto value = r.location.kind == storage::Location::Symbol ? acquire.move(r.location.id)
                                                                : r.location.id;
      const auto staged = stage(r.location, r.type, value);
      auto back = restore.move(staged);
      if (r.location.kind == storage::Location::Symbol)
        restore.bind(r.location.id, back);
      else
        restoredValues.emplace(r.location.id, back);
      continue;
    }
    const auto root = r.location.id;
    auto create = restore.operation(I::Create, r.type);
    restore.bind(root, restore.emit(create, true));
    std::function<void(TypeId, const std::map<uint32_t, core::OwnedFieldState> &,
                       std::vector<uint32_t>)>
        visit;
    visit = [&](TypeId parent, const auto &present, std::vector<uint32_t> prefix) {
      const auto fields = p.semantics.types.at(parent).fields;
      for (uint32_t n = 0; n < fields.size(); ++n) {
        const auto type = fields[n].type;
        const bool owned = p.semantics.managedFieldType(type);
        if (owned && !present.contains(n))
          continue;
        auto store = restore.operation(owned ? I::FieldStore : I::Write, type);
        store.slot = root;
        store.fieldPath = prefix;
        store.fieldIndex = n;
        if (owned && !complete(p.semantics, type, present.at(n).fields)) {

          auto empty = restore.operation(I::Create, type);
          store.args = {restore.emit(empty, true)};
          restore.emit(store);
          auto child = prefix;
          child.push_back(n);
          visit(type, present.at(n).fields, std::move(child));
          continue;
        }
        auto take = acquire.operation(owned ? I::FieldTake : I::Read, type);
        take.slot = root;
        take.fieldPath = prefix;
        take.fieldIndex = n;
        if (!owned) {
          take.placeProjection = I::PlaceProjection::Field;
          take.offset = fields[n].offset;
        }
        auto place = r.location;
        for (auto index : prefix)
          place.path.push_back({hir::ReferenceProjectionKind::Field, index});
        place.path.push_back({hir::ReferenceProjectionKind::Field, n});
        auto staged = stage(std::move(place), type, acquire.emit(take, true));
        ValueId back;
        if (owned)
          back = restore.move(staged);
        else {
          auto copy = restore.operation(I::Copy, type);
          copy.slot = staged;
          back = restore.emit(copy, true);
        }
        store.args = {back};
        if (!owned) {
          store.placeProjection = I::PlaceProjection::Field;
          store.offset = fields[n].offset;
        }
        restore.emit(store);
      }
    };
    visit(r.type, r.fields, {});
    auto drop = acquire.operation(I::Drop, r.type);
    drop.slot = root;
    acquire.emit(drop);
  }
  if (visited.size() != owners.size())
    fail(state.origin, DiagnosticCode::InvalidCore,
         "projection staging lost an owned cleanup obligation");
  auto body = std::find_if(p.functions.begin(), p.functions.end(),
                           [&](const auto &f) { return f.id == state.function; });
  std::vector<I> suffix(body->body.begin() + position, body->body.end());
  remapUses(suffix, restoredValues);
  body->body.resize(position);
  body->body.insert(body->body.end(), acquire.body.begin(), acquire.body.end());
  const auto markerIndex = body->body.size();
  body->body.push_back(acquire.operation(I::ScopeEnd));
  body->body.insert(body->body.end(), restore.body.begin(), restore.body.end());
  body->body.insert(body->body.end(), suffix.begin(), suffix.end());
  detail::normalizeFrameValues(normalized);
  auto checked = core::verify(std::move(normalized.program), {true});
  PreparedCapture prepared{std::move(checked), state.function, markerIndex,
                           std::move(normalized.valueRenaming), std::move(transfers)};
  const auto &after = prepared.witness();
  if (!after.cleanupValid) {
    if (after.blocker)
      throw Error(*after.blocker);
    fail(state.origin, DiagnosticCode::InvalidCore, "projection marker has no cleanup witness");
  }
  std::reverse(expectedDrop.begin(), expectedDrop.end());
  std::vector<SymbolId> actual;
  for (const auto &drop : after.cleanup)
    if (prepared.source.get().semantics.linear(drop.type)) {
      if (drop.location.kind != storage::Location::Symbol || !drop.location.path.empty())
        fail(state.origin, DiagnosticCode::InvalidCore,
             "projection cleanup retained an unstaged obligation");
      actual.push_back(drop.location.id);
    }
  if (actual != expectedDrop)
    fail(state.origin, DiagnosticCode::InvalidCore,
         "projection staging changed source destruction order");
  (void)build(prepared.source, after);
  return prepared;
}
}
