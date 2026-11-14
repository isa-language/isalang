#include "isac/backend.hpp"
#include "isac/erasure.hpp"
#include "isac/frames.hpp"
#include "isac/modules.hpp"
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>

int main(int argc, char **argv) {
  using namespace isac;
  using I = core::Inst;
  if (argc != 2 && argc != 6)
    return 1;
  auto require = [](bool ok, const char *message) {
    if (!ok)
      throw std::runtime_error(message);
  };
  auto target = selectTarget({});
  auto load = [&](const char *path) {
    std::ifstream file(std::filesystem::path(argv[1]) / path);
    std::string text((std::istreambuf_iterator<char>(file)), {});
    require(!text.empty(), "missing frame step fixture");
    return core::verify(core::elaborate(resolve(modules::compile(lex(text)).ast, target)), {true});
  };
  auto id = [&](const core::Program &p, const char *name) {
    for (const auto &f : p.semantics.functions)
      if (f.name == name)
        return f.id;
    throw std::runtime_error("missing step fixture function");
  };
  auto function = [&](core::Program &p, FunctionId id) -> core::Function & {
    for (auto &f : p.functions)
      if (f.id == id)
        return f;
    throw std::runtime_error("missing frame step function body");
  };
  auto marker = [&](const core::Program &p) -> const core::CaptureState & {
    for (const auto &f : p.functions)
      for (const auto &i : f.body)
        if (i.kind == I::Call && i.function == id(p, "marker"))
          for (const auto &s : p.captureStates)
            if (s.function == f.id && s.side == core::StateSide::Before &&
                s.point.block == i.semanticPoint)
              return s;
    throw std::runtime_error("missing checked marker");
  };
  auto options = [&](const core::Program &p) {
    frames::RoundTripOptions o;
    o.heap = true;
    o.outline = true;
    o.stateful = true;
    o.stepBefore = id(p, "checkpoint");
    return o;
  };
  if (argc == 6) {
    std::string mode = argv[2];
    const bool resultFixture = mode.starts_with("result-");
    if (resultFixture)
      mode.erase(0, std::string("result-").size());
    const bool completes = mode.starts_with("complete-");
    if (completes)
      mode.erase(0, std::string("complete-").size());
    auto source = load(resultFixture ? "frame_step/early_result.isa"
                       : completes   ? "frame_step/early_owned_result.isa"
                                     : "frame_step/owned_result.isa");
    const auto &point = marker(source.get());
    auto out = frames::lowerRoundTrip(source, point, frames::build(source, point).get(),
                                      options(source.get()));
    const auto s = *out.progress;
    const auto result = out.program.semantics.functions.at(point.function).result;
    auto &body = function(out.program, point.function).body;
    std::vector<size_t> advances;
    size_t getter = body.size();
    for (size_t n = 1; n < body.size(); ++n)
      if (body[n].kind == I::Call) {
        if (body[n].function == s.advance)
          advances.push_back(n - 1);
        if (body[n].function == s.take)
          getter = n - 1;
      }
    require(advances.size() == 2 && getter < body.size(), "missing two-stage caller handoff");
    auto op = [&](I::Kind kind, TypeId type = Unit) {
      I i{};
      i.kind = kind;
      i.type = type;
      i.loc = point.origin;
      i.quantity = out.program.semantics.linear(type) ? Quantity::One : Quantity::Omega;
      return i;
    };
    std::vector<I> extra;
    auto call = [&](FunctionId f, bool bind) {
      auto move = op(I::Move, s.type);
      move.slot = s.storage;
      move.result = out.program.values++;
      extra.push_back(move);
      auto invoke = op(I::Call, out.program.semantics.functions.at(f).result);
      invoke.function = f;
      invoke.args = {move.result};
      if (invoke.type != Unit)
        invoke.result = out.program.values++;
      extra.push_back(invoke);
      if (bind) {
        auto b = op(I::Bind, s.type);
        b.slot = s.storage;
        b.args = {invoke.result};
        extra.push_back(b);
      }
      return invoke.result;
    };
    auto ret = [&](ValueId value) {
      auto i = op(I::Return, result);
      i.args = {value};
      extra.push_back(i);
    };
    auto fallback = [&] {
      auto replacement = op(I::Call, result);
      replacement.function = id(out.program, "fallback");
      replacement.result = out.program.values++;
      extra.push_back(replacement);
      ret(replacement.result);
    };
    if (mode == "repeat") {
      call(s.advance, true);
      body.insert(body.begin() + getter, extra.begin(), extra.end());
    } else if (mode == "loop") {
      body.resize(advances[0]);
      hir::Symbol counter{};
      counter.id = static_cast<SymbolId>(out.program.semantics.symbols.size());
      counter.name = "$step.iteration";
      counter.type = I32;
      counter.loc = point.origin;
      out.program.semantics.symbols.push_back(counter);
      auto fresh = [&](I i) {
        i.result = out.program.values++;
        return i;
      };
      auto zero = fresh(op(I::Constant, I32));
      zero.text = "0";
      auto bind = op(I::Bind, I32);
      bind.slot = counter.id;
      bind.args = {zero.result};
      auto loop = op(I::Loop);
      loop.loop = 0;
      auto read = fresh(op(I::Copy, I32));
      read.slot = counter.id;
      auto limit = fresh(op(I::Constant, I32));
      limit.text = "3";
      auto less = fresh(op(I::Binary, Bool));
      less.text = "<";
      less.args = {read.result, limit.result};
      less.operatorClass = hir::Expr::OperatorClass::Comparison;
      loop.condition = {read, limit, less};
      loop.args = {less.result};
      call(s.advance, true);
      loop.body = std::move(extra);
      extra.clear();
      read = fresh(op(I::Copy, I32));
      read.slot = counter.id;
      auto one = fresh(op(I::Constant, I32));
      one.text = "1";
      auto add = fresh(op(I::Binary, I32));
      add.text = "+";
      add.args = {read.result, one.result};
      add.operatorClass = hir::Expr::OperatorClass::Arithmetic;
      auto write = op(I::Write, I32);
      write.slot = counter.id;
      write.args = {add.result};
      loop.body.insert(loop.body.end(), {read, one, add, write});
      body.insert(body.end(), {zero, bind, loop});
      ret(call(s.take, false));
      body.insert(body.end(), extra.begin(), extra.end());
    } else if (mode == "join") {
      body.resize(advances[1]);
      auto choice = op(I::Call, Bool);
      choice.function = id(out.program, "choose");
      choice.result = out.program.values++;
      auto fork = op(I::If);
      fork.args = {choice.result};
      call(s.advance, true);
      fork.body = std::move(extra);
      extra.clear();
      call(s.cancel, true);
      fork.otherwise = std::move(extra);
      extra.clear();
      body.insert(body.end(), {choice, fork});
      auto tag = op(I::Read, I64);
      tag.slot = s.storage;
      tag.result = out.program.values++;
      auto completed = op(I::Constant, I64);
      completed.text = "1";
      completed.result = out.program.values++;
      auto test = op(I::Binary, Bool);
      test.text = "==";
      test.args = {tag.result, completed.result};
      test.operatorClass = hir::Expr::OperatorClass::Comparison;
      test.result = out.program.values++;
      auto branch = op(I::If);
      branch.args = {test.result};
      ret(call(s.take, false));
      branch.body = std::move(extra);
      extra.clear();
      call(s.drop, false);
      fallback();
      branch.otherwise = std::move(extra);
      body.insert(body.end(), {tag, completed, test, branch});
    } else if (mode != "normal") {
      body.resize(advances[1]);
      if (mode == "take-checkpoint")
        ret(call(s.take, false));
      else {
        require(mode == "cancel-checkpoint" || mode == "drop-checkpoint" ||
                    mode == "advance-canceled",
                "unknown frame step fixture mode");
        if (mode != "drop-checkpoint")
          call(s.cancel, true);
        if (mode == "advance-canceled") {
          call(s.cancel, true);
          call(s.advance, true);
        }
        call(s.drop, false);
        fallback();
      }
      body.insert(body.end(), extra.begin(), extra.end());
    }
    auto checked = core::verify(std::move(out.program));
    if (mode == "join") {
      bool merged = false;
      for (const auto &f : checked.get().functions)
        if (f.id == point.function)
          for (const auto &i : f.body)
            for (const auto &merge : i.merges)
              merged |= merge.slot == s.storage;
      require(merged, "state checkpoint regression lacks a checked merge");
    }
    NativeOptions native;
    native.target = target;
    native.counters = true;
    native.optimization =
        std::string(argv[4]) == "O2" ? OptimizationLevel::O2 : OptimizationLevel::O0;
    native.sanitize = std::string(argv[5]) == "sanitized";
    auto ir = generate(runtime::lower(erase(checked)), BackendStage::LLVMIR, native.sanitize);
    compileNative(optimizeLLVM(ir, target, native.optimization), argv[3], native);
    return 0;
  }

  unsigned rejected = 0, gates = 0;
  for (const char *path :
       {"frame_step/owned.isa", "frame_step/containers.isa", "frame_step/local_regions.isa",
        "frame_step/owned_result.isa", "frame_step/scalar_result.isa", "frame_step/ssa.isa",
        "frame_step/early_owned_result.isa", "frame_step/early_unit.isa",
        "frame_step/early_scalar.isa", "frame_step/early_dead_return.isa",
        "frame_step/early_result.isa"}) {
    auto source = load(path);
    const auto &point = marker(source.get());
    auto make = [&] {
      return frames::lowerRoundTrip(source, point, frames::build(source, point).get(),
                                    options(source.get()));
    };
    auto out = make();
    require(out.step && out.progress, "missing internal owned checkpoint");
    const auto step = *out.step;
    const auto state = *out.progress;
    require(step.heapType != out.heapType && step.frameType != out.frameType &&
                step.continuation != out.continuation,
            "checkpoint reused the consumed input storage");
    require(out.program.semantics.functions.at(out.continuation).result == step.outcomeType &&
                out.program.semantics.functions.at(step.continuation).result ==
                    source.get().semantics.functions.at(point.function).result,
            "segment result contract does not match next-frame transfer");
    const auto result = source.get().semantics.functions.at(point.function).result;
    const auto &outcome = out.program.semantics.types.at(step.outcomeType);
    require(outcome.kind == hir::Type::Enum && outcome.variants.size() == 2 &&
                outcome.variants[0].payload == std::vector{step.heapType} &&
                outcome.variants[1].payload ==
                    (result == Unit ? std::vector<TypeId>{} : std::vector{result}) &&
                !out.program.semantics.containsReferences(step.outcomeType),
            "segment bypassed typed completion");
    const auto &type = out.program.semantics.types.at(state.type);
    require(type.kind == hir::Type::Enum && type.variants.size() == 4 &&
                type.variants[3].payload == std::vector{step.heapType} &&
                !out.program.semantics.containsReferences(state.type),
            "checkpoint bypassed ordinary enum ownership");
    for (auto f : {out.continuation, step.continuation, state.advance}) {
      const auto &signature = out.program.semantics.functions.at(f);
      require(!signature.trusted && signature.kind == FunctionKind::Normal &&
                  signature.parameterRegions.empty() && signature.payloadParameterRegions.empty(),
              "checkpoint granted unchecked region/capture privileges");
    }
    for (const auto &i : function(out.program, out.continuation).body)
      require(i.kind != I::Call ||
                  (i.function != step.continuation && i.function != id(out.program, "checkpoint")),
              "first segment executed the delayed continuation");
    auto checked = core::verify(out.program, {true});
    (void)erase(checked);
    if (std::string(path) == "frame_step/early_owned_result.isa") {
      bool partialCleanup = false;
      for (const auto &state : checked.get().captureStates)
        if (state.function == out.continuation && state.terminal)
          for (const auto &drop : state.completedCleanup)
            partialCleanup |= checked.get().semantics.types.at(drop.type).name == "Pair" &&
                              drop.absentFields.contains(0);
      require(partialCleanup, "early completion lost the remaining-field cleanup witness");
    }
    unsigned mutation = 0;
    auto reject = [&](auto corrupt) {
      ++mutation;
      auto bad = make();
      corrupt(bad);
      try {
        (void)core::verify(std::move(bad.program));
        throw std::runtime_error(std::string("invalid checkpoint Core accepted: ") + path +
                                 " mutation " + std::to_string(mutation));
      } catch (const Error &) {
        ++rejected;
      }
    };
    reject([&](auto &p) { p.program.semantics.functions[p.continuation].result = p.heapType; });
    reject([&](auto &p) {
      p.program.semantics.types[p.progress->type].variants[3].payload = {p.heapType};
    });
    reject([&](auto &p) {
      auto &body = function(p.program, p.continuation).body;
      auto duplicate = body.front();
      duplicate.result = p.program.values++;
      body.insert(body.begin() + 1, duplicate);
    });
    reject([&](auto &p) {
      auto &body = function(p.program, p.continuation).body;
      auto discard = body.back();
      discard.kind = I::Discard;
      discard.result = None;
      body.insert(body.end() - 1, discard);
    });
    reject([&](auto &p) {
      function(p.program, p.step->continuation).body.front().slot = p.heapStorage;
    });
    reject([&](auto &p) {
      p.program.semantics.functions[p.step->continuation].kind = FunctionKind::Async;
    });
    reject([&](auto &p) {
      auto &nested = function(p.program, p.progress->advance).body.back().otherwise;
      for (auto &i : nested)
        if (i.kind == I::Constant) {
          i.text = "1";
          return;
        }
      throw std::runtime_error("missing checkpoint variant test");
    });
    reject([&](auto &p) {
      auto &nested = function(p.program, p.progress->advance).body.back().otherwise.back().body;
      for (size_t n = 0; n < nested.size(); ++n)
        if (nested[n].kind == I::PayloadTake) {
          auto duplicate = nested[n];
          duplicate.result = p.program.values++;
          nested.insert(nested.begin() + n + 1, duplicate);
          return;
        }
      throw std::runtime_error("missing checkpoint payload take");
    });
    if (std::string(path) == "frame_step/early_owned_result.isa") {
      auto completion = [&](auto &p, auto change) {
        bool found = false;
        std::function<void(std::vector<I> &)> visit = [&](auto &body) {
          for (size_t n = 0; n < body.size() && !found; ++n) {
            auto &i = body[n];
            if (i.kind == I::EnumConstruct && i.type == p.step->outcomeType && i.offset == 1) {
              found = true;
              change(body, n);
              break;
            }
            visit(i.condition);
            visit(i.body);
            visit(i.otherwise);
            visit(i.latch);
          }
        };
        visit(function(p.program, p.continuation).body);
        require(found, "missing early completion proposal");
      };
      reject([&](auto &p) { completion(p, [&](auto &b, auto n) { b[n].args.clear(); }); });
      reject([&](auto &p) { completion(p, [&](auto &b, auto n) { b[n].type = I32; }); });
      reject([&](auto &p) { completion(p, [&](auto &b, auto n) { b[n].offset = 0; }); });
      reject([&](auto &p) {
        completion(p, [&](auto &b, auto n) {
          auto duplicate = b[n];
          duplicate.result = p.program.values++;
          b.insert(b.begin() + n + 1, duplicate);
        });
      });
    }
    auto gate = [&](frames::RoundTripOptions invalid) {
      try {
        (void)frames::lowerRoundTrip(source, point, frames::build(source, point).get(), invalid);
        throw std::runtime_error("invalid checkpoint option accepted");
      } catch (const Error &e) {
        require(e.code == "E8006", "incorrect checkpoint gate diagnostic");
        ++gates;
      }
    };
    auto invalid = options(source.get());
    invalid.stateful = false;
    gate(invalid);
    invalid = options(source.get());
    invalid.stepBefore = None;
    gate(invalid);
    invalid = options(source.get());
    invalid.stepBefore = id(source.get(), "main");
    gate(invalid);
    invalid = options(source.get());
    invalid.stepBefore.reset();
    invalid.stateExit = frames::RoundTripOptions::StateExit::Checkpoint;
    gate(invalid);
  }
  require(rejected == 92 && gates == 44, "incomplete checkpoint corruption/gate coverage");
  for (const char *path : {"frame_step/blocked_borrow.isa", "frame_step/blocked_partial.isa",
                           "frame_step/blocked_loop.isa", "frame_step/blocked_duplicate.isa",
                           "frame_step/blocked_protocol.isa"}) {
    auto source = load(path);
    const auto &point = marker(source.get());
    try {
      (void)frames::lowerRoundTrip(source, point, frames::build(source, point).get(),
                                   options(source.get()));
      throw std::runtime_error("unsupported checkpoint accepted");
    } catch (const Error &e) {
      const auto code = std::string(path) == "frame_step/blocked_protocol.isa" ? "E0410" : "E8006";
      require(e.code == code, "incorrect checkpoint boundary gate diagnostic");
    }
  }
}
