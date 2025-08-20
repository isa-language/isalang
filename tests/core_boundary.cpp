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
  std::cout << "Core gate rejects forged quantity, type and reference contracts\n";
}
