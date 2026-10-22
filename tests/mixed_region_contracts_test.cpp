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
  auto verified = core::verify(make("valid_transfer.isa"));
  const auto &p = verified.get();
  bool mixed = false, transfer = false, cleanup = false;
  for (const auto &f : p.semantics.functions) {
    if (f.name != "forward" && f.name != "consume")
      continue;
    auto parameter = f.params.at(0);
    auto type = p.semantics.symbols[parameter].type;
    require(p.semantics.needsDrop(type) && p.semantics.borrowedInputSupported(type),
            "mixed contract lost either ownership or reference responsibility");
    require(p.semantics.managedFields(type) == std::set<uint32_t>{0, 1} &&
                p.semantics.ownedFields(type) == std::set<uint32_t>{1},
            "the reference field was treated as owning its referent");
    require(!p.semantics.borrowedPayloadLayoutSupported(type, Access::Unique),
            "Shared mixed contracts enabled Unique storage");
    const auto &g = *std::find_if(p.regionGraphs.begin(), p.regionGraphs.end(),
                                  [&](const auto &g) { return g.function == f.id; });
    require(g.fieldRegions.at(g.entry).at(parameter) == g.contract.inputs.at(parameter),
            "the mixed input was not seeded in the semantic CFG");
    const hir::ReferencePath path{{hir::ReferenceProjectionKind::Field, 0}};
    if (f.name == "forward") {
      const auto transfers = p.semantics.referenceTransfers(f);
      require(transfers.size() == 1 && transfers.at(path).size() == 1 &&
                  transfers.at(path).begin()->parameter == parameter &&
                  transfers.at(path).begin()->path == path,
              "owned storage became a reference result source");
      transfer = true;
    }
    if (f.name == "consume") {
      const auto &body = *std::find_if(p.functions.begin(), p.functions.end(),
                                       [&](const auto &cf) { return cf.id == f.id; });
      for (const auto &i : body.body)
        if (i.kind == core::Inst::Return) {
          require(std::count(i.cleanup.begin(), i.cleanup.end(), parameter) == 1,
                  "transferred mixed input did not receive exactly one cleanup");
          const auto &fields = i.fieldCleanup.at(parameter).fields;
          require(fields.at(0).skip && !fields.at(1).skip,
                  "cleanup freed a reference or skipped the owned field");
          cleanup = true;
        }
    }
    mixed = true;
  }
  require(mixed && transfer && cleanup, "missing mixed transfer/cleanup coverage");

  auto raw = make("valid_transfer.isa");
  std::function<void(std::vector<core::Inst> &)> forge = [&](auto &body) {
    for (auto &i : body) {
      i.cleanup.clear();
      for (SymbolId id = 0; id < raw.semantics.symbols.size(); ++id)
        i.fieldCleanup[id].skip = true;
      if (i.result != None)
        i.carriers[{{hir::ReferenceProjectionKind::Field, 1}}] = {{0}, Access::Unique, 0};
      forge(i.body);
      forge(i.otherwise);
      forge(i.condition);
      forge(i.latch);
    }
  };
  for (auto &f : raw.functions)
    forge(f.body);
  auto reconfirmed = core::verify(std::move(raw));
  std::ostringstream original, rebuilt;
  runtime::print(erase(verified), original);
  runtime::print(erase(reconfirmed), rebuilt);
  require(original.str() == rebuilt.str(), "producer snapshots changed mixed drop or erasure");

  for (unsigned mode = 0; mode != 3; ++mode) {
    auto invalid = make("valid_transfer.isa");
    bool changed = false;
    for (auto &f : invalid.semantics.functions)
      if (f.name == "forward") {
        if (mode == 0)
          f.resultPayloadSource->path[0].index = 1;
        if (mode == 1)
          f.payloadParameterRegions.clear();
        if (mode == 2)
          f.resultAccess = Access::Unique;
        changed = true;
      }
    require(changed, "no mixed contract to corrupt");
    try {
      core::verify(std::move(invalid));
      throw std::runtime_error("forged mixed ownership/reference contract accepted");
    } catch (const Error &error) {
      require(error.code == "E9001", "wrong mixed contract corruption diagnostic");
    }
  }
  for (auto proof : {"reference.transfer", "reference.input", "field RegionArgs",
                     "carrier instance", "RegionId", "LoanId"})
    require(original.str().find(proof) == std::string::npos,
            "mixed field lifetime metadata survived erasure");
}
