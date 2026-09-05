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
  size_t moves = 0, mutations = 0, joins = 0;
  bool unknownCall = false, uniqueCall = false, loop = false, readOnly = false, released = false;
  bool releasedMove = false;
  for (const auto &function : p.functions) {
    const auto &graph = *std::find_if(p.regionGraphs.begin(), p.regionGraphs.end(),
                                      [&](const auto &g) { return g.function == function.id; });
    const auto &facts = graph.storage;
    for (const auto &[point, transition] : facts.transitions)
      for (const auto &[location, candidates] : transition.before) {
        if (location.kind != Location::Symbol)
          continue;
        std::map<StorageId, std::set<EpochId>> epochs;
        for (auto version : candidates.versions)
          epochs[version.storage].insert(version.epoch);
        for (const auto &[storage, possible] : epochs)
          joins += possible.size() > 1;
      }
    std::function<void(const std::vector<core::Inst> &)> check = [&](const auto &body) {
      for (const auto &i : body) {
        if (i.kind == core::Inst::Move && p.semantics.heap(i.type)) {
          const auto &step = facts.transitions.at(i.semanticPoint);
          auto before = step.before.at({Location::Symbol, i.slot});
          if (before.unknown || step.after.contains({Location::Symbol, i.slot}) ||
              step.after.at({Location::Value, i.result}) != before)
            throw std::runtime_error("move changed backing storage identity/epoch");
          ++moves;
          releasedMove |= before.versions.empty();
        }
        if (i.kind == core::Inst::Call && p.semantics.heap(i.type) &&
            !p.semantics.functions.at(i.function).returnsReference()) {
          const auto &step = facts.transitions.at(i.semanticPoint);
          unknownCall |= step.after.at({Location::Value, i.result}).unknown;
        }
        if (i.kind == core::Inst::ContainerCall && i.intrinsicId == IntrinsicId::Vec_len) {
          readOnly = true;
          for (const auto &epoch : facts.epochs)
            if (epoch.point == i.semanticPoint)
              throw std::runtime_error("read-only call invalidated storage");
        }
        loop |= i.kind == core::Inst::Loop;
        check(i.body);
        check(i.otherwise);
        check(i.condition);
        check(i.latch);
      }
    };
    check(function.body);
    for (const auto &epoch : facts.epochs) {
      const auto &step = facts.transitions.at(epoch.point);
      uniqueCall |= epoch.mutation == Mutation::Unknown;
      if (epoch.mutation == Mutation::Unknown)
        continue;
      for (const auto &place : epoch.places) {
        const auto &before = step.before.at(place);
        const auto &after = step.after.at(place);
        if (epoch.mutation == Mutation::Release) {
          if (before.versions.empty() || !after.versions.empty() || after.unknown)
            throw std::runtime_error("released protocol resource retained backing storage");
          released = true;
          continue;
        }
        for (auto old : before.versions)
          if (!after.versions.contains({old.storage, epoch.id}))
            throw std::runtime_error("invalidation lost backing storage identity");
        for (auto next : after.versions)
          if (next.epoch != epoch.id)
            throw std::runtime_error("invalidation retained old epoch");
        ++mutations;
      }
    }
  }
  if (moves < 3 || mutations < 4 || !joins || !unknownCall || !uniqueCall || !loop || !readOnly ||
      !released || !releasedMove)
    return 2;

  if (intrinsicContract(IntrinsicId::Vec_replace).mutation != Mutation::Elements ||
      intrinsicContract(IntrinsicId::Vec_pop).mutation != Mutation::Elements ||
      intrinsicContract(IntrinsicId::Vec_push).mutation != Mutation::Reallocate ||
      intrinsicContract(IntrinsicId::Box_get_mut).mutation != Mutation::None)
    return 3;
  auto invalid = p;
  auto changed = false;
  for (auto &graph : invalid.regionGraphs) {
    if (graph.storage.epochs.empty())
      continue;
    graph.storage.epochs[0].owners.clear();
    changed = true;
    break;
  }
  if (!changed)
    return 4;
  try {
    storage::validate(invalid);
    return 5;
  } catch (const Error &error) {
    if (error.code != "E9001")
      return 6;
  }

  bool barrier = false;
  for (const auto &graph : p.regionGraphs) {
    for (const auto &loan : graph.loans) {
      if (!graph.storage.loans.at(loan.id).backing ||
          graph.storage.loans.at(loan.id).candidates.unknown)
        continue;
      for (const auto &epoch : graph.storage.epochs) {
        if (!epoch.owners.contains(loan.place.root) || epoch.point == loan.created.block)
          continue;
        auto forged = p;
        auto &g = *std::find_if(forged.regionGraphs.begin(), forged.regionGraphs.end(),
                                [&](const auto &g) { return g.function == graph.function; });
        g.regions.at(loan.region).points.insert(epoch.point);
        try {
          storage::validate(forged);
          return 8;
        } catch (const Error &error) {
          if (error.code != "E0310")
            return 9;
        }
        barrier = true;
        break;
      }
      if (barrier)
        break;
    }
    if (barrier)
      break;
  }
  if (!barrier)
    return 10;

  auto untrusted = make();
  untrusted.regionGraphs = invalid.regionGraphs;
  auto rebuilt = core::verify(std::move(untrusted));
  storage::validate(rebuilt.get());
  std::ostringstream rich, erased;
  core::print(p, rich);
  runtime::print(erase(verified), erased);
  for (auto marker : {"storage identity #S", "storage epoch #E", "storage transition"})
    if (rich.str().find(marker) == std::string::npos ||
        erased.str().find(marker) != std::string::npos)
      return 7;
}
