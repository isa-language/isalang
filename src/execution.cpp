#include "isac/execution.hpp"
#include "isac/ffi.hpp"
#include <functional>

namespace isac::execution {
bool allocatorCrossThread() {
  return ISAC_ALLOCATOR_CROSS_THREAD != 0;
}
namespace {
using I = core::Inst;
bool primitive(TypeId type) {
  return type == Unit || scalar(type);
}
bool localRuntime(const hir::Program &p, const hir::Function &f) {

  static const std::map<std::string, TypeId> inputs = {
      {"isac_rt_stdout_write", Str},  {"isac_rt_stderr_write", Str}, {"isac_rt_print_i32", I32},
      {"isac_rt_print_usize", USize}, {"isac_rt_print_bool", Bool},  {"isac_rt_panic_str", Str}};
  const auto found = inputs.find(f.name);
  if (found == inputs.end() || f.foreignC || f.result != Unit || f.params.size() != 1)
    return false;
  const auto &s = p.symbols.at(f.params[0]);
  return !s.reference && !s.receiver && s.type == found->second && s.access == Access::Own;
}
Contract check(const core::Program &p, FunctionId root, Domain domain, bool callback,
               DiagnosticCode code) {
  Contract result{root, domain, {}};
  const auto &sem = p.semantics;
  auto failAt = [&](Loc loc, const std::string &message) { fail(loc, code, message); };
  if (root >= sem.functions.size())
    failAt({}, "execution requires a resolved function");
  auto closed = [&](TypeId type) { return closedStorage(sem, type); };
  const auto &entry = sem.functions[root];
  if (entry.owner != None || entry.external || entry.trusted || entry.kind != FunctionKind::Normal)
    failAt(entry.loc, "execution entry must be an ordinary top-level Isalang function");
  if (domain == Domain::ScalarWorker) {
    if (entry.params.size() != 1 || !integer(sem.symbols.at(entry.params[0]).type) ||
        !integer(entry.result) ||
        (callback &&
         (!entry.foreignC || entry.result != U64 || sem.symbols.at(entry.params[0]).type != U64)) ||
        (!callback && entry.foreignC))
      failAt(entry.loc, "scalar worker requires one owned integer argument and an integer result");
  } else {
    if (entry.foreignC || !closed(entry.result) || !entry.parameterRegions.empty() ||
        !entry.payloadParameterRegions.empty() || entry.resultOrigin != None ||
        entry.resultBorrowedContainer)
      failAt(entry.loc, "deferred entry requires closed owned parameters and a closed result");
    for (auto parameter : entry.params) {
      const auto &s = sem.symbols.at(parameter);
      if (s.reference || s.receiver || s.access != Access::Own || !closed(s.type))
        failAt(
            s.loc,
            "deferred parameter requires a complete owner without reference/protocol obligations");
    }
  }
  std::set<FunctionId> visiting;
  std::function<void(FunctionId)> visit;
  visit = [&](FunctionId id) {
    if (result.functions.contains(id) || !visiting.insert(id).second)
      return;
    const auto &f = sem.functions.at(id);
    if (f.kind != FunctionKind::Normal || f.unsafeFunction || f.effects.unsafeEffect ||
        f.effects.asynchronous)
      failAt(f.loc, "execution contract does not admit unsafe or suspension metadata");
    if (f.external) {
      if (domain != Domain::LocalDeferred || !localRuntime(sem, f))
        failAt(
            f.loc,
            "execution call graph reaches an external function without a compatible contract: `" +
                f.name + "`");
    } else if (f.intrinsicId != IntrinsicId::None)
      failAt(
          f.loc,
          "execution requires typed intrinsic operations rather than an unchecked function body: `" +
              f.name + "`");
    else {
      if (domain == Domain::ScalarWorker) {
        if (f.effects.io || f.effects.allocation || f.effects.threading ||
            (f.foreignC && id != root) || !primitive(f.result) || !f.parameterRegions.empty() ||
            !f.payloadParameterRegions.empty())
          failAt(f.loc,
                 "worker call graph must have scalar owned signatures and no external effects: `" +
                     f.name + "`");
        for (auto parameter : f.params) {
          const auto &s = sem.symbols.at(parameter);
          if (s.reference || s.receiver || s.access != Access::Own || !primitive(s.type))
            failAt(s.loc, "worker cannot receive references, handles or owned heap storage");
        }
      }
      const auto definition = std::find_if(p.functions.begin(), p.functions.end(),
                                           [&](const auto &body) { return body.id == id; });
      if (definition == p.functions.end())
        failAt(f.loc, "execution function has no verified Core body");
      std::function<void(const std::vector<I> &)> body;
      body = [&](const auto &instructions) {
        for (const auto &i : instructions) {
          if (domain == Domain::ScalarWorker) {
            if (!primitive(i.type))
              failAt(i.loc, "worker instruction carries non-scalar storage");
            if (i.slot != None) {
              const auto &s = sem.symbols.at(i.slot);
              if (s.reference || s.receiver || s.access != Access::Own || !primitive(s.type))
                failAt(i.loc, "worker place is not an independent scalar owner");
            }
            switch (i.kind) {
            case I::Constant:
            case I::Copy:
            case I::Read:
            case I::Write:
            case I::Bind:
            case I::ScopeEnd:
            case I::Binary:
            case I::Unary:
            case I::Cast:
            case I::Call:
            case I::Return:
            case I::If:
            case I::Loop:
            case I::Break:
            case I::Continue:
            case I::Discard:
            case I::EndViews:
              break;
            default:
              failAt(i.loc, "worker operation has no pure scalar execution contract");
            }
          } else
            switch (i.kind) {
            case I::UnsafeScope:
            case I::RawAddress:
            case I::RawLoad:
            case I::RawStore:
            case I::PointerCast:
            case I::FunctionAddress:
            case I::ForeignCall:
            case I::IndirectForeignCall:
              failAt(
                  i.loc,
                  "deferred execution requires a noescape/reentrancy contract for raw or foreign operations");
            default:
              break;
            }
          if (core::functionCall(i) || i.kind == I::ProtocolTransition)
            visit(i.function);
          body(i.body);
          body(i.otherwise);
          body(i.condition);
          body(i.latch);
        }
      };
      body(definition->body);
    }
    visiting.erase(id);
    result.functions.insert(id);
  };
  visit(root);
  return result;
}
}
Contract analyze(const core::Verified &source, FunctionId root, Domain domain) {
  return check(source.get(), root, domain, false, DiagnosticCode::FutureFeature);
}
Contract detail::check(const core::Program &p, FunctionId root, Domain domain, bool callback) {
  return execution::check(p, root, domain, callback, DiagnosticCode::InvalidCore);
}
void detail::checkThreadRuns(const core::Program &p) {
  std::set<FunctionId> checked;
  std::function<void(const std::vector<I> &)> visit = [&](const auto &body) {
    for (const auto &i : body) {
      if (i.kind == I::ThreadRun && checked.insert(i.function).second)
        (void)detail::check(p, i.function, Domain::ScalarWorker, true);
      visit(i.body);
      visit(i.otherwise);
      visit(i.condition);
      visit(i.latch);
    }
  };
  for (const auto &f : p.functions)
    visit(f.body);
}
core::Verified threadCalls(const core::Verified &source, FunctionId worker,
                           core::VerificationOptions options) {
  (void)analyze(source, worker, Domain::ScalarWorker);
  auto p = source.get();
  auto &sem = p.semantics;
  const auto original = sem.functions.at(worker);
  hir::Type callback{};
  callback.kind = hir::Type::FunctionPointer;
  callback.name = "$thread.callback";
  callback.element = U64;
  callback.parameters = {U64};
  const auto callbackType = static_cast<TypeId>(sem.types.size());
  sem.types.push_back(callback);
  hir::Symbol parameter{};
  parameter.id = sem.symbols.size();
  parameter.name = "$thread.cell";
  parameter.type = U64;
  parameter.parameter = true;
  parameter.loc = original.loc;
  sem.symbols.push_back(parameter);
  hir::Function bridge{};
  bridge.id = sem.functions.size();
  bridge.name = "isa_thread_bridge_" + std::to_string(bridge.id);
  while (std::any_of(sem.functions.begin(), sem.functions.end(), [&](const auto &f) {
    return f.name == bridge.name || f.cLinkName == bridge.name;
  }))
    bridge.name += '_';
  bridge.params = {parameter.id};
  bridge.result = U64;
  bridge.foreignC = true;
  bridge.loc = original.loc;
  const auto bridgeId = bridge.id;
  sem.functions.push_back(bridge);
  core::Function definition{bridgeId, {}, {p.values++}, {}};
  auto op = [&](I::Kind kind, TypeId type) {
    I i{};
    i.kind = kind;
    i.type = type;
    i.loc = original.loc;
    return i;
  };
  auto copy = op(I::Copy, U64);
  copy.slot = parameter.id;
  copy.result = p.values++;
  auto unpack = op(I::Cast, sem.symbols.at(original.params[0]).type);
  unpack.args = {copy.result};
  unpack.result = p.values++;
  auto call = op(I::Call, original.result);
  call.function = worker;
  call.args = {unpack.result};
  call.result = p.values++;
  auto pack = op(I::Cast, U64);
  pack.args = {call.result};
  pack.result = p.values++;
  auto ret = op(I::Return, U64);
  ret.args = {pack.result};
  definition.body = {copy, unpack, call, pack, ret};
  p.functions.push_back(std::move(definition));
  const auto entry = sem.entryPoint.function;
  auto main = std::find_if(p.functions.begin(), p.functions.end(),
                           [&](const auto &f) { return f.id == entry; });
  if (main == p.functions.end() || entry == worker)
    fail(original.loc, DiagnosticCode::FutureFeature,
         "thread worker must be called by the entrypoint");
  unsigned rewritten = 0;
  std::function<void(std::vector<I> &)> rewrite = [&](auto &body) {
    std::vector<I> out;
    for (auto &i : body) {
      rewrite(i.body);
      rewrite(i.otherwise);
      rewrite(i.condition);
      rewrite(i.latch);
      if (i.kind == I::Call && i.function == worker) {
        auto input = op(I::Cast, U64);
        input.loc = i.loc;
        input.args = i.args;
        input.result = p.values++;
        auto run = op(I::ThreadRun, U64);
        run.loc = i.loc;
        run.function = bridgeId;
        run.offset = callbackType;
        run.args = {input.result};
        run.result = p.values++;
        auto output = op(I::Cast, i.type);
        output.loc = i.loc;
        output.args = {run.result};
        output.result = i.result;
        out.insert(out.end(), {input, run, output});
        ++rewritten;
      } else
        out.push_back(std::move(i));
    }
    body = std::move(out);
  };
  rewrite(main->body);
  if (!rewritten)
    fail(original.loc, DiagnosticCode::FutureFeature,
         "entrypoint has no calls to the selected thread worker");
  sem.functions.at(entry).effects.threading = true;
  return core::verify(std::move(p), options);
}
}
