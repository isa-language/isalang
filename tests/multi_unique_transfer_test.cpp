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
  using storage::Location;
  using Op = regions::ParentOperation;
  if (argc != 2)
    return 1;
  auto require = [](bool condition, const char *message) {
    if (!condition)
      throw std::runtime_error(message);
  };
  auto make = [&](const char *name = "valid_forward.isa") {
    auto path = std::filesystem::path(argv[1]) / name;
    std::ifstream input(path);
    std::string source((std::istreambuf_iterator<char>(input)), {});
    auto tokens = lex(source);
    for (auto &token : tokens)
      token.loc.file = path.string();
    return core::elaborate(resolve(modules::compile(tokens).ast));
  };
  auto verified = core::verify(make());
  const auto &p = verified.get();
  const auto &f = *std::find_if(p.semantics.functions.begin(), p.semantics.functions.end(),
                                [](const auto &f) { return f.name == "forward"; });
  require(p.semantics.exclusivePayloadResult(f) && f.payloadResultSources().size() == 2 &&
              f.additionalResultOrigins.size() == 1,
          "missing injective multi-leaf contract");
  const auto transfers = p.semantics.referenceTransfers(f);
  require(transfers.size() == 2, "contract collapsed the output positions");
  bool call = false, contract = false;
  for (const auto &definition : p.functions) {
    const auto &g = *std::find_if(p.regionGraphs.begin(), p.regionGraphs.end(),
                                  [&](const auto &g) { return g.function == definition.id; });
    if (definition.id == f.id) {
      require(g.contract.result.size() == 2, "formal output regions were collapsed");
      std::set<RegionId> all;
      for (const auto &[path, sources] : transfers) {
        require(sources.size() == 1, "affine source is ambiguous");
        const auto &source = *sources.begin();
        const auto &regions = g.contract.result.at(path);
        require(regions.size() == 1 && all.insert(*regions.begin()).second &&
                    g.contract.resultSources.at(*regions.begin()) ==
                        g.contract.inputs.at(source.parameter).at(source.path),
                "output region was bound to other positions or merged with its sibling");
      }
      contract = true;
    }
    storage::detail::Places places(p, g, definition);
    for (auto *i : places.instructions)
      if (i && i->kind == core::Inst::Call && i->function == f.id) {
        const auto &op = g.parentOperations.at(i->semanticPoint);
        require(op.kind == Op::Forward && op.transfers.size() == 2 && op.seeds.empty() &&
                    op.inputs == i->args,
                "call is not a simultaneous positional transfer");
        const auto &step = g.carriers.transitions.at(i->semanticPoint);
        std::set<SymbolId> parents;
        std::set<carriers::CarrierId> occurrences;
        for (const auto &[path, source] : op.transfers) {
          const auto &before = step.before.at(source);
          const auto &after = step.after.at(storage::detail::append(op.destination, path));
          require(before.capability == Access::Unique && before.parent != None &&
                      parents.insert(before.parent).second && before.origins == after.origins &&
                      before.parent == after.parent && before.instances == after.instances &&
                      !step.after.contains(source),
                  "call duplicated, lost or confused exclusive obligations");
          for (auto id : after.instances)
            require(occurrences.insert(id).second, "outputs share an exclusive occurrence");
          require(g.parentStates.at(i->semanticPoint).at(source).contains(before.parent) &&
                      g.parentUses.at(i->semanticPoint).contains(before.parent),
                  "CFG omitted a suspended parent");
          require(g.valueRegions.at(i->result).at(path) == after.regions,
                  "output lost its positional region arguments");
          require(std::any_of(step.ends.begin(), step.ends.end(),
                              [&](const auto &end) {
                                return end.position == source &&
                                       end.kind == carriers::EndKind::Transfer;
                              }),
                  "exported loan was consumed instead of transferred");
        }
        call = true;
      }
  }
  require(call && contract, "did not inspect both the caller and callee contract");
  for (unsigned mode = 0; mode != 8; ++mode) {
    auto forged = make();
    auto &target = forged.semantics.functions.at(f.id);
    if (mode == 0)
      target.additionalPayloadSources.clear();
    if (mode == 1)
      target.additionalResultOrigins.clear();
    if (mode == 2)
      std::swap(target.resultOrigin, target.additionalResultOrigins.front());
    if (mode == 3)
      target.resultPayloadSource->path.clear();
    if (mode == 4)
      target.resultAccess = Access::Shared;
    if (mode == 5)
      forged.semantics.symbols.at(target.additionalResultOrigins.front()).access = Access::Shared;
    if (mode == 6)
      target.additionalResultOrigins.front() = target.resultOrigin;
    if (mode == 7)
      target.additionalPayloadSources.front() = *target.resultPayloadSource;
    try {
      (void)core::verify(std::move(forged));
      throw std::runtime_error("forged injective contract accepted");
    } catch (const Error &error) {
      require(error.code == "E9001", "wrong signature diagnostic");
    }
  }
  for (unsigned mode = 0; mode != 8; ++mode) {
    auto forged = p;
    bool changed = false;
    for (auto &g : forged.regionGraphs)
      for (auto &[point, op] : g.parentOperations)
        if (!changed && op.kind == Op::Forward && op.transfers.size() == 2) {
          auto first = op.transfers.begin(), second = std::next(first);
          if (mode == 0)
            op.transfers.erase(first);
          if (mode == 1)
            std::swap(first->second, second->second);
          if (mode == 2)
            second->second = first->second;
          if (mode == 3)
            op.transfers[{}] = first->second;
          if (mode == 4)
            first->second.id = op.destination.id;
          if (mode == 5)
            op.inputs.clear();
          if (mode == 6)
            g.parentStates.clear();
          if (mode == 7)
            g.parentUses.clear();
          changed = true;
        }
    require(changed, "no multi-leaf parent operation to forge");
    try {
      regions::validateBounds(forged);
      throw std::runtime_error("forged multi-leaf parent transfer accepted");
    } catch (const Error &error) {
      require(error.code == "E9001", "wrong parent transfer diagnostic");
    }
  }
  for (unsigned mode = 0; mode != 7; ++mode) {
    auto forged = p;
    bool changed = false;
    for (auto &g : forged.regionGraphs)
      for (const auto &[point, op] : g.parentOperations)
        if (!changed && op.kind == Op::Forward && op.transfers.size() == 2) {
          auto first = op.transfers.begin(), second = std::next(first);
          auto a = storage::detail::append(op.destination, first->first);
          auto b = storage::detail::append(op.destination, second->first);
          auto &step = g.carriers.transitions.at(point);
          if (mode == 0)
            step.after.at(a).parent = None;
          if (mode == 1)
            step.after.at(a).instances = step.after.at(b).instances;
          if (mode == 2)
            step.after.at(a).regions = step.after.at(b).regions;
          if (mode == 3)
            step.after.erase(a);
          if (mode == 4)
            step.after[first->second] = step.after.at(a);
          if (mode == 5)
            step.after.at(a).origins = step.after.at(b).origins;
          if (mode == 6)
            step.ends.front().kind = carriers::EndKind::Consume;
          changed = true;
        }
    require(changed, "no multi-leaf carrier transition to forge");
    try {
      carriers::validate(forged);
      throw std::runtime_error("forged multi-leaf occurrence accepted");
    } catch (const Error &error) {
      require(error.code == "E9001", "wrong carrier diagnostic");
    }
  }
  for (const char *name : {"invalid_local.isa", "invalid_owned_storage.isa"}) {
    auto raw = make(name);
    for (auto &definition : raw.functions)
      if (raw.semantics.functions.at(definition.id).name == "forward") {
        const auto &signature = raw.semantics.functions.at(definition.id);
        core::CarrierMap expected;
        for (const auto &[path, sources] : raw.semantics.referenceTransfers(signature))
          expected[path] = {{sources.begin()->origin}, Access::Unique};
        for (auto &i : definition.body)
          i.carriers = expected;
      }
    try {
      (void)core::verify(std::move(raw));
      throw std::runtime_error("forged escaping result accepted");
    } catch (const Error &error) {
      require(error.code == "E6003", "wrong escape diagnostic");
    }
  }
  std::ostringstream erased;
  runtime::print(erase(verified), erased);
  for (auto proof :
       {"reference.transfer", "parent.forward", "parent obligation", "RegionId", "LoanId"})
    require(erased.str().find(proof) == std::string::npos, "exclusive proof survived erasure");
}
