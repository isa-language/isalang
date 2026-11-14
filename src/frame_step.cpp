#include "frame_heap.hpp"
#include "frame_core_builder.hpp"
#include "isac/execution.hpp"
#include <functional>

namespace isac::frames::detail {
using I = core::Inst;

size_t stepBoundary(const core::Program &p, FunctionId source, FunctionId stepBefore, Loc loc) {
  const auto found = stepLocation(p, source, stepBefore, loc);
  if (found.branches)
    fail(loc, DiagnosticCode::FutureFeature,
         "frame boundary requires verified branch normalization");
  return found.position;
}

void splitContinuation(LoweredRoundTrip &lowered, FunctionId stepBefore, Loc loc,
                       bool projectFields, FunctionId source) {

  auto verified = core::verify(lowered.program, {true});
  const auto first = source != None          ? source
                     : lowered.steps.empty() ? lowered.continuation
                                             : lowered.steps.back().continuation;
  auto branch = prepareBranchBoundary(verified, first, stepBefore, loc);
  if (branch) {
    for (auto &[old, value] : lowered.restoredValues) {
      (void)old;
      value = branch->valueRenaming.at(value);
    }
    for (auto it = lowered.valueRenaming.begin(); it != lowered.valueRenaming.end();) {
      auto found = branch->valueRenaming.find(it->second);
      if (found == branch->valueRenaming.end())
        it = lowered.valueRenaming.erase(it);
      else {
        it->second = found->second;
        ++it;
      }
    }
    verified = std::move(branch->source);
  }
  const auto &p = verified.get();
  const auto position = stepBoundary(p, first, stepBefore, loc);
  const auto function = std::find_if(p.functions.begin(), p.functions.end(),
                                     [&](const auto &f) { return f.id == first; });
  const auto *boundary = &function->body[position];
  const auto result = p.semantics.functions.at(first).result;
  if (result != Unit &&
      (!p.semantics.containerElement(result) || p.semantics.containsProtocolFields(result)))
    fail(boundary->loc, DiagnosticCode::FutureFeature,
         "frame segment completion requires a reference-free, protocol-free result");
  const core::CaptureState *state = nullptr;
  for (const auto &s : p.captureStates)
    if (s.function == first && s.side == core::StateSide::Before &&
        s.point.block == boundary->semanticPoint) {
      if (state)
        fail(boundary->loc, DiagnosticCode::InvalidCore, "ambiguous frame step transfer witness");
      state = &s;
    }
  if (!state)
    fail(boundary->loc, DiagnosticCode::FutureFeature,
         "frame step boundary is unreachable or has no transferable capture witness");
  const auto origin = boundary->loc;
  auto projected =
      projectFields ? projectCapture(verified, *state) : std::optional<PreparedCapture>{};
  const auto &captureSource = projected ? projected->source : verified;
  const auto &captureState = projected ? projected->witness() : *state;
  if (projected) {
    for (auto &[old, value] : lowered.restoredValues) {
      (void)old;
      value = projected->valueRenaming.at(value);
    }
    for (auto it = lowered.valueRenaming.begin(); it != lowered.valueRenaming.end();) {
      auto found = projected->valueRenaming.find(it->second);
      if (found == projected->valueRenaming.end())
        it = lowered.valueRenaming.erase(it);
      else {
        it->second = found->second;
        ++it;
      }
    }
  }
  auto schema = build(captureSource, captureState).get();
  if (schema.slots.empty())
    fail(origin, DiagnosticCode::FutureFeature,
         "frame step requires nonempty owned or live Copy storage");
  RoundTripOptions options;
  options.heap = true;
  options.outline = true;
  auto next = lowerRoundTrip(captureSource, captureState, std::move(schema), options);
  auto found = std::find_if(next.program.functions.begin(), next.program.functions.end(),
                            [&](const auto &f) { return f.id == first; });
  if (found == next.program.functions.end())
    fail(origin, DiagnosticCode::InvalidCore, "missing first segment after outlining");
  auto &prefix = *found;
  const auto n = prefix.body.size();
  if (n < 3 || prefix.body[n - 3].kind != I::Move || prefix.body[n - 3].slot != next.heapStorage ||
      prefix.body[n - 2].kind != I::Call || prefix.body[n - 2].function != next.continuation ||
      prefix.body[n - 1].kind != I::Return)
    fail(origin, DiagnosticCode::InvalidCore,
         "missing checked next-frame handoff in first segment");
  auto &semantics = next.program.semantics;
  if (result != Unit && semantics.storageWidth(result) >= 65536)
    fail(origin, DiagnosticCode::FutureFeature,
         "frame segment outcome exceeds the existing enum layout limit");
  hir::Type outcome{};
  outcome.kind = hir::Type::Enum;
  outcome.name = "$frame.segment.result." + std::to_string(first);
  outcome.variants = {
      {SegmentContinue, "Continue", {next.heapType}},
      {SegmentComplete, "Complete", result == Unit ? std::vector<TypeId>{} : std::vector{result}}};
  outcome.width = std::max(2u, result == Unit ? 1u : 1 + semantics.storageWidth(result));
  const auto outcomeType = static_cast<TypeId>(semantics.types.size());
  semantics.types.push_back(std::move(outcome));

  const auto nextFrame = prefix.body[n - 3].result;
  prefix.body.resize(n - 3);
  auto construct = [&](VariantId variant, std::vector<ValueId> values, Loc where) {
    I i{};
    i.kind = I::EnumConstruct;
    i.type = outcomeType;
    i.loc = where;
    i.offset = variant;
    i.args = std::move(values);
    i.quantity = Quantity::One;
    i.result = next.program.values++;
    return i;
  };
  std::function<void(std::vector<I> &)> completeReturns = [&](auto &body) {
    std::vector<I> rewritten;
    for (auto &i : body) {
      completeReturns(i.condition);
      completeReturns(i.body);
      completeReturns(i.otherwise);
      completeReturns(i.latch);
      if (i.kind == I::Return) {
        auto done = construct(SegmentComplete, std::move(i.args), i.loc);
        i.type = outcomeType;
        i.quantity = Quantity::One;
        i.args = {done.result};
        rewritten.push_back(std::move(done));
      }
      rewritten.push_back(std::move(i));
    }
    body = std::move(rewritten);
  };
  completeReturns(prefix.body);

  I move{};
  move.kind = I::Move;
  move.type = next.heapType;
  move.slot = next.heapStorage;
  move.loc = origin;
  move.quantity = Quantity::One;
  move.result = nextFrame;
  auto onward = construct(SegmentContinue, {move.result}, origin);
  I ret{};
  ret.kind = I::Return;
  ret.type = outcomeType;
  ret.quantity = Quantity::One;
  ret.args = {onward.result};
  ret.loc = origin;
  prefix.body.insert(prefix.body.end(), {move, onward, ret});

  auto signature = semantics.functions.at(first);
  signature.id = static_cast<FunctionId>(semantics.functions.size());
  signature.name = "__frame_segment_" + std::to_string(signature.id);
  signature.result = outcomeType;
  const auto segment = signature.id;
  semantics.functions.push_back(std::move(signature));
  const auto parameters = prefix.parameters;
  auto segmentBody = std::move(prefix.body);
  std::vector<SymbolId> inputs;
  for (auto old : semantics.functions.at(segment).params) {
    const auto &parameter = semantics.symbols.at(old);
    if (parameter.reference || parameter.receiver || parameter.access != Access::Own ||
        !execution::closedStorage(semantics, parameter.type))
      fail(origin, DiagnosticCode::FutureFeature, "segment bridge requires closed owned inputs");
    hir::Symbol input{};
    input.id = static_cast<SymbolId>(semantics.symbols.size());
    input.name = "$frame.bridge.input";
    input.type = parameter.type;
    input.parameter = true;
    input.loc = origin;
    inputs.push_back(input.id);
    semantics.symbols.push_back(input);
  }
  hir::Symbol delivered{};
  delivered.id = static_cast<SymbolId>(semantics.symbols.size());
  delivered.name = "$frame.bridge.outcome";
  delivered.type = outcomeType;
  delivered.loc = origin;
  semantics.symbols.push_back(delivered);
  semantics.functions.at(first).params = inputs;
  prefix.parameters.clear();
  for (auto unused : inputs) {
    (void)unused;
    prefix.parameters.push_back(next.program.values++);
  }
  prefix.fieldPostStates.clear();
  CoreBuilder bridge{next.program, origin, {}}, onwardBody{next.program, origin, {}},
      doneBody{next.program, origin, {}};
  std::vector<ValueId> arguments;
  for (auto input : inputs)
    arguments.push_back(bridge.transfer(input));
  bridge.bind(delivered.id, bridge.call(segment, std::move(arguments)));
  auto continues = bridge.test(delivered.id, SegmentContinue);
  auto box = onwardBody.payload(delivered.id, SegmentContinue, next.heapType);
  onwardBody.ret(result, onwardBody.call(next.continuation, {box}));
  ValueId finalValue = None;
  if (result != Unit)
    finalValue = doneBody.payload(delivered.id, SegmentComplete, result);
  else {
    auto drop = doneBody.operation(I::Drop, outcomeType);
    drop.slot = delivered.id;
    doneBody.emit(drop);
  }
  doneBody.ret(result, finalValue);
  auto dispatch = bridge.operation(I::If);
  dispatch.args = {continues};
  dispatch.body = std::move(onwardBody.body);
  dispatch.otherwise = std::move(doneBody.body);
  bridge.emit(dispatch);
  prefix.body = std::move(bridge.body);
  next.program.functions.push_back({segment, std::move(segmentBody), parameters, {}});
  lowered.steps.push_back({next.frameType, next.heapType, next.continuation, origin, outcomeType,
                           segment,
                           projected ? projected->transfers : std::vector<ProjectionTransfer>{},
                           branch ? branch->rotations : 0});
  if (lowered.steps.size() == 1) {
    lowered.continuation = segment;
    lowered.step = lowered.steps.front();
  }
  for (auto &[old, value] : lowered.restoredValues) {
    (void)old;
    value = next.valueRenaming.at(value);
  }
  for (auto it = lowered.valueRenaming.begin(); it != lowered.valueRenaming.end();) {
    auto found = next.valueRenaming.find(it->second);
    if (found == next.valueRenaming.end())
      it = lowered.valueRenaming.erase(it);
    else {
      it->second = found->second;
      ++it;
    }
  }
  lowered.program = std::move(next.program);
}

std::optional<size_t> stepOrder(const core::Program &p, FunctionId source, RoundTripOptions options,
                                Loc loc) {
  if (!options.stepBefore) {
    if (!options.followingSteps.empty())
      fail(loc, DiagnosticCode::FutureFeature, "additional frame steps require a first boundary");
    return {};
  }
  if (options.followingSteps.size() >= None - 4u)
    fail(loc, DiagnosticCode::FutureFeature, "frame state variant identity space exhausted");
  std::vector<FunctionId> targets{*options.stepBefore};
  targets.insert(targets.end(), options.followingSteps.begin(), options.followingSteps.end());
  auto first = stepLocation(p, source, *options.stepBefore, loc).position, previous = first;
  for (size_t n = 0; n < targets.size(); ++n) {
    const auto site = stepLocation(p, source, targets[n], loc);
    if (site.branches && n + 1 != targets.size())
      fail(loc, DiagnosticCode::FutureFeature,
           "a branch-nested checkpoint must be the final requested boundary");
  }
  for (auto target : options.followingSteps) {
    auto position = stepLocation(p, source, target, loc).position;
    if (position <= previous)
      fail(loc, DiagnosticCode::FutureFeature,
           "frame step boundaries must be distinct and follow source order");
    previous = position;
  }
  return first;
}
}
