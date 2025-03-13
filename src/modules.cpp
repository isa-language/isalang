#include "isac/frontend.hpp"
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
namespace isac {
ast::Program loadImports(ast::Program source) {
  ast::Program result;
  std::set<std::string> loaded, loading;
  const std::set<std::string> allowed = {"core.runtime", "std.mem", "std.box", "std.buffer",
                                         "std.io"};
  auto append = [&](ast::Program p) {
    auto offset = result.functions.size();
    for (auto &impl : p.impls)
      for (auto &method : impl.methods)
        method += offset;
    result.impls.insert(result.impls.end(), p.impls.begin(), p.impls.end());
    result.structs.insert(result.structs.end(), p.structs.begin(), p.structs.end());
    result.protocols.insert(result.protocols.end(), p.protocols.begin(), p.protocols.end());
    result.functions.insert(result.functions.end(), p.functions.begin(), p.functions.end());
    result.imports.insert(result.imports.end(), p.imports.begin(), p.imports.end());
  };
  std::function<void(const std::string &)> load = [&](const std::string &name) {
    if (!allowed.contains(name))
      fail({}, "E0500", "unknown bundled module `" + name + "`");
    if (loaded.contains(name))
      return;
    if (!loading.insert(name).second)
      fail({}, "E0500", "cyclic module import");
    auto relative = name;
    relative[relative.find('.')] = std::filesystem::path::preferred_separator;
    auto path = std::filesystem::path(ISAC_LIBRARY_ROOT) / (relative + ".isa");
    std::ifstream file(path);
    if (!file)
      fail({}, "E0500", "cannot open bundled module: " + path.string());
    std::string text{std::istreambuf_iterator<char>(file), {}};
    auto tokens = lex(text);
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
  return result;
}
}
