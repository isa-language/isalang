#include "isac/frame_lifecycle.hpp"
#include <deque>
#include <ostream>

namespace isac::frames {
namespace {
using O = LifecycleOp;
using S = ContinuationState;
[[noreturn]] void reject(Loc origin, const char *message) {
  fail(origin, DiagnosticCode::InvalidCore, message);
}
S transfer(S state, O op, Loc origin) {
  switch (op) {
  case O::Restore:
    if (state != S::Ready)
      reject(origin, "frame restoration requires an unconsumed frame");
    return S::Restored;
  case O::Destroy:
    if (state != S::Ready)
      reject(origin, "frame destruction requires an unconsumed frame");
    return S::Destroyed;
  case O::Deliver:
    if (state != S::Restored)
      reject(origin, "continuation delivery requires an undelivered restored result");
    return S::Delivered;
  case O::Finish:
    if (state != S::Delivered && state != S::Destroyed)
      reject(origin, "frame lifecycle leaves an owned frame or restored result unconsumed");
    return state;
  }
  reject(origin, "unknown frame lifecycle operation");
}
const char *name(S state) {
  switch (state) {
  case S::Ready:
    return "ready";
  case S::Restored:
    return "restored";
  case S::Delivered:
    return "delivered";
  case S::Destroyed:
    return "destroyed";
  }
  return "invalid";
}
const char *name(O op) {
  switch (op) {
  case O::Restore:
    return "restore";
  case O::Destroy:
    return "destroy";
  case O::Deliver:
    return "deliver";
  case O::Finish:
    return "finish";
  }
  return "invalid";
}
}

VerifiedLifecycleGraph verifyLifecycleGraph(Verified frame, LifecycleGraph graph) {
  const auto count = graph.blocks.size();
  if (!count || graph.entry >= count)
    reject({}, "frame lifecycle CFG has no valid entry");
  std::vector<std::vector<LifecycleBlockId>> predecessors(count);
  std::vector<bool> terminal(count), reachable(count), canFinish(count);
  for (size_t id = 0; id < count; ++id) {
    const auto &block = graph.blocks[id];
    std::set<LifecycleBlockId> seen;
    for (auto next : block.successors) {
      if (next >= count || !seen.insert(next).second)
        reject(block.origin, "frame lifecycle CFG has an invalid or duplicate successor");
      predecessors[next].push_back(static_cast<LifecycleBlockId>(id));
    }
    for (size_t n = 0; n < block.operations.size(); ++n)
      if (block.operations[n] == O::Finish) {
        if (n + 1 != block.operations.size() || !block.successors.empty())
          reject(block.origin, "frame lifecycle continues after Finish");
        terminal[id] = true;
      }
    if (block.successors.empty() && !terminal[id])
      reject(block.origin, "frame lifecycle exit lacks Finish");
  }
  std::deque<LifecycleBlockId> work{graph.entry};
  reachable[graph.entry] = true;
  while (!work.empty()) {
    auto id = work.front();
    work.pop_front();
    for (auto next : graph.blocks[id].successors)
      if (!reachable[next]) {
        reachable[next] = true;
        work.push_back(next);
      }
  }
  for (size_t id = 0; id < count; ++id) {
    if (!reachable[id])
      reject(graph.blocks[id].origin, "frame lifecycle CFG contains an unreachable block");
    if (terminal[id]) {
      canFinish[id] = true;
      work.push_back(static_cast<LifecycleBlockId>(id));
    }
  }
  while (!work.empty()) {
    auto id = work.front();
    work.pop_front();
    for (auto previous : predecessors[id])
      if (!canFinish[previous]) {
        canFinish[previous] = true;
        work.push_back(previous);
      }
  }
  for (size_t id = 0; id < count; ++id)
    if (!canFinish[id])
      reject(graph.blocks[id].origin, "frame lifecycle block has no path to Finish");

  std::vector<LifecycleBlockFacts> facts(count);
  facts[graph.entry].incoming.insert(S::Ready);
  work.push_back(graph.entry);
  while (!work.empty()) {
    auto id = work.front();
    work.pop_front();
    auto &current = facts[id];
    const auto &block = graph.blocks[id];

    const auto incomingStates = current.incoming;
    for (auto incoming : incomingStates) {
      auto state = incoming;
      for (auto op : block.operations)
        state = transfer(state, op, block.origin);
      if (!current.outgoing.insert(state).second)
        continue;
      for (auto next : block.successors)
        if (facts[next].incoming.insert(state).second)
          work.push_back(next);
    }
  }
  return VerifiedLifecycleGraph(std::move(frame), std::move(graph), std::move(facts));
}

VerifiedLifecycle verifyLifecycle(Verified frame, std::vector<LifecycleOp> operations) {

  auto checked = verifyLifecycleGraph(std::move(frame), {0, {{operations, {}, {}}}});
  auto state = S::Ready;
  std::vector<S> states{state};
  for (auto op : operations) {
    state = transfer(state, op, {});
    states.push_back(state);
  }
  return VerifiedLifecycle(checked.construction(), std::move(operations), std::move(states));
}
VerifiedLifecycle buildLifecycle(Verified frame, bool destroy) {
  return verifyLifecycle(std::move(frame), destroy
                                               ? std::vector{O::Destroy, O::Finish}
                                               : std::vector{O::Restore, O::Deliver, O::Finish});
}
void print(const VerifiedLifecycle &lifecycle, std::ostream &out) {
  out << "  sealed lifecycle: ready";
  for (auto op : lifecycle.get())
    out << " -> " << name(op);
  out << '\n';
}
void print(const VerifiedLifecycleGraph &lifecycle, std::ostream &out) {
  out << "Lifecycle CFG (static ownership states) entry=bb" << lifecycle.get().entry << '\n';
  for (size_t id = 0; id < lifecycle.get().blocks.size(); ++id) {
    out << "  bb" << id << " in={";
    for (auto state : lifecycle.blockFacts()[id].incoming)
      out << ' ' << name(state);
    out << " } out={";
    for (auto state : lifecycle.blockFacts()[id].outgoing)
      out << ' ' << name(state);
    out << " } successors={";
    for (auto next : lifecycle.get().blocks[id].successors)
      out << " bb" << next;
    out << " } operations={";
    for (auto op : lifecycle.get().blocks[id].operations)
      out << ' ' << name(op);
    out << " }\n";
  }
}
}
