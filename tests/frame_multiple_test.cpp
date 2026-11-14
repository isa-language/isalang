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
    require(!text.empty(), "missing multiple-frame fixture");
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
  auto options = [&](const core::Program &p, unsigned count) {
    frames::RoundTripOptions o;
    o.heap = true;
    o.outline = true;
    o.stateful = true;
    o.stepBefore = id(p, "first");
    for (const char *name : {"second", "third", "fourth"})
      if (o.followingSteps.size() + 1 < count)
        o.followingSteps.push_back(id(p, name));
    return o;
  };
  if (argc == 6) {
    const std::string mode = argv[2];
    auto source = load("frame_multiple/owned_result.isa");
    const auto &point = marker(source.get());
    auto out = frames::lowerRoundTrip(source, point, frames::build(source, point).get(),
                                      options(source.get(), 3));
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
    require(advances.size() == 4 && getter < body.size(), "missing two-stage caller handoff");
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
      limit.text = "5";
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
      body.resize(advances[2]);
      auto choice = op(I::Call, Bool);
      choice.function = id(out.program, "choose_condition");
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
      completed.text = "2";
      completed.result = out.program.values++;
      auto test = op(I::Binary, Bool);
      test.text = "==";
      test.args = {tag.result, completed.result};
      test.operatorClass = hir::Expr::OperatorClass::Comparison;
      test.result = out.program.values++;
      auto branch = op(I::If);
      branch.args = {test.result};
      call(s.drop, false);
      fallback();
      branch.body = std::move(extra);
      extra.clear();
      call(s.advance, true);
      ret(call(s.take, false));
      branch.otherwise = std::move(extra);
      body.insert(body.end(), {tag, completed, test, branch});
    } else if (mode != "normal") {
      const bool taking = mode == "take:2";
      unsigned phase = taking               ? 2
                       : mode == "drop:2"   ? 2
                       : mode == "cancel:1" ? 1
                       : mode == "cancel:2" ? 2
                       : mode == "cancel:3" ? 3
                                            : 0;
      require(phase != 0, "unknown multiple-frame fixture mode");
      body.resize(advances.at(phase));
      if (taking)
        ret(call(s.take, false));
      else {
        if (mode != "drop:2")
          call(s.cancel, true);
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

  unsigned rejected = 0, plans = 0;
  for (const char *path : {"frame_multiple/owned.isa", "frame_multiple/owned_result.isa"}) {
    auto source = load(path);
    const auto &point = marker(source.get());
    for (unsigned count = 1; count <= (std::string(path).ends_with("owned_result.isa") ? 4u : 3u);
         ++count) {
      auto out = frames::lowerRoundTrip(source, point, frames::build(source, point).get(),
                                        options(source.get(), count));
      ++plans;
      require(out.steps.size() == count && out.progress && out.step &&
                  out.continuation == out.steps[0].segment,
              "lost ordered frame transitions");
      auto result = source.get().semantics.functions.at(point.function).result;
      require(out.program.semantics.types.at(out.progress->type).variants.size() == count + 3,
              "wrong state variants");
      std::set<TypeId> heaps{out.heapType};
      std::set<SymbolId> parameters;
      for (unsigned n = 0; n < count; ++n) {
        const auto &step = out.steps[n];
        require(heaps.insert(step.heapType).second, "two stages reused one frame identity");
        const auto &segment = out.program.semantics.functions.at(step.segment);
        const auto &bridge = out.program.semantics.functions.at(step.continuation);
        require(!segment.trusted && !bridge.trusted && segment.kind == FunctionKind::Normal &&
                    bridge.kind == FunctionKind::Normal && segment.result == step.outcomeType &&
                    bridge.result == result,
                "invalid segment/bridge contract");
        require(parameters.insert(segment.params[0]).second,
                "two segments shared parameter identity");
        require(out.program.semantics.types.at(step.outcomeType).variants[0].payload ==
                    std::vector{step.heapType},
                "outcome transfers wrong frame");
        require(out.program.semantics.types.at(out.progress->type).variants[n + 3].payload ==
                    std::vector{step.heapType},
                "checkpoint stores wrong frame");
        auto reject = [&](auto mutate) {
          auto bad = out.program;
          mutate(bad);
          try {
            (void)core::verify(std::move(bad));
            throw std::runtime_error("invalid multiple-frame Core accepted");
          } catch (const Error &) {
            ++rejected;
          }
        };
        reject([&](auto &p) {
          p.semantics.types.at(out.progress->type).variants[n + 3].payload = {out.heapType};
        });
        reject([&](auto &p) {
          p.semantics.symbols.at(p.semantics.functions.at(step.segment).params[0]).type =
              step.heapType;
        });
        reject([&](auto &p) {
          auto &b = function(p, step.segment).body;
          auto twice = b.front();
          twice.result = p.values++;
          b.insert(b.begin() + 1, twice);
        });
        reject([&](auto &p) {
          p.semantics.types.at(step.outcomeType).variants[0].payload = {out.heapType};
        });
      }
      (void)erase(core::verify(out.program, {true}));
    }
    auto gate = [&](auto o) {
      try {
        (void)frames::lowerRoundTrip(source, point, frames::build(source, point).get(), o);
        throw std::runtime_error("invalid boundary order accepted");
      } catch (const Error &e) {
        require(e.code == "E8006", "wrong multiple-frame gate diagnostic");
      }
    };
    auto o = options(source.get(), 3);
    o.followingSteps[0] = *o.stepBefore;
    gate(o);
    o = options(source.get(), 3);
    std::swap(o.followingSteps[0], o.followingSteps[1]);
    gate(o);
    o = options(source.get(), 3);
    o.stepBefore.reset();
    gate(o);
    o = options(source.get(), 3);
    o.followingSteps[1] = None;
    gate(o);
    o = options(source.get(), 3);
    o.checkpointIndex = 4;
    gate(o);
  }
  require(plans == 7 && rejected == 64, "missing general-arity corruption coverage");
  for (const char *path :
       {"frame_multiple/blocked_late_borrow.isa", "frame_multiple/blocked_late_partial.isa"}) {
    auto source = load(path);
    const auto &point = marker(source.get());
    try {
      (void)frames::lowerRoundTrip(source, point, frames::build(source, point).get(),
                                   options(source.get(), 2));
      throw std::runtime_error("unsupported later capture was accepted");
    } catch (const Error &e) {
      require(e.code == "E8006", "wrong later-capture diagnostic");
    }
  }
}
