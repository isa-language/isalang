#include "isac/backend.hpp"
#include "isac/erasure.hpp"
#include "isac/frames.hpp"
#include "isac/modules.hpp"
#include "../src/frame_heap.hpp"
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>

int main(int argc, char **argv) {
  using namespace isac;
  using I = core::Inst;
  if (argc != 2 && argc != 7)
    return 1;
  auto require = [](bool yes, const char *message) {
    if (!yes)
      throw std::runtime_error(message);
  };
  auto target = selectTarget({});
  auto id = [](const core::Program &p, const char *name) {
    for (const auto &f : p.semantics.functions)
      if (f.name == name)
        return f.id;
    throw std::runtime_error("missing branch fixture function");
  };
  auto function = [](core::Program &p, FunctionId id) -> core::Function & {
    return *std::find_if(p.functions.begin(), p.functions.end(),
                         [&](const auto &f) { return f.id == id; });
  };
  std::function<void(std::vector<I> &, const std::function<void(I &)> &)> walk;
  walk = [&](auto &body, const auto &visit) {
    for (auto &i : body) {
      visit(i);
      walk(i.body, visit);
      walk(i.otherwise, visit);
      walk(i.condition, visit);
      walk(i.latch, visit);
    }
  };
  auto load = [&](const std::string &name, bool rebind = false) {
    std::ifstream input(std::filesystem::path(argv[1]) / "frame_branch" / (name + ".isa"));
    std::string source((std::istreambuf_iterator<char>(input)), {});
    require(!source.empty(), "missing branch fixture");
    auto p = core::elaborate(resolve(modules::compile(lex(source)).ast, target));
    if (rebind) {
      const auto f = id(p, "main");
      auto &body = function(p, f).body;
      SymbolId early = None, late = None;
      for (const auto &i : body)
        if (i.kind == I::Bind) {
          if (p.semantics.symbols[i.slot].name == "early")
            early = i.slot;
          if (p.semantics.symbols[i.slot].name == "late")
            late = i.slot;
        }
      require(early != None && late != None, "missing rebind fixture identities");
      walk(body, [&](I &i) {
        if (i.slot == late)
          i.slot = early;
        std::set<SymbolId> seen;
        for (auto &local : i.locals)
          if (local == late)
            local = early;
        std::erase_if(i.locals, [&](auto local) { return !seen.insert(local).second; });
      });
    }
    return core::verify(std::move(p), {true});
  };
  auto marker = [&](const core::Program &p) -> const core::CaptureState & {
    for (const auto &f : p.functions)
      for (const auto &i : f.body)
        if (i.kind == I::Call && i.function == id(p, "marker"))
          for (const auto &s : p.captureStates)
            if (s.function == f.id && s.point.block == i.semanticPoint &&
                s.side == core::StateSide::Before)
              return s;
    throw std::runtime_error("missing branch marker witness");
  };
  auto options = [&](const core::Program &p, bool multiple = false) {
    frames::RoundTripOptions o;
    o.heap = o.outline = o.stateful = true;
    o.projectFields = true;
    o.stepBefore = id(p, multiple ? "first" : "checkpoint");
    if (multiple)
      o.followingSteps = {id(p, "checkpoint")};
    return o;
  };
  if (argc == 7) {
    const std::string name = argv[2], mode = argv[3];
    auto source = load(name, name == "rebind");
    const auto &point = marker(source.get());
    auto out = frames::lowerRoundTrip(source, point, frames::build(source, point).get(),
                                      options(source.get()));
    const auto state = *out.progress;
    const auto result = out.program.semantics.functions[point.function].result;
    auto &body = function(out.program, point.function).body;
    auto op = [&](I::Kind kind, TypeId type = Unit) {
      I i{};
      i.kind = kind;
      i.type = type;
      i.loc = point.origin;
      i.quantity = out.program.semantics.linear(type) ? Quantity::One : Quantity::Omega;
      return i;
    };
    std::vector<I> extra;
    auto call = [&](FunctionId callee, bool bind) {
      auto move = op(I::Move, state.type);
      move.slot = state.storage;
      move.result = out.program.values++;
      extra.push_back(move);
      const auto type = out.program.semantics.functions[callee].result;
      auto call = op(I::Call, type);
      call.function = callee;
      call.args = {move.result};
      if (type != Unit)
        call.result = out.program.values++;
      extra.push_back(call);
      if (bind) {
        auto b = op(I::Bind, state.type);
        b.slot = state.storage;
        b.args = {call.result};
        extra.push_back(b);
      }
      return call.result;
    };
    if (mode == "cancel") {
      unsigned advances = 0;
      size_t cut = body.size();
      for (size_t n = 1; n < body.size(); ++n)
        if (body[n].kind == I::Call && body[n].function == state.advance && ++advances == 2)
          cut = n - 1;
      require(cut < body.size(), "missing second advance");
      body.resize(cut);
      call(state.cancel, true);
      call(state.drop, false);
      auto ret = op(I::Return, result);
      if (result != Unit) {
        auto fallback = op(I::Call, result);
        fallback.function = id(out.program, "fallback");
        fallback.result = out.program.values++;
        extra.push_back(fallback);
        ret.args = {fallback.result};
      }
      extra.push_back(ret);
      body.insert(body.end(), extra.begin(), extra.end());
    } else if (mode == "repeat") {
      size_t take = body.size();
      for (size_t n = 1; n < body.size(); ++n)
        if (body[n].kind == I::Call && body[n].function == state.take)
          take = n - 1;
      require(take < body.size(), "missing take");
      call(state.advance, true);
      call(state.advance, true);
      body.insert(body.begin() + take, extra.begin(), extra.end());
    } else
      require(mode == "normal", "unknown branch API mode");
    auto checked = core::verify(std::move(out.program));
    NativeOptions native;
    native.target = target;
    native.counters = true;
    native.optimization =
        std::string(argv[5]) == "O2" ? OptimizationLevel::O2 : OptimizationLevel::O0;
    native.sanitize = std::string(argv[6]) == "sanitized";
    auto ir = generate(runtime::lower(erase(checked)), BackendStage::LLVMIR, native.sanitize);
    compileNative(optimizeLLVM(ir, target, native.optimization), argv[4], native);
    return 0;
  }
  unsigned plans = 0, mutations = 0;
  for (const std::string name : {"then", "otherwise", "nested", "partial", "argument", "multiple",
                                 "owned_result", "final_returns", "rebind"}) {
    try {
      auto source = load(name, name == "rebind");
      const auto &point = marker(source.get());
      const auto targetId = id(source.get(), "checkpoint");
      auto prepared =
          frames::detail::prepareBranchBoundary(source, point.function, targetId, point.origin);
      require(prepared.has_value() && prepared->rotations == (name == "nested" ? 2u : 1u),
              "lost conditional ancestry");
      require(!frames::detail::prepareBranchBoundary(prepared->source, point.function, targetId,
                                                     point.origin),
              "branch normalization is not idempotent");
      auto p = prepared->source.get();
      require(frames::detail::stepBoundary(p, point.function, targetId, point.origin) <
                  function(p, point.function).body.size(),
              "boundary still nested");
      if (name == "then") {
        std::vector<I *> loans, loops;
        walk(function(p, point.function).body, [&](I &i) {
          if (i.kind == I::BorrowShared)
            loans.push_back(&i);
          if (i.kind == I::Loop)
            loops.push_back(&i);
        });
        require(loans.size() == 2 && loans[0]->offset != loans[1]->offset &&
                    loans[0]->region != loans[1]->region,
                "common tail aliased copied reference identities");
        require(loops.size() == 2 && loops[0]->loop != loops[1]->loop,
                "common tail reused its loop target");
        const auto badRegion = loans[0]->region;
        const auto oldLoop = loops[0]->loop;
        auto reject = [&](bool region) {
          auto bad = prepared->source.get();
          bool first = true;
          walk(function(bad, point.function).body, [&](I &i) {
            if ((region && i.kind == I::BorrowShared) || (!region && i.kind == I::Loop)) {
              if (first)
                first = false;
              else {
                if (region)
                  i.region = badRegion;
                else
                  i.loop = oldLoop;
              }
            }
          });
          try {
            (void)core::verify(std::move(bad));
            throw std::runtime_error("corrupted cloned identity accepted");
          } catch (const Error &) {
            ++mutations;
          } catch (const std::exception &e) {
            throw std::runtime_error(std::string(region ? "cloned region: " : "cloned loop: ") +
                                     e.what());
          }
        };
        reject(true);
        reject(false);
        auto rejectControl = [&](unsigned mutation) {
          auto bad = prepared->source.get();
          auto &body = function(bad, point.function).body;
          bool changed = false;
          walk(body, [&](I &i) {
            if (changed)
              return;
            if (mutation == 0 && i.kind == I::Break) {
              i.loop = loops[1]->loop;
              changed = true;
            }
            if (mutation == 2 && i.kind == I::Loop) {
              I edge{};
              edge.kind = I::Break;
              edge.loop = i.loop;
              edge.loc = i.loc;
              i.latch.push_back(edge);
              changed = true;
            }
          });
          if (mutation == 1) {
            I edge{};
            edge.kind = I::Break;
            edge.loop = oldLoop;
            edge.loc = point.origin;
            body.insert(body.begin(), edge);
            changed = true;
          }
          require(changed, "missing loop control corruption");
          try {
            (void)core::verify(std::move(bad));
            throw std::runtime_error("invalid lexical loop edge accepted");
          } catch (const Error &e) {
            require(e.code == "E9001", "wrong invalid loop target diagnostic");
            ++mutations;
          }
        };
        rejectControl(0);
        rejectControl(1);
        rejectControl(2);
      }
      if (name == "rebind") {
        std::set<SymbolId> bindings;
        walk(function(p, point.function).body, [&](I &i) {
          if (i.kind == I::Bind && p.semantics.symbols[i.slot].name == "early")
            bindings.insert(i.slot);
        });
        require(bindings.size() == 1, "an external owner rebind became a distinct declaration");
      }
      auto o = options(source.get(), name == "multiple");
      auto out = frames::lowerRoundTrip(source, point, frames::build(source, point).get(), o);
      require(out.steps.back().branchRotations == (name == "nested" ? 2u : 1u),
              "lost branch preparation metadata");
      (void)erase(core::verify(std::move(out.program)));
      ++plans;
    } catch (const std::exception &error) {
      throw std::runtime_error(name + ": " + error.what());
    }
  }
  require(plans == 9 && mutations == 5, "missing branch invariant coverage");
}
