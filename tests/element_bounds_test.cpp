#include "isac/core.hpp"
#include "isac/erasure.hpp"
#include "isac/modules.hpp"
#include <algorithm>
#include <fstream>
#include <functional>
#include <sstream>

int main(int argc, char **argv) {
  using namespace isac;
  using namespace isac::storage;
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
  const auto &p = verified.get();
  bool absent = false, present = false, conditional = false, unique = false;
  bool box = false, nested = false, resultRegions = false, transferred = false, unboxed = false;
  for (const auto &graph : p.regionGraphs) {
    const auto &facts = graph.storage;
    for (const auto &bound : facts.elements) {
      absent |= bound.length == 0 && bound.inBounds == false;
      present |= bound.length == 2 && bound.index == 1 && bound.inBounds == true;
      conditional |= !bound.length && bound.index == 2 && !bound.inBounds;
      unique |= bound.borrowed && bound.capability == Access::Unique;
      box |= bound.guard == ElementGuard::InitializedBox && bound.length == 1 &&
             bound.inBounds == true;
      resultRegions |= bound.borrowed && !bound.resultRegions.empty();
      if (bound.guard == ElementGuard::OptionSome && bound.resultPath != hir::payloadPosition(0, 0))
        return 2;
      if (bound.borrowed && bound.guard == ElementGuard::OptionSome &&
          !bound.resultRegions.empty() && !bound.resultRegions.contains(bound.resultPath))
        return 3;
    }
    for (const auto &[point, step] : facts.transitions)
      for (const auto &[location, candidates] : step.before) {
        auto elements =
            std::count_if(location.path.begin(), location.path.end(), [](const auto &part) {
              return part.kind == hir::ReferenceProjectionKind::Element;
            });
        nested |= elements >= 3 && !candidates.unknown && !candidates.versions.empty();
      }
    const auto &function = *std::find_if(p.functions.begin(), p.functions.end(),
                                         [&](const auto &f) { return f.id == graph.function; });
    std::function<void(const std::vector<core::Inst> &)> inspect = [&](const auto &body) {
      for (const auto &i : body) {
        if ((i.kind == core::Inst::PayloadTake && p.semantics.heap(i.type)) ||
            (i.kind == core::Inst::ContainerCall && i.intrinsicId == IntrinsicId::Box_into_inner)) {
          const auto &step = facts.transitions.at(i.semanticPoint);
          const auto &result = step.after.at({Location::Value, i.result});
          if (result.unknown || result.versions.empty())
            throw std::runtime_error("lost element identity");
          for (auto version : result.versions) {
            bool original = false;
            for (const auto &[location, candidates] : step.before)
              original |= !location.path.empty() && candidates.versions.contains(version);
            if (!original)
              throw std::runtime_error("extraction invented a storage identity");
          }
          transferred |= i.kind == core::Inst::PayloadTake;
          unboxed |= i.intrinsicId == IntrinsicId::Box_into_inner;
        }
        inspect(i.body);
        inspect(i.otherwise);
        inspect(i.condition);
        inspect(i.latch);
      }
    };
    inspect(function.body);
  }
  if (!absent || !present || !conditional || !unique || !box || !nested || !resultRegions ||
      !transferred || !unboxed)
    return 4;
  auto invalid = p;
  bool forged = false;
  for (auto &graph : invalid.regionGraphs) {
    if (graph.storage.elements.empty())
      continue;
    auto &bound = graph.storage.elements[0];
    bound.length = 999;
    bound.guard = ElementGuard::InitializedBox;
    forged = true;
    break;
  }
  if (!forged)
    return 5;
  try {
    storage::validate(invalid);
    return 6;
  } catch (const Error &error) {
    if (error.code != "E9001")
      return 7;
  }
  auto untrusted = make();
  untrusted.regionGraphs = invalid.regionGraphs;
  storage::validate(core::verify(std::move(untrusted)).get());
  std::ostringstream erased;
  runtime::print(erase(verified), erased);
  if (erased.str().find("element bound") != std::string::npos)
    return 8;
}
