#include "isac/async.hpp"
#include "isac/execution.hpp"
#include "isac/frames.hpp"
#include "frame_core_builder.hpp"
#include "frame_heap.hpp"
#include <functional>

namespace isac::asyncs {
bool closedStorage(const hir::Program &p, TypeId type) {
  return execution::closedStorage(p, type);
}
TypeId declareFuture(hir::Program &p, const hir::Function &f) {
  if (f.owner != None || f.external || f.foreignC || f.unsafeFunction ||
      f.intrinsicId != IntrinsicId::None || !f.parameterRegions.empty() ||
      !f.payloadParameterRegions.empty() || f.resultOrigin != None || !closedStorage(p, f.result))
    fail(f.loc, DiagnosticCode::FutureFeature,
         "async function requires closed owned inputs/result and cancellation-safe initial states");
  hir::Type inputs{};
  inputs.kind = hir::Type::Structure;
  inputs.name = "$future.inputs." + std::to_string(f.id);
  uint32_t width = 0;
  for (auto id : f.params) {
    const auto &parameter = p.symbols.at(id);
    if (parameter.reference || parameter.receiver || parameter.access != Access::Own ||
        !closedStorage(p, parameter.type))
      fail(
          parameter.loc, DiagnosticCode::FutureFeature,
          "async capture requires an owned input without references and with a droppable initial state");
    const auto cells = p.storageWidth(parameter.type);
    if (cells >= 65536 || width > 65536 - cells)
      fail(f.loc, DiagnosticCode::FutureFeature,
           "Future inputs exceed the existing record layout limit");
    inputs.fields.push_back(
        {"input" + std::to_string(inputs.fields.size()), parameter.type, width});
    width += cells;
  }
  inputs.width = std::max(1u, width);
  const auto record = static_cast<TypeId>(p.types.size());
  p.types.push_back(std::move(inputs));
  hir::Type future{};
  future.kind = hir::Type::Structure;
  future.name = "Future<" + p.types.at(f.result).name + "; " + f.name + ">";
  future.representation = hir::Representation::FutureFrame;
  future.dropGlue = hir::DropGlue::FutureFrame;
  future.element = record;
  future.futureFunction = f.id;
  const auto type = static_cast<TypeId>(p.types.size());
  p.types.push_back(std::move(future));
  return type;
}

bool present(const core::Program &p) {
  for (const auto &f : p.semantics.functions)
    if (f.kind == FunctionKind::Async)
      return true;
  return false;
}
void check(const core::Program &p) {
  for (TypeId type = 0; type < p.semantics.types.size(); ++type) {
    const auto &shape = p.semantics.types[type];
    if (shape.futureFunction == None)
      continue;
    if (shape.futureFunction >= p.semantics.functions.size())
      fail({}, DiagnosticCode::InvalidCore, "Future has no resolved async producer");
    const auto &f = p.semantics.functions[shape.futureFunction];
    if (f.kind != FunctionKind::Async || f.futureType != type ||
        shape.representation != hir::Representation::FutureFrame || shape.protocol != None ||
        shape.kind != hir::Type::Structure || shape.width != 1 || !shape.fields.empty() ||
        shape.dropGlue != hir::DropGlue::FutureFrame || shape.element >= p.semantics.types.size())
      fail(f.loc, DiagnosticCode::InvalidCore, "invalid rich Future input contract");
    const auto &inputs = p.semantics.types[shape.element];
    if (inputs.kind != hir::Type::Structure || p.semantics.heap(shape.element) ||
        inputs.fields.size() != f.params.size())
      fail(f.loc, DiagnosticCode::InvalidCore, "Future has no closed input record");
    for (size_t n = 0; n < f.params.size(); ++n)
      if (inputs.fields[n].type != p.semantics.symbols.at(f.params[n]).type)
        fail(f.loc, DiagnosticCode::InvalidCore,
             "Future payload does not match its async parameters");
  }
  for (const auto &f : p.semantics.functions)
    if (f.kind == FunctionKind::Async) {
      if (f.futureType >= p.semantics.types.size() ||
          p.semantics.types[f.futureType].futureFunction != f.id || !f.effects.asynchronous ||
          f.owner != None || f.external || f.foreignC || f.unsafeFunction ||
          f.intrinsicId != IntrinsicId::None || f.resultOrigin != None ||
          !f.parameterRegions.empty() || !f.payloadParameterRegions.empty() ||
          !closedStorage(p.semantics, f.result))
        fail(f.loc, DiagnosticCode::InvalidCore,
             "async function is missing its typed Future/frame contract");
      for (auto id : f.params) {
        const auto &s = p.semantics.symbols.at(id);
        if (s.reference || s.receiver || s.access != Access::Own ||
            !closedStorage(p.semantics, s.type))
          fail(s.loc, DiagnosticCode::InvalidCore, "unproved async input capture");
      }
    }
}
}
