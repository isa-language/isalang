#pragma once
#include "isac/frames.hpp"

namespace isac::frames {

enum class LifecycleOp { Restore, Destroy, Deliver, Finish };
enum class ContinuationState { Ready, Restored, Delivered, Destroyed };
using LifecycleBlockId = uint32_t;

struct LifecycleBlock {
  std::vector<LifecycleOp> operations;
  std::vector<LifecycleBlockId> successors;
  Loc origin{};
};
struct LifecycleGraph {
  LifecycleBlockId entry = 0;
  std::vector<LifecycleBlock> blocks;
};
struct LifecycleBlockFacts {
  std::set<ContinuationState> incoming, outgoing;
};
class VerifiedLifecycleGraph {
  Verified frame;
  LifecycleGraph graph;
  std::vector<LifecycleBlockFacts> facts;
  VerifiedLifecycleGraph(Verified f, LifecycleGraph g, std::vector<LifecycleBlockFacts> s)
      : frame(std::move(f)), graph(std::move(g)), facts(std::move(s)) {}
  friend VerifiedLifecycleGraph verifyLifecycleGraph(Verified, LifecycleGraph);

public:
  const Verified &construction() const {
    return frame;
  }
  const LifecycleGraph &get() const {
    return graph;
  }
  const std::vector<LifecycleBlockFacts> &blockFacts() const {
    return facts;
  }
};

VerifiedLifecycleGraph verifyLifecycleGraph(Verified, LifecycleGraph);
void print(const VerifiedLifecycleGraph &, std::ostream &);
class VerifiedLifecycle {
  Verified frame;
  std::vector<LifecycleOp> operations;
  std::vector<ContinuationState> states;
  VerifiedLifecycle(Verified f, std::vector<LifecycleOp> ops, std::vector<ContinuationState> s)
      : frame(std::move(f)), operations(std::move(ops)), states(std::move(s)) {}
  friend VerifiedLifecycle verifyLifecycle(Verified, std::vector<LifecycleOp>);

public:
  const Verified &construction() const {
    return frame;
  }
  const std::vector<LifecycleOp> &get() const {
    return operations;
  }
  const std::vector<ContinuationState> &trace() const {
    return states;
  }
};

VerifiedLifecycle verifyLifecycle(Verified, std::vector<LifecycleOp>);
VerifiedLifecycle buildLifecycle(Verified, bool destroy);
void print(const VerifiedLifecycle &, std::ostream &);
}
