#include "isac/core.hpp"
#include "isac/erasure.hpp"
#include "isac/modules.hpp"
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>

int main(int argc, char **argv) {
  using namespace isac;
  if (argc != 2)
    return 1;
  auto require = [](bool value, const char *message) {
    if (!value)
      throw std::runtime_error(message);
  };
  auto make = [&](const char *name) {
    auto path = std::filesystem::path(argv[1]) / name;
    std::ifstream input(path);
    std::string source((std::istreambuf_iterator<char>(input)), {});
    auto tokens = lex(source);
    for (auto &token : tokens)
      token.loc.file = path.string();
    return core::elaborate(resolve(modules::compile(tokens).ast));
  };
  auto verified = core::verify(make("valid_shared.isa"));
  const auto &p = verified.get();
  bool shared = false, unique = false;
  for (const auto &f : p.functions) {
    const auto &g = *std::find_if(p.regionGraphs.begin(), p.regionGraphs.end(),
                                  [&](const auto &g) { return g.function == f.id; });
    std::function<void(const std::vector<core::Inst> &)> visit = [&](const auto &body) {
      for (const auto &i : body) {
        if (core::storedFieldReborrow(p.semantics, i)) {
          const storage::Location field{storage::Location::Symbol, i.slot, i.payloadPath};
          const storage::Location alias{storage::Location::Symbol, i.offset};
          const auto &op = g.parentOperations.at(i.semanticPoint);
          require(op.kind == regions::ParentOperation::Reborrow && op.place == field,
                  "field reborrow was represented as consumption");
          require(g.loanParents.at(i.offset).contains(i.slot),
                  "storage parent omitted from child dependencies");
          require(g.ownerBounds.at(i.region).contains(i.slot),
                  "child is not bounded by its containing storage");
          const auto &transition = g.carriers.transitions.at(i.semanticPoint);
          const auto &before = transition.before.at(field);
          const auto &after = transition.after.at(field);
          const auto &child = transition.after.at(alias);
          require(before == after && before.capability == Access::Unique && before.present,
                  "reborrow consumed or downgraded the source field");
          require(child.instances != before.instances && child.origins == before.origins &&
                      child.present,
                  "child did not get an independent carrier of the original referent");
          require(child.capability ==
                      (i.kind == core::Inst::BorrowShared ? Access::Shared : Access::Unique),
                  "child capability differs from its typed borrow");
          require(child.parent == None,
                  "storage parent was confused with the referent alias parent");
          shared |= child.capability == Access::Shared;
          unique |= child.capability == Access::Unique;
          for (auto next : g.blocks[i.semanticPoint].successors)
            require(g.parentStates.at(next).at(field) ==
                        g.parentStates.at(i.semanticPoint).at(field),
                    "reborrow transferred the original suspended-parent obligation");
        }
        visit(i.body);
        visit(i.otherwise);
        visit(i.condition);
        visit(i.latch);
      }
    };
    visit(f.body);
  }
  require(shared && unique, "missing Shared and Unique field coverage");
  for (unsigned mode = 0; mode != 7; ++mode) {
    auto forged = p;
    bool changed = false;
    for (auto &g : forged.regionGraphs) {
      for (auto &[point, op] : g.parentOperations) {
        if (op.kind != regions::ParentOperation::Reborrow)
          continue;
        if (mode == 0) {
          op.kind = regions::ParentOperation::Alias;
          changed = true;
        }
        if (mode == 1) {
          op.seeds.clear();
          changed = true;
        }
        if (mode == 2) {
          g.loanParents.at(op.destination.id).erase(op.place.id);
          changed = true;
        }
        if (mode == 3) {
          g.parentStates.at(point).clear();
          changed = true;
        }
        if (mode == 4) {
          g.blocks.at(point).uses.clear();
          changed = true;
        }
        if (mode == 5) {
          g.carriers.transitions.at(point).after.erase(op.place);
          changed = true;
        }
        if (mode == 6) {
          g.carriers.transitions.at(point).after.at(op.destination).capability = Access::Own;
          changed = true;
        }
        break;
      }
      if (changed)
        break;
    }
    require(changed, "no reborrow fact to forge");
    try {
      regions::validateBounds(forged);
      throw std::runtime_error("forged field reborrow accepted");
    } catch (const Error &error) {
      require(error.code == "E9001", "wrong reborrow forgery diagnostic");
    }
  }
  for (unsigned mode = 0; mode != 2; ++mode) {
    auto forged = make("valid_unique.isa");
    bool changed = false;
    for (auto &f : forged.functions)
      for (auto &i : f.body)
        if (!changed && core::storedFieldReborrow(forged.semantics, i)) {
          if (mode == 0)
            i.payloadPath.clear();
          else
            i.type = Bool;
          changed = true;
        }
    require(changed, "no typed field reborrow to forge");
    try {
      (void)core::verify(std::move(forged));
      throw std::runtime_error("forged field type accepted");
    } catch (const Error &error) {
      require(error.code == "E9001", "wrong field type forgery diagnostic");
    }
  }
  std::ostringstream erased;
  runtime::print(erase(verified), erased);
  for (auto proof : {"stored-field-reborrow", "parent obligation", "RegionId", "LoanId"})
    require(erased.str().find(proof) == std::string::npos, "field reborrow proof survived erasure");
}
