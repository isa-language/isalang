#include "frame_heap.hpp"
#include "frame_core_builder.hpp"
#include <algorithm>

namespace isac::frames::detail {
namespace {
using I = core::Inst;
constexpr VariantId Ready = 0, Completed = 1, Canceled = 2, Checkpoint = 3;
using Builder = CoreBuilder;
}

ProgressStorage defineProgressStorage(core::Program &p, TypeId result,
                                      const std::vector<TypeId> &initialTypes, FunctionId initial,
                                      const std::vector<ProgressStep> &steps, Loc loc) {
  auto panic = std::find_if(p.semantics.functions.begin(), p.semantics.functions.end(),
                            [](const auto &f) { return f.intrinsicId == IntrinsicId::Panic_str; });
  if (panic == p.semantics.functions.end())
    fail(loc, DiagnosticCode::FutureFeature,
         "frame state result access requires the resolved std panic binding");
  const auto panicId = panic->id;
  hir::Type state{};
  state.kind = hir::Type::Enum;
  state.name = "$frame.state." + std::to_string(initial);
  state.variants = {
      {Ready, "Ready", initialTypes},
      {Completed, "Completed", result == Unit ? std::vector<TypeId>{} : std::vector{result}},
      {Canceled, "Canceled", {}}};
  for (size_t n = 0; n < steps.size(); ++n)
    state.variants.push_back({static_cast<VariantId>(Checkpoint + n),
                              n == 0 ? "Checkpoint" : "Checkpoint" + std::to_string(n + 1),
                              {steps[n].heapType}});
  if (result != Unit && p.semantics.storageWidth(result) >= 65536)
    fail(loc, DiagnosticCode::FutureFeature, "frame state exceeds the existing enum layout limit");
  uint32_t initialWidth = 1;
  for (auto type : initialTypes) {
    const auto width = p.semantics.storageWidth(type);
    if (width >= 65536 || initialWidth > 65536 - width)
      fail(loc, DiagnosticCode::FutureFeature,
           "frame inputs exceed the existing enum layout limit");
    initialWidth += width;
  }
  state.width = std::max({steps.empty() ? 1u : 2u, initialWidth,
                          result == Unit ? 1u : 1 + p.semantics.storageWidth(result)});
  ProgressStorage info;
  info.type = static_cast<TypeId>(p.semantics.types.size());
  p.semantics.types.push_back(std::move(state));
  auto symbol = [&](TypeId type, bool parameter) {
    hir::Symbol s{};
    s.id = static_cast<SymbolId>(p.semantics.symbols.size());
    s.name = parameter ? "$state.input" : "$frame.progress";
    s.type = type;
    s.loc = loc;
    s.parameter = parameter;
    auto id = s.id;
    p.semantics.symbols.push_back(std::move(s));
    return id;
  };
  enum Role { Create, Advance, Cancel, Drop, Take };
  auto define = [&](Role role) {
    auto inputs = role == Create ? initialTypes : std::vector<TypeId>{info.type};
    auto output = role == Drop ? Unit : role == Take ? result : info.type;
    hir::Function f{};
    f.id = static_cast<FunctionId>(p.semantics.functions.size());
    static const char *roles[] = {"new", "advance", "cancel", "drop", "take"};
    f.name = "__frame_state_" + std::string(roles[role]) + "_" + std::to_string(f.id);
    f.result = output;
    for (auto input : inputs)
      f.params.push_back(symbol(input, true));
    f.loc = loc;
    if (role == Advance) {
      f.effects = p.semantics.functions.at(initial).effects;
      for (const auto &step : steps) {
        f.effects.merge(p.semantics.functions.at(step.segment).effects);
        f.effects.merge(p.semantics.functions.at(step.continuation).effects);
      }
    }
    if (role == Take)
      f.effects = p.semantics.functions.at(panicId).effects;
    const auto id = f.id;
    const auto parameters = f.params;
    const auto parameter = parameters.empty() ? None : parameters[0];
    p.semantics.functions.push_back(std::move(f));
    core::Function function{id, {}, {}, {}};
    for (auto unused : parameters) {
      (void)unused;
      function.parameters.push_back(p.values++);
    }
    Builder code{p, loc, {}};
    if (role == Create) {
      std::vector<ValueId> arguments;
      for (auto input : parameters)
        arguments.push_back(code.transfer(input));
      code.ret(info.type, code.construct(info.type, Ready, std::move(arguments)));
    } else if (role == Cancel || role == Drop) {
      auto discard = code.operation(I::Discard, info.type);
      discard.args = {code.move(parameter)};
      code.emit(discard);
      if (role == Cancel)
        code.ret(info.type, code.construct(info.type, Canceled));
      else
        code.ret(Unit);
    } else if (role == Advance) {

      Builder terminal{p, loc, {}};
      terminal.ret(info.type, terminal.move(parameter));
      auto tail = std::move(terminal.body);
      for (size_t stage = steps.size() + 1; stage-- > 0;) {
        Builder dispatch{p, loc, {}}, yes{p, loc, {}};
        const auto variant = stage == 0 ? Ready : static_cast<VariantId>(Checkpoint + stage - 1);
        const auto types =
            stage == 0 ? initialTypes : std::vector<TypeId>{steps[stage - 1].heapType};
        auto condition = dispatch.test(parameter, variant);
        auto arguments = yes.payloads(parameter, variant, types);
        const auto callee = stage == 0             ? initial
                            : stage < steps.size() ? steps[stage].segment
                                                   : steps.back().continuation;
        auto value = yes.call(callee, std::move(arguments));
        if (stage < steps.size()) {
          const auto &step = steps[stage];
          auto outcome = symbol(step.outcomeType, false);
          yes.bind(outcome, value);
          auto continues = yes.test(outcome, SegmentContinue);
          Builder onward{p, loc, {}}, done{p, loc, {}};
          auto nextFrame = onward.payload(outcome, SegmentContinue, step.heapType);
          onward.ret(
              info.type,
              onward.construct(info.type, static_cast<VariantId>(Checkpoint + stage), {nextFrame}));
          ValueId resultValue = None;
          if (result != Unit)
            resultValue = done.payload(outcome, SegmentComplete, result);
          else {
            auto drop = done.operation(I::Drop, step.outcomeType);
            drop.slot = outcome;
            done.emit(drop);
          }
          done.ret(info.type, done.construct(info.type, Completed,
                                             result == Unit ? std::vector<ValueId>{}
                                                            : std::vector{resultValue}));
          auto delivery = yes.operation(I::If);
          delivery.args = {continues};
          delivery.body = std::move(onward.body);
          delivery.otherwise = std::move(done.body);
          yes.emit(delivery);
        } else
          yes.ret(info.type,
                  yes.construct(info.type, Completed,
                                result == Unit ? std::vector<ValueId>{} : std::vector{value}));
        auto branch = dispatch.operation(I::If);
        branch.args = {condition};
        branch.body = std::move(yes.body);
        branch.otherwise = std::move(tail);
        dispatch.emit(branch);
        tail = std::move(dispatch.body);
      }
      code.body = std::move(tail);
    } else {
      auto condition = code.test(parameter, Completed);
      Builder yes{p, loc, {}}, no{p, loc, {}};
      ValueId value = None;
      if (result != Unit)
        value = yes.payload(parameter, Completed, result);
      else {
        auto drop = yes.operation(I::Drop, info.type);
        drop.slot = parameter;
        yes.emit(drop);
      }
      yes.ret(result, value);
      auto discard = no.operation(I::Discard, info.type);
      discard.args = {no.move(parameter)};
      no.emit(discard);
      auto message = no.operation(I::StaticStr, Str);
      message.text = "frame result is not completed";
      auto text = no.emit(message, true);
      Builder failure{p, loc, {}};
      failure.call(panicId, {text});

      auto loop = no.operation(I::Loop);
      loop.loop = 0;
      loop.unconditional = true;
      loop.body = std::move(failure.body);
      no.emit(loop);
      auto branch = code.operation(I::If);
      branch.args = {condition};
      branch.body = std::move(yes.body);
      branch.otherwise = std::move(no.body);
      code.emit(branch);
    }
    function.body = std::move(code.body);
    p.functions.push_back(std::move(function));
    return id;
  };
  info.create = define(Create);
  info.advance = define(Advance);
  info.cancel = define(Cancel);
  info.drop = define(Drop);
  info.take = define(Take);
  info.storage = symbol(info.type, false);
  return info;
}

void materializeProgress(LoweredRoundTrip &lowered, FunctionId source, Loc loc,
                         std::vector<I> &prefix, RoundTripOptions options) {
  auto &p = lowered.program;
  const auto result = p.semantics.functions.at(source).result;
  if (lowered.continuation == None ||
      (result != Unit &&
       (!p.semantics.containerElement(result) || p.semantics.containsProtocolFields(result))))
    fail(
        loc, DiagnosticCode::FutureFeature,
        "persistent frame state requires an outlined continuation with a reference-free, protocol-free result");
  const auto first = lowered.steps.empty() ? lowered.continuation : lowered.steps.front().segment;
  const auto info = defineProgressStorage(p, result, {lowered.heapType}, first, lowered.steps, loc);
  size_t cut = prefix.size();
  for (size_t n = 0; n < prefix.size(); ++n)
    if (prefix[n].kind == I::Bind && prefix[n].slot == lowered.heapStorage) {
      cut = n + 1;
      break;
    }
  if (cut == prefix.size())
    fail(loc, DiagnosticCode::InvalidCore, "missing owned heap frame for state storage");
  ValueId selector = options.destroyWhen ? prefix.at(cut).args.at(0) : None;
  prefix.resize(cut);
  Builder code{p, loc, {}};
  code.bind(info.storage, code.call(info.create, {code.move(lowered.heapStorage)}));
  auto path = [&](bool canceled, unsigned advances) {
    Builder branch{p, loc, {}};
    for (unsigned n = 0; n < advances; ++n)
      branch.bind(info.storage, branch.call(info.advance, {branch.move(info.storage)}));
    if (canceled) {
      branch.bind(info.storage, branch.call(info.cancel, {branch.move(info.storage)}));
      branch.call(info.drop, {branch.move(info.storage)});
      ValueId zero = None;
      if (result == I32) {
        auto i = branch.operation(I::Constant, I32);
        i.text = "0";
        zero = branch.emit(i, true);
      }
      branch.ret(result, zero);
    } else
      branch.ret(result, branch.call(info.take, {branch.move(info.storage)}));
    return branch.body;
  };
  if (selector != None) {
    auto branch = code.operation(I::If);
    branch.args = {selector};
    branch.body = path(true, 0);
    branch.otherwise = path(false, static_cast<unsigned>(lowered.steps.size() + 1));
    code.emit(branch);
  } else {
    auto advances = options.stateExit == RoundTripOptions::StateExit::Ready ? 0u
                    : options.stateExit == RoundTripOptions::StateExit::Checkpoint
                        ? options.checkpointIndex
                        : static_cast<unsigned>(lowered.steps.size() + 1);
    auto body = path(options.stateExit.has_value(), advances);
    code.body.insert(code.body.end(), body.begin(), body.end());
  }
  prefix.insert(prefix.end(), code.body.begin(), code.body.end());
  lowered.progress = info;
}
}
