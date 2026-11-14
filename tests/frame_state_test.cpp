#include "isac/backend.hpp"
#include "isac/erasure.hpp"
#include "isac/frames.hpp"
#include "isac/modules.hpp"
#include <filesystem>
#include <fstream>
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
  const auto target = selectTarget({});
  auto load = [&](const char *path) {
    std::ifstream file(std::filesystem::path(argv[1]) / path);
    std::string text((std::istreambuf_iterator<char>(file)), {});
    require(!text.empty(), "missing frame-state fixture");
    return core::verify(core::elaborate(resolve(modules::compile(lex(text)).ast, target)), {true});
  };
  auto marker = [&](const core::Program &p) -> const core::CaptureState & {
    for (const auto &f : p.functions)
      for (const auto &i : f.body)
        if (i.kind == I::Call && p.semantics.functions.at(i.function).name == "marker")
          for (const auto &s : p.captureStates)
            if (s.function == f.id && s.point.block == i.semanticPoint &&
                s.side == core::StateSide::Before)
              return s;
    throw std::runtime_error("missing state marker");
  };
  auto function = [&](core::Program &p, FunctionId id) -> core::Function & {
    for (auto &f : p.functions)
      if (f.id == id)
        return f;
    throw std::runtime_error("missing generated state function");
  };
  frames::RoundTripOptions options{{}, true, {}, true, true, {}};
  if (argc == 6) {
    const std::string mode = argv[2];
    auto source = load(mode == "join" ? "frame_state/join.isa" : "frame_state/owned_result.isa");
    const auto &point = marker(source.get());
    auto out = frames::lowerRoundTrip(source, point, frames::build(source, point).get(), options);
    const auto resultType = out.program.semantics.functions.at(point.function).result;
    auto info = *out.progress;
    auto &body = function(out.program, point.function).body;
    const bool pending = mode == "cancel-ready" || mode == "advance-canceled" ||
                         mode == "take-ready" || mode == "take-canceled";
    size_t cut = body.size();
    auto anchor = pending ? info.advance : info.take;
    for (size_t n = 1; n < body.size(); ++n)
      if (body[n].kind == I::Call && body[n].function == anchor) {
        cut = n - 1;
        break;
      }
    require(cut < body.size() && body[cut].kind == I::Move, "missing state test anchor");
    auto instruction = [&](I::Kind kind, TypeId type) {
      I i{};
      i.kind = kind;
      i.type = type;
      i.loc = point.origin;
      i.quantity = out.program.semantics.linear(type) ? Quantity::One : Quantity::Omega;
      return i;
    };
    std::vector<I> extra;
    auto call = [&](FunctionId id, bool bind) {
      auto move = instruction(I::Move, info.type);
      move.slot = info.storage;
      move.result = out.program.values++;
      extra.push_back(move);
      auto invoke = instruction(I::Call, out.program.semantics.functions.at(id).result);
      invoke.function = id;
      invoke.args = {move.result};
      if (invoke.type != Unit)
        invoke.result = out.program.values++;
      extra.push_back(invoke);
      if (bind) {
        auto b = instruction(I::Bind, info.type);
        b.slot = info.storage;
        b.args = {invoke.result};
        extra.push_back(b);
      }
      return invoke.result;
    };
    if (mode == "join") {
      FunctionId choose = None, fallback = None;
      for (const auto &f : out.program.semantics.functions) {
        if (f.name == "choose")
          choose = f.id;
        if (f.name == "fallback")
          fallback = f.id;
      }
      require(choose != None && fallback != None, "missing join fixture functions");

      for (size_t n = 1; n < body.size(); ++n)
        if (body[n].kind == I::Call && body[n].function == info.advance) {
          cut = n - 1;
          break;
        }
      body.resize(cut);
      auto choice = instruction(I::Call, Bool);
      choice.function = choose;
      choice.result = out.program.values++;
      body.push_back(choice);
      auto fork = instruction(I::If, Unit);
      fork.args = {choice.result};
      call(info.advance, true);
      fork.body = std::move(extra);
      extra.clear();
      call(info.cancel, true);
      fork.otherwise = std::move(extra);
      extra.clear();
      body.push_back(fork);
      auto tag = instruction(I::Read, I64);
      tag.slot = info.storage;
      tag.result = out.program.values++;
      auto completed = instruction(I::Constant, I64);
      completed.text = "1";
      completed.result = out.program.values++;
      auto test = instruction(I::Binary, Bool);
      test.text = "==";
      test.args = {tag.result, completed.result};
      test.operatorClass = hir::Expr::OperatorClass::Comparison;
      test.result = out.program.values++;
      auto branch = instruction(I::If, Unit);
      branch.args = {test.result};
      auto value = call(info.take, false);
      auto ret = instruction(I::Return, resultType);
      ret.args = {value};
      extra.push_back(ret);
      branch.body = std::move(extra);
      extra.clear();
      call(info.drop, false);
      auto replacement = instruction(I::Call, resultType);
      replacement.function = fallback;
      replacement.result = out.program.values++;
      extra.push_back(replacement);
      ret.args = {replacement.result};
      extra.push_back(ret);
      branch.otherwise = std::move(extra);
      body.insert(body.end(), {tag, completed, test, branch});
    } else if (mode == "repeat-loop") {
      hir::Symbol count{};
      count.id = static_cast<SymbolId>(out.program.semantics.symbols.size());
      count.name = "$state.repeat";
      count.type = I32;
      count.loc = point.origin;
      out.program.semantics.symbols.push_back(count);
      auto fresh = [&](I i) {
        i.result = out.program.values++;
        return i;
      };
      auto zero = fresh(instruction(I::Constant, I32));
      zero.text = "0";
      auto bind = instruction(I::Bind, I32);
      bind.slot = count.id;
      bind.args = {zero.result};
      auto loop = instruction(I::Loop, Unit);
      loop.loop = 0;
      auto read = fresh(instruction(I::Copy, I32));
      read.slot = count.id;
      auto limit = fresh(instruction(I::Constant, I32));
      limit.text = "3";
      auto less = fresh(instruction(I::Binary, Bool));
      less.text = "<";
      less.operatorClass = hir::Expr::OperatorClass::Comparison;
      less.args = {read.result, limit.result};
      loop.condition = {read, limit, less};
      loop.args = {less.result};
      call(info.advance, true);
      loop.body = std::move(extra);
      read = fresh(instruction(I::Copy, I32));
      read.slot = count.id;
      auto one = fresh(instruction(I::Constant, I32));
      one.text = "1";
      auto add = fresh(instruction(I::Binary, I32));
      add.text = "+";
      add.operatorClass = hir::Expr::OperatorClass::Arithmetic;
      add.args = {read.result, one.result};
      auto write = instruction(I::Write, I32);
      write.slot = count.id;
      write.args = {add.result};
      loop.body.insert(loop.body.end(), {read, one, add, write});
      body.insert(body.begin() + cut, {zero, bind, loop});
    } else if (mode == "repeat") {
      call(info.advance, true);
      body.insert(body.begin() + cut, extra.begin(), extra.end());
    } else if (mode != "normal") {
      body.resize(cut);
      if (mode == "cancel-ready" || mode == "cancel-completed" || mode == "advance-canceled" ||
          mode == "take-canceled")
        call(info.cancel, true);
      if (mode == "advance-canceled")
        call(info.advance, true);
      if (mode == "take-ready" || mode == "take-canceled") {
        auto value = call(info.take, false);
        auto ret = instruction(I::Return, resultType);
        ret.args = {value};
        extra.push_back(ret);
      } else {
        require(mode == "cancel-ready" || mode == "cancel-completed" ||
                    mode == "advance-canceled" || mode == "drop-completed",
                "unknown state fixture mode");
        call(info.drop, false);
        FunctionId fallback = None;
        for (const auto &f : out.program.semantics.functions)
          if (f.name == "fallback")
            fallback = f.id;
        require(fallback != None, "missing replacement owned result");
        auto replacement = instruction(I::Call, resultType);
        replacement.function = fallback;
        replacement.result = out.program.values++;
        extra.push_back(replacement);
        auto ret = instruction(I::Return, resultType);
        ret.args = {replacement.result};
        extra.push_back(ret);
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
              merged |= merge.slot == info.storage;
      require(merged, "join regression did not exercise a verified linear merge");
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
  unsigned rejected = 0;
  for (const char *path : {"frame_heap/valid_owned.isa", "frame_heap/valid_branches.isa",
                           "frame_heap/valid_containers.isa", "frame_state/owned_result.isa"}) {
    auto source = load(path);
    const auto &point = marker(source.get());
    auto make = [&] {
      return frames::lowerRoundTrip(source, point, frames::build(source, point).get(), options);
    };
    auto out = make();
    require(out.progress.has_value(), "no persistent state storage");
    const auto info = *out.progress;
    const auto result = source.get().semantics.functions.at(point.function).result;
    const auto &state = out.program.semantics.types.at(info.type);
    require(state.kind == hir::Type::Enum && state.protocol == None && state.fields.empty() &&
                state.variants.size() == 3 &&
                state.variants[0].payload == std::vector{out.heapType} &&
                state.variants[1].payload ==
                    (result == Unit ? std::vector<TypeId>{} : std::vector{result}) &&
                state.variants[2].payload.empty() &&
                !out.program.semantics.containsReferences(info.type),
            "state representation bypasses ordinary enum ownership");
    for (auto id : {info.create, info.advance, info.cancel, info.drop, info.take}) {
      const auto &f = out.program.semantics.functions.at(id);
      require(!f.trusted && f.kind == FunctionKind::Normal && f.intrinsicId == IntrinsicId::None &&
                  f.owner == None && f.params.size() == 1 && f.parameterRegions.empty(),
              "privileged state helper");
    }
    auto checked = core::verify(out.program, {true});
    (void)erase(checked);
    auto &advance = function(out.program, info.advance).body.back();
    require(advance.kind == I::If && advance.otherwise.front().kind == I::Move &&
                advance.otherwise.back().kind == I::Return,
            "terminal state is not transferred intact");
    auto reject = [&](auto change) {
      auto changed = make();
      change(changed, *changed.progress);
      try {
        (void)core::verify(std::move(changed.program));
        throw std::runtime_error("invalid state Core accepted");
      } catch (const Error &) {
        ++rejected;
      }
    };
    reject([&](auto &p, auto s) {
      p.program.semantics.functions[s.advance].kind = FunctionKind::Async;
    });
    reject([&](auto &p, auto s) { p.program.semantics.types[s.type].variants[0].payload = {I32}; });
    reject([&](auto &p, auto s) {
      auto &b = function(p.program, s.create).body;
      for (auto &i : b)
        if (i.kind == I::EnumConstruct)
          i.args.clear();
    });
    reject([&](auto &p, auto s) {
      auto &b = function(p.program, s.create).body;
      auto twice = b.front();
      twice.result = p.program.values++;
      b.insert(b.begin() + 1, twice);
    });
    reject([&](auto &p, auto s) {
      auto &b = function(p.program, s.advance).body;
      for (auto &i : b)
        if (i.kind == I::Constant) {
          i.text = "1";
          return;
        }
    });
    reject([&](auto &p, auto s) {
      auto &b = function(p.program, s.advance).body.back().body;
      for (size_t n = 0; n < b.size(); ++n)
        if (b[n].kind == I::PayloadTake) {
          auto twice = b[n];
          twice.result = p.program.values++;
          b.insert(b.begin() + n + 1, twice);
          return;
        }
      throw std::runtime_error("missing ready payload");
    });
    reject([&](auto &p, auto s) {
      auto &b = function(p.program, s.cancel).body;
      b.front().kind = I::Copy;
    });
    reject([&](auto &p, auto s) {
      auto &b = function(p.program, s.cancel).body;
      b.insert(b.begin() + 2, b[1]);
    });
    reject([&](auto &p, auto s) {
      auto &b = function(p.program, s.cancel).body;
      b[1].type = p.heapType;
    });
    reject([&](auto &p, auto s) { function(p.program, s.cancel).body.front().slot = s.storage; });
    reject([&](auto &p, auto s) { p.program.semantics.functions[s.create].result = p.heapType; });
    reject([&](auto &p, auto s) {
      auto &b = function(p.program, s.advance).body.back().body;
      for (auto &i : b)
        if (i.kind == I::PayloadTake) {
          i.payloadPath = hir::payloadPosition(1, 0);
          return;
        }
    });
    bool transferred = false;
    for (const auto &s : checked.get().captureStates)
      if (s.function == info.advance)
        for (const auto &r : s.resources)
          transferred |=
              r.type == info.type && r.movedPayloads.contains(hir::payloadPosition(0, 0));
    require(transferred, "ready payload transfer not present in verified Core facts");
    std::vector<frames::RoundTripOptions> invalidOptions;
    auto invalid = options;
    invalid.outline = false;
    invalidOptions.push_back(invalid);
    invalid = options;
    invalid.heap = false;
    invalidOptions.push_back(invalid);
    invalid = options;
    invalid.constructionExit = 0;
    invalidOptions.push_back(invalid);
    invalid = options;
    invalid.stateful = false;
    invalid.stateExit = frames::RoundTripOptions::StateExit::Ready;
    invalidOptions.push_back(invalid);
    invalid = options;
    invalid.stateExit = static_cast<frames::RoundTripOptions::StateExit>(999);
    invalidOptions.push_back(invalid);
    invalid = options;
    invalid.stateExit = frames::RoundTripOptions::StateExit::Ready;
    invalid.destroyWhen = None;
    invalidOptions.push_back(invalid);
    if (result != Unit && result != I32) {
      invalid = options;
      invalid.stateExit = frames::RoundTripOptions::StateExit::Ready;
      invalidOptions.push_back(invalid);
    }
    for (auto flags : invalidOptions) {
      try {
        (void)frames::lowerRoundTrip(source, point, frames::build(source, point).get(), flags);
        throw std::runtime_error("unproved frame state option accepted");
      } catch (const Error &e) {
        require(e.code == "E8006", "wrong state option gate diagnostic");
      }
    }
  }
  require(rejected == 48, "missing malformed state coverage");
  auto empty = core::verify(
      core::elaborate(
          resolve(modules::compile(lex("@import \"std.box\"\nfn main(){print(7);}")).ast, target)),
      {true});
  bool tested = false;
  for (const auto &point : empty.get().captureStates)
    if (point.function == empty.get().semantics.entryPoint.function &&
        point.side == core::StateSide::Before && point.resources.empty() && point.cleanupValid) {
      auto out = frames::lowerRoundTrip(empty, point, frames::build(empty, point).get(), options);
      require(!out.progress && out.heapType == None, "empty capture invented state storage");
      (void)core::verify(std::move(out.program));
      auto exit = options;
      exit.stateExit = frames::RoundTripOptions::StateExit::Ready;
      try {
        (void)frames::lowerRoundTrip(empty, point, frames::build(empty, point).get(), exit);
        throw std::runtime_error("empty capture ignored explicit state exit");
      } catch (const Error &e) {
        require(e.code == "E8006", "wrong empty state exit diagnostic");
      }
      tested = true;
      break;
    }
  require(tested, "missing empty state case");
}
