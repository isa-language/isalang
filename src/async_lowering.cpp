#include "isac/async.hpp"
#include "isac/frames.hpp"
#include "frame_core_builder.hpp"
#include "frame_heap.hpp"
#include <functional>

namespace isac::asyncs {
namespace {
using I = core::Inst;
using Builder = frames::detail::CoreBuilder;
core::Function &definition(core::Program &p, FunctionId id) {
  auto found = std::find_if(p.functions.begin(), p.functions.end(),
                            [&](const auto &f) { return f.id == id; });
  if (found == p.functions.end())
    fail({}, DiagnosticCode::InvalidCore, "missing async Core body");
  return *found;
}
const core::Function &definition(const core::Program &p, FunctionId id) {
  auto found = std::find_if(p.functions.begin(), p.functions.end(),
                            [&](const auto &f) { return f.id == id; });
  if (found == p.functions.end())
    fail({}, DiagnosticCode::InvalidCore, "missing async Core body");
  return *found;
}
template <class Ops, class Visit> void walk(Ops &body, const Visit &visit) {
  for (auto &i : body) {
    walk(i.condition, visit);
    visit(i);
    walk(i.body, visit);
    walk(i.otherwise, visit);
    walk(i.latch, visit);
  }
}
SymbolId parameter(core::Program &p, TypeId type, Loc loc, bool input = true) {
  hir::Symbol s{};
  s.id = p.semantics.symbols.size();
  s.name = "$async.input";
  s.parameter = input;
  s.type = type;
  s.loc = loc;
  p.semantics.symbols.push_back(s);
  return s.id;
}
FunctionId declare(core::Program &p, const std::string &name, const std::vector<TypeId> &inputs,
                   TypeId result, Loc loc, FunctionEffects effects = {}) {
  hir::Function f{};
  f.id = p.semantics.functions.size();
  f.name = name + std::to_string(f.id);
  f.result = result;
  f.loc = loc;
  f.effects = effects;
  f.effects.asynchronous = false;
  for (auto type : inputs)
    f.params.push_back(parameter(p, type, loc));
  const auto id = f.id;
  p.semantics.functions.push_back(std::move(f));
  return id;
}
void define(core::Program &p, FunctionId id, std::vector<I> body) {
  core::Function f{id, std::move(body), {}, {}};
  for (auto unused : p.semantics.functions[id].params) {
    (void)unused;
    f.parameters.push_back(p.values++);
  }
  p.functions.push_back(std::move(f));
}

void realizeType(core::Program &p, TypeId future, TypeId boxed) {
  auto &sem = p.semantics;
  if (boxed + 1 != sem.types.size() || sem.storageWidth(future) != 1 ||
      sem.storageWidth(boxed) != 1)
    fail({}, DiagnosticCode::InvalidCore, "invalid Future representation refinement");
  auto replace = [&](TypeId &type) {
    if (type == boxed)
      type = future;
  };
  auto name = sem.types[future].name;
  sem.types[future] = std::move(sem.types.back());
  sem.types.pop_back();
  sem.types[future].name = name;
  for (auto &type : sem.types) {
    replace(type.element);
    for (auto &field : type.fields)
      replace(field.type);
    for (auto &variant : type.variants)
      for (auto &payload : variant.payload)
        replace(payload);
    for (auto &argument : type.parameters)
      replace(argument);
  }
  for (auto &symbol : sem.symbols)
    replace(symbol.type);
  for (auto &instance : sem.instances) {
    replace(instance.type);
    for (auto &[param, type] : instance.substitution.types) {
      (void)param;
      replace(type);
    }
  }
  for (auto &f : sem.functions) {
    replace(f.owner);
    replace(f.result);
    replace(f.futureType);
  }
  for (auto &f : p.functions)
    walk(f.body, [&](auto &i) { replace(i.type); });
}
}

core::Verified lower(const core::Verified &source, core::VerificationOptions verification) {
  if (!present(source.get()))
    return source;
  std::vector<FunctionId> order;
  std::set<FunctionId> visiting, done;
  std::function<void(FunctionId)> dependency = [&](FunctionId root) {
    if (done.contains(root))
      return;
    const auto &p = source.get();
    if (!visiting.insert(root).second)
      fail(p.semantics.functions.at(root).loc, DiagnosticCode::FutureFeature,
           "recursive async construction requires a recursive frame contract");
    std::set<FunctionId> seen;
    std::function<void(FunctionId)> inspect = [&](FunctionId id) {
      if (!seen.insert(id).second)
        return;
      const auto &f = p.semantics.functions.at(id);
      if (f.external || f.intrinsicId != IntrinsicId::None)
        return;
      walk(definition(p, id).body, [&](const auto &i) {
        if (i.kind == I::FutureCreate || i.kind == I::Await || i.kind == I::BlockOn)
          dependency(i.function);
        else if (core::functionCall(i) || i.kind == I::ProtocolTransition)
          inspect(i.function);
      });
    };
    inspect(root);
    visiting.erase(root);
    done.insert(root);
    order.push_back(root);
  };
  for (const auto &f : source.get().semantics.functions)
    if (f.kind == FunctionKind::Async)
      dependency(f.id);
  auto checked = detail::reconstructBorrows(source);
  std::set<FunctionId> awaitSites;
  for (auto root : order) {
    auto p = checked.get();
    const auto original = p.semantics.functions.at(root);
    const auto future = original.futureType;
    const auto loc = original.loc;
    auto originalBody = std::move(definition(p, root));

    auto bodySignature = original;
    bodySignature.id = p.semantics.functions.size();
    bodySignature.name = "__async_body_" + std::to_string(root);
    bodySignature.kind = FunctionKind::Normal;
    bodySignature.futureType = None;
    bodySignature.effects.asynchronous = false;
    bodySignature.body.clear();
    const auto bodyId = bodySignature.id;
    p.semantics.functions.push_back(std::move(bodySignature));
    originalBody.id = bodyId;
    definition(p, root) = {root, {}, {}, {}};
    p.functions.push_back(std::move(originalBody));
    std::vector<SymbolId> fresh;
    for (auto old : original.params)
      fresh.push_back(parameter(p, p.semantics.symbols.at(old).type, loc));
    p.semantics.functions[root].params = fresh;
    p.semantics.functions[root].body.clear();
    Builder trampoline{p, loc, {}};
    std::vector<ValueId> arguments;
    for (auto input : fresh)
      arguments.push_back(trampoline.transfer(input));
    trampoline.ret(original.result, trampoline.call(bodyId, std::move(arguments)));
    auto &stub = definition(p, root);
    stub.body = std::move(trampoline.body);
    for (auto unused : fresh) {
      (void)unused;
      stub.parameters.push_back(p.values++);
    }
    std::vector<FunctionId> boundaries;
    walk(definition(p, bodyId).body, [&](const auto &i) {
      if (i.kind == I::Call && awaitSites.contains(i.function))
        boundaries.push_back(i.function);
    });
    checked = core::verify(std::move(p));
    auto task = frames::deferFunction(checked, bodyId, boundaries, true);
    p = task.verified.get();
    auto &sem = p.semantics;
    const auto box = hir::instantiateHeapContainer(sem, frames::detail::boxDeclaration(sem, loc),
                                                   {task.state.type}, loc);
    const auto makeBox = hir::owningBoxIntrinsic(sem, box, IntrinsicId::Box_new_generic, loc);
    const auto takeBox = hir::owningBoxIntrinsic(sem, box, IntrinsicId::Box_into_inner, loc);
    std::vector<TypeId> inputTypes;
    for (auto input : original.params)
      inputTypes.push_back(sem.symbols.at(input).type);
    const auto create = declare(p, "__future_create_", inputTypes, box, loc);
    Builder factory{p, loc, {}};
    arguments.clear();
    for (auto input : sem.functions[create].params)
      arguments.push_back(factory.transfer(input));
    auto initial = factory.call(task.state.create, std::move(arguments));
    auto allocate = factory.operation(I::ContainerCall, box);
    allocate.function = makeBox;
    allocate.intrinsicId = IntrinsicId::Box_new_generic;
    allocate.text = std::string(intrinsicInfo(allocate.intrinsicId).name);
    allocate.args = {initial};
    factory.ret(box, factory.emit(allocate, true));
    define(p, create, std::move(factory.body));
    sem.functions[create].effects.allocation = true;
    auto effects = original.effects;
    effects.asynchronous = false;
    const auto drain = declare(p, "__future_run_", {box}, original.result, loc, effects);
    Builder runner{p, loc, {}};
    auto extract = runner.operation(I::ContainerCall, task.state.type);
    extract.function = takeBox;
    extract.intrinsicId = IntrinsicId::Box_into_inner;
    extract.text = std::string(intrinsicInfo(extract.intrinsicId).name);
    extract.slot = sem.functions[drain].params[0];
    extract.access = Access::Own;
    const auto storage = parameter(p, task.state.type, loc, false);
    runner.bind(storage, runner.emit(extract, true));
    for (size_t n = 0; n <= boundaries.size(); ++n)
      runner.bind(storage, runner.call(task.state.advance, {runner.move(storage)}));
    runner.ret(original.result, runner.call(task.state.take, {runner.move(storage)}));
    define(p, drain, std::move(runner.body));

    const size_t definitions = p.functions.size();
    for (size_t n = 0; n < definitions; ++n) {
      auto body = std::move(p.functions[n].body);
      walk(body, [&](auto &i) {
        if ((i.kind != I::FutureCreate && i.kind != I::Await && i.kind != I::BlockOn) ||
            i.function != root)
          return;
        if (i.kind == I::FutureCreate)
          i.function = create;
        else if (i.kind == I::BlockOn)
          i.function = drain;
        else {
          const auto site = declare(p, "__await_site_", {box}, original.result, i.loc, effects);
          Builder bridge{p, i.loc, {}};
          bridge.ret(original.result,
                     bridge.call(drain, {bridge.move(sem.functions[site].params[0])}));
          define(p, site, std::move(bridge.body));
          awaitSites.insert(site);
          i.function = site;
        }
        i.kind = I::Call;
      });
      p.functions[n].body = std::move(body);
    }
    sem.functions[root].kind = FunctionKind::Normal;
    sem.functions[root].futureType = None;
    sem.functions[root].effects.asynchronous = false;
    Builder entry{p, loc, {}};
    arguments.clear();
    for (auto input : sem.functions[root].params)
      arguments.push_back(entry.transfer(input));
    entry.ret(original.result, entry.call(drain, {entry.call(create, std::move(arguments))}));
    definition(p, root).body = std::move(entry.body);
    realizeType(p, future, box);
    frames::LoweredRoundTrip normalized{};
    normalized.program = std::move(p);
    frames::detail::normalizeFrameValues(normalized);
    checked = core::verify(std::move(normalized.program), verification);
  }
  return checked;
}
}
