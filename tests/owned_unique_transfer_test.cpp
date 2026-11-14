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
  require(f.resultPayloadSource && p.semantics.exclusivePayloadResult(f),
          "missing owned input affine result contract");
  const auto &source = *f.resultPayloadSource;
  auto transfers = p.semantics.referenceTransfers(f);
  require(transfers.size() == 1 &&
              transfers.begin()->second ==
                  std::set<hir::ReferenceSource>{{source.parameter, source.path, f.resultOrigin,
                                                  hir::ReferenceSourceKind::Carrier}},
          "signature did not retain its one positional source");
  bool checked = false;
  for (const auto &definition : p.functions) {
    const auto &g = *std::find_if(p.regionGraphs.begin(), p.regionGraphs.end(),
                                  [&](const auto &g) { return g.function == definition.id; });
    storage::detail::Places places(p, g, definition);
    for (auto *i : places.instructions)
      if (i && i->kind == core::Inst::Call && i->function == f.id) {
        const auto &op = g.parentOperations.at(i->semanticPoint);
        require(op.kind == Op::Forward && op.seeds.empty() && op.inputs == i->args &&
                    op.transfers ==
                        std::map<hir::ReferencePath, Location>{
                            {transfers.begin()->first,
                             storage::detail::append(Location{Location::Value, i->args.at(0)},
                                                     source.path)}},
                "call created a new parent or consumed the wrong position");
        const auto &step = g.carriers.transitions.at(i->semanticPoint);
        const auto &place = op.transfers.begin()->second;
        const auto &before = step.before.at(place);
        auto output = storage::detail::append(op.destination, op.transfers.begin()->first);
        const auto &after = step.after.at(output);
        require(before.capability == Access::Unique && before.parent != None &&
                    before.parent == after.parent && before.origins == after.origins &&
                    before.instances == after.instances && !step.after.contains(place),
                "call lost, copied or resumed the affine input occurrence");
        require(g.parentStates.at(i->semanticPoint).at(place).contains(before.parent),
                "input did not carry its original suspended parent");
        require(g.valueRegions.at(i->result).at(output.path) == after.regions,
                "returned occurrence lost its positional regions");
        require(std::any_of(step.ends.begin(), step.ends.end(),
                            [&](const auto &end) {
                              return end.position == place &&
                                     end.kind == carriers::EndKind::Transfer;
                            }),
                "forwarded carrier ended as a consumed/dropped loan");
        checked = true;
      }
  }
  require(checked, "no affine call validated");
  for (unsigned mode = 0; mode != 6; ++mode) {
    auto forged = make();
    auto &target = forged.semantics.functions.at(f.id);
    if (mode == 0)
      target.resultPayloadSource->path.clear();
    if (mode == 1)
      target.resultPayloadSource->parameter = target.resultOrigin;
    if (mode == 2)
      target.resultAccess = Access::Shared;
    if (mode == 3)
      target.payloadParameterRegions.clear();
    if (mode == 4)
      target.resultRegion = None;
    if (mode == 5)
      target.additionalResultOrigins.push_back(target.resultOrigin);
    try {
      (void)core::verify(std::move(forged));
      throw std::runtime_error("forged affine signature accepted");
    } catch (const Error &error) {
      require(error.code == "E9001", "wrong affine signature diagnostic");
    }
  }
  for (unsigned mode = 0; mode != 7; ++mode) {
    auto forged = p;
    bool changed = false;
    for (auto &g : forged.regionGraphs)
      for (auto &[point, op] : g.parentOperations)
        if (!changed && op.kind == Op::Forward) {
          if (mode == 0)
            op.transfers.begin()->second.path.clear();
          if (mode == 1) {
            auto source = op.transfers.begin()->second;
            op.transfers.clear();
            op.transfers[{}] = source;
          }
          if (mode == 2)
            op.inputs.clear();
          if (mode == 3)
            op.kind = Op::Consume;
          if (mode == 4)
            op.destination = op.transfers.begin()->second;
          if (mode == 5)
            g.parentStates.clear();
          if (mode == 6)
            g.parentUses.clear();
          changed = true;
        }
    require(changed, "no parent transfer to forge");
    try {
      regions::validateBounds(forged);
      throw std::runtime_error("forged parent forwarding accepted");
    } catch (const Error &error) {
      require(error.code == "E9001", "wrong parent transfer diagnostic");
    }
  }
  for (unsigned mode = 0; mode != 4; ++mode) {
    auto forged = p;
    bool changed = false;
    for (auto &g : forged.regionGraphs)
      for (const auto &[point, op] : g.parentOperations)
        if (!changed && op.kind == Op::Forward) {
          const auto &place = op.transfers.begin()->second;
          auto output = storage::detail::append(op.destination, op.transfers.begin()->first);
          auto &step = g.carriers.transitions.at(point);
          auto &binding = step.after.at(output);
          if (mode == 0)
            binding.parent = None;
          if (mode == 1)
            binding.instances.clear();
          if (mode == 2)
            binding.capability = Access::Shared;
          if (mode == 3)
            step.after[place] = binding;
          changed = true;
        }
    require(changed, "no carrier transfer to forge");
    try {
      carriers::validate(forged);
      throw std::runtime_error("forged affine occurrence accepted");
    } catch (const Error &error) {
      require(error.code == "E9001", "wrong affine occurrence diagnostic");
    }
  }

  for (const char *name : {"invalid_local.isa", "invalid_owned_storage_heap.isa"}) {
    auto invalid = make(name);
    for (auto &definition : invalid.functions)
      if (definition.id == f.id)
        for (auto &i : definition.body)
          i.carriers = {{transfers.begin()->first, {{f.resultOrigin}, Access::Unique}}};
    try {
      (void)core::verify(std::move(invalid));
      throw std::runtime_error("forged local/owned input result accepted");
    } catch (const Error &error) {
      require(error.code == "E6003", "wrong local/owned input result diagnostic");
    }
  }
  std::ostringstream erased;
  runtime::print(erase(verified), erased);
  for (auto proof :
       {"reference.transfer", "parent.forward", "parent obligation", "RegionId", "LoanId"})
    require(erased.str().find(proof) == std::string::npos, "affine proof survived erasure");
}
