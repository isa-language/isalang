#include "isac/modules.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <set>
namespace isac {
namespace {
void append(ast::Program &result, ast::Program p) {
  result.constants.insert(result.constants.end(), p.constants.begin(), p.constants.end());
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
  result.importOrigins.insert(result.importOrigins.end(), p.importOrigins.begin(),
                              p.importOrigins.end());
}
class Loader {
  using Tree = macros::TokenTree;
  SourceManager *sources;
  std::vector<modules::Module> modules;
  std::map<std::string, uint32_t> files;
  std::set<uint32_t> scanned;
  static bool is(const Tree &t, const char *text) {
    return !t.delimiter && t.token.kind != Token::String && t.token.text == text;
  }
  void dependency(uint32_t source, const std::string &spec, Loc origin) {
    namespace fs = std::filesystem;
    if (spec.empty() || spec.find('\0') != std::string::npos)
      fail(origin, DiagnosticCode::InvalidImport, "import requires a nonempty module path");
    bool trusted = spec == "std" || spec.starts_with("std.") || spec.starts_with("core.");
    fs::path path;
    if (trusted) {
      if (spec.ends_with('.') || spec.find("..") != std::string::npos ||
          !std::all_of(spec.begin(), spec.end(),
                       [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '.'; }))
        fail(origin, DiagnosticCode::InvalidImport, "invalid bundled module name `" + spec + "`");
      std::string relative = spec;
      std::replace(relative.begin(), relative.end(), '.', fs::path::preferred_separator);
      auto library = fs::weakly_canonical(ISAC_LIBRARY_ROOT);
      path = fs::weakly_canonical(library / (relative + ".isa"));
      auto inLibrary = path.lexically_relative(library);
      if (inLibrary.empty() || *inLibrary.begin() == "..")
        fail(origin, DiagnosticCode::InvalidImport,
             "bundled import escapes its trusted library root");
    } else {
      fs::path local(spec);
      if (local.extension() != ".isa")
        fail(origin, DiagnosticCode::InvalidImport,
             "local imports require an .isa file; bundled modules use std.* or core.*");
      path = fs::weakly_canonical(
          local.is_absolute() ? local : fs::path(modules[source].path).parent_path() / local);
    }
    uint32_t target;
    if (auto found = files.find(path.string()); found != files.end()) {
      target = found->second;
      if (modules[target].trusted != trusted)
        fail(origin, DiagnosticCode::InvalidImport,
             "the same source cannot be imported as both trusted library and local code");
    } else {
      if (modules.size() >= 256)
        fail(origin, DiagnosticCode::InvalidImport, "module discovery limit reached (256)");
      if (!fs::is_regular_file(path))
        DiagnosticEngine()
            .error(DiagnosticCode::ImportNotFound, origin,
                   "cannot find imported module `" + spec + "`")
            .note("resolved path: " + path.string())
            .emit();
      if (fs::file_size(path) > 4 * 1024 * 1024)
        fail(origin, DiagnosticCode::InvalidImport, "imported module exceeds 4 MiB source limit");
      std::ifstream file(path);
      if (!file)
        fail(origin, DiagnosticCode::ImportNotFound, "cannot read imported module");
      std::string text{std::istreambuf_iterator<char>(file), {}};
      auto tokens = lex(text, sources ? sources->add(path.string(), text) : NoFile);
      for (auto &token : tokens)
        token.loc.file = path.string();
      target = modules.size();
      files[path.string()] = target;
      modules.push_back({target, path.string(), trusted, std::move(tokens), {}});
    }
    auto &edges = modules[source].dependencies;
    if (std::none_of(edges.begin(), edges.end(), [&](auto &edge) { return edge.target == target; }))
      edges.push_back({target, origin});
  }
  void scanTrees(uint32_t id, const std::vector<Tree> &trees, bool expanded, bool nested = false) {
    for (size_t n = 0; n < trees.size(); ++n) {
      if (!expanded && is(trees[n], "@") && n + 3 < trees.size() && is(trees[n + 1], "macro")) {
        n += 3;
        continue;
      }
      if (is(trees[n], "@") && n + 1 < trees.size() && is(trees[n + 1], "import")) {
        auto loc = trees[n].token.loc;
        if (nested)
          fail(loc, DiagnosticCode::InvalidImport, "@import is a module-scope construction");
        if (n + 2 >= trees.size())
          fail(loc, DiagnosticCode::InvalidImport, "expected import path");
        const auto &argument = trees[n + 2];
        if (!argument.delimiter && argument.token.kind == Token::String)
          dependency(id, argument.token.text, loc);
        else if (argument.delimiter == '(') {
          bool wantPath = true, any = false;
          for (const auto &part : argument.children) {
            if (wantPath && !part.delimiter && part.token.kind == Token::String) {
              dependency(id, part.token.text, loc);
              wantPath = false;
              any = true;
            } else if (!wantPath && is(part, ","))
              wantPath = true;
            else
              fail(loc, DiagnosticCode::InvalidImport,
                   "@import arguments must be string literals separated by commas");
          }
          if (!any)
            fail(loc, DiagnosticCode::InvalidImport, "@import requires at least one path");
        } else
          fail(loc, DiagnosticCode::InvalidImport, "@import requires compile-time string literals");
        n += 2;
      } else if (expanded && trees[n].delimiter)
        scanTrees(id, trees[n].children, true, true);
    }
  }
  void discover() {
    for (uint32_t id = 0; id < modules.size(); ++id)
      if (scanned.insert(id).second) {
        auto trees = macros::tokenTrees(modules[id].tokens);
        scanTrees(id, trees, false);
      }
  }
  std::vector<uint32_t> order() const {
    std::vector<unsigned> state(modules.size());
    std::vector<uint32_t> sorted, stack;
    std::function<void(uint32_t, unsigned)> visit = [&](uint32_t id, unsigned depth) {
      if (depth > 128)
        fail({}, DiagnosticCode::InvalidImport, "module nesting limit reached (128)");
      if (state[id] == 2)
        return;
      state[id] = 1;
      stack.push_back(id);
      for (const auto &edge : modules[id].dependencies) {
        if (state[edge.target] == 1) {
          auto diagnostic = DiagnosticEngine().error(DiagnosticCode::ImportCycle, edge.origin,
                                                     "cyclic module import");
          for (auto module : stack)
            diagnostic.note("import chain: " + modules[module].path);
          diagnostic.note("cycle returns to: " + modules[edge.target].path).emit();
        }
        visit(edge.target, depth + 1);
      }
      stack.pop_back();
      state[id] = 2;
      sorted.push_back(id);
    };
    visit(0, 0);
    return sorted;
  }

public:
  Loader(const std::vector<Token> &tokens, SourceManager *sources) : sources(sources) {
    namespace fs = std::filesystem;
    std::string path = tokens.front().loc.file;
    if (path.empty())
      path = (fs::current_path() / "<input>").string();
    path = fs::weakly_canonical(path).string();
    modules.push_back({0, path, false, tokens, {}});
    files[path] = 0;
  }
  modules::Compilation run() {
    discover();
    dependency(0, "std.prelude", {});
    discover();
    for (unsigned round = 0; round < 128; ++round) {
      auto before = modules.size();
      auto edgeCount = [&] {
        size_t count = 0;
        for (const auto &m : modules)
          count += m.dependencies.size();
        return count;
      };
      auto beforeEdges = edgeCount();
      std::vector<macros::ExpansionInput> inputs;
      for (auto id : order())
        inputs.push_back({id, modules[id].trusted, modules[id].tokens});
      auto expanded = macros::expandModules(inputs);
      for (const auto &module : expanded.modules)
        scanTrees(module.module, macros::tokenTrees(module.tokens), true);
      discover();

      auto sorted = order();
      if (modules.size() != before || edgeCount() != beforeEdges)
        continue;
      ast::Program ast;
      for (const auto &module : expanded.modules)
        append(ast, parse(module.tokens, module.trusted));
      return {std::move(ast), std::move(expanded), std::move(modules)};
    }
    fail({}, DiagnosticCode::InvalidImport, "module/macro discovery limit reached (128 rounds)");
  }
};
}
namespace modules {
Compilation compile(const std::vector<Token> &tokens, SourceManager *sources) {
  return Loader(tokens, sources).run();
}
void print(const std::vector<Module> &modules, std::ostream &out) {
  for (const auto &module : modules) {
    out << "module #M" << module.id << " " << module.path
        << " trust=" << (module.trusted ? "bundled" : "user") << "\n";
    for (const auto &edge : module.dependencies)
      out << "  import #M" << edge.target << "\n";
  }
}
}
ast::Program loadImports(ast::Program source, SourceManager *sources) {

  std::string directives;
  for (const auto &import : source.imports)
    directives += "@import " + quoteText(import) + "\n";
  auto tokens = lex(directives);
  Loc origin = !source.importOrigins.empty() ? source.importOrigins[0]
               : !source.functions.empty()   ? source.functions[0].loc
                                             : Loc{};
  for (auto &token : tokens)
    token.loc.file = origin.file;
  auto compilation = modules::compile(tokens, sources);
  append(compilation.ast, std::move(source));
  return std::move(compilation.ast);
}
}
