#include "isac/core.hpp"
#include <algorithm>
#include <iostream>

int main() {
  using namespace isac;

  const std::vector<std::string> sources{
      "fn test(flag:bool){let x=1;let p=&mut x;let r=&*p;"
      "if flag{print(r);}else{print(r);}let c=&mut *p;c=2;print(x);}",
      "fn test(){let x=1;let p=&mut x;let i=0;while i<3{"
      "let c=&mut *p;c=c+1;i=i+1;if i==1{continue;}"
      "let r=&*p;print(r);if i==2{break;}}print(x);}",
      "fn identity(x:&i32)->&i32{return x;}"
      "fn test(){let x=1;let r=&x;let s=identity(r);print(s);let w=&mut x;w=2;}",
      "fn test(flag:bool)->i32{let x=1;let p=&mut x;"
      "borrow shared p as r{print(r);if flag{return 1;}}p=2;return x;}"};
  size_t checkedLoans = 0, checkedConstraints = 0, checkedCycles = 0;
  for (const auto &source : sources) {
    const auto verified =
        core::verify(core::elaborate(resolve(parse(lex(source + "fn main(){}")))));
    const auto &program = verified.get();
    for (const auto &graph : program.regionGraphs) {
      for (const auto &block : graph.blocks) {
        if (!block.reachable)
          continue;
        std::set<SymbolId> out, in = block.uses;
        auto valueOut = block.valuesOut;

        for (const auto &other : graph.blocks)
          for (auto value : other.valueUses)
            valueOut.erase(value);
        for (const auto &other : graph.blocks)
          for (auto value : other.valueDefinitions)
            valueOut.erase(value);
        for (auto successor : block.successors) {
          const auto &next = graph.blocks.at(successor);
          out.insert(next.liveIn.begin(), next.liveIn.end());
          valueOut.merge(next.valuesIn);
        }
        for (auto symbol : out)
          if (!block.definitions.contains(symbol))
            in.insert(symbol);
        auto valueIn = valueOut;
        for (auto value : block.valueDefinitions)
          valueIn.erase(value);
        for (auto value : block.valueUses)
          valueIn.insert(value);
        if (block.liveOut != out || block.liveIn != in || block.valuesOut != valueOut ||
            block.valuesIn != valueIn)
          return 1;
        checkedCycles += block.terminator == regions::Terminator::LoopBackedge;
      }
      for (const auto &loan : graph.loans) {
        if (!loan.points.contains(loan.created.block) ||
            loan.points != graph.regions.at(loan.region).points)
          return 2;
        const auto &root = program.semantics.symbols.at(loan.place.root);
        if (root.reference && !root.parameter && !root.receiver)
          return 3;
        ++checkedLoans;
      }
      for (const auto &constraint : graph.constraints) {
        if (constraint.kind != regions::ConstraintKind::Subregion)
          continue;
        const auto &lower = graph.regions.at(constraint.region).points;
        const auto &upper = graph.regions.at(constraint.upper).points;
        if (!std::includes(upper.begin(), upper.end(), lower.begin(), lower.end()))
          return 4;
        ++checkedConstraints;
      }
    }
  }
  if (!checkedLoans || !checkedConstraints || !checkedCycles)
    return 5;
  std::cout << "Semantic CFG fixpoint, owner normalization and subregions are consistent\n";
}
