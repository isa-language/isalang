#include "isac/hir.hpp"
#include <algorithm>
#include <functional>
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
    more = descend(type.element, {ReferenceProjectionKind::Element,
                                  type.representation == Representation::GenericBox ? 0u : None});
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
    return (capability == Access::Shared || capability == Access::Unique) &&
           referenceCapability(id, capability) && localCarrierType(id);

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

  if (types.at(id).kind == Type::Structure)
    return containsReferences(id) && localCarrierType(id);
  return borrowedPayloadLayoutSupported(id, Access::Shared);
}
bool Program::borrowedResultLayoutSupported(TypeId id) const {

  return borrowedPayloadLayoutSupported(id, Access::Shared) ||
         borrowedPayloadLayoutSupported(id, Access::Unique) ||
         (types.at(id).kind == Type::Structure && localCarrierType(id));
}
Access Program::referenceResultAccess(const Function &function, const ReferencePath &path) const {
  if (!function.resultBorrowedContainer && function.returnsReference() && path.empty())
    return function.resultAccess;
  if (function.resultBorrowedContainer)
    if (auto leaf = referencePositionType(function.result, path); leaf && reference(*leaf))
      return types.at(*leaf).referenceAccess;
  fail(function.loc, DiagnosticCode::InvalidCore, "result capability has no reference position");
}
bool Program::referenceResultPresenceSupported(const Function &function,
                                               const ReferenceTransfers &transfers) const {

  std::set<ReferencePath> unavailable;
  for (const auto &[output, sources] : transfers)
    for (const auto &source : sources)
      if (source.kind == ReferenceSourceKind::Carrier &&
          conditionalReferencePosition(symbols.at(source.parameter).type, source.path))
        unavailable.insert(output);
  if (unavailable.empty())
    return true;
  std::function<bool(TypeId, ReferencePath)> possible = [&](TypeId type, ReferencePath path) {
    if (reference(type))
      return !unavailable.contains(path);
    if (!containsReferences(type))
      return true;
    const auto &shape = types.at(type);
    auto child = [&](TypeId type, ReferenceProjection projection) {
      auto next = path;
      next.push_back(projection);
      return possible(type, std::move(next));
    };
    if (shape.kind == Type::Structure) {
      for (uint32_t n = 0; n < shape.fields.size(); ++n)
        if (!child(shape.fields[n].type, {ReferenceProjectionKind::Field, n}))
          return false;
      return true;
    }
    if (shape.kind == Type::Enum)
      return std::any_of(shape.variants.begin(), shape.variants.end(), [&](const auto &variant) {
        for (uint32_t n = 0; n < variant.payload.size(); ++n)
          if (!child(variant.payload[n], {ReferenceProjectionKind::VariantPayload, n, variant.id}))
            return false;
        return true;
      });
    return false;
  };
  return function.resultBorrowedContainer && possible(function.result, {});
}
bool Program::borrowedReceiverSupported(TypeId id, Access capability) const {
  if (capability != Access::Shared && capability != Access::Unique)
    return false;

  if (types.at(id).kind == Type::Structure)
    return localCarrierType(id);
  return capability == Access::Shared && borrowedPayloadLayoutSupported(id, Access::Shared);
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
    if (part.kind == ReferenceProjectionKind::Element) {
      const auto &shape = types.at(id);
      auto child = referencePositionType(id, {part});
      if (!child)
        fail({}, DiagnosticCode::InvalidCore, "invalid reference element contract position");
      if (shape.representation == Representation::GenericVec || shape.kind == Type::Slice ||
          (shape.kind == Type::Array && shape.count == 0))
        return true;
      id = *child;
      continue;
    }
    if (part.kind != ReferenceProjectionKind::Field || types.at(id).kind != Type::Structure ||
        part.index >= types[id].fields.size())
      fail({}, DiagnosticCode::InvalidCore, "invalid reference contract position");
    id = types[id].fields[part.index].type;
  }
  return false;
}
ReferenceTransfers Program::referenceTransferCandidates(const Function &function) const {
  ReferenceTransfers result;
  std::vector<BorrowedReference> outputs;
  if (function.resultBorrowedContainer)
    outputs = borrowedReferences(function.result);
  else if (function.returnsReference())
    outputs.push_back({{function.result, function.resultAccess, function.resultRegion}, {}});
  for (const auto &output : outputs) {
    auto &sources = result[output.path];
    auto add = [&](SymbolId parameter, const ReferencePath &path, SymbolId origin,
                   ReferenceSourceKind kind) {
      if (origin >= symbols.size() || std::find(function.params.begin(), function.params.end(),
                                                parameter) == function.params.end())
        fail(function.loc, DiagnosticCode::InvalidCore,
             "reference source has no parameter position");
      const auto &input = symbols.at(origin);
      if (input.access == output.reference.capability &&
          borrowResultCompatible(input.type, output.reference.referent))
        sources.insert({parameter, path, origin, kind});
    };
    for (const auto &input : function.parameterRegions)
      add(input.parameter, {}, input.parameter, ReferenceSourceKind::Parameter);
    for (const auto &input : function.payloadParameterRegions)
      add(input.parameter, input.path, input.origin, ReferenceSourceKind::Carrier);
  }
  return result;
}
ReferenceTransfers Program::referenceTransfers(const Function &function) const {
  auto candidates = referenceTransferCandidates(function);
  if (!function.inferredTransfers)
    return candidates;
  const auto &selected = *function.inferredTransfers;
  if (function.resultAccess != Access::Unique || !function.resultBorrowedContainer ||
      function.external || !function.intrinsic.empty() || selected.size() != candidates.size())
    fail(function.loc, DiagnosticCode::InvalidCore, "invalid inferred reference transfer domain");
  for (const auto &[path, sources] : selected) {
    auto allowed = candidates.find(path);
    if (allowed == candidates.end() || sources.size() != 1 ||
        !allowed->second.contains(*sources.begin()))
      fail(function.loc, DiagnosticCode::InvalidCore,
           "inferred transfer is not a compatible positional input");
  }
  return selected;
}
bool Program::exclusivePayloadResult(const Function &function) const {
  if (function.external || function.foreignC || !function.intrinsic.empty() ||
      function.intrinsicId != IntrinsicId::None || function.resultAccess != Access::Unique ||
      !function.resultBorrowedContainer || function.resultOrigin >= symbols.size() ||
      function.resultRegion == None || !function.payloadInspectionParameters.empty() ||
      !borrowedResultLayoutSupported(function.result) ||
      referenceCapability(function.result, Access::Shared))
    return false;
  const auto transfers = referenceTransfers(function);
  if (transfers.empty() || !referenceResultPresenceSupported(function, transfers))
    return false;
  if (!function.resultPayloadSource) {
    if (transfers.size() != 1 || !function.additionalResultOrigins.empty() ||
        !function.additionalPayloadSources.empty() || !function.payloadParameterRegions.empty() ||
        std::find(function.params.begin(), function.params.end(), function.resultOrigin) ==
            function.params.end())
      return false;
    const auto &input = symbols[function.resultOrigin];
    const auto &sources = transfers.begin()->second;
    return input.reference && input.access == Access::Unique &&
           sources == std::set<ReferenceSource>{{input.id, {}, input.id}};
  }
  std::set<SymbolId> origins;
  std::set<std::pair<SymbolId, ReferencePath>> positions;
  std::set<ReferenceSource> exclusiveSources;
  for (const auto &[path, sources] : transfers) {
    if (sources.size() != 1)
      return false;
    const auto &source = *sources.begin();
    auto capability = referenceResultAccess(function, path);
    if (source.kind != ReferenceSourceKind::Carrier ||
        (capability == Access::Unique && !exclusiveSources.insert(source).second))
      return false;
    origins.insert(source.origin);
    positions.insert({source.parameter, source.path});
    const auto &parameter = symbols.at(source.parameter);
    const auto &origin = symbols.at(source.origin);
    if (parameter.reference || parameter.receiver ||
        types.at(parameter.type).kind != Type::Structure ||
        !borrowedInputSupported(parameter.type) || !origin.reference || origin.access != capability)
      return false;
    auto input = std::find_if(function.payloadParameterRegions.begin(),
                              function.payloadParameterRegions.end(), [&](const auto &input) {
                                return input.parameter == source.parameter &&
                                       input.path == source.path && input.origin == source.origin;
                              });
    if (input == function.payloadParameterRegions.end())
      return false;
  }
  std::set<SymbolId> declaredOrigins{function.resultOrigin};
  for (auto origin : function.additionalResultOrigins)
    if (!declaredOrigins.insert(origin).second)
      return false;
  std::set<std::pair<SymbolId, ReferencePath>> declaredPositions;
  for (const auto &source : function.payloadResultSources())
    if (!declaredPositions.insert({source.parameter, source.path}).second)
      return false;
  if (declaredOrigins != origins || declaredPositions != positions)
    return false;
  const auto &primary = *function.resultPayloadSource;
  return std::any_of(function.payloadParameterRegions.begin(),
                     function.payloadParameterRegions.end(), [&](const auto &input) {
                       return input.parameter == primary.parameter && input.path == primary.path &&
                              input.origin == function.resultOrigin;
                     });
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
  if (function.inferredTransfers)
    out << "  reference.routing inferred-from-typed-hir-cfg; verify-effective-returns\n";
  for (const auto &input : function.payloadParameterRegions) {
    out << "  reference.input #v" << input.parameter << " path=";
    printPath(input.path);
    out << " region=#R" << input.region << " origin=#v" << input.origin << "\n";
  }
  for (auto parameter : function.params) {
    const auto &symbol = program.symbols.at(parameter);
    if (symbol.receiver && program.containsReferences(symbol.type))
      out << "  reference.preserve #v" << parameter << " identity/capability/parent per position\n";
  }
  for (const auto &[path, sources] : program.referenceTransfers(function)) {
    out << "  reference.transfer result=";
    printPath(path);
    out << " capability=" << accessName(program.referenceResultAccess(function, path));
    out << " <- {";
    for (const auto &source : sources) {
      out << " #v" << source.parameter << ":";
      printPath(source.path);
      out << "(origin=#v" << source.origin
          << ",source=" << (source.kind == ReferenceSourceKind::Parameter ? "parameter" : "carrier")
          << ")";
    }
    out << " }\n";
  }
}
}
