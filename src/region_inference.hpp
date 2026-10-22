#pragma once
#include "isac/core.hpp"

namespace isac::regions::detail {
using Contracts = std::map<FunctionId, FunctionRegionContract>;
RegionId freshRegionStart(const core::Program &);
Contracts contracts(const core::Program &, RegionId &nextRegion);
struct FieldRegionFlow {
  std::map<ValueId, hir::RegionArgs> values;
  std::map<uint32_t, std::map<SymbolId, hir::RegionArgs>> before;
  std::map<SymbolId, hir::RegionArgs> summaries;
};
std::map<uint32_t, FieldRegionOperation> fieldRegionOperations(const core::Program &,
                                                               const FunctionGraph &);
FieldRegionFlow fieldArguments(const FunctionGraph &);
void validateFieldArguments(const core::Program &);
struct ParentFlow {
  std::map<uint32_t, ParentState> before;
  std::map<uint32_t, ParentSet> uses;
  std::map<SymbolId, ParentSet> aliases, summaries;
};
std::map<uint32_t, ParentOperation> parentOperations(const core::Program &, const FunctionGraph &);
ParentFlow parentObligations(const FunctionGraph &);
void validateParentObligations(const core::Program &);
void arguments(core::Program &, FunctionGraph &, const std::vector<core::Inst *> &,
               const std::map<SymbolId, SymbolId> &parents, const Contracts &, RegionId &);
void storageLimits(core::Program &, FunctionGraph &, const std::vector<core::Inst *> &, RegionId &);
void cleanupUses(core::Program &, FunctionGraph &, const std::vector<core::Inst *> &,
                 const std::map<SymbolId, SymbolId> &parents);
void validateArguments(const core::Program &);
void projectionLimits(const core::Program &, FunctionGraph &);
void validateProjections(const core::Program &);
}
