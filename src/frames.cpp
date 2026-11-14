#include "isac/frames.hpp"
#include "isac/frame_lifecycle.hpp"
#include <functional>
#include <ostream>

namespace isac::frames {
namespace {
struct Inputs {
  std::map<storage::Location, const core::CaptureResource *> resources;
  std::map<storage::Location, const core::CaptureDrop *> drops;
  std::vector<storage::Location> dropOrder;
};
[[noreturn]] void invalid(const core::CaptureState &state, const char *message) {
  fail(state.origin, DiagnosticCode::InvalidCore, message);
}
Inputs inputs(const core::Verified &verified, const core::CaptureState &state) {
  const auto &p = verified.get();
  if (std::none_of(p.captureStates.begin(), p.captureStates.end(),
                   [&](const auto &s) { return &s == &state; }))
    invalid(state, "frame construction requires a witness belonging to this Verified Core");
  if (!state.cleanupValid) {
    if (state.blocker)
      throw Error(*state.blocker);
    invalid(state, "frame construction has no checked destruction witness");
  }
  if (state.terminal)
    invalid(state, "terminal edge cannot supply a frame continuation");
  const auto &types = p.semantics;
  std::function<bool(TypeId, const std::map<uint32_t, core::OwnedFieldState> &)> complete =
      [&](TypeId type, const auto &fields) {
        for (auto index : types.managedFields(type)) {
          auto field = fields.find(index);
          if (field == fields.end() ||
              !complete(types.types.at(type).fields.at(index).type, field->second.fields))
            return false;
        }
        return true;
      };
  Inputs result;
  for (const auto &resource : state.resources) {
    if (!types.linear(resource.type) && !resource.executionLive)
      continue;
    if (resource.memory != MemoryState::Own || resource.callerOwned || resource.root != None ||
        resource.shared || resource.unique || resource.exclusive || !resource.carriers.empty() ||
        types.containsReferences(resource.type) || resource.type == Str ||
        types.types.at(resource.type).kind == hir::Type::Slice)
      invalid(state, "borrowed source cannot enter an owned frame schema");
    if (!resource.absentFields.empty() || !resource.movedPayloads.empty() ||
        !complete(resource.type, resource.fields))
      DiagnosticEngine()
          .error(
              DiagnosticCode::FutureFeature, state.origin,
              "frame capture of a partially consumed source requires projection transfer contracts")
          .note(
              "ordinary partial cleanup remains supported; whole-value capture requires a complete source")
          .emit();
    const auto mode =
        types.linear(resource.type) ? core::CaptureMode::Move : core::CaptureMode::Copy;
    if (resource.mode != mode || !resource.location.path.empty() ||
        !result.resources.emplace(resource.location, &resource).second)
      invalid(state, "invalid source identity or capability in frame witness");
  }
  for (const auto &drop : state.cleanup)
    if (result.resources.contains(drop.location)) {
      if (!types.linear(drop.type))
        continue;
      const auto &source = *result.resources.at(drop.location);
      if (source.type != drop.type || source.allocation != drop.allocation ||
          !result.drops.emplace(drop.location, &drop).second)
        invalid(state, "frame destruction does not match its checked source");
      result.dropOrder.push_back(drop.location);
    }
  for (const auto &[place, source] : result.resources)
    if (types.linear(source->type) && !result.drops.contains(place))
      invalid(state, "linear frame input has no checked drop obligation");
  return result;
}
std::vector<SlotId> dropOrder(const Program &program, const Inputs &inputs) {
  std::map<storage::Location, SlotId> slots;
  std::vector<SlotId> result;

  for (auto it = program.slots.rbegin(); it != program.slots.rend(); ++it) {
    slots.emplace(it->source, it->id);
    if (it->mode == core::CaptureMode::Copy)
      result.push_back(it->id);
  }
  for (const auto &place : inputs.dropOrder)
    result.push_back(slots.at(place));
  return result;
}
}
Verified verify(const core::Verified &verified, const core::CaptureState &witness,
                Program program) {
  const auto source = inputs(verified, witness);
  if (program.slots.size() != source.resources.size())
    invalid(witness, "frame schema must account for every live value and owned cleanup obligation");
  std::set<storage::Location> captured;
  std::vector<core::CaptureResource> contents;
  std::vector<std::optional<core::CaptureDrop>> drops;
  for (size_t n = 0; n < program.slots.size(); ++n) {
    const auto &slot = program.slots[n];
    auto found = source.resources.find(slot.source);
    if (slot.id != n || found == source.resources.end() || !captured.insert(slot.source).second ||
        slot.type != found->second->type || slot.mode != found->second->mode)
      invalid(witness,
              "frame slot type, identity or capture mode differs from its verified source");
    contents.push_back(*found->second);
    auto drop = source.drops.find(slot.source);
    drops.push_back(drop == source.drops.end() ? std::nullopt : std::optional{*drop->second});
  }
  State current;
  std::set<SlotId> acquired;
  std::vector<State> states;
  bool initializing = false, sealed = false;
  for (const auto &operation : program.construct) {
    if (sealed)
      invalid(witness, "frame constructor has instructions after Seal");
    if (operation.kind == Operation::Seal) {
      if (operation.slot != None || !current.staged.empty() ||
          current.initialized.size() != program.slots.size())
        invalid(witness, "frame Seal requires every slot initialized exactly once");
      if (!initializing)
        states.push_back(current);
      sealed = true;
      continue;
    }
    if (operation.slot >= program.slots.size())
      invalid(witness, "frame operation uses an invalid slot");
    const auto &slot = program.slots[operation.slot];
    switch (operation.kind) {
    case Operation::AcquireMove:
    case Operation::AcquireCopy:
      if (initializing || !acquired.insert(slot.id).second ||
          (operation.kind == Operation::AcquireMove) != (slot.mode == core::CaptureMode::Move))
        invalid(witness, "frame acquisition duplicates or changes an input capability");
      current.staged.insert(slot.id);
      break;
    case Operation::Initialize:
      if (!initializing) {
        if (acquired.size() != program.slots.size())
          invalid(witness, "frame initialization requires ownership of all capture inputs");
        states.push_back(current);
        initializing = true;
      }
      if (!current.staged.erase(slot.id) || !current.initialized.insert(slot.id).second)
        invalid(witness, "frame slot initialized twice or without a staged value");
      states.push_back(current);
      break;
    default:
      invalid(witness, "invalid operation in frame constructor");
    }
  }
  if (!sealed || states.size() != program.slots.size() + 1)
    invalid(witness, "frame constructor lacks a complete initialization proof");
  if (program.cleanup.size() != states.size())
    invalid(witness, "frame construction lacks cleanup for an initialization prefix");
  const auto order = dropOrder(program, source);
  std::set<uint32_t> prefixes;
  for (const auto &edge : program.cleanup) {
    if (edge.prefix >= states.size() || !prefixes.insert(edge.prefix).second)
      invalid(witness, "frame cleanup edge has an invalid or duplicate construction state");
    auto remaining = states[edge.prefix];
    bool finished = false;
    size_t next = 0;
    for (const auto &operation : edge.body) {
      if (finished)
        invalid(witness, "frame cleanup continues after Finish");
      if (operation.kind == Operation::Finish) {
        if (operation.slot != None || !remaining.staged.empty() || !remaining.initialized.empty() ||
            next != order.size())
          invalid(witness, "frame Finish leaves initialized or staged values undestroyed");
        finished = true;
        continue;
      }
      if (operation.slot >= program.slots.size() || next >= order.size() ||
          order[next++] != operation.slot)
        invalid(witness, "frame cleanup changes checked destruction order or drops a value twice");
      if (operation.kind == Operation::DropSlot) {
        if (!remaining.initialized.erase(operation.slot))
          invalid(witness, "frame cleanup drops an uninitialized slot");
      } else if (operation.kind == Operation::DropStaged) {
        if (!remaining.staged.erase(operation.slot))
          invalid(witness, "frame cleanup drops a transferred staging value");
      } else
        invalid(witness, "invalid operation in frame cleanup");
    }
    if (!finished)
      invalid(witness, "frame cleanup edge lacks Finish");
  }
  return Verified(std::move(program), std::move(states), std::move(contents), std::move(drops),
                  {witness.function, witness.point, witness.side, witness.entryState});
}
Verified build(const core::Verified &verified, const core::CaptureState &witness) {
  const auto source = inputs(verified, witness);
  Program program;
  auto slot = [&](const storage::Location &place) {
    const auto &resource = *source.resources.at(place);
    program.slots.push_back(
        {static_cast<SlotId>(program.slots.size()), place, resource.type, resource.mode});
  };

  for (auto it = source.dropOrder.rbegin(); it != source.dropOrder.rend(); ++it)
    slot(*it);
  for (const auto &[place, resource] : source.resources)
    if (resource->mode == core::CaptureMode::Copy)
      slot(place);
  for (const auto &s : program.slots)
    program.construct.push_back(
        {s.mode == core::CaptureMode::Move ? Operation::AcquireMove : Operation::AcquireCopy,
         s.id});
  for (const auto &s : program.slots)
    program.construct.push_back({Operation::Initialize, s.id});
  program.construct.push_back({Operation::Seal});
  const auto order = dropOrder(program, source);
  for (uint32_t prefix = 0; prefix <= program.slots.size(); ++prefix) {
    CleanupEdge edge{prefix, {}};
    for (auto id : order)
      edge.body.push_back({id < prefix ? Operation::DropSlot : Operation::DropStaged, id});
    edge.body.push_back({Operation::Finish});
    program.cleanup.push_back(std::move(edge));
  }
  return verify(verified, witness, std::move(program));
}
void print(const core::Verified &verified, std::ostream &out) {
  static constexpr const char *names[] = {"acquire.move", "acquire.copy", "initialize", "seal",
                                          "drop.slot",    "drop.staged",  "finish"};
  out << "Typed frame IR: verification only; async/coroutines/threads are not implemented\n"
         "Construction cleanup consumes staging inputs; it does not restore moved caller bindings\n";
  auto operation = [&](const Operation &op) {
    out << "    " << names[op.kind];
    if (op.slot != None)
      out << " slot" << op.slot;
    out << "\n";
  };
  for (const auto &state : verified.get().captureStates) {
    out << "fn #F" << state.function << " bb" << state.point.block << ":0 "
        << (state.side == core::StateSide::Before ? "before" : "after")
        << " entry-state=" << state.entryState << "\n";
    try {
      auto frame = build(verified, state);
      for (const auto &s : frame.get().slots) {
        const auto &value = frame.content(s.id);
        out << "  slot" << s.id << " type=#T" << s.type
            << " source=" << (s.source.kind == storage::Location::Symbol ? "#S" : "%")
            << s.source.id << " state=" << value.protocolState << " allocation=" << value.allocation
            << "\n";
      }
      out << "  constructor:\n";
      for (const auto &op : frame.get().construct)
        operation(op);
      print(buildLifecycle(frame, false), out);
      print(buildLifecycle(frame, true), out);
      for (const auto &edge : frame.get().cleanup) {
        const auto &facts = frame.constructionStates()[edge.prefix];
        out << "  cleanup prefix=" << edge.prefix << " initialized=" << facts.initialized.size()
            << " staged=" << facts.staged.size() << ":\n";
        for (const auto &op : edge.body)
          operation(op);
      }
    } catch (const Error &error) {
      if (error.diagnostic.code == DiagnosticCode::InvalidCore)
        throw;
      out << "  blocked " << error.code << ": " << error.diagnostic.summary << "\n";
    }
  }
}
}
