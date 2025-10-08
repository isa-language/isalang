#include "isac/core.hpp"
#include "isac/erasure.hpp"
#include <iostream>

int main() {
  using namespace isac;
  const std::string source = "fn main() -> i32 { let x = 1; return x; }";
  auto make = [&] { return core::elaborate(resolve(parse(lex(source)))); };
  auto valid = core::verify(make());
  if (erase(valid).functions.empty())
    return 1;
  auto bad = make();
  bad.functions[0].body[0].quantity = Quantity::One;
  try {
    auto ignored = core::verify(std::move(bad));
    (void)ignored;
    return 1;
  } catch (const Error &e) {
    if (e.code != "E9001")
      return 1;
  }
  bad = make();
  bad.functions[0].body[0].type = Bool;
  try {
    auto ignored = core::verify(std::move(bad));
    (void)ignored;
    return 1;
  } catch (const Error &e) {
    if (e.code != "E9001")
      return 1;
  }
  const std::string borrowed = "fn read(x: &i32) -> i32 { return x; } "
                               "fn main() -> i32 { let x = 1; return read(&x); }";
  auto referenceCore = [&] { return core::elaborate(resolve(parse(lex(borrowed)))); };
  (void)core::verify(referenceCore());
  auto missingContract = referenceCore();
  missingContract.semantics.functions[0].parameterRegions.clear();
  try {
    (void)core::verify(std::move(missingContract));
    return 1;
  } catch (const Error &e) {
    if (e.code != "E9001")
      return 1;
  }
  auto forgedReference = referenceCore();
  bool changed = false;
  for (auto &function : forgedReference.functions)
    for (auto &instruction : function.body)
      if (instruction.kind == core::Inst::ReferenceArgument) {
        instruction.access = Access::Unique;
        changed = true;
      }
  if (!changed)
    return 1;
  try {
    (void)core::verify(std::move(forgedReference));
    return 1;
  } catch (const Error &e) {
    if (e.code != "E9001")
      return 1;
  }
  auto forgedReceiver = make();
  forgedReceiver.semantics.functions[0].consumesSelf = true;
  try {
    (void)core::verify(std::move(forgedReceiver));
    return 1;
  } catch (const Error &e) {
    if (e.code != "E9001")
      return 1;
  }
  const std::string returning = "fn identity(x: &i32) -> &i32 { return x; } "
                                "fn main() { let x = 1; let r = identity(&x); print(r); }";
  auto resultCore = [&] { return core::elaborate(resolve(parse(lex(returning)))); };
  (void)core::verify(resultCore());
  auto missingResultContract = resultCore();
  missingResultContract.semantics.functions[0].resultOrigin = None;
  try {
    (void)core::verify(std::move(missingResultContract));
    return 1;
  } catch (const Error &e) {
    if (e.code != "E9001")
      return 1;
  }
  auto forgedResultFlavor = resultCore();
  forgedResultFlavor.semantics.functions[0].resultBorrowedContainer = true;
  try {
    (void)core::verify(std::move(forgedResultFlavor));
    return 1;
  } catch (const Error &e) {
    if (e.code != "E9001")
      return 1;
  }
  auto forgedResultOrigin = resultCore();
  forgedResultOrigin.semantics.functions[0].resultOrigin = UINT32_MAX - 1;
  try {
    (void)core::verify(std::move(forgedResultOrigin));
    return 1;
  } catch (const Error &e) {
    if (e.code != "E9001")
      return 1;
  }
  const std::string projecting =
      "struct Data { value: i32, } "
      "fn field(data: &Data) -> &i32 { return &data.value; } "
      "fn main() { let data = Data::new(); let r = field(&data); print(r); }";
  auto projectionCore = [&] { return core::elaborate(resolve(parse(lex(projecting)))); };
  (void)core::verify(projectionCore());
  auto forgedProjection = projectionCore();
  bool foundProjection = false;
  for (auto &function : forgedProjection.functions)
    for (auto &instruction : function.body)
      if (instruction.placeProjection == core::Inst::PlaceProjection::Field) {
        instruction.fieldIndex = UINT32_MAX;
        foundProjection = true;
      }
  if (!foundProjection)
    return 1;
  try {
    (void)core::verify(std::move(forgedProjection));
    return 1;
  } catch (const Error &e) {
    if (e.code != "E9001")
      return 1;
  }
  const std::string indexing =
      "fn at(values: &i32[], index: &usize) -> &i32 { return &values[index]; } "
      "fn main() {}";
  auto forgedIndex = core::elaborate(resolve(parse(lex(indexing))));
  auto &indexBody = forgedIndex.functions[0].body;
  bool foundIndex = false;
  for (size_t n = 0; n < indexBody.size(); ++n) {
    if (indexBody[n].placeProjection != core::Inst::PlaceProjection::Index)
      continue;
    core::Inst reference{};
    reference.kind = core::Inst::ReferenceArgument;
    reference.result = forgedIndex.values++;
    reference.type = USize;
    reference.slot = forgedIndex.semantics.functions[0].params[1];
    reference.access = Access::Shared;
    reference.quantity = Quantity::Omega;
    indexBody[n].args = {reference.result};
    indexBody.insert(indexBody.begin() + n, reference);
    foundIndex = true;
    break;
  }
  if (!foundIndex)
    return 1;
  try {
    (void)core::verify(std::move(forgedIndex));
    return 1;
  } catch (const Error &e) {
    if (e.code != "E9001")
      return 1;
  }
  std::cout << "Core gate rejects forged quantity, type and reference contracts\n";
}
