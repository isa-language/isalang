#include "isac/core.hpp"
#include "isac/erasure.hpp"
#include "isac/modules.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

int main(int argc, char **argv) {
  using namespace isac;
  if (argc != 2)
    return 1;
  auto require = [](bool condition, const char *message) {
    if (!condition)
      throw std::runtime_error(message);
  };
  auto make = [&](const char *name = "valid_swap.isa") {
    auto path = std::filesystem::path(argv[1]) / name;
    std::ifstream input(path);
    std::string source((std::istreambuf_iterator<char>(input)), {});
    auto tokens = lex(source);
    for (auto &token : tokens)
      token.loc.file = path.string();
    return core::elaborate(resolve(modules::compile(tokens).ast));
  };
  auto raw = make();
  const auto &signature =
      *std::find_if(raw.semantics.functions.begin(), raw.semantics.functions.end(),
                    [](const auto &f) { return f.name == "forward"; });
  const auto id = signature.id;
  require(signature.inferredTransfers && !signature.transferInferencePending,
          "routing was not completed before Core");
  const auto candidates = raw.semantics.referenceTransferCandidates(signature);
  const auto selected = raw.semantics.referenceTransfers(signature);
  require(candidates.size() == 2 && selected.size() == 2, "missing same-type output leaves");
  std::set<hir::ReferenceSource> used;
  for (const auto &[path, sources] : selected) {
    require(candidates.at(path).size() == 2 && sources.size() == 1 &&
                used.insert(*sources.begin()).second,
            "routing guessed an origin or duplicated a capability");
    require(path.front().index != sources.begin()->path.front().index,
            "swap was inferred as an identity");
  }
  auto verified = core::verify(std::move(raw));
  const auto &p = verified.get();
  const auto &graph = *std::find_if(p.regionGraphs.begin(), p.regionGraphs.end(),
                                    [&](const auto &g) { return g.function == id; });
  for (const auto &[path, sources] : selected) {
    const auto &source = *sources.begin();
    auto output = *graph.contract.result.at(path).begin();
    require(graph.contract.resultSources.at(output) ==
                graph.contract.inputs.at(source.parameter).at(source.path),
            "disambiguated output region retained unrelated candidates");
  }
  for (unsigned mode = 0; mode != 8; ++mode) {
    auto forged = make();
    auto &f = forged.semantics.functions.at(id);
    auto &mapping = *f.inferredTransfers;
    if (mode == 0)
      f.transferInferencePending = true;
    if (mode == 1)
      f.inferredTransfers.reset();
    if (mode == 2)
      mapping.erase(mapping.begin());
    if (mode == 3)
      mapping.begin()->second = candidates.begin()->second;
    if (mode == 4) {
      auto source = *mapping.begin()->second.begin();
      source.origin = None;
      mapping.begin()->second = {source};
    }
    if (mode == 5)
      f.additionalResultOrigins.clear();
    if (mode == 6)
      f.resultAccess = Access::Shared;
    if (mode == 7) {
      auto source = *mapping.begin()->second.begin();
      source.kind = hir::ReferenceSourceKind::Parameter;
      mapping.begin()->second = {source};
    }
    try {
      (void)core::verify(std::move(forged));
      throw std::runtime_error("forged routing shape accepted");
    } catch (const Error &error) {
      require(error.code == "E9001", "wrong routing shape diagnostic");
    }
  }

  for (unsigned mode = 0; mode != 2; ++mode) {
    auto forged = make();
    auto &f = forged.semantics.functions.at(id);
    auto &mapping = *f.inferredTransfers;
    std::swap(mapping.begin()->second, std::next(mapping.begin())->second);
    if (mode)
      for (auto &definition : forged.functions)
        if (definition.id == id) {
          core::CarrierMap expected;
          for (const auto &[path, sources] : mapping)
            expected[path] = {{sources.begin()->origin}, Access::Unique};
          for (auto &i : definition.body)
            i.carriers = expected;
        }
    try {
      (void)core::verify(std::move(forged));
      throw std::runtime_error("false inferred routing accepted");
    } catch (const Error &error) {
      require(error.code == "E6003", "effective return provenance was not reconfirmed");
    }
  }
  for (unsigned mode = 0; mode != 3; ++mode) {
    auto forged = p;
    auto &g = *std::find_if(forged.regionGraphs.begin(), forged.regionGraphs.end(),
                            [&](const auto &g) { return g.function == id; });
    if (mode == 0)
      std::swap(g.contract.resultSources.begin()->second,
                std::next(g.contract.resultSources.begin())->second);
    if (mode == 1)
      g.contract.resultSources.begin()->second.clear();
    if (mode == 2)
      std::swap(g.contract.result.begin()->second, std::next(g.contract.result.begin())->second);
    try {
      regions::validateBounds(forged);
      throw std::runtime_error("forged inferred region contract accepted");
    } catch (const Error &error) {
      require(error.code == "E9001", "wrong inferred region contract diagnostic");
    }
  }

  auto selection = core::verify(make("valid_selection.isa"));
  const auto &program = selection.get();
  const auto &f =
      *std::find_if(program.semantics.functions.begin(), program.semantics.functions.end(),
                    [](const auto &f) { return f.name == "select"; });
  require(f.payloadParameterRegions.size() == 2 && f.payloadResultSources().size() == 1 &&
              f.payloadResultSources().front().path.front().index == 1,
          "noescape input was retained as an output source");
  std::ostringstream erased;
  runtime::print(erase(verified), erased);
  for (auto proof :
       {"reference.routing", "reference.transfer", "parent.forward", "RegionId", "LoanId"})
    require(erased.str().find(proof) == std::string::npos,
            "inferred routing proof survived erasure");
}
