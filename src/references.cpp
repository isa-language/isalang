#include "isac/hir.hpp"

namespace isac::hir {
RegionArgs substituteRegions(const RegionArgs &arguments, const RegionSubstitution &substitution) {
  RegionArgs result;
  for (const auto &[path, regions] : arguments)
    for (auto region : regions) {
      auto found = substitution.find(region);
      if (found == substitution.end())
        fail({}, DiagnosticCode::InvalidCore, "missing argument for a semantic region parameter");
      if (!found->second.empty())
        result[path].insert(found->second.begin(), found->second.end());
    }
  return result;
}
namespace {

template <class Visitor>
bool visit(const Program &program, TypeId id, std::vector<ReferenceProjection> &path,
           std::set<TypeId> &seen, Visitor &visitor, bool distinctPaths) {
  const auto &type = program.types.at(id);
  if (auto reference = program.referenceInfo(id))
    return visitor(*reference, path);
  const bool elements =
      program.dynamicContainer(id) || type.kind == Type::Array || type.kind == Type::Slice;
  if (!elements && type.kind != Type::Structure && type.kind != Type::Enum)
    return true;
  if (!seen.insert(id).second)
    return true;
  auto descend = [&](TypeId child, ReferenceProjection projection) {
    path.push_back(projection);
    bool more = visit(program, child, path, seen, visitor, distinctPaths);
    path.pop_back();
    return more;
  };
  bool more = true;
  if (elements)
    more = descend(type.element, {ReferenceProjectionKind::Element});
  else if (type.kind == Type::Structure) {
    for (uint32_t n = 0; more && n < type.fields.size(); ++n)
      more = descend(type.fields[n].type, {ReferenceProjectionKind::Field, n});
  } else if (type.kind == Type::Enum) {
    for (const auto &variant : type.variants)
      for (uint32_t n = 0; more && n < variant.payload.size(); ++n)
        more =
            descend(variant.payload[n], {ReferenceProjectionKind::VariantPayload, n, variant.id});
  }
  if (distinctPaths)
    seen.erase(id);
  return more;
}
template <class Visitor>
bool visit(const Program &program, TypeId id, Visitor visitor, bool distinctPaths = false) {
  std::vector<ReferenceProjection> path;
  std::set<TypeId> seen;
  return visit(program, id, path, seen, visitor, distinctPaths);
}
}

std::optional<ReferenceInfo> Program::referenceInfo(TypeId id, RegionId region) const {
  const auto &type = types.at(id);
  if (type.kind != Type::Reference)
    return {};
  return ReferenceInfo{type.element, type.referenceAccess, region};
}
std::optional<ReferenceInfo> Program::symbolReference(SymbolId id) const {
  const auto &symbol = symbols.at(id);
  if (symbol.reference)
    return ReferenceInfo{symbol.type, symbol.access, symbol.region};
  return referenceInfo(symbol.type, symbol.region);
}
std::optional<ReferenceInfo> Program::functionReferenceResult(FunctionId id) const {
  const auto &function = functions.at(id);
  if (function.returnsReference())
    return ReferenceInfo{function.result, function.resultAccess, function.resultRegion};
  return {};
}
std::vector<BorrowedReference> Program::borrowedReferences(TypeId id) const {
  std::vector<BorrowedReference> references;
  visit(
      *this, id,
      [&](ReferenceInfo reference, const auto &path) {
        references.push_back({reference, path});
        return true;
      },
      true);
  return references;
}
bool Program::containsReferences(TypeId id) const {

  return !visit(*this, id, [](ReferenceInfo, const auto &) { return false; });
}
bool Program::referenceCapability(TypeId id, Access capability) const {
  return visit(*this, id, [&](ReferenceInfo reference, const auto &) {
    return reference.capability == capability;
  });
}
}
