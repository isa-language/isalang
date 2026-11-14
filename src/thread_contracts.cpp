#include "isac/execution.hpp"
#include "isac/async.hpp"
#include "isac/concurrency.hpp"
#include <functional>

namespace isac::execution {
namespace {
using I = core::Inst;
std::string position(const hir::Program &p, TypeId type,
                     const concurrency::TypeObligation &obligation) {
  std::string result = p.types.at(type).name;
  for (const auto &part : obligation.path) {
    const auto &shape = p.types.at(type);
    switch (part.kind) {
    case regions::ProjectionKind::Field:
      result += '.' + shape.fields.at(part.index).name;
      type = shape.fields.at(part.index).type;
      break;
    case regions::ProjectionKind::VariantPayload:
      result +=
          "::" + shape.variants.at(part.variant).name + "(" + std::to_string(part.index) + ")";
      type = shape.variants.at(part.variant).payload.at(part.index);
      break;
    case regions::ProjectionKind::Deref:
      result = "*(" + result + ")";
      type = shape.element;
      break;
    case regions::ProjectionKind::Index:
      result += "[element]";
      type = shape.element;
      break;
    }
  }
  return result;
}

bool runtimeTypes(const hir::Program &p, TypeId type) {
  const auto &s = p.types.at(type);
  if (s.futureFunction != None || s.representation == hir::Representation::FutureFrame)
    return false;
  if (p.heap(type))
    switch (s.representation) {
    case hir::Representation::GenericBox:
    case hir::Representation::GenericVec:
      return runtimeTypes(p, s.element);
    case hir::Representation::StringRecord:
    case hir::Representation::BoxPointer:
    case hir::Representation::BufferPointer:
    case hir::Representation::InputRecord:
      return true;
    default:
      return false;
    }
  if (p.reference(type) || s.kind == hir::Type::Array || s.kind == hir::Type::Slice)
    return runtimeTypes(p, s.element);
  for (const auto &f : s.fields)
    if (!runtimeTypes(p, f.type))
      return false;
  for (const auto &v : s.variants)
    for (auto t : v.payload)
      if (!runtimeTypes(p, t))
        return false;
  return true;
}
class WorkerContract {
  const core::Verified &source;
  concurrency::Analysis capabilities;
  std::set<FunctionId> visited;
  std::set<std::pair<TypeId, concurrency::Capability>> types;
  const hir::Program &sem() const {
    return source.get().semantics;
  }
  bool protocolContract(TypeId type) {
    const auto &shape = sem().types.at(type);

    if (shape.kind != hir::Type::Structure || sem().heap(type))
      return false;
    for (const auto &method : sem().functions)
      if (method.owner == type)
        visit(method.id);
    return true;
  }
  void capability(TypeId type, concurrency::Capability wanted, Loc loc) {
    if (type == Unit || !types.insert({type, wanted}).second)
      return;
    const auto report = capabilities.type(type, wanted);
    for (const auto &obligation : report.obligations) {
      const auto r = obligation.requirement;
      if (r == concurrency::Requirement::AllocatorContract &&
          (obligation.capability == concurrency::Capability::Sync || allocatorCrossThread()))
        continue;
      if (r == concurrency::Requirement::RuntimeTypeContract &&
          runtimeTypes(sem(), obligation.type))
        continue;
      if (r == concurrency::Requirement::ProtocolContract && protocolContract(obligation.type))
        continue;
      DiagnosticEngine()
          .error(DiagnosticCode::FutureFeature, loc,
                 "type `" + sem().types.at(type).name +
                     "` has no proved scoped thread transfer contract")
          .note("`" + position(sem(), type, obligation) + "` requires " +
                (obligation.capability == concurrency::Capability::Send ? "Send" : "Sync") +
                " for `" + sem().types.at(obligation.type).name + "`")
          .note(
              r == concurrency::Requirement::AllocatorContract
                  ? "the allocator provider must support concurrent allocation and cross-thread deallocation"
                  : "raw/foreign, protocol and unregistered runtime obligations cannot be inferred from layout")
          .emit();
    }
  }
  void visit(FunctionId id) {
    if (!visited.insert(id).second)
      return;
    const auto &f = sem().functions.at(id);
    if (f.external || f.foreignC || f.unsafeFunction || f.kind != FunctionKind::Normal ||
        f.effects.io || f.effects.unsafeEffect || f.effects.asynchronous || f.effects.threading ||
        f.intrinsicId != IntrinsicId::None)
      fail(f.loc, DiagnosticCode::FutureFeature,
           "scoped worker call graph has an unproved external/IO/unsafe/async effect: `" + f.name +
               "`");
    if (f.effects.allocation && !allocatorCrossThread())
      fail(f.loc, DiagnosticCode::FutureFeature,
           "worker allocation requires a cross-thread allocator contract");
    const auto body = std::find_if(source.get().functions.begin(), source.get().functions.end(),
                                   [&](const auto &b) { return b.id == id; });
    if (body == source.get().functions.end())
      fail(f.loc, DiagnosticCode::InvalidCore, "missing scoped worker body");

    std::map<ValueId, Access> references;
    for (size_t n = 0; n < f.params.size(); ++n)
      if (sem().symbols.at(f.params[n]).reference)
        references.emplace(body->parameters[n], sem().symbols.at(f.params[n]).access);
    std::function<void(const std::vector<I> &)> collect = [&](const auto &ops) {
      for (const auto &i : ops) {
        if (i.kind == I::ReferenceArgument)
          references.emplace(i.result, i.access);
        else if ((core::functionCall(i) || i.kind == I::ContainerCall) &&
                 sem().functions.at(i.function).returnsReference())
          references.emplace(i.result, sem().functions.at(i.function).resultAccess);
        collect(i.condition);
        collect(i.body);
        collect(i.otherwise);
        collect(i.latch);
      }
    };
    collect(body->body);
    std::function<void(const std::vector<I> &)> inspect = [&](const auto &ops) {
      for (const auto &i : ops) {
        auto wanted = concurrency::Capability::Send;
        if (i.kind == I::BorrowShared ||
            (i.kind == I::ReferenceArgument && i.access == Access::Shared) ||
            (i.kind == I::EndBorrow && sem().symbols.at(i.slot).access == Access::Shared) ||
            (i.kind == I::Return && f.returnsReference() && f.resultAccess == Access::Shared) ||
            (i.kind == I::Discard && references.contains(i.args.at(0)) &&
             references.at(i.args[0]) == Access::Shared) ||
            ((core::functionCall(i) || i.kind == I::ContainerCall) &&
             sem().functions.at(i.function).returnsReference() &&
             sem().functions.at(i.function).resultAccess == Access::Shared))
          wanted = concurrency::Capability::Sync;
        capability(i.type, wanted, i.loc);
        if (i.slot != None) {
          const auto &s = sem().symbols.at(i.slot);
          capability(s.type,
                     s.access == Access::Shared ? concurrency::Capability::Sync
                                                : concurrency::Capability::Send,
                     i.loc);
        }
        switch (i.kind) {
        case I::UnsafeScope:
        case I::RawAddress:
        case I::RawLoad:
        case I::RawStore:
        case I::PointerCast:
        case I::FunctionAddress:
        case I::ForeignCall:
        case I::IndirectForeignCall:
        case I::ThreadCall:
        case I::ThreadRun:
        case I::FutureCreate:
        case I::Await:
        case I::BlockOn:
        case I::Print:
        case I::TextPrint:
        case I::InputCall:
        case I::HeapAlloc:
        case I::HeapFree:
        case I::HeapLoad:
        case I::HeapStore:
        case I::MemCopy:
        case I::MemMove:
          fail(i.loc, DiagnosticCode::FutureFeature,
               "worker operation lacks a typed reentrancy/thread contract");
        default:
          break;
        }
        if (i.kind == I::ContainerCall || i.kind == I::StringCall) {
          auto e = intrinsicInfo(i.intrinsicId).effects;
          if (e.io || e.unsafeEffect || e.asynchronous || (e.allocation && !allocatorCrossThread()))
            fail(i.loc, DiagnosticCode::FutureFeature,
                 "worker intrinsic lacks a compatible runtime effect contract");
        }
        if (core::functionCall(i) || i.kind == I::ProtocolTransition)
          visit(i.function);
        inspect(i.condition);
        inspect(i.body);
        inspect(i.otherwise);
        inspect(i.latch);
      }
    };
    inspect(body->body);
  }

public:
  explicit WorkerContract(const core::Verified &v) : source(v), capabilities(v) {}
  void root(FunctionId id, Loc loc) {
    const auto &f = sem().functions.at(id);
    if (f.owner != None || f.returnsReference() || f.resultBorrowedContainer ||
        !asyncs::closedStorage(sem(), f.result))
      fail(loc, DiagnosticCode::FutureFeature,
           "scoped worker requires a top-level function with an owned, reference-free result");
    capability(f.result, concurrency::Capability::Send, loc);
    for (auto id : f.params) {
      const auto &s = sem().symbols.at(id);
      const bool view = s.type == Str || sem().types[s.type].kind == hir::Type::Slice;

      const bool carrier = sem().containsReferences(s.type) &&
                           (s.reference ? sem().borrowedReceiverSupported(s.type, s.access)
                                        : sem().borrowedInputSupported(s.type));
      if (s.receiver ||
          (!view && !closedStorage(sem(), s.type, StorageHorizon::JoinedTransfer) && !carrier) ||
          (view && s.type != Str &&
           (!s.reference || !asyncs::closedStorage(sem(), sem().types[s.type].element))))
        fail(s.loc, DiagnosticCode::FutureFeature,
             "worker inputs require closed owners, scoped views or verified stack carriers");
      capability(s.type,
                 (s.reference && s.access == Access::Shared) || s.type == Str
                     ? concurrency::Capability::Sync
                     : concurrency::Capability::Send,
                 s.loc);
    }
    visit(id);
  }
};
}
void detail::checkThreadCalls(const core::Verified &source) {
  WorkerContract contract(source);
  std::set<FunctionId> checked;
  std::function<void(const std::vector<I> &)> inspect = [&](const auto &body) {
    for (const auto &i : body) {
      if (i.kind == I::ThreadCall && checked.insert(i.function).second)
        contract.root(i.function, i.loc);
      inspect(i.body);
      inspect(i.otherwise);
      inspect(i.condition);
      inspect(i.latch);
    }
  };
  for (const auto &f : source.get().functions)
    inspect(f.body);
}
bool hasThreads(const core::Program &p) {
  bool found = false;
  std::function<void(const std::vector<I> &)> inspect = [&](const auto &body) {
    for (const auto &i : body) {
      found |= i.kind == I::ThreadCall || i.kind == I::ThreadRun;
      inspect(i.body);
      inspect(i.otherwise);
      inspect(i.condition);
      inspect(i.latch);
    }
  };
  for (const auto &f : p.functions)
    inspect(f.body);
  return found;
}
}
