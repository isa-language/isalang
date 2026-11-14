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
  auto make = [&](const char *name = "valid_while.isa") {
    auto path = std::filesystem::path(argv[1]) / name;
    std::ifstream input(path);
    std::string source((std::istreambuf_iterator<char>(input)), {});
    auto tokens = lex(source);
    for (auto &token : tokens)
      token.loc.file = path.string();
    return core::elaborate(resolve(modules::compile(tokens).ast));
  };
  auto signature = [&](const auto &p, const char *name = "forward") -> const hir::Function & {
    return *std::find_if(p.semantics.functions.begin(), p.semantics.functions.end(),
                         [&](const auto &f) { return f.name == name; });
  };
  auto verified = core::verify(make());
  const auto &p = verified.get();
  const auto &f = signature(p);
  const auto selected = p.semantics.referenceTransfers(f);
  require(f.inferredTransfers && !f.transferInferencePending && selected.size() == 2,
          "typed CFG routing did not finish before Core verification");
  for (const auto &[path, sources] : selected)
    require(sources.size() == 1 && path.front().index != sources.begin()->path.front().index,
            "loop swap routing was guessed as identity");
  const auto &graph = *std::find_if(p.regionGraphs.begin(), p.regionGraphs.end(),
                                    [&](const auto &g) { return g.function == f.id; });
  require(
      std::any_of(graph.blocks.begin(), graph.blocks.end(),
                  [](const auto &b) { return b.terminator == regions::Terminator::LoopBackedge; }),
      "rich CFG lost the backedge required for lifetime proof");
  for (const auto &[path, sources] : selected) {
    auto output = *graph.contract.result.at(path).begin();
    const auto &source = *sources.begin();
    require(graph.contract.resultSources.at(output) ==
                graph.contract.inputs.at(source.parameter).at(source.path),
            "output region includes unrelated compatible input positions");
  }

  auto fields = core::verify(make("valid_field_positions.isa"));
  const auto &fieldProgram = fields.get();
  const auto &fieldFunction = signature(fieldProgram);
  const auto &shape = fieldProgram.semantics.types.at(
      fieldProgram.semantics.symbols.at(fieldFunction.params[0]).type);
  require(shape.fields.at(1).offset == 2 && shape.fields.at(2).offset == 3,
          "fixture does not separate positions and layout");
  auto fieldRoutes = fieldProgram.semantics.referenceTransfers(fieldFunction);
  require(fieldRoutes.size() == 2 && fieldRoutes.begin()->first.front().index == 1 &&
              fieldRoutes.begin()->second.begin()->path.front().index == 2 &&
              std::next(fieldRoutes.begin())->first.front().index == 2 &&
              std::next(fieldRoutes.begin())->second.begin()->path.front().index == 1,
          "routing interpreted flattened layout offsets as semantic paths");

  for (auto name : {"valid_match_option.isa", "valid_match_result.isa"}) {
    auto matched = core::verify(make(name));
    const auto &program = matched.get();
    const auto &s = signature(program, "select");
    auto routes = program.semantics.referenceTransfers(s);
    require(s.inferredTransfers && routes.size() == 1 && routes.begin()->second.size() == 1,
            "match alias lost or invented a source");
    const auto &source = *routes.begin()->second.begin();
    require(source.path.size() == 2 &&
                source.path[1].kind == hir::ReferenceProjectionKind::VariantPayload &&
                source.path[0].index == (std::string(name) == "valid_match_option.isa" ? 1u : 0u),
            "payload binding was detached from its original input field");
  }
  for (unsigned mode = 0; mode != 7; ++mode) {
    auto forged = make();
    auto &s = forged.semantics.functions.at(f.id);
    auto &mapping = *s.inferredTransfers;
    if (mode == 0)
      s.transferInferencePending = true;
    if (mode == 1)
      s.inferredTransfers.reset();
    if (mode == 2)
      mapping.erase(mapping.begin());
    if (mode == 3)
      mapping.begin()->second = forged.semantics.referenceTransferCandidates(s).begin()->second;
    if (mode == 4) {
      auto source = *mapping.begin()->second.begin();
      source.origin = None;
      mapping.begin()->second = {source};
    }
    if (mode == 5)
      s.additionalResultOrigins.clear();
    if (mode == 6)
      s.resultAccess = Access::Shared;
    try {
      (void)core::verify(std::move(forged));
      throw std::runtime_error("invalid routing shape accepted");
    } catch (const Error &error) {
      require(error.code == "E9001", "wrong routing shape diagnostic");
    }
  }

  for (auto name : {"valid_while.isa", "valid_match_bool.isa", "valid_field_positions.isa"})
    for (bool fakeSnapshots : {false, true}) {
      auto forged = make(name);
      auto id =
          signature(forged, std::string(name) == "valid_match_bool.isa" ? "choose" : "forward").id;
      auto &mapping = *forged.semantics.functions.at(id).inferredTransfers;
      std::swap(mapping.begin()->second, std::next(mapping.begin())->second);
      if (fakeSnapshots) {
        core::CarrierMap expected;
        for (const auto &[path, sources] : mapping)
          expected[path] = {{sources.begin()->origin}, Access::Unique};
        auto patch = [&](auto &&self, std::vector<core::Inst> &body) -> void {
          for (auto &i : body) {
            i.carriers = expected;
            self(self, i.body);
            self(self, i.otherwise);
            self(self, i.condition);
            self(self, i.latch);
          }
        };
        for (auto &definition : forged.functions)
          if (definition.id == id)
            patch(patch, definition.body);
      }
      try {
        (void)core::verify(std::move(forged));
        throw std::runtime_error("false CFG routing accepted");
      } catch (const Error &error) {
        require(error.code == "E6003", "Core did not reconfirm effective CFG return origins");
      }
    }
  for (unsigned mode = 0; mode != 3; ++mode) {
    auto forged = p;
    auto &g = *std::find_if(forged.regionGraphs.begin(), forged.regionGraphs.end(),
                            [&](const auto &g) { return g.function == f.id; });
    if (mode == 0)
      std::swap(g.contract.resultSources.begin()->second,
                std::next(g.contract.resultSources.begin())->second);
    if (mode == 1)
      g.contract.resultSources.begin()->second.clear();
    if (mode == 2)
      std::swap(g.contract.result.begin()->second, std::next(g.contract.result.begin())->second);
    try {
      regions::validateBounds(forged);
      throw std::runtime_error("forged CFG region contract accepted");
    } catch (const Error &error) {
      require(error.code == "E9001", "wrong CFG region diagnostic");
    }
  }
  for (unsigned mode = 0; mode != 3; ++mode) {
    auto forged = p;
    bool changed = false;
    for (auto &g : forged.regionGraphs)
      for (auto &[point, op] : g.parentOperations)
        if (!changed && op.kind == regions::ParentOperation::Forward && op.transfers.size() == 2) {
          auto first = op.transfers.begin(), second = std::next(first);
          if (mode == 0)
            op.transfers.erase(first);
          if (mode == 1)
            std::swap(first->second, second->second);
          if (mode == 2)
            second->second = first->second;
          changed = true;
        }
    require(changed, "no positional forwarding proof to corrupt");
    try {
      regions::validateBounds(forged);
      throw std::runtime_error("forged parent transfer accepted");
    } catch (const Error &error) {
      require(error.code == "E9001", "wrong parent transfer diagnostic");
    }
  }

  std::string stress =
      "struct Views{a:&mut i32,b:&mut i32,} fn route(input:Views)->Views{let i=0;while i<2{";
  for (unsigned n = 0; n != 1000; ++n)
    stress += "i=i+1;";
  stress += "}return input.move();}fn main(){}";
  auto resolved = resolve(modules::compile(lex(stress)).ast);
  const auto &route = *std::find_if(resolved.functions.begin(), resolved.functions.end(),
                                    [](const auto &s) { return s.name == "route"; });
  require(route.inferredTransfers && resolved.referenceTransfers(route).size() == 2,
          "scalar-heavy CFG failed reference routing");
  std::ostringstream erased;
  runtime::print(erase(verified), erased);
  for (auto proof :
       {"reference.routing", "reference.transfer", "parent.forward", "RegionId", "LoanId"})
    require(erased.str().find(proof) == std::string::npos, "CFG routing proof survived erasure");
}
