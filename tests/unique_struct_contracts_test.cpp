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
  auto require = [](bool condition, const char *message) {
    if (!condition)
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
  auto verified = core::verify(make("valid_result.isa"));
  const auto &p = verified.get();
  const auto &f =
      *std::find_if(p.semantics.functions.begin(), p.semantics.functions.end(), [&](const auto &f) {
        return f.resultBorrowedContainer &&
               p.semantics.types[f.result].kind == hir::Type::Structure;
      });
  require(p.semantics.exclusivePayloadResult(f), "missing affine exclusive result contract");
  auto transfer = p.semantics.referenceTransfers(f);
  require(transfer.size() == 1 &&
              transfer.begin()->second ==
                  std::set<hir::ReferenceSource>{{f.resultOrigin, {}, f.resultOrigin}},
          "single output did not retain exactly one direct Unique origin");
  bool call = false;
  for (const auto &definition : p.functions) {
    const auto &g = *std::find_if(p.regionGraphs.begin(), p.regionGraphs.end(),
                                  [&](const auto &g) { return g.function == definition.id; });
    for (const auto &i : definition.body)
      if (i.kind == core::Inst::Call && i.function == f.id) {
        require(i.carriers.size() == 1, "exclusive result lost its carrier");
        const auto &provenance = i.carriers.begin()->second;
        require(provenance.capability == Access::Unique && provenance.parent != None &&
                    provenance.origins.size() == 1,
                "call did not retain its suspended input parent");
        const auto &op = g.parentOperations.at(i.semanticPoint);
        require(op.seeds.at(i.carriers.begin()->first).contains(provenance.parent),
                "CFG omitted the result's parent dependency");
        const auto &args = g.valueRegions.at(i.result);
        require(args.contains(i.carriers.begin()->first) && args.begin()->second.size() == 1,
                "result region lost its field path");
        call = true;
      }
  }
  require(call, "missing Unique result call");
  for (unsigned mode = 0; mode != 5; ++mode) {
    auto forged = make("valid_result.isa");
    auto &target = forged.semantics.functions.at(f.id);
    if (mode == 0)
      target.resultAccess = Access::Shared;
    if (mode == 1)
      target.resultRegion = None;
    if (mode == 2)
      target.additionalResultOrigins.push_back(target.resultOrigin);
    if (mode == 3)
      forged.semantics.symbols.at(target.resultOrigin).access = Access::Shared;
    if (mode == 4)
      target.parameterRegions.clear();
    try {
      (void)core::verify(std::move(forged));
      throw std::runtime_error("forged affine result accepted");
    } catch (const Error &error) {
      require(error.code == "E9001", "wrong affine result forgery diagnostic");
    }
  }
  for (unsigned mode = 0; mode != 3; ++mode) {
    auto forged = p;
    bool changed = false;
    for (auto &g : forged.regionGraphs)
      for (auto &[point, op] : g.parentOperations)
        if (!changed && op.kind == regions::ParentOperation::Seed) {
          if (mode == 0)
            op.seeds.clear();
          if (mode == 1)
            g.parentStates.clear();
          if (mode == 2)
            g.parentUses.clear();
          changed = true;
        }
    require(changed, "no suspended parent proof to forge");
    try {
      regions::validateBounds(forged);
      throw std::runtime_error("forged result parent accepted");
    } catch (const Error &error) {
      require(error.code == "E9001", "wrong parent forgery diagnostic");
    }
  }
  auto receiver = core::verify(make("valid_receivers.isa"));
  auto raw = make("invalid_receiver_change.isa");

  for (auto &definition : raw.functions) {
    const auto &signature = raw.semantics.functions[definition.id];
    if (signature.payloadParameterRegions.empty())
      continue;
    const auto &input = signature.payloadParameterRegions.front();
    core::CarrierMap expected{{input.path, {{input.origin}, Access::Unique}}};
    for (auto &i : definition.body)
      i.carriers = expected;
    const auto &checked =
        *std::find_if(receiver.get().functions.begin(), receiver.get().functions.end(),
                      [&](const auto &fn) { return !fn.fieldPostStates.empty(); });
    definition.fieldPostStates = checked.fieldPostStates;
  }
  try {
    (void)core::verify(std::move(raw));
    throw std::runtime_error("forged receiver preservation accepted");
  } catch (const Error &error) {
    require(error.code == "E0310", "wrong receiver preservation diagnostic");
  }
  std::ostringstream erased;
  runtime::print(erase(verified), erased);
  for (auto proof :
       {"reference.transfer", "reference.preserve", "parent obligation", "RegionId", "LoanId"})
    require(erased.str().find(proof) == std::string::npos, "reference proof survived erasure");
}
