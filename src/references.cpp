#include "isac/hir.hpp"
#include <algorithm>
#include <ostream>

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
bool Program::borrowedPayloadLayoutSupported(TypeId id, Access capability) const {

  if (types.at(id).kind == Type::Structure)
    return capability == Access::Shared && referenceCapability(id, Access::Shared) &&
           localCarrierType(id);

  if (needsDrop(id))
    return false;
  if (types.at(id).kind != Type::Enum)
    return false;
  auto references = borrowedReferences(id);
  if (references.empty())
    return false;
  for (const auto &leaf : references)
    if (leaf.path.size() != 1 || leaf.path[0].kind != ReferenceProjectionKind::VariantPayload ||
        leaf.reference.capability != capability ||
        !supportsStoredReferenceReferent(leaf.reference.referent))
      return false;
  return true;
}
bool Program::borrowedInputSupported(TypeId id) const {
  return borrowedPayloadLayoutSupported(id, Access::Shared);
}
bool Program::referenceFreeValuePossible(TypeId id) const {
  if (!containsReferences(id))
    return true;
  const auto &type = types.at(id);
  if (type.kind == Type::Structure)
    return std::all_of(type.fields.begin(), type.fields.end(),
                       [&](const auto &f) { return referenceFreeValuePossible(f.type); });
  if (type.kind == Type::Enum)
    return std::any_of(type.variants.begin(), type.variants.end(), [&](const auto &v) {
      return std::all_of(v.payload.begin(), v.payload.end(),
                         [&](auto t) { return referenceFreeValuePossible(t); });
    });
  return false;
}
bool Program::conditionalReferencePosition(TypeId id, const ReferencePath &path) const {
  for (const auto &part : path) {
    if (part.kind == ReferenceProjectionKind::VariantPayload)
      return true;
    if (part.kind != ReferenceProjectionKind::Field || types.at(id).kind != Type::Structure ||
        part.index >= types[id].fields.size())
      fail({}, DiagnosticCode::InvalidCore, "invalid reference contract position");
    id = types[id].fields[part.index].type;
  }
  return false;
}
ReferenceTransfers Program::referenceTransfers(const Function &function) const {
  ReferenceTransfers result;
  std::vector<BorrowedReference> outputs;
  if (function.resultBorrowedContainer)
    outputs = borrowedReferences(function.result);
  else if (function.returnsReference())
    outputs.push_back({{function.result, function.resultAccess, function.resultRegion}, {}});
  for (const auto &output : outputs) {
    auto &sources = result[output.path];
    auto add = [&](SymbolId parameter, const ReferencePath &path, SymbolId origin) {
      if (origin >= symbols.size() || std::find(function.params.begin(), function.params.end(),
                                                parameter) == function.params.end())
        fail(function.loc, DiagnosticCode::InvalidCore,
             "reference source has no parameter position");
      const auto &input = symbols.at(origin);
      if (input.access == output.reference.capability &&
          borrowResultCompatible(input.type, output.reference.referent))
        sources.insert({parameter, path, origin});
    };
    for (const auto &input : function.parameterRegions)
      add(input.parameter, {}, input.parameter);
    for (const auto &input : function.payloadParameterRegions)
      add(input.parameter, input.path, input.origin);
  }
  return result;
}
void printReferenceContracts(const Program &program, const Function &function, std::ostream &out) {
  auto printPath = [&](const ReferencePath &path) {
    if (path.empty())
      out << "self";
    for (const auto &part : path) {
      if (part.kind == ReferenceProjectionKind::VariantPayload)
        out << "#V" << part.variant << ".payload[" << part.index << "]";
      else if (part.kind == ReferenceProjectionKind::Field)
        out << ".field[" << part.index << "]";
      else {
        out << ".element[";
        if (part.index == None)
          out << "*";
        else
          out << part.index;
        out << "]";
      }
    }
  };
  for (const auto &input : function.payloadParameterRegions) {
    out << "  reference.input #v" << input.parameter << " path=";
    printPath(input.path);
    out << " region=#R" << input.region << " origin=#v" << input.origin << "\n";
  }
  for (const auto &[path, sources] : program.referenceTransfers(function)) {
    out << "  reference.transfer result=";
    printPath(path);
    out << " <- {";
    for (const auto &source : sources) {
      out << " #v" << source.parameter << ":";
      printPath(source.path);
      out << "(origin=#v" << source.origin << ")";
    }
    out << " }\n";
  }
}
}
