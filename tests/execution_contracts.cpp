#include "isac/execution.hpp"
#include "isac/modules.hpp"
#include <functional>
#include <stdexcept>

int main() {
  using namespace isac;
  using I = core::Inst;
  auto load = [](const char *text) {
    return core::verify(core::elaborate(resolve(modules::compile(lex(text)).ast)));
  };
  auto id = [](const core::Program &p, const char *name) {
    for (const auto &f : p.semantics.functions)
      if (f.name == name)
        return f.id;
    throw std::runtime_error("missing worker fixture function");
  };
  auto source = load("fn helper(x:i32)->i32{return x+1;}fn worker(x:i32)->i32{return helper(x);}"
                     "fn main(){print(worker(5));}");
  auto checked = execution::threadCalls(source, id(source.get(), "worker"));
  unsigned rejected = 0;
  auto reject = [&](const std::function<void(core::Program &)> &mutate) {
    auto p = checked.get();
    mutate(p);
    try {
      (void)core::verify(std::move(p));
    } catch (const Error &e) {
      if (e.diagnostic.code != DiagnosticCode::InvalidCore)
        throw std::runtime_error("worker forgery failed outside typed Core gate");
      ++rejected;
      return;
    }
    throw std::runtime_error("Core accepted an invalid worker execution contract");
  };
  std::function<I &(core::Program &)> thread = [&](auto &p) -> I & {
    for (auto &f : p.functions)
      for (auto &i : f.body)
        if (i.kind == I::ThreadRun)
          return i;
    throw std::runtime_error("missing native thread op");
  };
  reject([&](auto &p) { thread(p).function = None; });
  reject([&](auto &p) { thread(p).offset = None; });
  reject([&](auto &p) { thread(p).args.clear(); });
  reject([&](auto &p) { thread(p).type = I64; });
  reject([&](auto &p) { p.semantics.functions[thread(p).function].foreignC = false; });
  reject([&](auto &p) { p.semantics.functions[id(p, "helper")].effects.io = true; });
  reject([&](auto &p) { p.semantics.functions[id(p, "helper")].unsafeFunction = true; });
  reject([&](auto &p) {
    for (auto &f : p.semantics.functions)
      f.effects = {};
    for (auto &f : p.functions)
      if (f.id == id(p, "helper")) {
        I unsafe{};
        unsafe.kind = I::UnsafeScope;
        unsafe.type = Unit;
        f.body.insert(f.body.begin(), unsafe);
      }
  });
  reject([&](auto &p) {
    auto target = thread(p).function;
    for (auto &f : p.functions)
      if (f.id == target)
        for (auto &i : f.body)
          if (i.kind == I::Call)
            i.function = id(p, "main");
  });
  reject([&](auto &p) {
    for (auto &f : p.functions)
      if (f.id == id(p, "main"))
        for (auto &i : f.body)
          if (i.kind == I::Print)
            i.type = I32;
  });
  if (rejected != 10)
    throw std::runtime_error("incomplete worker forgery checks");
}
