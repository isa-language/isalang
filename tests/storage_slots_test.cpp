#include "isac/core.hpp"
#include "isac/erasure.hpp"
#include "isac/modules.hpp"
#include "../src/storage_paths.hpp"
#include <algorithm>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>

int main(int argc, char **argv) {
  using namespace isac;
  using namespace isac::storage;
  if (argc != 2)
    return 1;
  auto require = [](bool ok, const char *why) {
    if (!ok)
      throw std::runtime_error(why);
  };
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
  bool insertion = false, swapping = false, replacement = false, removal = false;
  bool nested = false, parameter = false, joined = false, unknown = false, cyclic = false;
  bool preserved = false, fields = false, barrier = false;
  for (const auto &graph : p.regionGraphs) {
    const auto &facts = graph.storage;
    const auto &function = *std::find_if(p.functions.begin(), p.functions.end(),
                                         [&](const auto &f) { return f.id == graph.function; });
    detail::Places places(p, graph, function);
    for (const auto &identity : facts.slots)
      parameter |= identity.parameter != None;
    for (const auto &bound : facts.elements) {
      if (!bound.slot)
        continue;
      const auto &candidates = bound.slot->candidates;
      if (bound.inBounds == false)
        require(!candidates.initialized && candidates.versions.empty(),
                "absent element claimed initialized");
      if (bound.slot->position.path.back().index == None)
        require(!candidates.initialized, "summary claimed a concrete initialized slot");
      joined |= candidates.initialized && candidates.versions.size() == 2;
      unknown |= !bound.length && !candidates.initialized && candidates.unknown;
      fields |= candidates.initialized && bound.owner.path.size() == 1;
      for (const auto &version : candidates.versions) {
        require(version.content < facts.slots.size(), "invalid content identity");
        require(version.generation == None || version.generation < graph.blocks.size(),
                "invalid generation point");
      }
    }
    for (auto *i : places.instructions) {
      if (!i || i->kind != core::Inst::ContainerCall)
        continue;
      std::vector<const ElementBound *> bounds;
      for (const auto &bound : facts.elements)
        if (bound.point == i->semanticPoint)
          bounds.push_back(&bound);
      if (bounds.empty() || !facts.slotTransitions.contains(i->semanticPoint))
        continue;
      const auto &bound = *bounds[0];
      const auto &step = facts.slotTransitions.at(i->semanticPoint);
      auto at = [&](const SlotState &state, uint32_t n) -> const SlotCandidates & {
        return state.at(detail::element(bound.owner, n));
      };
      auto conserved = [&](const SlotCandidates &before, const SlotCandidates &after) {
        for (auto version : before.versions)
          require(after.versions.contains({version.content, i->semanticPoint}),
                  "shift invented/lost content identity");
      };
      if (i->intrinsicId == IntrinsicId::Vec_push && bound.length && *bound.length < None) {
        if (!step.after.contains(detail::element(bound.owner, *bound.length))) {
          const auto &summary = at(step.after, None);
          require(summary.unknown && !summary.initialized,
                  "unobserved slot was not conservatively summarized");
          continue;
        }
        const auto &added = at(step.after, *bound.length);
        require(added.initialized && !added.unknown && added.versions.size() == 1,
                "push did not initialize one slot");
        for (auto version : added.versions)
          require(facts.slots.at(version.content).point == i->semanticPoint, "wrong push origin");
        for (const auto &[position, candidates] : step.before)
          if (detail::within(position, bound.owner)) {
            require(step.after.at(position) == candidates,
                    "push changed surviving content generation");
            preserved = true;
          }
      }
      if (i->intrinsicId == IntrinsicId::Vec_insert && bound.length && bound.index) {
        for (uint32_t n = *bound.index; n < *bound.length; ++n) {
          conserved(at(step.before, n), at(step.after, n + 1));
          insertion = true;
        }
      }
      if (i->intrinsicId == IntrinsicId::Vec_swap && bounds.size() == 2 && bound.index &&
          bounds[1]->index) {
        auto a = uint32_t(*bound.index), b = uint32_t(*bounds[1]->index);
        if (a == b)
          require(at(step.before, a) == at(step.after, a), "self-swap renewed generation");
        else {
          conserved(at(step.before, a), at(step.after, b));
          conserved(at(step.before, b), at(step.after, a));
          swapping = true;
        }
      }
      if (i->intrinsicId == IntrinsicId::Vec_replace && bound.index && bound.length) {
        const auto &after = at(step.after, *bound.index);
        require(after.initialized && after.versions.size() == 1, "replace lost initialization");
        require(facts.slotInvalidations.contains(i->semanticPoint),
                "replace event disappeared in a loop");
        replacement = true;
        for (auto version : at(step.before, *bound.index).versions)
          cyclic |= version.content == after.versions.begin()->content &&
                    version.generation == i->semanticPoint;
      }
      if (i->intrinsicId == IntrinsicId::Vec_remove && bound.index && bound.length) {
        for (uint32_t n = *bound.index + 1; n < *bound.length; ++n)
          conserved(at(step.before, n), at(step.after, n - 1));
        require(!step.after.contains(detail::element(bound.owner, *bound.length - 1)),
                "remove retained dead slot");
        removal = true;
      }
      if ((i->intrinsicId == IntrinsicId::Box_into_inner ||
           i->intrinsicId == IntrinsicId::Vec_remove) &&
          bound.index) {
        auto base = detail::element(bound.owner, *bound.index);
        for (const auto &[position, candidates] : step.before) {
          if (!detail::within(position, base) || position.path.size() == base.path.size())
            continue;
          hir::ReferencePath relative(position.path.begin() + base.path.size(),
                                      position.path.end());
          require(step.after.at({Location::Value, i->result, relative}) == candidates,
                  "nested extraction changed owned descendant generations");
          nested = true;
        }
      }
    }

    for (const auto &loan : graph.loans) {
      const auto &storage = facts.loans.at(loan.id);
      if (!storage.slot)
        continue;
      for (const auto &[point, positions] : facts.slotInvalidations) {
        if (point == loan.created.block ||
            std::any_of(facts.epochs.begin(), facts.epochs.end(),
                        [&](const auto &e) { return e.point == point; }))
          continue;
        if (!std::any_of(positions.begin(), positions.end(), [&](const auto &position) {
              return detail::covers(position, storage.slot->position);
            }))
          continue;
        auto invalid = p;
        auto &g = *std::find_if(invalid.regionGraphs.begin(), invalid.regionGraphs.end(),
                                [&](const auto &g) { return g.function == graph.function; });
        g.regions.at(loan.region).points.insert(point);
        try {
          storage::validate(invalid);
          throw std::runtime_error("loan survived slot destruction");
        } catch (const Error &error) {
          require(error.code == "E0310", "wrong slot invalidation diagnostic");
        }
        barrier = true;
      }
    }
  }
  require(insertion && swapping && replacement && removal && nested && parameter && joined &&
              unknown && cyclic && preserved && fields && barrier,
          "missing slot transfer/CFG/bound coverage");

  for (int forgery = 0; forgery < 4; ++forgery) {
    auto invalid = p;
    bool changed = false;
    for (auto &graph : invalid.regionGraphs) {
      auto &facts = graph.storage;
      if (forgery == 0 && !facts.slots.empty()) {
        facts.slots[0].point = graph.blocks.size();
        changed = true;
      } else if (forgery == 1 && !facts.slotTransitions.empty()) {
        facts.slotTransitions.begin()->second.after[{Location::Symbol, None}] = {{}, false, true};
        changed = true;
      } else if (forgery == 2 && !facts.slotInvalidations.empty()) {
        facts.slotInvalidations.clear();
        changed = true;
      } else if (forgery == 3)
        for (auto &bound : facts.elements)
          if (bound.slot) {
            bound.slot->candidates.initialized = !bound.slot->candidates.initialized;
            changed = true;
            break;
          }
      if (changed)
        break;
    }
    require(changed, "missing annotation to forge");
    try {
      storage::validate(invalid);
      throw std::runtime_error("forged slot proof accepted");
    } catch (const Error &error) {
      require(error.code == "E9001", "wrong forged proof diagnostic");
    }
    auto untrusted = make();
    untrusted.regionGraphs = invalid.regionGraphs;
    storage::validate(core::verify(std::move(untrusted)).get());
  }
  Location a{Location::Symbol, 1, {{hir::ReferenceProjectionKind::Element, 0}}};
  auto b = a;
  b.path[0].index = 1;
  require(!detail::overlaps(a, b), "different concrete positions overlap in slot domain");
  b.path[0].index = None;
  require(detail::overlaps(a, b) && detail::overlaps(b, a),
          "summary failed to overlap concrete position");
  require(detail::overlaps(a, {Location::Symbol, 1}), "ancestor failed to overlap element");
  require(detail::covers({Location::Symbol, 1}, a) && !detail::covers(a, {Location::Symbol, 1}),
          "descendant mutation invalidated parent descriptor");
  std::ostringstream erased;
  runtime::print(erase(verified), erased);
  for (auto marker : {"slot identity", "slot transition", "slot invalidation", "initialized="})
    require(erased.str().find(marker) == std::string::npos, "slot proof survived erasure");
}
