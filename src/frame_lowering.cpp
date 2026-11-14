#include "isac/frames.hpp"
#include "frame_heap.hpp"
#include <functional>
#include <tuple>

namespace isac::frames {
namespace {
using I = core::Inst;
[[noreturn]] void unsupported(Loc origin, const char *message) {
  fail(origin, DiagnosticCode::FutureFeature, message);
}
size_t boundary(const core::Verified &source, const core::CaptureState &state,
                const Verified &frame, bool heap = false) {
  const auto &p = source.get();
  const auto position = detail::capturePosition(source, state);
  for (const auto &slot : frame.get().slots) {
    if (slot.mode == core::CaptureMode::Copy ? !scalar(slot.type)
                                             : !p.semantics.managedFieldType(slot.type))
      unsupported(state.origin,
                  "frame slot requires a supported scalar or managed owned-field transfer");
  }
  if (heap)
    detail::validateHeapInputs(p, frame, state.origin);
  return position;
}
std::optional<SlotId> dispatchSource(const core::Verified &source, const core::CaptureState &state,
                                     const Verified &frame, RoundTripOptions options) {
  if (!options.destroyWhen)
    return {};
  const auto result = source.get().semantics.functions.at(state.function).result;
  if (!options.heap || options.constructionExit || (result != Unit && result != I32))
    unsupported(state.origin,
                "frame dispatch requires a complete heap frame and a unit/i32 function result");
  for (const auto &slot : frame.get().slots)
    if (slot.source.kind == storage::Location::Symbol && slot.source.id == *options.destroyWhen &&
        slot.type == Bool && slot.mode == core::CaptureMode::Copy)
      return slot.id;
  unsupported(state.origin,
              "frame dispatch selector must be a live captured bool symbol at this boundary");
}
void progressOptions(const core::Verified &source, const core::CaptureState &state,
                     RoundTripOptions options) {
  if (options.stateful && (!options.heap || !options.outline || options.constructionExit))
    unsupported(state.origin,
                "frame state storage requires heap and outline without construction-prefix exits");
  if (options.stepBefore && !options.stateful)
    unsupported(state.origin, "frame step requires persistent frame state storage");
  if (options.projectFields && (!options.stateful || !options.stepBefore))
    unsupported(state.origin,
                "field projection capture requires persistent state and a checkpoint boundary");
  if (!options.stepBefore && !options.followingSteps.empty())
    unsupported(state.origin, "additional frame steps require a first boundary");
  if (options.checkpointIndex != 1 && options.stateExit != RoundTripOptions::StateExit::Checkpoint)
    unsupported(state.origin, "checkpoint index requires a checkpoint exit");
  if (!options.stateExit)
    return;
  using E = RoundTripOptions::StateExit;
  const auto result = source.get().semantics.functions.at(state.function).result;
  if (!options.stateful || options.destroyWhen || (result != Unit && result != I32) ||
      (*options.stateExit != E::Ready && *options.stateExit != E::Completed &&
       *options.stateExit != E::Checkpoint) ||
      (*options.stateExit == E::Checkpoint && !options.stepBefore))
    unsupported(
        state.origin,
        "frame state exit requires a unit/i32 function and excludes runtime destroy selection");
  if (*options.stateExit == E::Checkpoint &&
      (!options.checkpointIndex || options.checkpointIndex > options.followingSteps.size() + 1))
    unsupported(state.origin, "frame checkpoint exit index is outside the requested boundaries");
}
void remap(std::vector<I> &body, const std::map<SymbolId, SymbolId> &symbols,
           const std::map<ValueId, ValueId> &values) {
  auto symbol = [&](SymbolId &id) {
    if (auto found = symbols.find(id); found != symbols.end())
      id = found->second;
  };
  auto value = [&](ValueId &id) {
    if (auto found = values.find(id); found != values.end())
      id = found->second;
  };
  for (auto &i : body) {
    symbol(i.slot);
    for (auto &id : i.locals)
      symbol(id);
    for (auto &id : i.args)
      value(id);

    remap(i.body, symbols, values);
    remap(i.otherwise, symbols, values);
    remap(i.condition, symbols, values);
    remap(i.latch, symbols, values);
  }
}

void normalizeValues(LoweredRoundTrip &lowered) {
  auto &p = lowered.program;
  std::map<ValueId, ValueId> ids;
  ValueId next = 0;
  std::function<void(const std::vector<I> &)> collect = [&](const auto &body) {
    for (const auto &i : body) {
      collect(i.condition);
      if (i.result != None)
        ids.emplace(i.result, next++);
      collect(i.body);
      collect(i.otherwise);
      collect(i.latch);
    }
  };
  for (const auto &f : p.functions) {
    for (auto value : f.parameters)
      ids.emplace(value, next++);
    collect(f.body);
  }
  auto value = [&](ValueId &id) {
    if (id != None)
      id = ids.at(id);
  };
  std::function<void(std::vector<I> &)> rewrite = [&](auto &body) {
    for (auto &i : body) {
      value(i.result);
      for (auto &id : i.args)
        value(id);
      i.resourceInput = None;
      i.merges.clear();
      i.inferredEnds.clear();
      i.exitEnds.clear();
      i.semanticPoint = None;
      i.cleanup.clear();
      i.carriers.clear();
      i.fieldCleanup.clear();
      i.movedPayloads.clear();
      i.absentFields.clear();
      rewrite(i.condition);
      rewrite(i.body);
      rewrite(i.otherwise);
      rewrite(i.latch);
    }
  };
  for (auto &f : p.functions) {
    for (auto &id : f.parameters)
      value(id);
    rewrite(f.body);
  }
  for (auto &[old, restored] : lowered.restoredValues) {
    (void)old;
    value(restored);
  }
  if (lowered.valueRenaming.empty())
    lowered.valueRenaming = ids;
  else
    for (auto it = lowered.valueRenaming.begin(); it != lowered.valueRenaming.end();) {
      auto found = ids.find(it->second);
      if (found == ids.end())
        it = lowered.valueRenaming.erase(it);
      else {
        it->second = found->second;
        ++it;
      }
    }
  p.values = next;
}

void propagateCallEffects(core::Program &p) {
  std::map<FunctionId, std::set<FunctionId>> callees;
  for (const auto &function : p.functions) {
    std::function<void(const std::vector<I> &)> visit = [&](const auto &body) {
      for (const auto &i : body) {
        if (core::functionCall(i))
          callees[function.id].insert(i.function);
        visit(i.condition);
        visit(i.body);
        visit(i.otherwise);
        visit(i.latch);
      }
    };
    visit(function.body);
  }
  bool changed;
  do {
    changed = false;
    for (const auto &[caller, targets] : callees) {
      auto &effects = p.semantics.functions.at(caller).effects;
      const auto before = effects;
      for (auto callee : targets)
        effects.merge(p.semantics.functions.at(callee).effects);
      changed |= before != effects;
    }
  } while (changed);
}
}
void detail::normalizeFrameValues(LoweredRoundTrip &lowered) {
  normalizeValues(lowered);
}
LoweredRoundTrip lowerRoundTrip(const core::Verified &source, const core::CaptureState &witness,
                                Program proposal, RoundTripOptions options) {

  auto frame = verify(source, witness, std::move(proposal));
  auto position = boundary(source, witness, frame, options.heap);
  if (options.outline && (!options.heap || options.constructionExit))
    unsupported(
        witness.origin,
        "continuation extraction requires a complete heap frame and excludes construction-prefix exits");
  progressOptions(source, witness, options);
  (void)detail::stepOrder(source.get(), witness.function, options, witness.origin);
  const auto selectorSlot = dispatchSource(source, witness, frame, options);
  auto count = static_cast<uint32_t>(frame.get().slots.size());
  const auto &signature = source.get().semantics.functions.at(witness.function);
  if (options.constructionExit &&
      (*options.constructionExit > count || (signature.result != Unit && signature.result != I32)))
    unsupported(witness.origin,
                "construction exit requires a valid prefix and a unit/i32 function result");
  LoweredRoundTrip result{source.get(), None, None, {}, {}};
  auto &p = result.program;
  p.captureStates.clear();
  p.regionGraphs.clear();
  if (!count) {
    if (options.stateExit || options.stepBefore)
      unsupported(witness.origin, "frame state exits/steps require a nonempty capture");
    if (options.constructionExit) {
      auto function = std::find_if(p.functions.begin(), p.functions.end(),
                                   [&](const auto &f) { return f.id == witness.function; });
      function->body.resize(position);
      ValueId value = None;
      if (signature.result == I32) {
        I zero{};
        zero.kind = I::Constant;
        zero.type = I32;
        zero.loc = witness.origin;
        zero.quantity = Quantity::Omega;
        zero.text = "0";
        zero.result = p.values++;
        value = zero.result;
        function->body.push_back(std::move(zero));
      }
      I ret{};
      ret.kind = I::Return;
      ret.type = signature.result;
      ret.loc = witness.origin;
      ret.quantity = Quantity::Omega;
      if (value != None)
        ret.args = {value};
      function->body.push_back(std::move(ret));
      normalizeValues(result);
    }
    return result;
  }
  hir::Type shape{};
  shape.kind = hir::Type::Structure;
  shape.name =
      "$frame." + std::to_string(witness.function) + "." + std::to_string(witness.point.block);

  std::vector<uint32_t> fieldForSlot(count);
  const auto &completeCleanup =
      *std::find_if(frame.get().cleanup.begin(), frame.get().cleanup.end(),
                    [&](const auto &edge) { return edge.prefix == count; });
  uint32_t width = 0;
  for (auto it = completeCleanup.body.rbegin(); it != completeCleanup.body.rend(); ++it) {
    if (it->kind == Operation::Finish)
      continue;
    const auto &slot = frame.get().slots.at(it->slot);
    auto cells = p.semantics.storageWidth(slot.type);
    if (cells > 65536 || width > 65536 - cells)
      unsupported(witness.origin, "frame storage exceeds the existing aggregate layout limit");
    fieldForSlot[slot.id] = static_cast<uint32_t>(shape.fields.size());
    shape.fields.push_back({"slot" + std::to_string(slot.id), slot.type, width});
    width += cells;
  }
  shape.width = std::max(1u, width);
  result.frameType = static_cast<TypeId>(p.semantics.types.size());
  p.semantics.types.push_back(std::move(shape));
  if (options.heap)
    detail::defineHeapFactories(result, witness.origin);
  auto binding = [&](TypeId type, const std::string &name, Loc origin) {
    auto id = static_cast<SymbolId>(p.semantics.symbols.size());
    hir::Symbol s{};
    s.id = id;
    s.type = type;
    s.name = name;
    s.loc = std::move(origin);
    p.semantics.symbols.push_back(std::move(s));
    return id;
  };
  result.storage = binding(result.frameType, "$frame.storage", witness.origin);

  if (!options.constructionExit)
    for (const auto &slot : frame.get().slots)
      if (slot.source.kind == storage::Location::Symbol)
        result.restoredSymbols.emplace(slot.source.id, slot.source.id);
  auto instruction = [&](I::Kind kind, TypeId type) {
    I i{};
    i.kind = kind;
    i.type = type;
    i.loc = witness.origin;
    i.quantity = p.semantics.linear(type) ? Quantity::One : Quantity::Omega;
    return i;
  };
  std::vector<I> code;
  auto emit = [&](I i, bool produces = false) {
    if (produces)
      i.result = p.values++;
    auto id = i.result;
    code.push_back(std::move(i));
    return id;
  };
  std::vector<ValueId> staged(count, None);
  for (const auto &op : frame.get().construct) {
    if (op.kind != Operation::AcquireMove && op.kind != Operation::AcquireCopy)
      continue;
    const auto &slot = frame.get().slots.at(op.slot);
    if (slot.source.kind == storage::Location::Value)
      staged[slot.id] = slot.source.id;
    else {
      auto i = instruction(slot.mode == core::CaptureMode::Move ? I::Move : I::Copy, slot.type);
      i.slot = slot.source.id;
      staged[slot.id] = emit(i, true);
    }
  }
  auto create = instruction(I::Create, result.frameType);
  auto temporary = emit(create, true);
  auto bind = instruction(I::Bind, result.frameType);
  bind.slot = result.storage;
  bind.args = {temporary};
  emit(bind);
  uint32_t initialized = 0;
  for (const auto &op : frame.get().construct)
    if (op.kind == Operation::Initialize) {
      if (options.constructionExit && initialized == *options.constructionExit)
        break;
      const auto &slot = frame.get().slots.at(op.slot);
      auto i =
          instruction(slot.mode == core::CaptureMode::Move ? I::FieldStore : I::Write, slot.type);
      i.slot = result.storage;
      i.fieldIndex = fieldForSlot[slot.id];
      i.args = {staged[slot.id]};
      if (slot.mode == core::CaptureMode::Copy) {
        i.placeProjection = I::PlaceProjection::Field;
        i.offset = p.semantics.types[result.frameType].fields[i.fieldIndex].offset;
      }
      emit(i);
      ++initialized;
    }
  bool sealedHeap = options.heap && initialized == count;
  if (sealedHeap) {
    auto move = instruction(I::Move, result.frameType);
    move.slot = result.storage;
    auto value = emit(move, true);
    auto call = instruction(I::Call, result.heapType);
    call.function = result.factory;
    call.args = {value};
    auto heap = emit(call, true);
    auto bindHeap = instruction(I::Bind, result.heapType);
    bindHeap.slot = result.heapStorage;
    bindHeap.args = {heap};
    emit(bindHeap);
    const auto selector = selectorSlot ? std::optional{staged.at(*selectorSlot)} : std::nullopt;
    auto lifecycle = detail::lowerHeapLifecycle(result, frame, witness.origin, signature.result,
                                                options.constructionExit.has_value(), selector);
    code.insert(code.end(), lifecycle.begin(), lifecycle.end());
  }
  if (options.constructionExit && !sealedHeap) {
    const auto &edges = frame.get().cleanup;
    const auto &edge = *std::find_if(edges.begin(), edges.end(),
                                     [&](const auto &e) { return e.prefix == initialized; });
    for (const auto &op : edge.body) {
      if (op.kind == Operation::Finish)
        continue;
      const auto &slot = frame.get().slots.at(op.slot);
      if (slot.mode == core::CaptureMode::Copy)
        continue;
      auto i = instruction(op.kind == Operation::DropSlot ? I::FieldDrop : I::Discard, slot.type);
      if (op.kind == Operation::DropSlot) {
        i.slot = result.storage;
        i.fieldIndex = fieldForSlot[slot.id];
      } else
        i.args = {staged[slot.id]};
      emit(i);
    }
  } else if (!options.constructionExit) {

    auto restore = [&](const Slot &slot) {
      auto i =
          instruction(slot.mode == core::CaptureMode::Move ? I::FieldTake : I::Read, slot.type);
      i.slot = result.storage;
      i.fieldIndex = fieldForSlot[slot.id];
      if (slot.mode == core::CaptureMode::Copy) {
        i.placeProjection = I::PlaceProjection::Field;
        i.offset = p.semantics.types[result.frameType].fields[i.fieldIndex].offset;
      }
      auto value = emit(i, true);
      if (slot.source.kind == storage::Location::Value)
        result.restoredValues.emplace(slot.source.id, value);
      else {
        auto b = instruction(I::Bind, slot.type);
        b.slot = result.restoredSymbols.at(slot.source.id);
        b.args = {value};
        emit(b);
      }
    };
    for (const auto &slot : frame.get().slots)
      if (slot.mode == core::CaptureMode::Copy)
        restore(slot);
    for (const auto &slot : frame.get().slots)
      if (slot.mode == core::CaptureMode::Move)
        restore(slot);
  }
  if (!options.constructionExit || !sealedHeap) {
    auto drop = instruction(I::Drop, result.frameType);
    drop.slot = result.storage;
    emit(drop);
  }
  if (options.constructionExit) {
    ValueId value = None;
    if (signature.result == I32) {
      auto zero = instruction(I::Constant, I32);
      zero.text = "0";
      value = emit(zero, true);
    }
    auto ret = instruction(I::Return, signature.result);
    if (value != None)
      ret.args = {value};
    emit(ret);
  }
  auto function = std::find_if(p.functions.begin(), p.functions.end(),
                               [&](const auto &f) { return f.id == witness.function; });
  std::vector<I> suffix;
  if (!options.constructionExit) {
    suffix.assign(function->body.begin() + position, function->body.end());
    remap(suffix, result.restoredSymbols, result.restoredValues);
  }
  if (options.outline) {
    detail::outlineContinuation(result, witness.function, witness.origin, code, std::move(suffix),
                                options.destroyWhen.has_value());
    suffix.clear();

    function = std::find_if(p.functions.begin(), p.functions.end(),
                            [&](const auto &f) { return f.id == witness.function; });
  }
  function->body.erase(function->body.begin() + position, function->body.end());
  function->body.insert(function->body.end(), code.begin(), code.end());
  function->body.insert(function->body.end(), suffix.begin(), suffix.end());
  if (sealedHeap)
    propagateCallEffects(p);
  normalizeValues(result);
  if (options.stepBefore) {
    auto targets = options.followingSteps;
    targets.insert(targets.begin(), *options.stepBefore);
    for (auto target : targets) {
      detail::splitContinuation(result, target, witness.origin, options.projectFields);
      normalizeValues(result);
    }
  }
  if (options.stateful) {

    function = std::find_if(p.functions.begin(), p.functions.end(),
                            [&](const auto &f) { return f.id == witness.function; });
    auto prefix = std::move(function->body);
    detail::materializeProgress(result, witness.function, witness.origin, prefix, options);
    function = std::find_if(p.functions.begin(), p.functions.end(),
                            [&](const auto &f) { return f.id == witness.function; });
    function->body = std::move(prefix);
    propagateCallEffects(p);
    normalizeValues(result);
  }
  return result;
}
core::Verified roundTrip(const core::Verified &source, const core::CaptureState &state,
                         Program proposal, RoundTripOptions options,
                         core::VerificationOptions verification) {
  return core::verify(lowerRoundTrip(source, state, std::move(proposal), options).program,
                      verification);
}
core::Verified roundTripEntry(const core::Verified &source, RoundTripOptions options,
                              core::VerificationOptions verification,
                              std::string_view destroyWhenName, std::string_view stepBeforeName) {
  if (!stepBeforeName.empty()) {
    if (options.stepBefore || !options.followingSteps.empty())
      unsupported({}, "frame step cannot use both names and semantic identities");
    while (!stepBeforeName.empty()) {
      const auto end = stepBeforeName.find(',');
      auto name = stepBeforeName.substr(0, end);
      if (name.empty())
        unsupported({}, "empty frame step function name");
      FunctionId target = None;
      for (const auto &f : source.get().semantics.functions)
        if (f.name == name) {
          if (target != None)
            unsupported({}, "ambiguous frame step function name");
          target = f.id;
        }
      if (target == None)
        unsupported({}, "unknown frame step function name");
      if (!options.stepBefore)
        options.stepBefore = target;
      else
        options.followingSteps.push_back(target);
      if (end == std::string_view::npos)
        break;
      stepBeforeName.remove_prefix(end + 1);
      if (stepBeforeName.empty())
        unsupported({}, "empty frame step function name");
    }
  }
  if (options.stepBefore && !options.stateful)
    unsupported({}, "frame step requires persistent frame state storage");
  if (!destroyWhenName.empty() && options.destroyWhen)
    unsupported({}, "frame dispatch selector cannot use both a name and a semantic identity");
  if ((!destroyWhenName.empty() || options.destroyWhen) &&
      (!options.heap || options.constructionExit))
    unsupported({}, "frame dispatch requires --frame-heap and excludes construction-prefix exits");
  if (options.outline && (!options.heap || options.constructionExit))
    unsupported(
        {}, "continuation extraction requires --frame-heap and excludes construction-prefix exits");
  if (options.stateExit && !destroyWhenName.empty())
    unsupported({}, "frame state exit excludes runtime destroy selection");
  if (options.heap)
    (void)detail::boxDeclaration(source.get().semantics, {});
  const auto entry = source.get().semantics.entryPoint.function;
  std::optional<size_t> stepPosition;
  stepPosition = detail::stepOrder(source.get(), entry, options, {});
  const core::CaptureState *selected = nullptr;
  std::optional<Program> proposal;
  auto selectedOptions = options;
  std::tuple<size_t, size_t, size_t> best{0, 0, 0};
  for (const auto &state : source.get().captureStates)
    if (state.function == entry && state.side == core::StateSide::Before) {
      try {
        auto frame = build(source, state);
        progressOptions(source, state, options);
        const auto position = boundary(source, state, frame, options.heap);
        if (stepPosition && position > *stepPosition)
          continue;
        auto candidateOptions = options;
        if (!destroyWhenName.empty()) {
          for (const auto &slot : frame.get().slots)
            if (slot.source.kind == storage::Location::Symbol &&
                source.get().semantics.symbols.at(slot.source.id).name == destroyWhenName) {
              if (candidateOptions.destroyWhen)
                unsupported(state.origin,
                            "ambiguous frame dispatch selector name at this boundary");
              candidateOptions.destroyWhen = slot.source.id;
            }
          if (!candidateOptions.destroyWhen)
            continue;
        }
        (void)dispatchSource(source, state, frame, candidateOptions);
        if (frame.get().slots.empty() ||
            (options.constructionExit && *options.constructionExit > frame.get().slots.size()))
          continue;
        size_t owned = 0, bound = 0;
        for (const auto &slot : frame.get().slots)
          if (slot.mode == core::CaptureMode::Move) {
            ++owned;
            bound += slot.source.kind == storage::Location::Symbol;
          }
        auto score = std::tuple{owned, bound, frame.get().slots.size()};
        if (!selected || score > best) {
          selected = &state;
          proposal = frame.get();
          best = score;
          selectedOptions = candidateOptions;
        }
      } catch (const Error &error) {
        if (error.diagnostic.code == DiagnosticCode::InvalidCore)
          throw;
      }
    }
  if (!selected)
    unsupported(
        {}, !destroyWhenName.empty() || options.destroyWhen
                ? "no supported entrypoint boundary with the requested live bool dispatch selector"
                : "no supported nonempty entrypoint boundary for experimental frame handoff");
  return roundTrip(source, *selected, std::move(*proposal), selectedOptions, verification);
}
}
