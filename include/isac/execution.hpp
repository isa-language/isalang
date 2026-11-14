#pragma once
#include "isac/core.hpp"

namespace isac::execution {
enum class Domain { LocalDeferred, ScalarWorker };
enum class StorageHorizon { Cancelable, JoinedTransfer };

bool closedStorage(const hir::Program &, TypeId, StorageHorizon = StorageHorizon::Cancelable);

struct Contract {
  FunctionId root;
  Domain domain;
  std::set<FunctionId> functions;
};
Contract analyze(const core::Verified &, FunctionId, Domain);
namespace detail {
Contract check(const core::Program &, FunctionId, Domain, bool callback = false);
void checkThreadRuns(const core::Program &);
void checkThreadCalls(const core::Verified &);
}

core::Verified threadCalls(const core::Verified &, FunctionId, core::VerificationOptions = {});
inline constexpr const char *threadRunAbi = "isac_rt_thread_run_u64";
inline constexpr const char *threadCallAbi = "isac_rt_thread_call";
bool hasThreads(const core::Program &);
bool allocatorCrossThread();
}
