#include "isac/async.hpp"
#include "isac/backend.hpp"
#include "isac/erasure.hpp"
#include "isac/modules.hpp"
#include "../src/frame_core_builder.hpp"
#include <functional>
#include <stdexcept>

int main(int argc, char **argv) {
  using namespace isac;
  using I = core::Inst;
  auto require = [](bool ok, const char *why) {
    if (!ok)
      throw std::runtime_error(why);
  };
  auto target = selectTarget({});
  auto parse = [&](const std::string &text) {
    return core::elaborate(resolve(modules::compile(lex(text)).ast, target));
  };
  const std::string text =
      "@import \"std.async\"\n"
      "protocol State{initial Idle;final Idle;state Idle{begin(self)->Busy;}state Busy{finish(self)->Idle;}}"
      "struct Token:State{value:i32,}impl Token{fn begin(self){self.value=41;}fn finish(self){self.value=42;}}"
      "async fn leaf()->i32{print(7);return 7;}"
      "async fn main(){let token=Token::new();token.begin();token.finish();let text=String::from(\"owned\");let edit=&mut text;"
      "let a=await leaf();edit.push_str(\"1\");let b=await leaf();edit.push_str(\"2\");print(text);print(token.value);}";
  auto rich = core::verify(parse(text), {true});
  unsigned count = 0;
  for (const auto &f : rich.get().functions)
    for (const auto &i : f.body)
      if (i.kind == I::Await) {
        require(i.suspensionBorrows.size() == 1, "missing verified suspension recipe");
        ++count;
      }
  require(count == 2, "missing multi-boundary fixture");
  auto normalized = asyncs::detail::reconstructBorrows(rich);
  for (const auto &f : normalized.get().functions)
    for (const auto &i : f.body)
      if (i.kind == I::Await) {
        require(i.suspensionBorrows.empty(), "normalized await still has an active loan");
        const auto &graph = *std::find_if(normalized.get().regionGraphs.begin(),
                                          normalized.get().regionGraphs.end(),
                                          [&](const auto &g) { return g.function == f.id; });
        for (const auto &loan : graph.loans)
          require(!loan.points.contains(i.semanticPoint), "loan stored at a movable boundary");
      }
  auto proposal = rich.get();
  for (auto &f : proposal.functions)
    for (auto &i : f.body)
      for (auto &recipe : i.suspensionBorrows)
        recipe.owner = None;
  auto reconfirmed = core::verify(std::move(proposal));
  for (const auto &f : reconfirmed.get().functions)
    for (const auto &i : f.body)
      for (const auto &recipe : i.suspensionBorrows)
        require(recipe.owner != None, "forged suspension recipe was trusted");
  auto bad = parse(
      "@import \"std.async\"\nasync fn leaf(){}async fn main(){let data=1;let view=Option::Some(&data);await leaf();match view{Some(r)=>print(r),None=>{}}}");
  for (auto &f : bad.functions)
    for (auto &i : f.body)
      if (i.kind == I::Await)
        i.suspensionBorrows = {{0, 1, I32, BorrowKind::Shared, 0, {}}};
  bool rejected = false;
  try {
    (void)core::verify(std::move(bad));
  } catch (const Error &) {
    rejected = true;
  }
  require(rejected, "forged recipe admitted an unsupported stored capture");
  const auto entry = rich.get().semantics.entryPoint.function;
  const auto futureType = rich.get().semantics.functions[entry].futureType;
  auto machine = asyncs::lower(rich);
  if (argc == 1)
    return 0;
  require(argc == 5, "phase output optimization sanitizer expected");
  const auto phase = std::stoul(argv[1]);
  require(phase <= 3, "invalid cancellation phase");
  auto p = machine.get();
  FunctionId create = None, take = None;
  for (const auto &f : p.semantics.functions) {
    if (f.name.starts_with("__future_create_") && f.result == futureType)
      create = f.id;
    if (f.owner == futureType && f.intrinsicId == IntrinsicId::Box_into_inner)
      take = f.id;
  }
  require(create != None && take != None, "missing realized factory/state extractor");
  const auto state = p.semantics.functions[take].result;
  FunctionId advance = None, cancel = None, drop = None;
  for (const auto &f : p.semantics.functions)
    if (f.params.size() == 1 && p.semantics.symbols[f.params[0]].type == state) {
      if (f.name.starts_with("__frame_state_advance_"))
        advance = f.id;
      if (f.name.starts_with("__frame_state_cancel_"))
        cancel = f.id;
      if (f.name.starts_with("__frame_state_drop_"))
        drop = f.id;
    }
  require(advance != None && cancel != None && drop != None,
          "missing ordinary cancellation dispatch");
  auto symbol = [&](TypeId type) {
    hir::Symbol s{};
    s.id = p.semantics.symbols.size();
    s.type = type;
    s.name = "$test.state";
    p.semantics.symbols.push_back(s);
    return s.id;
  };
  const auto box = symbol(futureType), storage = symbol(state);
  frames::detail::CoreBuilder code{p, {}, {}};
  code.bind(box, code.call(create, {}));
  auto extract = code.operation(I::ContainerCall, state);
  extract.slot = box;
  extract.function = take;
  extract.access = Access::Own;
  extract.intrinsicId = IntrinsicId::Box_into_inner;
  extract.text = std::string(intrinsicInfo(extract.intrinsicId).name);
  code.bind(storage, code.emit(extract, true));
  for (unsigned n = 0; n < phase; ++n)
    code.bind(storage, code.call(advance, {code.move(storage)}));
  code.bind(storage, code.call(cancel, {code.move(storage)}));
  code.call(drop, {code.move(storage)});
  code.ret(Unit);
  for (auto &f : p.functions)
    if (f.id == entry)
      f.body = std::move(code.body);
  auto checked = core::verify(std::move(p));
  NativeOptions native;
  native.target = target;
  native.counters = true;
  native.sanitize = std::string(argv[4]) == "sanitized";
  native.optimization =
      std::string(argv[3]) == "O2" ? OptimizationLevel::O2 : OptimizationLevel::O0;
  auto ir = generate(runtime::lower(erase(checked)), BackendStage::LLVMIR, native.sanitize);
  compileNative(optimizeLLVM(ir, target, native.optimization), argv[2], native);
}
