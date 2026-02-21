#include "isac/core.hpp"
#include "isac/erasure.hpp"
#include "isac/modules.hpp"
#include <functional>
#include <iostream>

int main() {
  using namespace isac;
  const std::string source =
      "enum Choice<T>{Left(T),Right(T),Empty,}"
      "fn inspect(value:Choice<&i32>){match &value{Choice::Left(r)=>print(r),"
      "Choice::Right(r)=>print(r),Choice::Empty=>{}}}"
      "fn main(){let a=10;inspect(Choice::Right(&a));a=20;}";
  auto make = [&] { return core::elaborate(resolve(parse(lex(source)))); };
  auto program = make();
  bool found = false;
  for (TypeId id = 0; id < program.semantics.types.size(); ++id) {
    const auto &type = program.semantics.types[id];
    if (type.kind != hir::Type::Enum || type.variants.size() != 3 ||
        !program.semantics.containsReferences(id))
      continue;
    auto left = hir::payloadPosition(type.variants[0].id, 0);
    auto right = hir::payloadPosition(type.variants[1].id, 0);
    if (left == right || program.semantics.borrowedPayloadPaths(id).size() != 2 ||
        program.semantics.payloadType(id, left) != program.semantics.payloadType(id, right) ||
        program.semantics.payloadOffset(id, type.variants[0].id, 0) !=
            program.semantics.payloadOffset(id, type.variants[1].id, 0))
      return 1;
    if (program.semantics.payloadType(id, hir::payloadPosition(9999, 0)) ||
        program.semantics.payloadType(id, hir::payloadPosition(type.variants[0].id, 1)))
      return 2;
    found = true;
  }
  if (!found)
    return 3;
  auto verified = core::verify(std::move(program));
  if (erase(verified).functions.empty())
    return 4;
  size_t carriers = 0;
  std::function<void(const std::vector<core::Inst> &)> checkCarriers;
  checkCarriers = [&](const auto &body) {
    for (const auto &i : body) {
      for (const auto &[path, provenance] : i.carriers) {
        if (provenance.origins.empty() || provenance.capability != Access::Shared ||
            provenance.parent != None)
          throw std::runtime_error("invalid carrier snapshot");
        if (!path.empty() && path != hir::payloadPosition(1, 0))
          throw std::runtime_error("carrier conflated equal-layout variants");
        ++carriers;
      }
      checkCarriers(i.body);
      checkCarriers(i.otherwise);
      checkCarriers(i.condition);
      checkCarriers(i.latch);
    }
  };
  for (const auto &f : verified.get().functions)
    checkCarriers(f.body);
  if (carriers < 2)
    return 10;
  size_t projections = 0;
  for (const auto &graph : verified.get().regionGraphs)
    for (const auto &loan : graph.loans)
      for (const auto &projection : loan.place.projections)
        if (projection.kind == regions::ProjectionKind::VariantPayload) {
          if (projection.variant == None || projection.index != 0)
            return 5;
          ++projections;
        }
  if (projections != 2)
    return 6;

  auto annotated = make();
  std::function<void(std::vector<core::Inst> &)> annotate;
  annotate = [&](auto &body) {
    for (auto &i : body) {
      i.carriers[hir::payloadPosition(9999, 9999)] = {{9999}, Access::Unique, 9999};
      annotate(i.body);
      annotate(i.otherwise);
      annotate(i.condition);
      annotate(i.latch);
    }
  };
  for (auto &f : annotated.functions)
    annotate(f.body);
  auto recomputed = core::verify(std::move(annotated));
  for (const auto &f : recomputed.get().functions)
    checkCarriers(f.body);
  auto earlyReturn = core::elaborate(resolve(parse(lex("fn main(){return;}"))));
  for (auto &f : earlyReturn.functions)
    annotate(f.body);
  auto verifiedReturn = core::verify(std::move(earlyReturn));
  for (const auto &f : verifiedReturn.get().functions)
    checkCarriers(f.body);

  for (int corruption = 0; corruption != 5; ++corruption) {
    auto forged = make();
    bool changed = false;
    std::function<void(std::vector<core::Inst> &)> mutate;
    mutate = [&](auto &body) {
      for (auto &i : body) {
        if (!changed && i.kind == core::Inst::BorrowShared && !i.payloadPath.empty()) {
          if (corruption == 0)
            i.payloadPath[0].variant = 9999;
          if (corruption == 1)
            i.payloadPath[0].index = 1;
          if (corruption == 2)
            i.payloadPath[0].kind = hir::ReferenceProjectionKind::Field;
          if (corruption == 3)
            i.payloadPath.push_back(i.payloadPath.front());
          if (corruption == 4)
            i.payloadPath[0].variant ^= 1;
          changed = true;
        }
        mutate(i.body);
        mutate(i.otherwise);
        mutate(i.condition);
        mutate(i.latch);
      }
    };
    for (auto &f : forged.functions)
      mutate(f.body);
    if (!changed)
      return 7;
    try {
      (void)core::verify(std::move(forged));
      return 8;
    } catch (const Error &error) {
      if (error.code != "E9001")
        return 9;
    }
  }
  auto owned = [] {
    return core::elaborate(resolve(
        modules::compile(lex("enum Owned{First(String),Second(String),}fn main(){"
                             "let x=Owned::Second(String::from(\"owned\"));"
                             "match x{Owned::First(s)=>print(s),Owned::Second(s)=>print(s),}}"))
            .ast));
  };
  auto verifiedOwned = core::verify(owned());
  size_t partialDrops = 0;
  std::set<hir::ReferencePath> droppedPositions;
  std::function<void(const std::vector<core::Inst> &)> checkDrops;
  checkDrops = [&](const auto &body) {
    for (const auto &i : body) {
      for (const auto &[symbol, paths] : i.movedPayloads) {
        (void)symbol;
        for (const auto &path : paths) {
          if (path != hir::payloadPosition(0, 0) && path != hir::payloadPosition(1, 0))
            throw std::runtime_error("partial drop did not use semantic position");
          droppedPositions.insert(path);
          ++partialDrops;
        }
      }
      checkDrops(i.body);
      checkDrops(i.otherwise);
      checkDrops(i.condition);
      checkDrops(i.latch);
    }
  };
  for (const auto &f : verifiedOwned.get().functions)
    checkDrops(f.body);
  if (!partialDrops || droppedPositions.size() != 2 || erase(verifiedOwned).functions.empty())
    return 11;
  auto forgedOwned = owned();
  bool changed = false;
  std::function<void(std::vector<core::Inst> &)> forgeTransfer;
  forgeTransfer = [&](auto &body) {
    for (auto &i : body) {
      if (!changed && i.kind == core::Inst::PayloadTake && i.payloadPath[0].variant == 1) {
        i.payloadPath[0].variant ^= 1;
        changed = true;
      }
      forgeTransfer(i.body);
      forgeTransfer(i.otherwise);
    }
  };
  for (auto &f : forgedOwned.functions)
    forgeTransfer(f.body);
  if (!changed)
    return 12;
  try {
    (void)core::verify(std::move(forgedOwned));
    return 13;
  } catch (const Error &error) {
    if (error.code != "E9001")
      return 14;
  }
  std::cout << "Semantic carrier positions distinguish equal-layout variants before erasure\n";
}
