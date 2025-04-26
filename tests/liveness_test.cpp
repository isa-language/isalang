#include "isac/liveness.hpp"
#include <cassert>
using namespace isac::runtime;
int main() {
  CFGFunction f;
  Instruction constant{};
  constant.kind = Instruction::Constant;
  constant.result = 1;
  Instruction use{};
  use.kind = Instruction::Binary;
  use.result = 2;
  use.args = {1, 0};
  f.blocks = {{0, {constant}, {Terminator::Branch, 0, 1, 2}},
              {1, {use}, {Terminator::Jump, NoValue, 3}},
              {2, {}, {Terminator::Jump, NoValue, 3}},
              {3, {}, {Terminator::Return, 1}}};
  auto sets = liveness(f);
  assert(sets[0].liveIn == std::set<Value>{0});
  assert((sets[0].liveOut == std::set<Value>{0, 1}));
  assert((sets[1].liveIn == std::set<Value>{0, 1}));
  assert(sets[2].liveIn == std::set<Value>{1});
  assert(sets[3].liveOut.empty());

  f.blocks[3].end = {Terminator::Branch, 0, 0, 2};
  sets = liveness(f);
  assert(sets[3].liveIn.contains(0));
  assert(!sets[3].liveIn.contains(1));
}
