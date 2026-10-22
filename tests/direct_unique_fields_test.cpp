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
  auto verified = core::verify(make("valid_replace.isa"));
  const auto &p = verified.get();
  auto named = [&](const char *name) {
    auto found = std::find_if(p.semantics.symbols.begin(), p.semantics.symbols.end(),
                              [&](const auto &s) { return s.name == name; });
    require(found != p.semantics.symbols.end(), "missing semantic test binding");
    return found->id;
  };
  auto first = named("first"), second = named("second"), holder = named("view");
  auto type = p.semantics.symbols[holder].type;
  require(p.semantics.localCarrierType(type) && !p.semantics.borrowedInputSupported(type) &&
              !p.semantics.containerElement(type),
          "local Unique storage widened signature or heap-element permissions");
  bool replaced = false, extracted = false;
  for (const auto &f : p.functions) {
    const auto &g = *std::find_if(p.regionGraphs.begin(), p.regionGraphs.end(),
                                  [&](const auto &g) { return g.function == f.id; });
    std::function<void(const std::vector<core::Inst> &)> visit = [&](const auto &body) {
      for (const auto &i : body) {
        if (i.kind == core::Inst::FieldStore && i.slot == holder) {
          const storage::Location position{
              storage::Location::Symbol, holder, {{hir::ReferenceProjectionKind::Field, 0}}};
          require(g.parentStates.at(i.semanticPoint).at(position) == regions::ParentSet{first},
                  "replacement lost the old position before its cleanup");
          require(g.parentUses.at(i.semanticPoint).contains(first) &&
                      g.parentUses.at(i.semanticPoint).contains(second),
                  "replacement did not retain both parents through transfer");
          for (auto next : g.blocks.at(i.semanticPoint).successors) {
            require(g.parentStates.at(next).at(position) == regions::ParentSet{second},
                    "replacement did not strongly install the current parent");
            for (const auto &[place, parents] : g.parentStates.at(next))
              require(!parents.contains(first), "the historical parent survived replacement");
          }
          replaced = true;
        }
        if (i.kind == core::Inst::BorrowUnique &&
            p.semantics.reference(p.semantics.symbols.at(i.slot).type)) {
          require(g.loanParents.at(i.offset) == regions::ParentSet{second},
                  "field alias inherited the historical parent summary");
          extracted = true;
        }
        visit(i.body);
        visit(i.otherwise);
        visit(i.condition);
        visit(i.latch);
      }
    };
    visit(f.body);
  }
  require(replaced && extracted, "missing positional replacement and alias transfer coverage");

  auto moved = core::verify(make("valid_transfer.isa"));
  bool carried = false;
  std::function<void(const std::vector<core::Inst> &)> carriers = [&](const auto &body) {
    for (const auto &i : body) {
      if (i.kind == core::Inst::StructInit || i.kind == core::Inst::FieldTake)
        for (const auto &[path, provenance] : i.carriers)
          if (provenance.capability == Access::Unique) {
            require(provenance.parent != None && provenance.origins.size() == 1,
                    "field transfer detached the suspended parent from provenance");
            require(path.size() == (i.kind == core::Inst::StructInit ? 1u : 0u),
                    "field transfer retained or omitted a semantic prefix");
            carried = true;
          }
      carriers(i.body);
      carriers(i.otherwise);
      carriers(i.condition);
      carriers(i.latch);
    }
  };
  for (const auto &f : moved.get().functions)
    carriers(f.body);
  require(carried, "missing exclusive field provenance coverage");

  for (unsigned mode = 0; mode != 8; ++mode) {
    auto forged = p;
    bool changed = false;
    for (auto &g : forged.regionGraphs) {
      if (g.parentStates.empty())
        continue;
      if (mode == 0) {
        g.parentStates.clear();
        changed = true;
      }
      if (mode == 1) {
        g.parentUses.clear();
        changed = true;
      }
      if (mode == 2) {
        g.loanParents.clear();
        changed = true;
      }
      if (mode == 3) {
        g.storedResultParents.clear();
        changed = true;
      }
      for (auto &[point, op] : g.parentOperations) {
        if (mode == 4 && op.kind == regions::ParentOperation::Store) {
          op.place.path[0].index = None;
          changed = true;
          break;
        }
        if (mode == 5 && op.kind == regions::ParentOperation::Seed) {
          op.seeds.clear();
          changed = true;
          break;
        }
        if (mode == 6 && op.kind == regions::ParentOperation::Store) {
          g.blocks.at(point).uses.erase(first);
          changed = true;
          break;
        }
        if (mode == 7 && op.kind == regions::ParentOperation::Store) {
          op.reads.clear();
          changed = true;
          break;
        }
      }
    }
    require(changed, "no parent fact to forge");
    try {
      regions::validateBounds(forged);
      throw std::runtime_error("forged parent obligation accepted");
    } catch (const Error &error) {
      require(error.code == "E9001", "wrong forged parent diagnostic");
    }
    auto raw = make("valid_replace.isa");
    raw.regionGraphs = forged.regionGraphs;
    regions::validateBounds(core::verify(std::move(raw)).get());
  }

  for (unsigned mode = 0; mode != 4; ++mode) {
    auto forged = make("valid_direct.isa");
    bool changed = false;
    for (auto &f : forged.functions)
      for (auto &i : f.body) {
        if (i.kind == core::Inst::StoreReference && !changed) {
          if (mode == 0)
            i.access = Access::Shared;
          if (mode == 1)
            forged.semantics.types.at(i.type).referenceAccess = Access::Shared;
          if (mode == 2) {
            auto owner =
                std::find_if(forged.semantics.symbols.begin(), forged.semantics.symbols.end(),
                             [](const auto &s) { return s.name == "data"; });
            i.slot = owner->id;
          }
          if (mode < 3)
            changed = true;
        }
        if (mode == 3 && i.kind == core::Inst::BorrowUnique &&
            forged.semantics.reference(forged.semantics.symbols.at(i.slot).type)) {
          i.kind = core::Inst::BorrowShared;
          i.access = Access::Shared;
          changed = true;
          break;
        }
      }
    require(changed, "no direct reference fact to forge");
    try {
      (void)core::verify(std::move(forged));
      throw std::runtime_error("forged Unique reference accepted");
    } catch (const Error &error) {
      require(error.code == "E9001", "wrong direct reference forgery diagnostic");
    }
  }
  std::ostringstream erased;
  runtime::print(erase(verified), erased);
  for (auto proof :
       {"parent obligation", "parent summary", "field RegionArgs", "RegionId", "LoanId"})
    require(erased.str().find(proof) == std::string::npos,
            "parent liveness proof survived erasure");
}
