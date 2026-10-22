#include "isac/core.hpp"
#include "isac/erasure.hpp"
#include "isac/modules.hpp"
#include "../src/storage_paths.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

int main(int argc, char **argv) {
  using namespace isac;
  using namespace carriers;
  using storage::Location;
  if (argc != 2)
    return 1;
  auto require = [](bool condition, const char *message) {
    if (!condition)
      throw std::runtime_error(message);
  };

  Binding a{{1}, {10}, {20}, Access::Shared, None, true};
  Binding b{{2}, {10}, {20}, Access::Shared, None, true};
  Location source{Location::Value, 0};
  State leaf{{Location{Location::Value, None}, a}}, tree;
  auto left = storage::detail::field({Location::Symbol, 7}, 0);
  auto right = storage::detail::field({Location::Symbol, 7}, 1);
  auto boxed = storage::detail::element(left, 0);
  auto nested =
      storage::detail::append(storage::detail::element(boxed, 3), hir::payloadPosition(0, 0));
  store(tree, nested, leaf);
  store(tree, right, {{Location{Location::Value, None}, b}});
  auto moved = extract(tree, left);
  erase(tree, left);
  store(tree, {Location::Value, 9}, moved);
  require(tree.at(right) == b && tree.size() == 2, "subtree transfer dropped a sibling carrier");
  erase(tree, {Location::Value, 9});
  require(tree.size() == 1 && tree.at(right).origins == a.origins,
          "dropping one carrier ended another reference to the same origin");
  store(tree, right, leaf, true);
  require(!tree.at(right).present && tree.at(right).instances == std::set<CarrierId>{1, 2},
          "weak storage claimed definite presence or lost a carrier");
  State missing;
  join(missing, tree);
  require(!missing.at(right).present, "join invented must-presence on an absent edge");
  State exact{{source, a}}, other{{source, b}};
  join(exact, other);
  require(exact.at(source).present && exact.at(source).instances == std::set<CarrierId>{1, 2},
          "join lost may-instances or definite presence");
  join(exact, {});
  require(!exact.at(source).present, "missing backedge position remained definitely present");
  State indexed;
  auto container = Location{Location::Symbol, 10};
  auto index = storage::detail::element(container, 3);
  auto summary = storage::detail::element(container);
  store(indexed, index, leaf);
  store(indexed, summary, leaf, true);
  require(!indexed.at(index).present && !indexed.at(summary).present && indexed.size() == 2,
          "unknown slot store claimed initialization or erased a possible carrier");
  for (int operation = 0; operation < 3; ++operation) {
    try {
      if (operation == 0)
        (void)extract(indexed, summary);
      else if (operation == 1)
        erase(indexed, summary);
      else
        store(indexed, summary, leaf);
      throw std::runtime_error("summary selector authorized a strong transfer");
    } catch (const Error &error) {
      require(error.code == "E9001", "wrong summary transfer diagnostic");
    }
  }

  std::ifstream input(argv[1]);
  std::string text((std::istreambuf_iterator<char>(input)), {});
  auto make = [&] {
    auto tokens = lex(text);
    for (auto &token : tokens)
      token.loc.file = argv[1];
    return core::elaborate(resolve(modules::compile(tokens).ast));
  };
  auto verified = core::verify(make());
  const auto &p = verified.get();
  bool transfer = false, dropSibling = false, cleanup = false, returned = false;
  bool parameter = false, slot = false, cyclic = false, hasRegions = false;
  bool sharedAlias = false, uniqueAlias = false, endedAlias = false;
  for (const auto &graph : p.regionGraphs) {
    const auto &facts = graph.carriers;
    const auto &function = *std::find_if(p.functions.begin(), p.functions.end(),
                                         [&](const auto &f) { return f.id == graph.function; });
    storage::detail::Places places(p, graph, function);
    for (auto *i : places.instructions) {
      if (!i || !i->storedPayloadReference)
        continue;
      const auto &step = facts.transitions.at(i->semanticPoint);
      auto source = storage::detail::append(*places.get(i->slot), i->payloadPath);
      const auto &before = step.before.at(source);
      const auto &alias = step.after.at({Location::Symbol, i->offset});
      require(alias.present && alias.origins == before.origins &&
                  alias.regions == std::set<RegionId>{i->region},
              "payload alias lost provenance or its loan region");
      if (i->kind == core::Inst::BorrowShared) {
        require(step.after.contains(source) && before.instances != alias.instances,
                "shared payload alias consumed/collapsed its original carrier");
        sharedAlias = true;
      } else {
        require(!step.after.contains(source) && before.instances == alias.instances,
                "exclusive payload transfer retained or duplicated its source occurrence");
        uniqueAlias = true;
      }
    }
    for (const auto &identity : facts.identities)
      parameter |= identity.parameter != None;
    for (const auto &[point, step] : facts.transitions) {
      require(graph.resourcePoints.contains(point),
              "pruned instruction acquired a carrier witness");
      for (const auto &[position, binding] : step.after) {
        require(!binding.instances.empty() && !binding.origins.empty(),
                "unbounded carrier occurrence");
        for (auto id : binding.instances)
          require(id < facts.identities.size(), "invalid carrier site");
        for (auto region : binding.regions) {
          require(graph.regions.contains(region), "carrier has undefined regional argument");
          hasRegions = true;
        }
      }
      for (const auto &end : step.ends) {
        cleanup |= end.kind == EndKind::Cleanup;
        returned |= end.kind == EndKind::Return;
        endedAlias |= end.kind == EndKind::BorrowEnd;
        for (const auto &[position, binding] : step.after) {
          transfer |= end.kind == EndKind::Transfer && position != end.position &&
                      binding.instances == end.binding.instances;
          dropSibling |= end.kind == EndKind::Drop && binding.origins == end.binding.origins &&
                         binding.instances != end.binding.instances;
        }
      }
      if (graph.blocks[point].terminator == regions::Terminator::Return)
        require(step.after.empty(), "local carrier survived return cleanup");
    }
    for (const auto &[point, attachments] : facts.referents)
      for (const auto &attachment : attachments) {
        const auto &step = facts.transitions.at(point);
        const auto &state = attachment.after ? step.after : step.before;
        require(state.contains(attachment.carrier), "referent attached to a nonexistent carrier");
        require(attachment.slot == *graph.storage.loans.at(attachment.loan).slot,
                "carrier replaced its borrowed slot generation");
        require(attachment.slot.candidates.initialized, "fixture lost initialized Box content");
        slot = true;
      }
    for (const auto &block : graph.blocks)
      cyclic |= block.reachable && block.terminator == regions::Terminator::LoopBackedge;
  }
  require(transfer, "missing occurrence transfer coverage");
  require(dropSibling, "missing surviving sibling after drop coverage");
  require(cleanup, "missing carrier cleanup coverage");
  require(returned, "missing carrier return transfer coverage");
  require(parameter, "missing carrier parameter coverage");
  require(slot, "missing carrier referent slot coverage");
  require(cyclic, "missing cyclic CFG coverage");
  require(hasRegions, "missing per-occurrence RegionArgs coverage");
  require(sharedAlias && uniqueAlias && endedAlias, "missing payload alias/loan-end coverage");
  for (int corruption = 0; corruption < 5; ++corruption) {
    auto invalid = p;
    bool changed = false;
    for (auto &graph : invalid.regionGraphs) {
      if (changed)
        break;
      if (corruption == 0 && !graph.carriers.identities.empty()) {
        graph.carriers.identities[0].point = graph.blocks.size();
        changed = true;
      } else if (corruption == 1 || corruption == 2) {
        for (auto &[point, step] : graph.carriers.transitions)
          if (!step.after.empty()) {
            if (corruption == 1)
              step.after.begin()->second.origins = {None};
            else
              step.after.begin()->second.instances = {None};
            changed = true;
            break;
          }
      } else if (corruption == 3) {
        for (auto &[point, step] : graph.carriers.transitions)
          if (!step.ends.empty()) {
            step.ends.clear();
            changed = true;
            break;
          }
      }
      if (corruption == 4 && !graph.carriers.referents.empty()) {
        graph.carriers.referents.begin()->second[0].slot.candidates.versions.clear();
        changed = true;
      }
    }
    require(changed, "missing carrier metadata to forge");
    try {
      carriers::validate(invalid);
      throw std::runtime_error("forged carrier instance accepted");
    } catch (const Error &error) {
      require(error.code == "E9001", "wrong carrier forgery diagnostic");
    }
    auto raw = make();
    raw.regionGraphs = invalid.regionGraphs;
    carriers::validate(core::verify(std::move(raw)).get());
  }
  std::ostringstream erased;
  runtime::print(erase(verified), erased);
  for (auto marker : {"carrier instance", "carrier transition", "carrier end", "carrier referent"})
    require(erased.str().find(marker) == std::string::npos, "carrier metadata survived erasure");
}
