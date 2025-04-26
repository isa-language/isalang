#pragma once
#include "isac/runtime.hpp"
#include <set>
namespace isac::runtime {
struct BlockLiveness {
  std::set<Value> use, def, liveIn, liveOut;
};

std::vector<BlockLiveness> liveness(const CFGFunction &);
void printLiveness(const LIR &, std::ostream &);
}
