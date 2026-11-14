#pragma once
#include "isac/core.hpp"
#include <string_view>

namespace isac::frames {
using SlotId = uint32_t;
struct Slot {
  SlotId id;
  storage::Location source;
  TypeId type;
  core::CaptureMode mode;
};
struct Operation {
  enum Kind { AcquireMove, AcquireCopy, Initialize, Seal, DropSlot, DropStaged, Finish } kind;
  SlotId slot = None;
};
struct CleanupEdge {
  uint32_t prefix;
  std::vector<Operation> body;
};
struct Program {
  std::vector<Slot> slots;

  std::vector<Operation> construct;
  std::vector<CleanupEdge> cleanup;
};
struct State {
  std::set<SlotId> staged, initialized;
};
struct Origin {
  FunctionId function;
  regions::ProgramPoint point;
  core::StateSide side;
  StateId entryState;
};
class Verified {
  Program program;
  std::vector<State> states;

  std::vector<core::CaptureResource> contents;
  std::vector<std::optional<core::CaptureDrop>> drops;
  Origin source;
  Verified(Program p, std::vector<State> s, std::vector<core::CaptureResource> c,
           std::vector<std::optional<core::CaptureDrop>> d, Origin o)
      : program(std::move(p)), states(std::move(s)), contents(std::move(c)), drops(std::move(d)),
        source(o) {}
  friend Verified verify(const core::Verified &, const core::CaptureState &, Program);

public:
  const Program &get() const {
    return program;
  }
  const Origin &origin() const {
    return source;
  }
  const std::vector<State> &constructionStates() const {
    return states;
  }
  const core::CaptureResource &content(SlotId id) const {
    return contents.at(id);
  }
  const std::optional<core::CaptureDrop> &drop(SlotId id) const {
    return drops.at(id);
  }
};

Verified verify(const core::Verified &, const core::CaptureState &, Program);
Verified build(const core::Verified &, const core::CaptureState &);
void print(const core::Verified &, std::ostream &);

struct RoundTripOptions {
  std::optional<uint32_t> constructionExit;
  bool heap = false;

  std::optional<SymbolId> destroyWhen;
  bool outline = false;
  bool stateful = false;
  enum class StateExit { Ready, Completed, Checkpoint };
  std::optional<StateExit> stateExit;

  std::optional<FunctionId> stepBefore;
  std::vector<FunctionId> followingSteps = {};
  uint32_t checkpointIndex = 1;
  bool projectFields = false;
};
struct ProjectionTransfer {
  storage::Location source;
  TypeId type = Unit;
  SymbolId staging = None;
};
struct PreparedCapture {
  core::Verified source;
  FunctionId function;
  size_t boundaryIndex;
  std::map<ValueId, ValueId> valueRenaming;
  std::vector<ProjectionTransfer> transfers;
  const core::CaptureState &witness() const;
};

std::optional<PreparedCapture> projectCapture(const core::Verified &, const core::CaptureState &);
struct ProgressStep {
  TypeId frameType = None, heapType = None;
  FunctionId continuation = None;
  Loc origin{};
  TypeId outcomeType = None;
  FunctionId segment = None;
  std::vector<ProjectionTransfer> projections = {};
  uint32_t branchRotations = 0;
};
struct ProgressStorage {
  TypeId type = None;
  SymbolId storage = None;
  FunctionId create = None, advance = None, cancel = None, drop = None, take = None;
};
struct LoweredRoundTrip {
  core::Program program;
  TypeId frameType = None;
  SymbolId storage = None;
  std::map<SymbolId, SymbolId> restoredSymbols;
  std::map<ValueId, ValueId> restoredValues;
  TypeId heapType = None;
  SymbolId heapStorage = None;
  FunctionId factory = None, restore = None, destroy = None;
  FunctionId continuation = None;

  std::map<SymbolId, SymbolId> continuationSymbols = {};
  std::optional<ProgressStorage> progress = {};
  std::optional<ProgressStep> step = {};
  std::vector<ProgressStep> steps = {};

  std::map<ValueId, ValueId> valueRenaming = {};
};

LoweredRoundTrip lowerRoundTrip(const core::Verified &, const core::CaptureState &, Program,
                                RoundTripOptions = {});
core::Verified roundTrip(const core::Verified &, const core::CaptureState &, Program,
                         RoundTripOptions = {}, core::VerificationOptions = {});

core::Verified roundTripEntry(const core::Verified &, RoundTripOptions = {},
                              core::VerificationOptions = {}, std::string_view destroyWhenName = {},
                              std::string_view stepBeforeName = {});

struct DeferredFunction {
  core::Verified verified;
  ProgressStorage state;
  std::vector<ProgressStep> steps;
};
DeferredFunction deferFunction(const core::Verified &, FunctionId source,
                               const std::vector<FunctionId> &checkpoints = {},
                               bool projectFields = false);

core::Verified deferCalls(const core::Verified &, FunctionId source,
                          const std::vector<FunctionId> &checkpoints = {},
                          core::VerificationOptions = {});
}
