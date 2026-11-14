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
    o.projectFields = true;
    for (const char *name : {"second", "third", "fourth"})
      if (o.followingSteps.size() + 1 < count)
        o.followingSteps.push_back(id(p, name));
    return o;
  };
  if (argc == 6) {
    const std::string mode = argv[2];
    auto source = load("frame_projection/owned_result.isa");
    const auto &point = marker(source.get());
    auto out = frames::lowerRoundTrip(source, point, frames::build(source, point).get(),
                                      options(source.get(), 2));
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
    require(advances.size() == 3 && getter < body.size(), "missing two-stage caller handoff");
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
      const bool taking = mode == "take:1" || mode == "take:2";
      unsigned phase = mode == "take:1"     ? 1
                       : taking             ? 2
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

  unsigned rejected = 0, preparedCount = 0;
  for (const char *path : {"frame_projection/direct.isa", "frame_projection/nested_complete.isa",
                           "frame_projection/ssa.isa", "frame_projection/owned_result.isa"}) {
    auto source = load(path);
    const auto target = id(source.get(), "first");
    const core::CaptureState *point = nullptr;
    size_t originalIndex = 0;
    for (const auto &f : source.get().functions)
      for (size_t n = 0; n < f.body.size(); ++n)
        if (f.body[n].kind == I::Call && f.body[n].function == target)
          for (const auto &state : source.get().captureStates)
            if (state.function == f.id && state.side == core::StateSide::Before &&
                state.point.block == f.body[n].semanticPoint) {
              point = &state;
              originalIndex = n;
            }
    require(point != nullptr, "missing projection witness");
    auto projected = frames::projectCapture(source, *point);
    require(projected.has_value(), "partial source was not normalized");
    ++preparedCount;
    const auto &after = projected->witness();
    (void)frames::build(projected->source, after);
    require(!frames::projectCapture(projected->source, after),
            "normalized capture still contains partial owners");
    auto other = source;
    try {
      (void)frames::projectCapture(other, *point);
      throw std::runtime_error("foreign witness accepted");
    } catch (const Error &e) {
      require(e.code == "E9001", "wrong foreign witness diagnostic");
    }
    const auto &p = projected->source.get();
    std::map<SymbolId, storage::Location> origins;
    const frames::ProjectionTransfer *leaf = nullptr;
    for (const auto &transfer : projected->transfers) {
      require(origins.emplace(transfer.staging, transfer.source).second,
              "duplicated carrier identity");
      if (!transfer.source.path.empty() && p.semantics.linear(transfer.type))
        leaf = &transfer;
    }
    require(leaf != nullptr && leaf->source.path.size() == 1, "missing semantic field projection");
    std::vector<storage::Location> expected, actual;
    for (const auto &drop : point->cleanup)
      if (source.get().semantics.linear(drop.type)) {
        const core::CaptureResource *r = nullptr;
        for (const auto &v : point->resources)
          if (v.location == drop.location)
            r = &v;
        require(r != nullptr, "missing original drop input");
        if (!r->absentFields.empty()) {
          const auto &shape = source.get().semantics.types.at(r->type);
          for (size_t n = shape.fields.size(); n-- > 0;)
            if (r->fields.contains(n)) {
              auto place = drop.location;
              place.path.push_back({hir::ReferenceProjectionKind::Field, static_cast<uint32_t>(n)});
              expected.push_back(place);
            }
        } else
          expected.push_back(drop.location);
      }
    for (const auto &drop : after.cleanup)
      if (p.semantics.linear(drop.type))
        actual.push_back(origins.at(drop.location.id));
    require(expected == actual, "source destruction order changed across projection staging");
    const auto root = leaf->source.id, field = leaf->source.path[0].index;
    const auto &preparedBody =
        std::find_if(p.functions.begin(), p.functions.end(), [&](const auto &f) {
          return f.id == point->function;
        })->body;
    size_t acquire = preparedBody.size(), store = preparedBody.size(),
           rootDrop = preparedBody.size();
    for (size_t n = originalIndex; n < preparedBody.size(); ++n) {
      const auto &i = preparedBody[n];
      if (n < projected->boundaryIndex && i.kind == I::FieldTake && i.slot == root &&
          i.fieldIndex == field)
        acquire = n;
      if (n < projected->boundaryIndex && i.kind == I::Drop && i.slot == root)
        rootDrop = n;
      if (n > projected->boundaryIndex && i.kind == I::FieldStore && i.slot == root &&
          i.fieldIndex == field && store == preparedBody.size())
        store = n;
    }
    require(acquire < projected->boundaryIndex && store < preparedBody.size() &&
                rootDrop < projected->boundaryIndex,
            "missing ordinary projection transfer operations");
    const auto &originalRoot =
        *std::find_if(point->resources.begin(), point->resources.end(), [&](const auto &r) {
          return r.location == storage::Location{storage::Location::Symbol, root, {}};
        });
    bool restoredPresence = false;
    for (const auto &state : p.captureStates)
      if (state.function == point->function && state.side == core::StateSide::Before)
        for (const auto &i : preparedBody)
          if (i.kind == I::Call && i.function == target && i.semanticPoint == state.point.block)
            for (const auto &r : state.resources)
              if (r.location == originalRoot.location)
                restoredPresence |=
                    r.absentFields == originalRoot.absentFields && r.fields == originalRoot.fields;
    require(restoredPresence, "restoration manufactured or lost field presence");
    auto reject = [&](auto mutate) {
      auto bad = p;
      ++bad.values;
      mutate(function(bad, point->function).body);
      try {
        (void)core::verify(std::move(bad));
        throw std::runtime_error("invalid projection Core accepted");
      } catch (const Error &) {
        ++rejected;
      }
    };
    reject([&](auto &b) { b[acquire].fieldIndex = *originalRoot.absentFields.begin(); });
    reject([&](auto &b) { b[store].type = I32; });
    reject([&](auto &b) {
      auto twice = b[acquire];
      twice.result = static_cast<ValueId>(p.values);
      b.insert(b.begin() + acquire + 1, twice);
    });
    reject([&](auto &b) { b.erase(b.begin() + rootDrop); });
    reject([&](auto &b) { b[projected->boundaryIndex].locals = {leaf->staging}; });
    reject([&](auto &b) { b[acquire].kind = I::Copy; });
  }
  require(preparedCount == 4 && rejected == 24, "missing projection corruption coverage");
}
