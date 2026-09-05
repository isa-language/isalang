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
  const auto &program = verified.get();
  bool fieldMove = false, fieldMutation = false, paramPaths = false, opaquePost = false,
       releasedPost = false;
  for (const auto &graph : program.regionGraphs) {
    const auto &facts = graph.storage;
    std::set<hir::ReferencePath> inputs;
    for (const auto &identity : facts.identities)
      if (identity.kind == OriginKind::Parameter && identity.path.size() == 2)
        inputs.insert(identity.path);
    paramPaths |= inputs.size() == 2;
    for (const auto &epoch : facts.epochs) {
      const auto &step = facts.transitions.at(epoch.point);
      for (const auto &place : epoch.places) {
        if (place.path.size() != 2)
          continue;
        const auto &after = step.after.at(place);
        if (epoch.mutation == Mutation::Reallocate) {
          if (after.unknown || after.versions.empty())
            return 2;
          for (auto old : step.before.at(place).versions)
            if (!after.versions.contains({old.storage, epoch.id}))
              return 3;

          for (const auto &[other, candidates] : step.before)
            if (other.kind == place.kind && other.id == place.id && other.path.size() == 2 &&
                other.path != place.path && step.after.at(other) != candidates)
              return 4;
          fieldMutation = true;
        } else if (epoch.mutation == Mutation::Unknown)
          opaquePost |= after.unknown;
      }
      if (epoch.mutation == Mutation::Unknown)
        for (const auto &[place, candidates] : step.after)
          releasedPost |= !place.path.empty() && !candidates.unknown && candidates.versions.empty();
    }
    const auto &function = *std::find_if(program.functions.begin(), program.functions.end(),
                                         [&](const auto &f) { return f.id == graph.function; });
    std::function<void(const std::vector<core::Inst> &)> inspect = [&](const auto &body) {
      for (const auto &i : body) {
        if (i.kind == core::Inst::FieldTake && program.semantics.heap(i.type)) {
          const auto &step = facts.transitions.at(i.semanticPoint);
          auto result = step.after.at({Location::Value, i.result});
          if (result.unknown || (result.versions.empty() && i.protocol == None))
            throw std::runtime_error("lost field identity");
          for (auto version : result.versions) {
            bool found = false;
            for (const auto &[place, candidates] : step.before)
              if (place.kind == Location::Symbol && !place.path.empty() &&
                  candidates.versions.contains(version) && !step.after.contains(place))
                found = true;
            if (!found)
              throw std::runtime_error("field extraction invented backing storage");
          }
          fieldMove = true;
        }
        inspect(i.body);
        inspect(i.otherwise);
        inspect(i.condition);
        inspect(i.latch);
      }
    };
    inspect(function.body);
  }
  if (!fieldMove || !fieldMutation || !paramPaths || !opaquePost || !releasedPost)
    return 5;
  auto invalid = program;
  bool forged = false;
  for (auto &graph : invalid.regionGraphs)
    for (auto &epoch : graph.storage.epochs) {
      if (epoch.places.empty() || epoch.places.begin()->path.empty())
        continue;
      auto place = *epoch.places.begin();
      epoch.places.erase(place);
      place.path[0].index = 999;
      epoch.places.insert(place);
      forged = true;
      break;
    }
  if (!forged)
    return 6;
  try {
    storage::validate(invalid);
    return 7;
  } catch (const Error &error) {
    if (error.code != "E9001")
      return 8;
  }
  std::ostringstream rich, erased;
  core::print(program, rich);
  runtime::print(erase(verified), erased);
  if (rich.str().find("places={ #v") == std::string::npos ||
      erased.str().find("storage identity") != std::string::npos)
    return 9;
}
