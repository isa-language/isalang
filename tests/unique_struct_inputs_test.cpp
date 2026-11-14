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
  auto verified = core::verify(make("valid_fields.isa"));
  const auto &p = verified.get();
  auto target = std::find_if(p.semantics.functions.begin(), p.semantics.functions.end(),
                             [](const auto &f) { return f.payloadParameterRegions.size() == 2; });
  require(target != p.semantics.functions.end(), "missing positional Unique input contracts");
  auto fid = target->id;
  const auto &first = target->payloadParameterRegions[0];
  const auto &second = target->payloadParameterRegions[1];
  require(first.region != second.region && first.origin != second.origin &&
              first.path != second.path,
          "equal referent types conflated two input leaves");
  const auto &g = *std::find_if(p.regionGraphs.begin(), p.regionGraphs.end(),
                                [&](const auto &graph) { return graph.function == fid; });
  for (const auto &contract : target->payloadParameterRegions) {
    require(p.semantics.symbols.at(contract.origin).access == Access::Unique,
            "Unique input was downgraded to Shared");
    require(g.contract.inputs.at(contract.parameter).at(contract.path) ==
                std::set<RegionId>{contract.region},
            "input region lost its semantic position");
  }
  uint32_t reborrowPoint = None;
  storage::Location source;
  const auto &definition = *std::find_if(p.functions.begin(), p.functions.end(),
                                         [&](const auto &f) { return f.id == fid; });
  std::function<void(const std::vector<core::Inst> &)> visit = [&](const auto &body) {
    for (const auto &i : body) {
      if (core::storedFieldReborrow(p.semantics, i)) {
        auto input = std::find_if(
            target->payloadParameterRegions.begin(), target->payloadParameterRegions.end(),
            [&](const auto &c) { return c.parameter == i.slot && c.path == i.payloadPath; });
        require(input != target->payloadParameterRegions.end(), "uncontracted input projection");
        const storage::Location field{storage::Location::Symbol, i.slot, i.payloadPath};
        const storage::Location alias{storage::Location::Symbol, i.offset};
        const auto &transition = g.carriers.transitions.at(i.semanticPoint);
        const auto &before = transition.before.at(field);
        require(before.capability == Access::Unique &&
                    before.origins == std::set<SymbolId>{input->origin} && before.present,
                "input occurrence has wrong capability, origin or presence");
        require(transition.after.at(field) == before, "callee reborrow consumed its input field");
        require(g.loanParents.at(i.offset).contains(i.slot) &&
                    g.ownerBounds.at(i.region).contains(i.slot),
                "input field child lost its storage lifetime bound");
        for (auto next : g.blocks.at(i.semanticPoint).successors)
          require(g.parentStates.at(next).at(alias).contains(i.slot),
                  "empty parent entry omitted the child's input storage dependency");
        reborrowPoint = i.semanticPoint;
        source = field;
      }
      visit(i.body);
      visit(i.otherwise);
      visit(i.condition);
      visit(i.latch);
    }
  };
  visit(definition.body);
  require(reborrowPoint != None, "missing callee reborrow");
  for (unsigned mode = 0; mode != 6; ++mode) {
    auto forged = make("valid_fields.isa");
    auto &f = forged.semantics.functions.at(fid);
    auto &contract = f.payloadParameterRegions[0];
    if (mode == 0)
      forged.semantics.symbols.at(contract.origin).access = Access::Shared;
    if (mode == 1)
      f.payloadParameterRegions.pop_back();
    if (mode == 2)
      contract.path = f.payloadParameterRegions[1].path;
    if (mode == 3)
      forged.semantics.symbols.at(contract.origin).region = None;
    if (mode == 4)
      forged.semantics.symbols.at(contract.parameter).reference = true;
    if (mode == 5)
      contract.origin = f.payloadParameterRegions[1].origin;
    try {
      (void)core::verify(std::move(forged));
      throw std::runtime_error("forged input contract accepted");
    } catch (const Error &error) {
      require(error.code == "E9001", "wrong input contract forgery diagnostic");
    }
  }
  for (unsigned mode = 0; mode != 6; ++mode) {
    auto forged = p;
    auto &graph = *std::find_if(forged.regionGraphs.begin(), forged.regionGraphs.end(),
                                [&](const auto &item) { return item.function == fid; });
    auto &occurrence = graph.carriers.transitions.at(reborrowPoint).before.at(source);
    if (mode == 0)
      occurrence.capability = Access::Shared;
    if (mode == 1)
      occurrence.origins.clear();
    if (mode == 2)
      occurrence.regions.clear();
    if (mode == 3)
      occurrence.present = false;
    if (mode == 4)
      graph.parentOperations.at(reborrowPoint).seeds.clear();
    if (mode == 5)
      graph.loanParents.clear();
    try {
      regions::validateBounds(forged);
      throw std::runtime_error("forged input proof accepted");
    } catch (const Error &error) {
      require(error.code == "E9001", "wrong input proof forgery diagnostic");
    }
  }
  std::ostringstream erased;
  runtime::print(erase(verified), erased);
  for (auto proof :
       {"payload.region", "parent obligation", "RegionId", "LoanId", "carrier instance"})
    require(erased.str().find(proof) == std::string::npos, "input contract survived erasure");
}
