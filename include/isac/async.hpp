#pragma once
#include "isac/core.hpp"

namespace isac::asyncs {

bool closedStorage(const hir::Program &, TypeId);
TypeId declareFuture(hir::Program &, const hir::Function &);
void check(const core::Program &);
bool present(const core::Program &);

core::Verified lower(const core::Verified &, core::VerificationOptions = {});
namespace detail {

core::Verified reconstructBorrows(const core::Verified &);
}
}
