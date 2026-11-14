#include "isac/async.hpp"
#include "isac/erasure.hpp"
#include "isac/execution.hpp"
#include "isac/modules.hpp"
#include <functional>
#include <stdexcept>

static bool localAllocator = false;
extern "C" bool __real__ZN4isac9execution20allocatorCrossThreadEv();
extern "C" bool __wrap__ZN4isac9execution20allocatorCrossThreadEv() {
  return !localAllocator && __real__ZN4isac9execution20allocatorCrossThreadEv();
}
int main() {
  using namespace isac;
  using I = core::Inst;
  auto require = [](bool yes, const char *message) {
    if (!yes)
      throw std::runtime_error(message);
  };
  auto parseCore = [](const std::string &text) {
    return core::elaborate(resolve(modules::compile(lex(text)).ast));
  };
  auto rich = core::verify(
      parseCore("@import \"std.async\"\nasync fn task(x:i32)->String{return String::from(\"x\");}"
                "async fn main(){let a=await task(1);let b=await task(2);print(a);print(b);}"));
  require(asyncs::present(rich.get()), "async source lost its rich semantics");
  try {
    (void)erase(rich);
    throw std::runtime_error("untransformed async Core erased");
  } catch (const Error &e) {
    require(e.diagnostic.code == DiagnosticCode::InvalidCore, "wrong erasure gate");
  }
  auto machine = asyncs::lower(rich);
  require(!asyncs::present(machine.get()), "machine still has async functions");
  unsigned sites = 0;
  for (const auto &f : machine.get().semantics.functions)
    if (f.name.starts_with("__await_site_")) {
      require(!f.trusted && f.kind == FunctionKind::Normal,
              "await helper bypasses ordinary verification");
      ++sites;
    }
  require(sites == 2, "repeated awaits of the same producer did not get independent boundaries");
  for (const auto &t : machine.get().semantics.types)
    require(t.futureFunction == None, "unrealized Future shape");
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
  auto operation = [&](core::Program &p, I::Kind kind) -> I & {
    I *found = nullptr;
    for (auto &f : p.functions)
      walk(f.body, [&](I &i) {
        if (!found && i.kind == kind)
          found = &i;
      });
    require(found, "missing fixture operation");
    return *found;
  };
  unsigned rejected = 0;
  auto reject = [&](const core::Verified &source,
                    const std::function<void(core::Program &)> &mutate) {
    auto p = source.get();
    mutate(p);
    try {
      (void)core::verify(std::move(p));
    } catch (const Error &) {
      ++rejected;
      return;
    }
    throw std::runtime_error("invalid Core proposal accepted");
  };
  reject(rich, [&](auto &p) { operation(p, I::FutureCreate).function = None; });
  reject(rich, [&](auto &p) { operation(p, I::FutureCreate).args.clear(); });
  reject(rich, [&](auto &p) { operation(p, I::Await).kind = I::Call; });
  reject(rich, [&](auto &p) {
    p.semantics.functions[operation(p, I::Await).function].futureType = None;
  });
  reject(rich, [&](auto &p) {
    auto t = operation(p, I::FutureCreate).type;
    p.semantics.types[t].futureFunction = None;
  });
  reject(rich, [&](auto &p) {
    auto t = operation(p, I::FutureCreate).type;
    p.semantics.types[t].element = t;
  });
  reject(rich, [&](auto &p) {
    auto t = operation(p, I::FutureCreate).type;
    auto input = p.semantics.types[t].element;
    p.semantics.types[input].fields[0].type = I64;
  });
  auto threaded = core::verify(parseCore(
      "fn helper(x:i32)->i32{return x+1;}fn worker(x:i32)->i32{return helper(x);}fn main(){print(@thread(worker(5)));}"));
  require(execution::hasThreads(threaded.get()),
          "public worker was expanded away without a contract");
  reject(threaded, [&](auto &p) { operation(p, I::ThreadCall).function = None; });
  reject(threaded, [&](auto &p) { operation(p, I::ThreadCall).args.clear(); });
  reject(threaded, [&](auto &p) {
    for (auto &f : p.semantics.functions)
      f.effects = {};
    for (auto &f : p.functions)
      if (p.semantics.functions[f.id].name == "helper") {
        I print{};
        print.kind = I::Print;
        print.type = Unit;
        print.args = {f.parameters[0]};
        f.body.insert(f.body.begin(), print);
      }
  });
  reject(threaded, [&](auto &p) {
    auto &call = operation(p, I::ThreadCall);
    call.body.push_back(call);
  });
  localAllocator = true;
  try {
    (void)core::verify(parseCore(
        "fn worker(text:String)->String{return text.move();}fn main(){let value=@thread(worker(String::from(\"x\")));print(value);}"));
    throw std::runtime_error("local-only allocator accepted owned heap transfer");
  } catch (const Error &) {
    ++rejected;
  }
  (void)core::verify(parseCore(
      "fn worker(x:&i32[])->i32{return x[0];}fn main(){let data:i32[1]=[1];print(@thread(worker(&data)));}"));

  (void)core::verify(parseCore(
      "fn worker(text:&String)->usize{return text.len();}fn main(){let text=String::from(\"x\");print(@thread(worker(&text)));}"));
  localAllocator = false;
  require(rejected == 12, "incomplete async/thread forgery gates");
}
