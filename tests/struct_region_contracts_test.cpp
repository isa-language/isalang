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
  auto verified = core::verify(make("valid_paths.isa"));
  const auto &p = verified.get();
  bool seeded = false, positional = false;
  for (const auto &f : p.semantics.functions) {
    if (f.name != "number")
      continue;
    const auto &g = *std::find_if(p.regionGraphs.begin(), p.regionGraphs.end(),
                                  [&](const auto &g) { return g.function == f.id; });
    const auto &input = g.contract.inputs.at(f.params[0]);
    require(input.size() == 3 && g.fieldRegions.at(g.entry).at(f.params[0]) == input,
            "struct inputs did not seed the field CFG with their positional regions");
    require(f.resultPayloadSource && f.resultPayloadSource->path.size() == 2,
            "recursive input source was truncated to an enum offset");
    auto transfers = p.semantics.referenceTransfers(f);
    require(transfers.size() == 1 && transfers.begin()->second.size() == 2,
            "same-type candidates were arbitrarily collapsed");
    for (const auto &source : transfers.begin()->second)
      require(p.semantics.symbols[source.origin].type == I32 &&
                  source.path[0].kind == hir::ReferenceProjectionKind::Field,
              "a result retained an unrelated referent or lost its field position");
    seeded = positional = true;
  }
  require(seeded && positional, "missing struct input/result contract coverage");
  auto methods = core::verify(make("valid_methods.isa"));
  const auto &m = methods.get();
  bool precise = false;
  for (const auto &g : m.regionGraphs) {
    std::set<RegionId> old;
    for (const auto &loan : g.loans)
      if (m.semantics.symbols[loan.place.root].name == "old")
        old.insert(loan.region);
    if (old.empty())
      continue;
    unsigned calls = 0;
    std::function<void(const std::vector<core::Inst> &)> visit = [&](const auto &body) {
      for (const auto &i : body) {
        if (i.kind == core::Inst::Call &&
            m.semantics.functions[i.function].name.ends_with("::rewrap")) {
          const auto &args = g.valueRegions.at(i.result);
          require(args.size() == 1 &&
                      args.begin()->first ==
                          hir::ReferencePath{{hir::ReferenceProjectionKind::Field, 0}},
                  "method result lost its positional region argument");
          if (++calls > 1)
            for (auto region : old)
              require(!args.begin()->second.contains(region),
                      "Shared method used a replaced field's historic origin");
        }
        visit(i.body);
        visit(i.otherwise);
        visit(i.condition);
        visit(i.latch);
      }
    };
    for (const auto &f : m.functions)
      if (f.id == g.function)
        visit(f.body);
    require(calls == 3, "missing named and alias Shared calls");
    precise = true;
  }
  require(precise, "missing per-point method-region substitution coverage");
  auto optional = core::verify(make("valid_optional.isa"));
  const auto &o = optional.get();
  bool mandatory = false, conditional = false, none = false;
  for (TypeId t = 0; t < o.semantics.types.size(); ++t)
    if (o.semantics.types[t].name == "Optional") {
      require(o.semantics.referenceFreeValuePossible(t),
              "optional field was mistaken for a required reference");
      for (const auto &leaf : o.semantics.borrowedReferences(t))
        require(o.semantics.conditionalReferencePosition(t, leaf.path),
                "conditional nested reference was marked required");
      conditional = true;
    }
  for (TypeId t = 0; t < m.semantics.types.size(); ++t)
    if (m.semantics.types[t].name == "View<i32>") {
      require(!m.semantics.referenceFreeValuePossible(t) &&
                  m.semantics.borrowedPayloadLayoutSupported(t, Access::Shared) &&
                  !m.semantics.borrowedPayloadLayoutSupported(t, Access::Unique),
              "struct layout widened its capability/drop contract");
      mandatory = true;
    }
  std::function<void(const std::vector<core::Inst> &)> checkNone = [&](const auto &body) {
    for (const auto &i : body) {
      if (i.kind == core::Inst::FieldTake && o.semantics.types[i.type].kind == hir::Type::Enum &&
          i.carriers.empty()) {
        require(i.possibleVariants == std::vector<VariantId>{1},
                "absent input fabricated a live reference variant");
        none = true;
      }
      checkNone(i.body);
      checkNone(i.otherwise);
      checkNone(i.condition);
      checkNone(i.latch);
    }
  };
  for (const auto &f : o.functions)
    checkNone(f.body);
  require(mandatory && conditional && none, "missing required/conditional presence coverage");
  for (unsigned mode = 0; mode != 4; ++mode) {
    auto raw = make("valid_paths.isa");
    bool changed = false;
    for (auto &f : raw.semantics.functions)
      if (f.name == "number") {
        if (mode == 0)
          f.resultPayloadSource->path[0].index = None;
        if (mode == 1)
          f.resultPayloadSource->path.pop_back();
        if (mode == 2)
          f.payloadParameterRegions.pop_back();
        if (mode == 3)
          f.resultAccess = Access::Unique;
        changed = true;
      }
    require(changed, "missing contract to forge");
    try {
      core::verify(std::move(raw));
      throw std::runtime_error("forged struct position/capability contract accepted");
    } catch (const Error &error) {
      require(error.code == "E9001", "wrong contract forgery diagnostic");
    }
  }
  auto corrupt = p;
  for (auto &g : corrupt.regionGraphs)
    if (corrupt.semantics.functions[g.function].name == "number")
      g.fieldRegions.at(g.entry).clear();
  try {
    regions::validateBounds(corrupt);
    throw std::runtime_error("missing parameter field regions accepted");
  } catch (const Error &error) {
    require(error.code == "E9001", "wrong field-input forgery diagnostic");
  }
  auto raw = make("valid_paths.isa");
  raw.regionGraphs = corrupt.regionGraphs;
  regions::validateBounds(core::verify(std::move(raw)).get());
  std::ostringstream erased;
  runtime::print(erase(verified), erased);
  for (auto proof : {"reference.transfer", "reference.input", "field RegionArgs",
                     "carrier instance", "RegionId", "LoanId"})
    require(erased.str().find(proof) == std::string::npos,
            "struct region contract survived erasure");
}
