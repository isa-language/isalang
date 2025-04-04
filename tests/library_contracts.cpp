#include "isac/core.hpp"
#include <functional>
#include <iostream>
using namespace isac;
static bool rejects(const std::string &module, const std::function<void(ast::Program &)> &modify,
                    const std::string &code) {
  auto ast = loadImports(parse(lex("@import \"" + module + "\" fn main() -> i32 { return 0; }")));
  modify(ast);
  try {
    auto verified = core::verify(core::elaborate(resolve(ast)));
    (void)verified;
    return false;
  } catch (const Error &e) {
    if (e.code != code)
      std::cerr << e.what() << "\n";
    return e.code == code;
  }
}
int main() {

  if (!rejects(
          "std.box",
          [](auto &p) {
            for (auto &f : p.functions)
              if (f.owner == "BoxI32" && f.name == "set")
                f.params[0].access = Access::Shared;
          },
          "E0311"))
    return 1;

  if (!rejects(
          "std.buffer",
          [](auto &p) {
            for (auto &f : p.functions)
              if (f.owner == "Buffer" && f.name == "release")
                f.body.clear();
          },
          "E0430"))
    return 2;
  if (!rejects(
          "std.buffer",
          [](auto &p) {
            for (auto &f : p.functions)
              if (f.owner == "Buffer" && f.name == "release")
                f.body.push_back(f.body[0]);
          },
          "E0430"))
    return 3;
  return 0;
}
