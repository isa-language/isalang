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
  result.foreignTypes.insert(result.foreignTypes.end(), p.foreignTypes.begin(),
                             p.foreignTypes.end());
  result.importAliases.insert(result.importAliases.end(), p.importAliases.begin(),
                              p.importAliases.end());
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

ast::Program resolveNamespaces(const std::vector<modules::Module> &modules,
                               const macros::Expanded &expanded,
                               const ast::Program *root = nullptr) {
  using Names = std::map<std::string, std::string>;
  using Locals = std::set<std::pair<std::string, SyntaxContext>>;
  std::vector<ast::Program> programs(modules.size());
  std::vector<Names> names(modules.size());
  std::vector<std::set<std::string>> constants(modules.size());
  for (const auto &input : expanded.modules) {
    auto &p = programs[input.module];
    p = input.module == 0 && root ? *root : parse(input.tokens, input.trusted);
    auto declare = [&](const std::string &name) {
      names[input.module][name] = input.module == 0 || input.trusted
                                      ? name
                                      : "module$" + std::to_string(input.module) + "$" + name;
    };
    for (auto &x : p.structs)
      declare(x.name);
    for (auto &x : p.foreignTypes)
      declare(x.name);
    for (auto &x : p.enums)
      declare(x.name);
    for (auto &x : p.protocols)
      declare(x.name);
    for (auto &x : p.constants) {
      declare(x.name);
      constants[input.module].insert(x.name);
    }
    for (auto &x : p.functions)
      if (x.owner.empty())
        declare(x.name);
  }
  auto definitionModule = [&](uint32_t module, const Loc &loc) {
    if (loc.context && loc.context <= expanded.expansions.size()) {
      auto &expansion = expanded.expansions[loc.context - 1];
      return expanded.declarations[expansion.macro].module;
    }
    return module;
  };
  auto visible = [&](uint32_t module) {
    std::set<uint32_t> result;
    std::function<void(uint32_t)> visit = [&](uint32_t id) {
      if (!result.insert(id).second)
        return;
      for (auto &edge : modules[id].dependencies)
        if (!edge.qualifiedOnly)
          visit(edge.target);
    };
    visit(module);
    for (auto &m : modules)
      if (m.trusted)
        result.insert(m.id);
    return result;
  };
  auto resolve = [&](uint32_t module, const std::string &path, const Loc &loc) {
    module = definitionModule(module, loc);
    auto split = path.find("::");
    auto head = path.substr(0, split);
    auto tail = split == std::string::npos ? "" : path.substr(split);
    if (split != std::string::npos) {
      for (auto &edge : modules[module].dependencies)
        if (edge.alias == head) {
          auto item = path.substr(split + 2);
          auto separator = item.find("::");
          auto key = item.substr(0, separator);
          auto found = names[edge.target].find(key);
          if (found == names[edge.target].end())
            fail(loc, DiagnosticCode::UnknownName,
                 "module `" + head + "` has no item `" + key + "`");
          return found->second + (separator == std::string::npos ? "" : item.substr(separator));
        }
    }
    if (auto found = names[module].find(head); found != names[module].end())
      return found->second + tail;
    std::set<std::string> candidates;
    for (auto id : visible(module))
      if (auto found = names[id].find(head); found != names[id].end())
        candidates.insert(found->second);
    if (candidates.size() > 1)
      DiagnosticEngine()
          .error(DiagnosticCode::AmbiguousImport, loc, "ambiguous imported name `" + head + "`")
          .help("qualify the item with its module alias, or import with `as alias`")
          .emit();
    return candidates.empty() ? path : *candidates.begin() + tail;
  };
  for (auto &m : modules) {
    auto &p = programs[m.id];
    auto resolveType = [&](auto &&self, TypeSyntax &t,
                           const std::set<std::string> &parameters) -> void {
      if (!parameters.contains(t.name) && t.name != "self")
        t.name = resolve(m.id, t.name, t.loc);
      if (!t.countName.empty())
        t.countName = resolve(m.id, t.countName, t.loc);
      for (auto &[count, name] : t.dimensions)
        if (!name.empty())
          name = resolve(m.id, name, t.loc);
      for (auto &a : t.arguments)
        self(self, a, parameters);
    };
    std::function<void(ast::Expr &, Locals &, const std::set<std::string> &)> expression;
    std::function<void(std::vector<ast::Stmt> &, Locals, const std::set<std::string> &)> block;
    auto pattern = [&](ast::Pattern &pat, Locals &locals) {
      if (pat.kind == ast::Pattern::Variant)
        pat.name = resolve(m.id, pat.name, pat.loc);
      else if (pat.kind == ast::Pattern::Binding) {
        bool constant = false;
        for (auto id : visible(m.id))
          constant |= constants[id].contains(pat.name);
        if (constant)
          pat.name = resolve(m.id, pat.name, pat.loc);
        else
          locals.insert({pat.name, pat.loc.context});
      }
      for (auto &name : pat.bindings)
        if (name != "_")
          locals.insert({name, pat.loc.context});
    };
    expression = [&](ast::Expr &e, Locals &locals, const std::set<std::string> &parameters) {
      if ((e.kind == ast::Expr::Name || e.kind == ast::Expr::Call ||
           (e.kind == ast::Expr::BraceInit && !e.text.empty())) &&
          !locals.contains({e.text, e.loc.context}) && !parameters.contains(e.text))
        e.text = resolve(m.id, e.text, e.loc);
      for (auto &t : e.typeArguments)
        resolveType(resolveType, t, parameters);
      for (auto &a : e.args)
        expression(a, locals, parameters);
      block(e.body, locals, parameters);
    };
    block = [&](std::vector<ast::Stmt> &body, Locals locals,
                const std::set<std::string> &parameters) {
      for (auto &s : body) {
        if (s.classicFor && s.kind == ast::Stmt::For) {
          auto inner = locals;
          for (auto &init : s.headerInit) {
            resolveType(resolveType, init.annotation, parameters);
            expression(init.expr, inner, parameters);
            if (init.kind == ast::Stmt::Let)
              inner.insert({init.name, init.loc.context});
          }
          expression(s.expr, inner, parameters);
          expression(s.limit, inner, parameters);
          block(s.body, inner, parameters);
          continue;
        }
        resolveType(resolveType, s.annotation, parameters);
        expression(s.expr, locals, parameters);
        expression(s.limit, locals, parameters);
        auto inner = locals;
        if (s.kind == ast::Stmt::Let || s.kind == ast::Stmt::Const)
          locals.insert({s.name, s.loc.context});
        if (s.kind == ast::Stmt::Borrow || s.kind == ast::Stmt::For)
          inner.insert({s.name, s.loc.context});
        if (s.conditionalPattern || s.pattern.kind == ast::Pattern::Variant ||
            !s.pattern.name.empty())
          pattern(s.pattern, inner);
        block(s.body, inner, parameters);
        block(s.otherwise, locals, parameters);
      }
    };
    for (auto &x : p.constants) {
      Locals locals;
      expression(x.expr, locals, {});
      resolveType(resolveType, x.annotation, {});
      x.name = names[m.id].at(x.name);
    }
    for (auto &x : p.structs) {
      if (x.foreignC)
        x.cName = x.name;
      std::set<std::string> parameters(x.parameters.begin(), x.parameters.end());
      for (auto &f : x.fields)
        resolveType(resolveType, f.type, parameters);
      if (!x.protocol.empty())
        x.protocol = resolve(m.id, x.protocol, x.loc);
      x.name = names[m.id].at(x.name);
    }
    for (auto &x : p.foreignTypes) {
      x.cName = x.name;
      resolveType(resolveType, x.target, {});
      x.name = names[m.id].at(x.name);
    }
    for (auto &x : p.enums) {
      std::set<std::string> parameters(x.parameters.begin(), x.parameters.end());
      for (auto &v : x.variants)
        for (auto &t : v.payload)
          resolveType(resolveType, t, parameters);
      x.name = names[m.id].at(x.name);
    }
    for (auto &x : p.protocols)
      x.name = names[m.id].at(x.name);
    std::map<uint32_t, std::set<std::string>> methodParameters;
    for (auto &x : p.impls) {
      std::set<std::string> parameters(x.parameters.begin(), x.parameters.end());
      resolveType(resolveType, x.target, parameters);
      x.owner = resolve(m.id, x.owner, x.loc);
      for (auto method : x.methods)
        methodParameters[method] = parameters;
    }
    for (uint32_t n = 0; n < p.functions.size(); ++n) {
      auto &f = p.functions[n];
      auto parameters = methodParameters[n];
      if (f.foreignC)
        f.cLinkName = f.name;
      if (f.owner.empty())
        f.name = names[m.id].at(f.name);
      else
        f.owner = resolve(m.id, f.owner, f.loc);
      Locals locals;
      for (auto &param : f.params) {
        resolveType(resolveType, param.type, parameters);
        locals.insert({param.name, param.loc.context});
      }
      resolveType(resolveType, f.result, parameters);
      block(f.body, locals, parameters);
    }
  }
  ast::Program result;
  for (auto &input : expanded.modules)
    append(result, std::move(programs[input.module]));
  return result;
}

class Loader {
  using Tree = macros::TokenTree;
  SourceManager *sources;
  const ast::Program *root;
  std::vector<modules::Module> modules;
  std::map<std::string, uint32_t> files;
  std::set<uint32_t> scanned;
  static bool is(const Tree &t, const char *text) {
    return !t.delimiter && t.token.kind != Token::String && t.token.text == text;
  }
  void dependency(uint32_t source, const std::string &spec, Loc origin, std::string alias = {}) {
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
    bool qualifiedOnly = !alias.empty();
    if (alias.empty())
      alias = trusted ? spec.substr(spec.find_last_of('.') + 1) : fs::path(spec).stem().string();
    auto &edges = modules[source].dependencies;
    for (const auto &edge : edges)
      if (edge.alias == alias && edge.target != target)
        fail(origin, DiagnosticCode::InvalidImport,
             "duplicate import alias `" + alias + "`; use an explicit alias");
    if (std::none_of(edges.begin(), edges.end(), [&](auto &edge) {
          return edge.target == target && edge.alias == alias &&
                 edge.qualifiedOnly == qualifiedOnly;
        }))
      edges.push_back({target, origin, alias, qualifiedOnly});
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
        auto paths = [&](const std::vector<Tree> &parts) {
          size_t index = 0;
          while (index < parts.size()) {
            if (parts[index].delimiter || parts[index].token.kind != Token::String)
              fail(loc, DiagnosticCode::InvalidImport, "@import requires string literal paths");
            auto spec = parts[index++].token.text;
            std::string alias;
            if (index < parts.size() && is(parts[index], "as")) {
              ++index;
              if (index >= parts.size() || parts[index].token.kind != Token::Identifier)
                fail(loc, DiagnosticCode::InvalidImport, "expected an import alias");
              alias = parts[index++].token.text;
            }
            dependency(id, spec, loc, alias);
            if (index == parts.size())
              break;
            if (!is(parts[index++], ","))
              fail(loc, DiagnosticCode::InvalidImport, "@import paths require commas");
          }
          if (parts.empty())
            fail(loc, DiagnosticCode::InvalidImport, "@import requires at least one path");
        };
        if (!argument.delimiter && argument.token.kind == Token::String) {
          std::vector<Tree> parts{argument};
          if (n + 3 < trees.size() && is(trees[n + 3], "as")) {
            if (n + 4 >= trees.size())
              fail(loc, DiagnosticCode::InvalidImport, "expected import alias");
            parts.push_back(trees[n + 3]);
            parts.push_back(trees[n + 4]);
            n += 2;
          }
          paths(parts);
        } else if (argument.delimiter == '(')
          paths(argument.children);
        else
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
  Loader(const std::vector<Token> &tokens, SourceManager *sources,
         const ast::Program *root = nullptr)
      : sources(sources), root(root) {
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
      for (auto id : order()) {
        macros::ExpansionInput input{id, modules[id].trusted, modules[id].tokens, {}, {}};
        std::set<uint32_t> visible;
        std::function<void(uint32_t)> expose = [&](uint32_t m) {
          if (!visible.insert(m).second)
            return;
          for (auto &edge : modules[m].dependencies)
            if (!edge.qualifiedOnly)
              expose(edge.target);
        };
        expose(id);

        for (auto &m : modules)
          if (m.trusted)
            visible.insert(m.id);
        input.visible.assign(visible.begin(), visible.end());
        for (auto &edge : modules[id].dependencies)
          input.aliases[edge.alias] = edge.target;
        inputs.push_back(std::move(input));
      }
      auto expanded = macros::expandModules(inputs);
      for (const auto &module : expanded.modules)
        scanTrees(module.module, macros::tokenTrees(module.tokens), true);
      discover();

      auto sorted = order();
      if (modules.size() != before || edgeCount() != beforeEdges)
        continue;
      auto ast = resolveNamespaces(modules, expanded, root);
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
      out << "  import #M" << edge.target << " as " << edge.alias
          << (edge.qualifiedOnly ? " qualified" : " open") << "\n";
  }
}
}
ast::Program loadImports(ast::Program source, SourceManager *sources) {

  std::string directives;
  for (size_t n = 0; n < source.imports.size(); ++n) {
    directives += "@import " + quoteText(source.imports[n]);
    if (n < source.importAliases.size() && !source.importAliases[n].empty())
      directives += " as " + source.importAliases[n];
    directives += "\n";
  }
  auto tokens = lex(directives);
  Loc origin = !source.importOrigins.empty() ? source.importOrigins[0]
               : !source.functions.empty()   ? source.functions[0].loc
                                             : Loc{};
  for (auto &token : tokens)
    token.loc.file = origin.file;
  auto compilation = Loader(tokens, sources, &source).run();
  return std::move(compilation.ast);
}
}
