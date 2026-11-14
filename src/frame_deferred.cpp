#include "frame_heap.hpp"
#include "isac/execution.hpp"
#include "frame_core_builder.hpp"
#include <functional>

namespace isac::frames {
DeferredFunction deferFunction(const core::Verified &source, FunctionId function,
                               const std::vector<FunctionId> &checkpoints, bool projectFields) {
  (void)execution::analyze(source, function, execution::Domain::LocalDeferred);
  const auto &signature = source.get().semantics.functions.at(function);

  RoundTripOptions options;
  if (!checkpoints.empty()) {
    options.stepBefore = checkpoints.front();
    options.followingSteps.assign(checkpoints.begin() + 1, checkpoints.end());
    (void)detail::stepOrder(source.get(), function, options, signature.loc);
  }
  std::vector<TypeId> inputs;
  for (auto id : signature.params)
    inputs.push_back(source.get().semantics.symbols.at(id).type);
  LoweredRoundTrip lowered{};
  lowered.program = source.get();
  lowered.continuation = function;
  for (size_t n = 0; n < checkpoints.size(); ++n)
    detail::splitContinuation(lowered, checkpoints[n], signature.loc, projectFields,
                              n == 0 ? function : None);
  const auto initial = lowered.steps.empty() ? function : lowered.steps.front().segment;
  const auto state = detail::defineProgressStorage(lowered.program, signature.result, inputs,
                                                   initial, lowered.steps, signature.loc);
  detail::normalizeFrameValues(lowered);

  auto verified = core::verify(std::move(lowered.program));
  return {std::move(verified), state, std::move(lowered.steps)};
}
core::Verified deferCalls(const core::Verified &source, FunctionId function,
                          const std::vector<FunctionId> &checkpoints,
                          core::VerificationOptions options) {
  auto task = deferFunction(source, function, checkpoints);
  LoweredRoundTrip lowered{};
  lowered.program = task.verified.get();
  auto &p = lowered.program;
  const auto entry = p.semantics.entryPoint.function;
  if (entry == function)
    fail({}, DiagnosticCode::FutureFeature, "deferred worker must be called by the entrypoint");
  auto main = std::find_if(p.functions.begin(), p.functions.end(),
                           [&](const auto &f) { return f.id == entry; });
  if (main == p.functions.end())
    fail({}, DiagnosticCode::FutureFeature, "deferred call requires an entrypoint");
  unsigned rewritten = 0;
  std::function<void(std::vector<core::Inst> &)> rewrite = [&](auto &body) {
    std::vector<core::Inst> output;
    for (auto &i : body) {
      rewrite(i.body);
      rewrite(i.otherwise);
      rewrite(i.condition);
      rewrite(i.latch);
      if (i.kind == core::Inst::Call && i.function == function) {
        hir::Symbol state{};
        state.id = static_cast<SymbolId>(p.semantics.symbols.size());
        state.name = "$deferred.task";
        state.type = task.state.type;
        state.loc = i.loc;
        p.semantics.symbols.push_back(state);
        detail::CoreBuilder code{p, i.loc, {}};
        code.bind(state.id, code.call(task.state.create, i.args));
        for (size_t n = 0; n <= checkpoints.size(); ++n)
          code.bind(state.id, code.call(task.state.advance, {code.move(state.id)}));
        const auto result = code.call(task.state.take, {code.move(state.id)});
        if (result != None)
          code.body.back().result = i.result;
        output.insert(output.end(), code.body.begin(), code.body.end());
        ++rewritten;
      } else
        output.push_back(std::move(i));
    }
    body = std::move(output);
  };
  rewrite(main->body);
  if (!rewritten)
    fail({}, DiagnosticCode::FutureFeature,
         "entrypoint has no calls to the selected deferred worker");
  detail::normalizeFrameValues(lowered);
  return core::verify(std::move(p), options);
}
}
