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
  const regions::FunctionGraph *activeGraph = nullptr;
  bool reordered = false, partial = false, fieldCarrier = false;
  std::function<void(const std::vector<core::Inst> &)> visit = [&](const auto &body) {
    for (const auto &i : body) {
      if (i.kind == core::Inst::StructInit && p.semantics.types[i.type].name == "View" &&
          !i.fieldPath.empty() && i.fieldPath[0] == 1) {
        const auto &g = *activeGraph;
        const auto &args = g.valueRegions.at(i.result);
        require(args.size() == 1 &&
                    args.begin()->first[0].kind == hir::ReferenceProjectionKind::Field &&
                    args.begin()->first[0].index == 0,
                "initializer source order replaced semantic field identity");
        reordered = true;
      }
      if (i.kind == core::Inst::FieldTake && !i.carriers.empty()) {
        require(!p.semantics.needsDrop(i.type) && p.semantics.managedFieldType(i.type),
                "borrow obligation was confused with owning an allocation");
        const auto schema = p.semantics.borrowedReferences(i.type);
        for (const auto &[path, provenance] : i.carriers) {
          require(std::any_of(schema.begin(), schema.end(),
                              [&](const auto &leaf) { return leaf.path == path; }),
                  "field extraction retained the enclosing struct path");
          require(provenance.capability == Access::Shared && !provenance.origins.empty(),
                  "field extraction lost its loan responsibility");
        }
        fieldCarrier = true;
      }
      for (const auto &[owner, plan] : i.fieldCleanup)
        for (const auto &[field, child] : plan.fields)
          if (child.skip &&
              p.semantics.containsReferences(
                  p.semantics.types[p.semantics.symbols[owner].type].fields[field].type))
            partial = true;
      visit(i.body);
      visit(i.otherwise);
      visit(i.condition);
      visit(i.latch);
    }
  };
  for (const auto &f : p.functions) {
    activeGraph = &*std::find_if(p.regionGraphs.begin(), p.regionGraphs.end(),
                                 [&](const auto &g) { return g.function == f.id; });
    visit(f.body);
  }
  require(reordered && partial && fieldCarrier, "missing field path/partial drop coverage");

  auto replacement = core::verify(make("valid_replace.isa"));
  const auto &r = replacement.get();
  bool exact = false;
  for (const auto &graph : r.regionGraphs) {
    std::set<RegionId> oldRegions;
    for (const auto &loan : graph.loans)
      if (r.semantics.symbols[loan.place.root].name == "old")
        oldRegions.insert(loan.region);
    for (const auto &[point, op] : graph.fieldRegionOperations)
      if (op.kind == regions::FieldRegionOperation::Take) {
        const auto &arguments = graph.valueRegions.at(op.result);
        for (const auto &[path, regions] : arguments)
          for (auto region : oldRegions) {
            require(!regions.contains(region) && !graph.regions.at(region).points.contains(point),
                    "a replaced field kept a historical origin live");
            exact = true;
          }
      }
  }
  require(exact, "missing flow-sensitive replacement coverage");
  for (int corruption = 0; corruption != 3; ++corruption) {
    auto invalid = r;
    bool changed = false;
    for (auto &graph : invalid.regionGraphs) {
      if (corruption == 0 && !graph.fieldRegions.empty()) {
        graph.fieldRegions.begin()->second.clear();
        changed = true;
      } else if (corruption == 1)
        for (auto &[point, op] : graph.fieldRegionOperations)
          if (op.kind == regions::FieldRegionOperation::Store) {
            op.place.path[0].index = None;
            changed = true;
            break;
          }
      if (corruption == 2)
        for (const auto &[point, op] : graph.fieldRegionOperations)
          if (op.kind == regions::FieldRegionOperation::Take) {
            graph.valueRegions.at(op.result).clear();
            changed = true;
            break;
          }
      if (changed)
        break;
    }
    require(changed, "missing field metadata to forge");
    try {
      regions::validateBounds(invalid);
      throw std::runtime_error("forged field region flow accepted");
    } catch (const Error &error) {
      require(error.code == "E9001", "wrong field-flow forgery diagnostic");
    }
    auto raw = make("valid_replace.isa");
    raw.regionGraphs = invalid.regionGraphs;
    regions::validateBounds(core::verify(std::move(raw)).get());
  }
  for (uint32_t cell : {0u, 1u}) {
    auto raw = make("valid_fields.isa");
    bool changed = false;
    std::function<void(std::vector<core::Inst> &)> forge = [&](auto &body) {
      for (auto &i : body) {
        if (!changed && i.kind == core::Inst::Read && i.slot != None &&
            raw.semantics.types[raw.semantics.symbols[i.slot].type].name == "View") {
          i.offset = cell;
          changed = true;
        }
        forge(i.body);
        forge(i.otherwise);
        forge(i.condition);
        forge(i.latch);
      }
    };
    for (auto &f : raw.functions)
      forge(f.body);
    require(changed, "missing marker read for scalar-cell forgery");
    try {
      core::verify(std::move(raw));
      throw std::runtime_error("scalar read accessed a managed reference field");
    } catch (const Error &error) {
      require(error.code == "E9001", "wrong field cell forgery diagnostic");
    }
  }
  std::ostringstream erased;
  runtime::print(erase(verified), erased);
  for (auto marker : {"field RegionArgs", "carrier instance", "RegionId", "LoanId"})
    require(erased.str().find(marker) == std::string::npos,
            "field lifetime proofs survived erasure");
}
