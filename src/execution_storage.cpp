#include "isac/execution.hpp"

namespace isac::execution {
bool closedStorage(const hir::Program &p, TypeId type, StorageHorizon horizon) {
  if (type == Unit)
    return true;
  const auto &s = p.types.at(type);
  if (type == RawPtr || type == Str || p.reference(type) || p.rawPointer(type) ||
      p.functionPointer(type) || s.kind == hir::Type::ForeignRecord ||
      s.kind == hir::Type::ForeignOpaque || s.kind == hir::Type::Slice ||
      p.containsReferences(type))
    return false;
  if (s.protocol != None && horizon == StorageHorizon::Cancelable) {
    const auto &q = p.protocols.at(s.protocol);
    if (std::find(q.finals.begin(), q.finals.end(), q.initial) == q.finals.end())
      return false;
  }
  if (p.heap(type) && !p.dynamicContainer(type) && s.futureFunction == None)
    return true;
  if ((p.dynamicContainer(type) || s.kind == hir::Type::Array || s.futureFunction != None) &&
      !closedStorage(p, s.element, horizon))
    return false;
  for (const auto &field : s.fields)
    if (!closedStorage(p, field.type, horizon))
      return false;
  for (const auto &variant : s.variants)
    for (auto payload : variant.payload)
      if (!closedStorage(p, payload, horizon))
        return false;
  return true;
}
}
