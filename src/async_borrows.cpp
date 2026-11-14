#include "isac/async.hpp"
#include <functional>

namespace isac::asyncs::detail {
namespace {
using I = core::Inst;
template <class Ops, class Visit> void walk(Ops &ops, const Visit &visit) {
  for (auto &i : ops) {
    visit(i);
    walk(i.condition, visit);
    walk(i.body, visit);
    walk(i.otherwise, visit);
    walk(i.latch, visit);
  }
}
void rename(I &i, const std::map<SymbolId, SymbolId> &aliases) {
  auto replace = [&](SymbolId &id) {
    if (auto found = aliases.find(id); found != aliases.end())
      id = found->second;
  };
  replace(i.slot);
  for (auto &id : i.locals)
    replace(id);
  if (i.kind == I::BorrowShared || i.kind == I::BorrowUnique)
    replace(i.offset);
}
}
core::Verified reconstructBorrows(const core::Verified &source) {
  bool needed = false;
  for (const auto &f : source.get().functions)
    walk(f.body, [&](const auto &i) { needed |= !i.suspensionBorrows.empty(); });
  if (!needed)
    return source;
  auto p = source.get();
  RegionId fresh = 0;
  auto reserve = [&](RegionId id) {
    if (id != None)
      fresh = std::max(fresh, id + 1);
  };
  for (const auto &s : p.semantics.symbols)
    reserve(s.region);
  for (const auto &f : p.semantics.functions) {
    reserve(f.resultRegion);
    for (const auto &r : f.parameterRegions)
      reserve(r.region);
    for (const auto &r : f.payloadParameterRegions)
      reserve(r.region);
  }
  for (const auto &f : p.functions)
    walk(f.body, [&](const auto &i) { reserve(i.region); });
  for (auto &f : p.functions) {
    std::map<SymbolId, SymbolId> aliases;
    auto current = [&](SymbolId id) {
      auto found = aliases.find(id);
      return found == aliases.end() ? id : found->second;
    };
    std::vector<I> output;
    for (auto &i : f.body) {
      const auto recipes = i.suspensionBorrows;
      walk(i.condition, [&](auto &op) { rename(op, aliases); });
      walk(i.body, [&](auto &op) { rename(op, aliases); });
      walk(i.otherwise, [&](auto &op) { rename(op, aliases); });
      walk(i.latch, [&](auto &op) { rename(op, aliases); });
      rename(i, aliases);
      for (const auto &recipe : recipes) {
        I end{};
        end.kind = I::EndBorrow;
        end.type = recipe.type;
        end.quantity = p.semantics.linear(recipe.type) ? Quantity::One : Quantity::Omega;
        end.slot = current(recipe.alias);
        end.loc = i.loc;
        output.push_back(std::move(end));
      }
      output.push_back(std::move(i));
      for (const auto &recipe : recipes) {
        if (fresh == None)
          fail(recipe.origin, DiagnosticCode::InvalidCore,
               "suspension region identity space exhausted");
        auto symbol = p.semantics.symbols.at(current(recipe.alias));
        symbol.id = p.semantics.symbols.size();
        symbol.parameter = false;
        symbol.receiver = false;
        symbol.region = fresh++;
        symbol.lastUse = {};
        p.semantics.symbols.push_back(symbol);
        I borrow{};
        borrow.kind = recipe.kind == BorrowKind::Shared ? I::BorrowShared : I::BorrowUnique;
        borrow.quantity = Quantity::One;
        borrow.access = recipe.kind == BorrowKind::Shared ? Access::Shared : Access::Unique;
        borrow.slot = current(recipe.owner);
        borrow.offset = symbol.id;
        borrow.type = recipe.type;
        borrow.region = symbol.region;
        borrow.loc = recipe.origin;
        output.push_back(std::move(borrow));
        aliases[recipe.alias] = symbol.id;
      }
    }
    f.body = std::move(output);
  }

  return core::verify(std::move(p));
}
}
