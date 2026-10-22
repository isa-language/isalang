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
  auto verified = core::verify(make("valid_direct.isa"));
  const auto &p = verified.get();
  bool direct = false, nested = false, extracted = false;
  std::function<void(const std::vector<core::Inst> &)> visit = [&](const auto &body) {
    for (const auto &i : body) {
      if (i.storedPayloadReference && !i.payloadPath.empty() &&
          i.payloadPath[0].kind == hir::ReferenceProjectionKind::Field) {
        auto type =
            p.semantics.referencePositionType(p.semantics.symbols[i.slot].type, i.payloadPath);
        require(type && p.semantics.reference(*type) && p.semantics.types[*type].element == i.type,
                "direct field alias lost its resolved reference leaf");
        direct = true;
        nested |= i.payloadPath.size() == 2;
      }
      if (i.kind == core::Inst::FieldTake && p.semantics.reference(i.type)) {
        require(i.carriers.size() == 1 && i.carriers.contains({}) &&
                    i.carriers.at({}).capability == Access::Shared &&
                    !i.carriers.at({}).origins.empty(),
                "direct extraction lost its origin");
        extracted = true;
      }
      visit(i.body);
      visit(i.otherwise);
      visit(i.condition);
      visit(i.latch);
    }
  };
  for (const auto &f : p.functions)
    visit(f.body);
  require(direct && nested && extracted, "missing direct/nested/extraction coverage");

  auto replaced = core::verify(make("valid_replace.isa"));
  const auto &r = replaced.get();
  bool exact = false;
  for (const auto &g : r.regionGraphs) {
    std::set<RegionId> old;
    for (const auto &loan : g.loans)
      if (r.semantics.symbols[loan.place.root].name == "old")
        old.insert(loan.region);
    std::function<void(const std::vector<core::Inst> &)> check = [&](const auto &body) {
      for (const auto &i : body) {
        if (i.storedPayloadReference && i.slot != None &&
            r.semantics.symbols[i.offset].name == "current") {
          bool source = false;
          for (const auto &constraint : g.constraints)
            if (constraint.kind == regions::ConstraintKind::Subregion &&
                constraint.region == i.region &&
                constraint.reason == regions::ConstraintReason::StoredReference) {
              require(!old.contains(constraint.upper),
                      "replacement read retained its historic field origin");
              source = true;
            }
          require(source, "field alias has no positional region constraint");
          exact = true;
        }
        check(i.body);
        check(i.otherwise);
        check(i.condition);
        check(i.latch);
      }
    };
    for (const auto &f : r.functions)
      if (f.id == g.function)
        check(f.body);
  }
  require(exact, "missing flow-sensitive field read coverage");
  for (int corruption = 0; corruption != 3; ++corruption) {
    auto raw = make("valid_direct.isa");
    bool changed = false;
    std::function<void(std::vector<core::Inst> &)> forge = [&](auto &body) {
      for (auto &i : body) {
        if (!changed && i.kind == core::Inst::BorrowShared && !i.payloadPath.empty() &&
            i.payloadPath[0].kind == hir::ReferenceProjectionKind::Field &&
            raw.semantics.types[raw.semantics.symbols[i.slot].type].name == "View") {
          if (corruption == 0)
            i.payloadPath[0].index = 1;
          if (corruption == 1)
            i.payloadPath[0].index = None;
          if (corruption == 2)
            i.kind = core::Inst::BorrowUnique;
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
    require(changed, "missing stored field borrow to forge");
    try {
      core::verify(std::move(raw));
      throw std::runtime_error("forged stored field projection accepted");
    } catch (const Error &error) {
      require(error.code == "E9001", "wrong field forgery diagnostic");
    }
  }
  std::ostringstream erased;
  runtime::print(erase(verified), erased);
  for (auto proof : {"reference.field=#", "carrier instance", "RegionId", "LoanId"})
    require(erased.str().find(proof) == std::string::npos, "direct-field proof survived erasure");
}
