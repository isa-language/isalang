#pragma once
#include "isac/core.hpp"

namespace isac::regions::detail {
using Contracts = std::map<FunctionId, FunctionRegionContract>;
Contracts contracts(const core::Program &, RegionId &nextRegion);
void arguments(core::Program &, FunctionGraph &, const std::vector<core::Inst *> &,
               const std::map<SymbolId, SymbolId> &parents, const Contracts &, RegionId &);
void storageLimits(core::Program &, FunctionGraph &, const std::vector<core::Inst *> &, RegionId &);
void cleanupUses(core::Program &, FunctionGraph &, const std::vector<core::Inst *> &,
                 const std::map<SymbolId, SymbolId> &parents);
void validateArguments(const core::Program &);
}
