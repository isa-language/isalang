#include "isac/backend.hpp"
#include "isac/erasure.hpp"
#include "isac/frames.hpp"
#include "isac/modules.hpp"
#include "../src/frame_core_builder.hpp"
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>

int main(int argc, char **argv) {
  using namespace isac;
  using I = core::Inst;
  if (argc != 2 && argc != 7)
    return 1;
  auto require = [](bool valid, const char *message) {
    if (!valid)
      throw std::runtime_error(message);
  };
  auto target = selectTarget({});
  auto load = [&](const std::string &text) {
    return core::verify(core::elaborate(resolve(modules::compile(lex(text)).ast, target)));
  };
  std::ifstream file(std::filesystem::path(argv[1]) / "deferred_task.isa");
  const std::string text((std::istreambuf_iterator<char>(file)), {});
  require(!text.empty(), "missing deferred fixture");
  auto source = load(text);
  auto id = [](const core::Program &p, const char *name) {
    for (const auto &f : p.semantics.functions)
      if (f.name == name)
        return f.id;
    throw std::runtime_error("missing deferred function");
  };
  auto make = [&](const char *name) {
    const auto &p = source.get();
    return frames::deferFunction(
        source, id(p, name),
        std::string(name) == "work"
            ? std::vector<FunctionId>{id(p, "checkpoint_one"), id(p, "checkpoint_two")}
            : std::vector<FunctionId>{});
  };
  auto build = [&](frames::DeferredFunction task, const std::string &mode) {
    auto p = task.verified.get();
    const auto info = task.state;
    auto symbol = [&] {
      hir::Symbol s{};
      s.id = p.semantics.symbols.size();
      s.name = "$task";
      s.type = info.type;
      p.semantics.symbols.push_back(s);
      return s.id;
    };
    frames::detail::CoreBuilder code{p, {}, {}};
    auto create = [&](int seed, const char *input) {
      std::vector<ValueId> arguments;
      if (input) {
        auto c = code.operation(I::Constant, I32);
        c.text = std::to_string(seed);
        arguments.push_back(code.emit(c, true));
        arguments.push_back(code.call(id(p, input), {}));
      }
      auto slot = symbol();
      code.bind(slot, code.call(info.create, std::move(arguments)));
      return slot;
    };
    auto advance = [&](SymbolId slot) {
      code.bind(slot, code.call(info.advance, {code.move(slot)}));
    };
    auto cancel = [&](SymbolId slot) {
      code.bind(slot, code.call(info.cancel, {code.move(slot)}));
      if (mode == "cancel-repeat")
        advance(slot);
      code.call(info.drop, {code.move(slot)});
    };
    auto take = [&](SymbolId slot) {
      auto value = code.call(info.take, {code.move(slot)});
      auto result = p.semantics.functions.at(info.take).result;
      if (result != Unit) {
        if (p.semantics.linear(result))
          code.call(id(p, "output"), {value});
        else {
          auto print = code.operation(I::Print);
          print.args = {value};
          code.emit(print);
        }
      }
    };
    const bool work = task.steps.size() == 2;
    auto first = create(mode == "early" ? 0 : 1, work ? "input_one" : nullptr);
    if (mode == "interleave") {
      auto second = create(2, "input_two");
      for (unsigned n = 0; n < 3; ++n) {
        advance(first);
        advance(second);
      }
      take(first);
      take(second);
    } else if (mode == "cancel-ready" || mode == "cancel-repeat")
      cancel(first);
    else if (mode == "take-ready")
      take(first);
    else {
      const unsigned advances = mode == "cancel-one" ? 1
                                : mode == "cancel-two"
                                    ? 2
                                    : static_cast<unsigned>(task.steps.size() + 1);
      for (unsigned n = 0; n < advances; ++n)
        advance(first);
      if (mode.starts_with("cancel-"))
        cancel(first);
      else if (mode == "drop-completed")
        code.call(info.drop, {code.move(first)});
      else {
        if (mode == "repeat" || mode == "early") {
          advance(first);
          advance(first);
        }
        take(first);
      }
    }
    code.ret(Unit);
    const auto entry = p.semantics.entryPoint.function;
    for (auto &f : p.functions)
      if (f.id == entry)
        f.body = std::move(code.body);
    return core::verify(std::move(p));
  };
  if (argc == 7) {
    auto task = make(argv[2]);
    auto verified = build(std::move(task), argv[3]);
    NativeOptions native;
    native.target = target;
    native.counters = true;
    native.optimization =
        std::string(argv[5]) == "O2" ? OptimizationLevel::O2 : OptimizationLevel::O0;
    native.sanitize = std::string(argv[6]) == "sanitized";
    auto ir = generate(runtime::lower(erase(verified)), BackendStage::LLVMIR, native.sanitize);
    compileNative(optimizeLLVM(ir, target, native.optimization), argv[4], native);
    return 0;
  }
  for (const char *name : {"work", "empty", "unit"}) {
    auto task = make(name);
    const auto &p = task.verified.get();
    const auto state = task.state;
    const auto &initial = p.semantics.types[state.type].variants.front();
    require(initial.payload.size() == (std::string(name) == "work" ? 2u : 0u),
            "factory lost source parameters");
    require(p.semantics.functions[state.create].params.size() == initial.payload.size(),
            "factory arity mismatch");
    for (const auto &f : p.functions)
      if (f.id == state.create)
        for (const auto &i : f.body)
          require(i.kind != I::Call && i.kind != I::ContainerCall && i.kind != I::ThreadRun,
                  "creation executes the deferred body");
    for (auto f : {state.create, state.advance, state.cancel, state.drop, state.take})
      require(!p.semantics.functions[f].trusted &&
                  p.semantics.functions[f].kind == FunctionKind::Normal,
              "task helper has special verification privileges");
    (void)build(task, "normal");
    auto forged = build(task, "normal").get();
    const auto entry = forged.semantics.entryPoint.function;
    bool mutated = false;
    for (auto &f : forged.functions)
      if (f.id == entry) {
        for (size_t n = 0; n < f.body.size(); ++n)
          if (f.body[n].kind == I::Call && f.body[n].function == state.take) {
            auto duplicate = f.body[n - 1];
            duplicate.result = forged.values++;
            f.body.insert(f.body.begin() + n + 1, duplicate);
            mutated = true;
            break;
          }
      }
    require(mutated, "missing final token take");
    try {
      (void)core::verify(std::move(forged));
      throw std::runtime_error("task token consumed twice");
    } catch (const Error &) {
    }
  }
  for (const char *unsafeSource :
       {"fn task(x:&i32)->i32{return 1;}fn main(){}",
        "fn task()->i32{unsafe{} return 1;}fn main(){}",
        "extern \"C\" fn host()->i32;fn task()->i32{unsafe{return host();}}fn main(){}"}) {
    auto bad = load(unsafeSource);
    try {
      (void)frames::deferFunction(bad, id(bad.get(), "task"));
      throw std::runtime_error("deferred unsafe capture admitted");
    } catch (const Error &e) {
      require(e.diagnostic.code == DiagnosticCode::FutureFeature, "wrong deferred gate");
    }
  }
  auto borrowed =
      load("@import \"std.box\"\nfn checkpoint(){}\nfn task()->i32{let text=String::from(\"x\");"
           "let view=text.as_str();checkpoint();print(view);return 0;}fn main(){}");
  try {
    (void)frames::deferFunction(borrowed, id(borrowed.get(), "task"),
                                {id(borrowed.get(), "checkpoint")});
    throw std::runtime_error("live borrowed capture admitted into a deferred frame");
  } catch (const Error &e) {
    require(e.diagnostic.code == DiagnosticCode::FutureFeature, "wrong capture gate");
  }
}
