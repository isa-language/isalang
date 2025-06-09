#include "isac/frontend.hpp"
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
namespace isac {
ast::Program loadImports(ast::Program source, SourceManager *sources) {
  ast::Program result;
  std::set<std::string> loaded, loading;
  const std::set<std::string> allowed = {
      "core.runtime", "std.mem",     "std.box",    "std.buffer",      "std.io",       "std.string",
      "std",          "std.prelude", "std.fmt",    "std.macros",      "std.option",   "std.result",
      "std.parse",    "std.math",    "std.random", "std.ui.geometry", "std.ui.color", "std.time"};
  auto append = [&](ast::Program p) {
    auto offset = result.functions.size();
    for (auto &impl : p.impls)
      for (auto &method : impl.methods)
        method += offset;
    result.impls.insert(result.impls.end(), p.impls.begin(), p.impls.end());
    result.enums.insert(result.enums.end(), p.enums.begin(), p.enums.end());
    result.structs.insert(result.structs.end(), p.structs.begin(), p.structs.end());
    result.protocols.insert(result.protocols.end(), p.protocols.begin(), p.protocols.end());
    result.functions.insert(result.functions.end(), p.functions.begin(), p.functions.end());
    result.imports.insert(result.imports.end(), p.imports.begin(), p.imports.end());
  };
  std::function<void(const std::string &)> load = [&](const std::string &name) {
    if (!allowed.contains(name))
      fail({}, DiagnosticCode::RestrictedOperation, "unknown bundled module `" + name + "`");
    if (name == "std.macros")
      return;
    if (loaded.contains(name))
      return;
    if (!loading.insert(name).second)
      fail({}, DiagnosticCode::RestrictedOperation, "cyclic module import");
    auto relative = name;
    for (auto &character : relative)
      if (character == '.')
        character = std::filesystem::path::preferred_separator;
    auto path = std::filesystem::path(ISAC_LIBRARY_ROOT) / (relative + ".isa");
    std::ifstream file(path);
    if (!file)
      fail({}, DiagnosticCode::RestrictedOperation, "cannot open bundled module: " + path.string());
    std::string text{std::istreambuf_iterator<char>(file), {}};
    auto tokens = lex(text, sources ? sources->add(path.string(), text) : NoFile);
    for (auto &t : tokens)
      t.loc.file = path.string();
    auto p = parse(tokens, true);
    for (const auto &import : p.imports)
      load(import);
    append(std::move(p));
    loading.erase(name);
    loaded.insert(name);
  };
  for (const auto &name : source.imports)
    load(name);
  append(std::move(source));
  load("std.prelude");
  return result;
}
}
