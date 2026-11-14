#include "isac/core.hpp"
#include "isac/erasure.hpp"
#include "isac/modules.hpp"
#include "../src/storage_paths.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

int main(int argc, char **argv) {
  using namespace isac;
  using Op = regions::ParentOperation;
  if (argc != 2)
    return 1;
  auto require = [](bool condition, const char *message) {
    if (!condition)
      throw std::runtime_error(message);
  };
  auto make = [&](const char *name = "valid_selection.isa") {
    auto path = std::filesystem::path(argv[1]) / name;
    std::ifstream input(path);
    std::string source((std::istreambuf_iterator<char>(input)), {});
    auto tokens = lex(source);
    for (auto &token : tokens)
      token.loc.file = path.string();
    return core::elaborate(resolve(modules::compile(tokens).ast));
  };
  auto signature = [&](const auto &p, const char *name = "select") -> const hir::Function & {
    return *std::find_if(p.semantics.functions.begin(), p.semantics.functions.end(),
                         [&](const auto &f) { return f.name == name; });
  };
  auto verified = core::verify(make());
  const auto &p = verified.get();
  const auto &f = signature(p);
  const auto transfers = p.semantics.referenceTransfers(f);
  require(f.inferredTransfers && p.semantics.exclusivePayloadResult(f) && transfers.size() == 2,
          "missing mixed positional contract");
  require(p.semantics.referenceResultAccess(f, transfers.begin()->first) == Access::Shared &&
              p.semantics.referenceResultAccess(f, std::next(transfers.begin())->first) ==
                  Access::Unique,
          "summary capability replaced a field capability");
  bool inspected = false;
  for (const auto &definition : p.functions) {
    const auto &g = *std::find_if(p.regionGraphs.begin(), p.regionGraphs.end(),
                                  [&](const auto &g) { return g.function == definition.id; });
    if (definition.id == f.id)
      for (const auto &[path, sources] : transfers) {
        const auto &source = *sources.begin();
        require(g.contract.resultSources.at(*g.contract.result.at(path).begin()) ==
                    g.contract.inputs.at(source.parameter).at(source.path),
                "mixed output lost its own regional bound");
      }
    storage::detail::Places places(p, g, definition);
    for (auto *i : places.instructions)
      if (i && i->kind == core::Inst::Call && i->function == f.id) {
        const auto &op = g.parentOperations.at(i->semanticPoint);
        require(op.kind == Op::Forward && op.transfers.size() == 2,
                "missing positional parent flow");
        const auto &step = g.carriers.transitions.at(i->semanticPoint);
        for (const auto &[path, source] : op.transfers) {
          auto destination = storage::detail::append(op.destination, path);
          const auto &before = step.before.at(source), &after = step.after.at(destination);
          auto capability = p.semantics.referenceResultAccess(f, path);
          require(before.capability == capability && after.capability == capability &&
                      before.origins == after.origins,
                  "call upgraded, downgraded or lost a reference capability");
          require(g.valueRegions.at(i->result).at(path) == after.regions &&
                      !step.after.contains(source),
                  "call lost positional regions or retained a consumed source");
          auto end = std::find_if(step.ends.begin(), step.ends.end(),
                                  [&](const auto &e) { return e.position == source; });
          require(end != step.ends.end(), "input carrier has no disposition");
          if (capability == Access::Unique)
            require(before.parent != None && before.parent == after.parent &&
                        before.instances == after.instances &&
                        end->kind == carriers::EndKind::Transfer,
                    "Unique obligation was recreated or parent resumed");
          else
            require(after.parent == None && before.instances != after.instances &&
                        end->kind == carriers::EndKind::Consume,
                    "Shared result reused an exclusive transfer or an input occurrence");
        }
        inspected = true;
      }
  }
  require(inspected, "did not inspect the mixed call");
  unsigned corruptions = 0;
  auto reject = [&](core::Program forged, const char *code) {
    try {
      (void)core::verify(std::move(forged));
      throw std::runtime_error("forged mixed contract accepted");
    } catch (const Error &error) {
      require(error.code == code, "wrong mixed contract diagnostic");
    }
    ++corruptions;
  };
  for (unsigned mode = 0; mode != 8; ++mode) {
    auto forged = make();
    auto &s = forged.semantics.functions.at(f.id);
    auto &map = *s.inferredTransfers;
    if (mode == 0)
      s.transferInferencePending = true;
    if (mode == 1)
      s.inferredTransfers.reset();
    if (mode == 2)
      map.erase(map.begin());
    if (mode == 3)
      map.begin()->second = std::next(map.begin())->second;
    if (mode == 4)
      s.resultAccess = Access::Shared;
    if (mode == 5)
      s.additionalResultOrigins.clear();
    if (mode == 6)
      forged.semantics.symbols.at(map.begin()->second.begin()->origin).access = Access::Unique;
    if (mode == 7)
      s.payloadParameterRegions.pop_back();
    reject(std::move(forged), "E9001");
  }

  for (unsigned output = 0; output != 2; ++output)
    for (bool snapshots : {false, true}) {
      auto forged = make();
      auto &s = forged.semantics.functions.at(f.id);
      auto &map = *s.inferredTransfers;
      auto position = std::next(map.begin(), output);
      position->second = {
          *forged.semantics.referenceTransferCandidates(s).at(position->first).begin()};
      std::set<hir::ReferenceSource> sources;
      for (const auto &[path, choices] : map)
        sources.insert(choices.begin(), choices.end());
      s.resultOrigin = None;
      s.resultPayloadSource.reset();
      s.additionalResultOrigins.clear();
      s.additionalPayloadSources.clear();
      for (const auto &source : sources)
        if (s.resultOrigin == None) {
          s.resultOrigin = source.origin;
          s.resultPayloadSource = {source.parameter, source.path};
        } else {
          s.additionalResultOrigins.push_back(source.origin);
          s.additionalPayloadSources.push_back({source.parameter, source.path});
        }
      if (snapshots) {
        core::CarrierMap expected;
        for (const auto &[path, choices] : map)
          expected[path] = {{choices.begin()->origin},
                            forged.semantics.referenceResultAccess(s, path)};
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
          if (definition.id == f.id)
            patch(patch, definition.body);
      }
      reject(std::move(forged), "E6003");
    }
  for (unsigned mode = 0; mode != 7; ++mode) {
    auto forged = p;
    bool changed = false;
    for (auto &g : forged.regionGraphs)
      for (auto &[point, op] : g.parentOperations)
        if (!changed && op.kind == Op::Forward && op.transfers.size() == 2) {
          auto shared = op.transfers.begin(), unique = std::next(shared);
          auto read = storage::detail::append(op.destination, shared->first);
          auto edit = storage::detail::append(op.destination, unique->first);
          auto &step = g.carriers.transitions.at(point);
          if (mode == 0)
            shared->second = unique->second;
          if (mode == 1)
            g.valueRegions.at(op.destination.id).at(shared->first) =
                g.valueRegions.at(op.destination.id).at(unique->first);
          if (mode == 2)
            step.after.at(read).capability = Access::Unique;
          if (mode == 3)
            step.after.at(read).instances = step.after.at(edit).instances;
          if (mode == 4)
            step.after.at(edit).parent = None;
          if (mode == 5 || mode == 6)
            for (auto &end : step.ends)
              if (end.position == (mode == 5 ? shared->second : unique->second))
                end.kind = mode == 5 ? carriers::EndKind::Transfer : carriers::EndKind::Consume;
          changed = true;
        }
    require(changed, "no mixed call transition to corrupt");
    try {
      if (mode < 2)
        regions::validateBounds(forged);
      else
        carriers::validate(forged);
      throw std::runtime_error("forged mixed per-position proof accepted");
    } catch (const Error &error) {
      require(error.code == "E9001", "wrong mixed fact diagnostic");
    }
    ++corruptions;
  }
  auto copies = core::verify(make("valid_shared_copy_inferred.isa"));
  const auto &copyProgram = copies.get();
  const auto &cf = signature(copyProgram, "forward");
  auto routes = copyProgram.semantics.referenceTransfers(cf);
  require(cf.inferredTransfers && routes.size() == 3 &&
              routes.begin()->second == std::next(routes.begin())->second,
          "Shared copying was incorrectly constrained by Unique injectivity");
  bool copyChecked = false;
  for (const auto &g : copyProgram.regionGraphs)
    for (const auto &[point, op] : g.parentOperations)
      if (op.kind == Op::Forward && op.transfers.size() == 3) {
        const auto &step = g.carriers.transitions.at(point);
        auto first = storage::detail::append(op.destination, op.transfers.begin()->first);
        auto second =
            storage::detail::append(op.destination, std::next(op.transfers.begin())->first);
        require(step.after.at(first).instances != step.after.at(second).instances &&
                    step.after.at(first).origins == step.after.at(second).origins,
                "independently live Shared copies collapsed to one occurrence");
        auto forged = copyProgram;
        auto &target = *std::find_if(forged.regionGraphs.begin(), forged.regionGraphs.end(),
                                     [&](const auto &x) { return x.function == g.function; });
        target.carriers.transitions.at(point).after.at(second).instances =
            step.after.at(first).instances;
        try {
          carriers::validate(forged);
          throw std::runtime_error("forged Shared copy identity accepted");
        } catch (const Error &error) {
          require(error.code == "E9001", "wrong copied carrier diagnostic");
        }
        ++corruptions;
        copyChecked = true;
      }
  require(copyChecked && corruptions == 20, "mixed proof corruption coverage changed");
  std::ostringstream erased;
  runtime::print(erase(verified), erased);
  for (auto proof : {"reference.transfer", "capability=", "parent.forward", "RegionId", "LoanId"})
    require(erased.str().find(proof) == std::string::npos,
            "mixed reference proof survived erasure");
}
