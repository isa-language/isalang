#include "region_inference.hpp"
#include <optional>

namespace isac::regions::detail {
namespace {
std::optional<ProjectionStorageBound> projection(const core::Program &p, const Loan &loan) {
  auto type = p.semantics.symbols.at(loan.place.root).type;
  std::vector<uint32_t> fields;
  bool owned = false;
  for (const auto &part : loan.place.projections) {

    if (part.kind != ProjectionKind::Field)
      return {};
    const auto &shape = p.semantics.types.at(type);
    if (shape.kind != hir::Type::Structure || part.index >= shape.fields.size())
      fail(loan.origin, DiagnosticCode::InvalidCore, "invalid field place in region graph");
    type = shape.fields[part.index].type;
    owned |= p.semantics.ownedFieldType(type);
    fields.push_back(part.index);
  }
  if (!owned)
    return {};
  return ProjectionStorageBound{
      loan.place.root, fields, type, p.semantics.symbols.at(loan.place.root).loc, {loan.id}, {}};
}
std::string name(const core::Program &p, const ProjectionStorageBound &bound) {
  const auto &owner = p.semantics.symbols.at(bound.owner);
  auto result = owner.name;
  auto type = owner.type;
  for (auto index : bound.fields) {
    const auto &member = p.semantics.types.at(type).fields.at(index);
    result += "." + member.name;
    type = member.type;
  }
  return result;
}
}
void projectionLimits(const core::Program &p, FunctionGraph &graph) {
  graph.projectionBounds.clear();
  graph.resourcePoints.clear();
  std::map<std::pair<SymbolId, std::vector<uint32_t>>, uint32_t> positions;
  for (const auto &loan : graph.loans)
    if (auto bound = projection(p, loan)) {
      auto [position, added] = positions.emplace(std::make_pair(bound->owner, bound->fields),
                                                 graph.projectionBounds.size());
      if (added)
        graph.projectionBounds.emplace(position->second, std::move(*bound));
      else
        graph.projectionBounds.at(position->second).loans.insert(loan.id);
    }
}
void validateProjections(const core::Program &p) {
  for (const auto &graph : p.regionGraphs) {
    std::map<LoanId, const ProjectionStorageBound *> bounds;
    for (const auto &[id, bound] : graph.projectionBounds) {
      if (bound.loans.empty())
        fail(bound.origin, DiagnosticCode::InvalidCore, "field storage bound has no loan");
      for (auto loan : bound.loans)
        if (loan >= graph.loans.size() || graph.loans[loan].id != loan ||
            !bounds.emplace(loan, &bound).second)
          fail(bound.origin, DiagnosticCode::InvalidCore,
               "invalid or duplicate field storage loan");
    }
    for (auto point : graph.resourcePoints)
      if (point >= graph.blocks.size() || !graph.blocks[point].reachable)
        fail(p.semantics.functions[graph.function].loc, DiagnosticCode::InvalidCore,
             "field initialization witness is outside the semantic CFG");
    for (const auto &loan : graph.loans) {
      const auto expected = projection(p, loan);
      auto found = bounds.find(loan.id);
      if (!expected) {
        if (found != bounds.end())
          fail(loan.origin, DiagnosticCode::InvalidCore, "unexpected field storage bound");
        continue;
      }
      if (found == bounds.end() || found->second->owner != expected->owner ||
          found->second->fields != expected->fields || found->second->type != expected->type)
        fail(loan.origin, DiagnosticCode::InvalidCore,
             "field storage bound contradicts its loan place");
      const auto &bound = *found->second;
      for (auto point : bound.points)
        if (!graph.resourcePoints.contains(point))
          fail(loan.origin, DiagnosticCode::InvalidCore,
               "field storage has no initialization witness");
      for (auto point : graph.regions.at(loan.region).points) {
        if (!graph.resourcePoints.contains(point) || bound.points.contains(point))
          continue;
        const auto &block = graph.blocks.at(point);
        DiagnosticEngine()
            .error(DiagnosticCode::BorrowConflict, block.origin,
                   "reference requires uninitialized or consumed field `" + name(p, bound) + "`")
            .label(block.origin, "this field must remain initialized while its reference is live")
            .secondary(loan.origin, "reference to this projection is created here")
            .secondary(bound.origin, "enclosing owner is declared here")
            .note("an initialized owner does not imply that every owned field is initialized")
            .help("restore the field before borrowing it, or end the reference before consuming it")
            .emit();
      }
    }
  }
}
}
