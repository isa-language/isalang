#include "isac/liveness.hpp"
#include <algorithm>
#include <stdexcept>
namespace isac::runtime {
std::vector<BlockLiveness> liveness(const CFGFunction &f) {
  std::vector<BlockLiveness> result(f.blocks.size());
  for (auto &block : f.blocks) {
    auto &sets = result.at(block.id);
    auto use = [&](Value v) {
      if (v != NoValue && !sets.def.contains(v))
        sets.use.insert(v);
    };
    for (auto &i : block.instructions) {
      for (auto v : i.args)
        use(v);
      if (i.result != NoValue)
        sets.def.insert(i.result);
    }
    use(block.end.value);
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t n = f.blocks.size(); n-- > 0;) {
      auto &block = f.blocks[n];
      auto &sets = result[n];
      std::set<Value> out;
      auto edge = [&](uint32_t target) {
        auto &in = result.at(target).liveIn;
        out.insert(in.begin(), in.end());
      };
      if (block.end.kind == Terminator::Jump || block.end.kind == Terminator::Branch)
        edge(block.end.first);
      if (block.end.kind == Terminator::Branch)
        edge(block.end.second);
      auto in = sets.use;
      for (auto v : out)
        if (!sets.def.contains(v))
          in.insert(v);
      changed |= in != sets.liveIn || out != sets.liveOut;
      sets.liveIn = std::move(in);
      sets.liveOut = std::move(out);
    }
  }
  return result;
}
void printLiveness(const LIR &lir, std::ostream &out) {
  for (auto &f : lir.functions) {
    if (f.external)
      continue;
    out << "fn " << f.name << "\n";
    auto sets = liveness(f);
    for (size_t n = 0; n < sets.size(); ++n) {
      out << "  bb" << n << " live_in={";
      for (auto v : sets[n].liveIn)
        out << " %" << v;
      out << " } live_out={";
      for (auto v : sets[n].liveOut)
        out << " %" << v;
      out << " }\n";
    }
  }
}
}
