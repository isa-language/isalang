#include "isac/core.hpp"
#include "isac/erasure.hpp"
#include "isac/modules.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>

int main(int argc, char **argv) {
  using namespace isac;
  if (argc != 2)
    return 1;
  std::ifstream input(argv[1]);
  std::string source((std::istreambuf_iterator<char>(input)), {});
  auto make = [&] {
    auto tokens = lex(source);
    for (auto &token : tokens)
      token.loc.file = argv[1];
    return core::elaborate(resolve(modules::compile(tokens).ast));
  };
  auto verified = core::verify(make());
  const auto &program = verified.get();
  bool gap = false, parentRemains = false, restored = false, sharedWitness = false;
  for (const auto &graph : program.regionGraphs) {
    for (const auto &[id, bound] : graph.projectionBounds) {
      sharedWitness |= bound.loans.size() > 1;
      if (bound.fields.size() != 2)
        continue;
      for (auto point : graph.resourcePoints) {
        if (bound.points.contains(point) || !graph.storageBounds.contains(bound.owner) ||
            !graph.storageBounds.at(bound.owner).points.contains(point))
          continue;
        for (const auto &[parentId, parent] : graph.projectionBounds)
          if (parent.owner == bound.owner && parent.fields.size() == 1 &&
              parent.fields[0] == bound.fields[0] && parent.points.contains(point))
            parentRemains = true;
        for (auto next : graph.blocks.at(point).successors)
          restored |= bound.points.contains(next);
        auto forged = program;
        auto &target = *std::find_if(forged.regionGraphs.begin(), forged.regionGraphs.end(),
                                     [&](const auto &g) { return g.function == graph.function; });
        auto before = target.projectionBounds.at(id).points;
        target.regions.at(target.loans.at(*bound.loans.begin()).region).points.insert(point);
        try {
          regions::validateBounds(forged);
          return 2;
        } catch (const Error &error) {
          if (error.code != "E0310")
            return 3;
        }
        if (target.projectionBounds.at(id).points != before)
          return 4;
        gap = true;
        break;
      }
    }
  }
  if (!gap || !parentRemains || !sharedWitness)
    return 5;

  for (const auto &graph : program.regionGraphs)
    for (const auto &[id, bound] : graph.projectionBounds)
      for (auto point : graph.resourcePoints)
        if (!bound.points.contains(point))
          for (auto next : graph.blocks.at(point).successors)
            restored |= bound.points.contains(next);
  if (!restored)
    return 6;

  auto untrusted = make();
  untrusted.regionGraphs = program.regionGraphs;
  for (auto &graph : untrusted.regionGraphs)
    for (auto &[id, bound] : graph.projectionBounds) {
      bound.fields = {999};
      bound.points.clear();
    }
  auto rebuilt = core::verify(std::move(untrusted));
  regions::validateBounds(rebuilt.get());
  auto invalid = program;
  bool badPath = false;
  for (auto &graph : invalid.regionGraphs) {
    if (graph.projectionBounds.empty())
      continue;
    graph.projectionBounds.begin()->second.fields = {999};
    try {
      regions::validateBounds(invalid);
      return 7;
    } catch (const Error &error) {
      if (error.code != "E9001")
        return 8;
    }
    badPath = true;
    break;
  }
  if (!badPath)
    return 9;
  std::ostringstream rich, erased;
  core::print(program, rich);
  runtime::print(erase(verified), erased);
  for (auto marker : {"projection storage bound", "ProjectionStorage"})
    if (rich.str().find(marker) == std::string::npos ||
        erased.str().find(marker) != std::string::npos)
      return 10;
}
