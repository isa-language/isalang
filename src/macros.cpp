#include "isac/macros.hpp"
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <set>
namespace isac::macros {
namespace {
using Trees = std::vector<TokenTree>;
bool is(const TokenTree &t, const std::string &s) {
  return !t.delimiter && t.token.kind != Token::String && t.token.text == s;
}
void flatten(const Trees &trees, std::vector<Token> &out) {
  for (auto &t : trees) {
    out.push_back(t.token);
    if (t.delimiter) {
      flatten(t.children, out);
      out.push_back(t.closing);
    }
  }
}
Trees group(const std::vector<Token> &tokens, size_t &pos, char end = 0, unsigned depth = 0) {
  if (depth > 128)
    fail(tokens.at(pos).loc, DiagnosticCode::MacroRecursion,
         "token-tree nesting limit reached (128)");
  Trees result;
  while (pos < tokens.size() && tokens[pos].kind != Token::End) {
    auto token = tokens[pos++];
    bool punct = token.kind == Token::Punctuation;
    if (punct && (token.text == ")" || token.text == "}" || token.text == "]")) {
      if (token.text[0] != end)
        fail(token.loc, DiagnosticCode::UnexpectedToken, "mismatched token-tree delimiter");
      return result;
    }
    TokenTree tree{token, 0, {}, {}};
    if (punct && (token.text == "(" || token.text == "{" || token.text == "[")) {
      tree.delimiter = token.text[0];
      tree.children = group(tokens, pos,
                            tree.delimiter == '('   ? ')'
                            : tree.delimiter == '{' ? '}'
                                                    : ']',
                            depth + 1);
      tree.closing = tokens[pos - 1];
    }
    result.push_back(std::move(tree));
  }
  if (end)
    fail(tokens.back().loc, DiagnosticCode::UnexpectedToken, "unterminated token-tree group");
  return result;
}
using Captures = std::map<std::string, std::vector<Trees>>;
struct Repetition {
  Trees body;
  std::string separator;
  bool plus;
  size_t next;
};
Repetition repetition(const Trees &p, size_t i) {
  auto l = p[i].token.loc;
  if (i + 2 >= p.size() || p[i + 1].delimiter != '(')
    fail(l, DiagnosticCode::MacroSyntax, "expected $(...) repetition");
  Repetition r{p[i + 1].children, "", false, i + 2};
  if (is(p[r.next], ",") || is(p[r.next], ";"))
    r.separator = p[r.next++].token.text;
  if (r.next == p.size() || (!is(p[r.next], "*") && !is(p[r.next], "+")))
    fail(l, DiagnosticCode::MacroSyntax, "repetition requires * or +");
  r.plus = is(p[r.next++], "+");
  return r;
}
bool fragment(const std::string &kind, const Trees &value) {
  if (value.empty())
    return false;
  if (kind == "tt")
    return value.size() == 1;
  if (kind == "ident")
    return value.size() == 1 && !value[0].delimiter && value[0].token.kind == Token::Identifier;
  if (kind == "literal") {
    size_t n = value.size() == 2 && is(value[0], "-") ? 1 : 0;
    if (value.size() != n + 1 || value[n].delimiter)
      return false;
    auto &t = value[n].token;
    return t.kind == Token::Number ||
           (!n && (t.kind == Token::String || t.kind == Token::Character || is(value[n], "true") ||
                   is(value[n], "false")));
  }
  if (kind == "expr" || kind == "type") {
    std::vector<Token> tokens;
    flatten(value, tokens);
    tokens.push_back({"<eof>", value.back().token.loc, Token::End});
    return parseFragment(tokens, kind);
  }
  std::string prefix, suffix;
  if (kind == "expr") {
    prefix = "fn __fragment() { let value = ";
    suffix = "; }";
  } else if (kind == "type") {
    prefix = "fn __fragment(value: ";
    suffix = ") {}";
  } else if (kind == "pattern") {
    prefix = "fn __fragment() { match value { ";
    suffix = " => {}, } }";
  } else
    fail(value[0].token.loc, DiagnosticCode::MacroSyntax, "unknown macro fragment `" + kind + "`");
  auto tokens = lex(prefix);
  tokens.pop_back();
  flatten(value, tokens);
  auto tail = lex(suffix);
  tokens.insert(tokens.end(), tail.begin(), tail.end());
  try {
    auto p = parse(tokens);
    return p.functions.at(0).body.at(0).body.size() == 1;
  } catch (const Error &) {
    return false;
  }
}
using Builtin = BuiltinId;
const std::map<std::string, Builtin> builtinRegistry = {
    {"__format", Builtin::Format}, {"__print", Builtin::Print},  {"__println", Builtin::Println},
    {"__input", Builtin::Input},   {"size_of", Builtin::SizeOf}, {"align_of", Builtin::AlignOf}};
struct FormatSegment {
  enum Kind { Literal, Display, Debug } kind;
  std::string text;
};
std::vector<FormatSegment> formatString(const Token &token) {
  std::vector<FormatSegment> segments;
  std::string literal;
  auto flush = [&] {
    if (!literal.empty()) {
      segments.push_back({FormatSegment::Literal, literal});
      literal.clear();
    }
  };
  for (size_t i = 0; i < token.text.size();) {
    char c = token.text[i++];
    if (c != '{' && c != '}') {
      literal += c;
      continue;
    }
    if (i < token.text.size() && token.text[i] == c) {
      literal += c;
      ++i;
      continue;
    }
    if (c == '}')
      fail(token.loc, DiagnosticCode::FormatArguments,
           "unmatched } in format string; use }} for a literal brace");
    auto end = token.text.find('}', i);
    if (end == std::string::npos)
      fail(token.loc, DiagnosticCode::FormatArguments, "unclosed format placeholder");
    auto spec = token.text.substr(i, end - i);
    if (!spec.empty() && spec != ":?")
      fail(token.loc, DiagnosticCode::FormatArguments,
           "unsupported format specifier; use {} or {:?}");
    flush();
    segments.push_back({spec.empty() ? FormatSegment::Display : FormatSegment::Debug, {}});
    i = end + 1;
  }
  flush();
  return segments;
}
class Expander {
  Expanded result;
  std::map<std::string, size_t> names;
  std::map<std::string, std::string> captureKinds;
  size_t budget = 100000;
  std::optional<AtSymbol> resolveAt(const std::string &name) const {
    if (names.contains(name))
      return AtSymbol{AtSymbolKind::Macro, static_cast<uint32_t>(names.at(name))};
    if (builtinRegistry.contains(name)) {
      auto id = builtinRegistry.at(name);
      return AtSymbol{id == Builtin::SizeOf || id == Builtin::AlignOf
                          ? AtSymbolKind::Intrinsic
                          : AtSymbolKind::CompilerBuiltin,
                      static_cast<uint32_t>(id)};
    }
    return std::nullopt;
  }
  void validatePattern(const Trees &trees, std::set<std::string> &captures, bool repeated = false) {
    static const std::set<std::string> fragments = {"expr",    "ident", "type",
                                                    "literal", "tt",    "pattern"};
    for (size_t n = 0; n < trees.size();) {
      if (is(trees[n], "$")) {
        if (n + 1 < trees.size() && trees[n + 1].delimiter) {
          if (repeated)
            fail(trees[n].token.loc, DiagnosticCode::MacroSyntax,
                 "nested repetitions are outside this PoC");
          auto r = repetition(trees, n);
          if (r.body.empty())
            fail(trees[n].token.loc, DiagnosticCode::MacroSyntax, "empty repetition matcher");
          validatePattern(r.body, captures, true);
          n = r.next;
          continue;
        }
        if (n + 3 >= trees.size() || trees[n + 1].token.kind != Token::Identifier ||
            !is(trees[n + 2], ":") || !fragments.contains(trees[n + 3].token.text))
          fail(trees[n].token.loc, DiagnosticCode::MacroSyntax,
               "expected $name:fragment (expr, ident, type, literal, tt or pattern)");
        if (!captures.insert(trees[n + 1].token.text).second)
          fail(trees[n].token.loc, DiagnosticCode::MacroSyntax,
               "duplicate metavariable in matcher");
        n += 4;
        continue;
      }
      if (trees[n].delimiter)
        validatePattern(trees[n].children, captures, repeated);
      ++n;
    }
  }
  Trees format(Builtin builtin, const Trees &arguments, Loc call,
               std::string inputType = "String") {
    std::vector<Trees> args(1);
    for (auto &tree : arguments) {
      if (is(tree, ","))
        args.emplace_back();
      else
        args.back().push_back(tree);
    }
    if (args.back().empty())
      args.pop_back();
    for (auto &arg : args)
      if (arg.empty())
        fail(call, DiagnosticCode::FormatArguments, "empty formatting argument");
    Token fmt{"", call, Token::String};
    size_t first = 0;
    if (!args.empty()) {
      if (args[0].size() == 1 && args[0][0].token.kind == Token::String) {
        fmt = args[0][0].token;
        first = 1;
      } else
        fail(call, DiagnosticCode::FormatArguments,
             "format string must be a compile-time string literal");
    } else if (builtin == Builtin::Format)
      fail(call, DiagnosticCode::FormatArguments, "@format requires a string literal");
    auto segments = formatString(fmt);
    size_t expected = 0;
    for (auto &s : segments)
      expected += s.kind != FormatSegment::Literal;
    if (expected != args.size() - first)
      DiagnosticEngine()
          .error(DiagnosticCode::FormatArguments, call,
                 "format string expects " + std::to_string(expected) + " arguments but " +
                     std::to_string(args.size() - first) + " provided")
          .secondary(fmt.loc, "format string declared here")
          .emit();

    std::vector<Token> tokens;
    auto add = [&](const std::string &source) {
      auto part = lex(source);
      part.pop_back();
      for (auto &t : part) {
        t.loc = call;
        t.loc.context = 0x80000000u + result.expansions.size();
      }
      tokens.insert(tokens.end(), part.begin(), part.end());
    };
    add("{ let __formatted = String::new();");
    size_t argument = first;
    for (auto &s : segments) {
      if (s.kind == FormatSegment::Literal) {
        add("__formatted.push_str(");
        tokens.push_back({s.text, call, Token::String});
        add(");");
      } else {
        add(s.kind == FormatSegment::Display ? "__formatted.append_display("
                                             : "__formatted.append_debug(");
        flatten(args[argument++], tokens);
        add(");");
      }
    }
    if (builtin == Builtin::Format)
      add("__formatted.move() }");
    else {
      add("write_stdout(__formatted.as_str());");
      if (builtin == Builtin::Println)
        add("write_stdout(\"\\n\");");
      if (builtin == Builtin::Input)
        add(inputType == "String" ? "read_line() }" : "read_" + inputType + "() }");
      else
        add("}");
    }
    tokens.push_back({"<eof>", call, Token::End});
    return tokenTrees(tokens);
  }
  bool match(const Trees &p, size_t i, const Trees &v, size_t j, Captures &caps, size_t &end,
             bool full) {
    if (!budget--)
      fail(p.empty() ? Loc{} : p[0].token.loc, DiagnosticCode::MacroMatch,
           "macro matching work limit reached");
    if (i == p.size()) {
      end = j;
      return !full || j == v.size();
    }
    if (is(p[i], "$") && i + 1 < p.size() && p[i + 1].delimiter) {
      auto r = repetition(p, i);
      std::vector<std::pair<size_t, Captures>> states{{j, caps}};
      for (;;) {
        auto start = states.back().first;
        if (states.size() > 1 && !r.separator.empty()) {
          if (start == v.size() || !is(v[start], r.separator))
            break;
          ++start;
        }
        auto next = states.back().second;
        size_t stop = start;
        if (!match(r.body, 0, v, start, next, stop, false) || stop == start)
          break;
        states.push_back({stop, std::move(next)});
      }
      for (size_t n = states.size(); n-- > (r.plus ? 1u : 0u);) {
        auto next = states[n].second;
        if (match(p, r.next, v, states[n].first, next, end, full)) {
          caps = std::move(next);
          return true;
        }
      }
      return false;
    }
    if (is(p[i], "$")) {
      if (i + 3 >= p.size() || !is(p[i + 2], ":"))
        fail(p[i].token.loc, DiagnosticCode::MacroSyntax, "capture requires $name:fragment");
      auto name = p[i + 1].token.text, kind = p[i + 3].token.text;
      captureKinds[name] = kind;
      for (size_t n = v.size(); n > j; --n) {
        Trees value(v.begin() + j, v.begin() + n);
        if (!fragment(kind, value))
          continue;
        auto next = caps;
        next[name].push_back(std::move(value));
        if (match(p, i + 4, v, n, next, end, full)) {
          caps = std::move(next);
          return true;
        }
      }
      return false;
    }
    if (j == v.size() || p[i].delimiter != v[j].delimiter)
      return false;
    auto next = caps;
    if (p[i].delimiter) {
      size_t inner;
      if (!match(p[i].children, 0, v[j].children, 0, next, inner, true))
        return false;
    } else if (p[i].token.kind != v[j].token.kind || p[i].token.text != v[j].token.text)
      return false;
    if (!match(p, i + 1, v, j + 1, next, end, full))
      return false;
    caps = std::move(next);
    return true;
  }
  void captureNames(const Trees &p, std::set<std::string> &out) {
    for (size_t i = 0; i < p.size(); ++i) {
      if (is(p[i], "$") && i + 1 < p.size() && !p[i + 1].delimiter)
        out.insert(p[i + 1].token.text);
      if (p[i].delimiter)
        captureNames(p[i].children, out);
    }
  }
  Trees substitute(const Trees &p, const Captures &caps, const MacroDecl &m, Loc call,
                   ExpansionId id, size_t index = SIZE_MAX) {
    Trees out;
    for (size_t i = 0; i < p.size();) {
      if (is(p[i], "$")) {
        if (i + 1 == p.size())
          fail(call, DiagnosticCode::MacroSyntax, "incomplete metavariable");
        if (p[i + 1].delimiter) {
          if (index != SIZE_MAX)
            fail(call, DiagnosticCode::MacroSyntax, "nested repetitions are outside this PoC");
          auto r = repetition(p, i);
          std::set<std::string> variables;
          captureNames(r.body, variables);
          size_t count = SIZE_MAX;
          for (auto &name : variables) {
            auto it = caps.find(name);
            size_t n = it == caps.end() ? 0 : it->second.size();
            if (count != SIZE_MAX && n != count)
              fail(call, DiagnosticCode::MacroMatch, "repetition captures have different lengths");
            count = n;
          }
          if (count == SIZE_MAX || (r.plus && !count))
            fail(call, DiagnosticCode::MacroMatch, "repetition has no matching captures");
          for (size_t n = 0; n < count; ++n) {
            if (n && !r.separator.empty())
              out.push_back({{r.separator, call, Token::Punctuation}, 0, {}, {}});
            auto part = substitute(r.body, caps, m, call, id, n);
            out.insert(out.end(), part.begin(), part.end());
          }
          i = r.next;
          continue;
        }
        auto name = p[i + 1].token.text;
        auto it = caps.find(name);
        if (it == caps.end() || (index == SIZE_MAX && it->second.size() != 1))
          fail(call, DiagnosticCode::MacroMatch,
               "unbound or repeating metavariable `" + name + "`");
        auto value = it->second.at(index == SIZE_MAX ? 0 : index);
        std::function<void(Trees &)> origin = [&](Trees &ts) {
          for (auto &t : ts) {
            t.token.loc.expansions.push_back({id, m.origin.span, m.name});
            origin(t.children);
          }
        };
        origin(value);
        if (captureKinds[name] == "expr") {
          TokenTree group;
          group.delimiter = '(';
          group.token = {"(", value.front().token.loc, Token::Punctuation};
          group.closing = {")", value.back().token.loc, Token::Punctuation};
          group.children = std::move(value);
          value = {std::move(group)};
        }

        out.insert(out.end(), value.begin(), value.end());
        i += 2;
        continue;
      }
      auto t = p[i++];
      auto annotate = [&](Token &token) {
        token.loc = call;
        token.loc.context = id;
        token.loc.expansions.push_back({id, m.origin.span, m.name});
      };
      annotate(t.token);
      if (t.delimiter) {
        annotate(t.closing);
        t.children = substitute(t.children, caps, m, call, id, index);
      }
      out.push_back(std::move(t));
    }
    return out;
  }
  Trees expandTrees(const Trees &trees, unsigned depth, ExpansionId parent) {
    Trees out;
    for (size_t i = 0; i < trees.size(); ++i) {
      auto t = trees[i];
      if (is(t, "@") && i + 5 < trees.size() && is(trees[i + 1], "input") &&
          is(trees[i + 2], "<") && is(trees[i + 4], ">") && trees[i + 5].delimiter == '(') {
        auto type = trees[i + 3].token.text;
        if (!std::set<std::string>{"String", "i32", "i64", "u32", "u64", "f32", "f64", "bool"}
                 .contains(type))
          fail(t.token.loc, DiagnosticCode::FormatType,
               "unsupported typed input target `" + type + "`");
        auto &macro = result.declarations.at(names.at("input"));
        ExpansionId expansion = result.expansions.size() + 1;
        t.token.loc.span.end = trees[i + 5].closing.loc.span.end;
        result.expansions.push_back({expansion, parent, macro.id, t.token.loc, macro.origin});
        t.token.loc.expansions.push_back({expansion, macro.origin.span, macro.name});
        auto expanded = format(Builtin::Input, trees[i + 5].children, t.token.loc, type);
        expanded = expandTrees(expanded, depth + 1, parent);
        out.insert(out.end(), expanded.begin(), expanded.end());
        i += 5;
        continue;
      }
      if (is(t, "@") && i + 2 < trees.size() && builtinRegistry.contains(trees[i + 1].token.text) &&
          builtinRegistry.at(trees[i + 1].token.text) != Builtin::SizeOf &&
          builtinRegistry.at(trees[i + 1].token.text) != Builtin::AlignOf) {
        if (trees[i + 2].delimiter != '(')
          fail(t.token.loc, DiagnosticCode::MacroSyntax, "builtin requires parentheses");
        auto expanded =
            format(builtinRegistry.at(trees[i + 1].token.text), trees[i + 2].children, t.token.loc);
        expanded = expandTrees(expanded, depth + 1, parent);
        out.insert(out.end(), expanded.begin(), expanded.end());
        i += 2;
        continue;
      }
      auto symbol =
          is(t, "@") && i + 1 < trees.size() ? resolveAt(trees[i + 1].token.text) : std::nullopt;
      if (symbol && symbol->kind == AtSymbolKind::Macro && i + 2 < trees.size()) {
        if (depth >= 128)
          DiagnosticEngine()
              .error(DiagnosticCode::MacroRecursion, t.token.loc,
                     "macro expansion recursion limit reached")
              .note("limit: 128")
              .emit();
        t.token.loc.span.end = trees[i + 2].closing.loc.span.end;
        auto &m = result.declarations.at(symbol->id);
        if (trees[i + 2].delimiter != '(')
          fail(t.token.loc, DiagnosticCode::MacroSyntax, "macro invocation requires parentheses");
        Captures captures;
        const Rule *selected = nullptr;
        for (auto &r : m.rules) {
          Captures candidate;
          size_t end;
          budget = 100000;
          if (match(r.pattern, 0, trees[i + 2].children, 0, candidate, end, true)) {
            selected = &r;
            captures = std::move(candidate);
            break;
          }
        }
        if (!selected)
          fail(t.token.loc, DiagnosticCode::MacroMatch, "no matching rule for @" + m.name);
        if (result.expansions.size() >= 10000)
          fail(t.token.loc, DiagnosticCode::MacroRecursion,
               "total macro expansion limit reached (10000)");
        ExpansionId id = result.expansions.size() + 1;
        result.expansions.push_back({id, parent, m.id, t.token.loc, m.origin});
        auto expanded = expandTrees(substitute(selected->expansion, captures, m, t.token.loc, id),
                                    depth + 1, id);
        expanded = expandTrees(expanded, depth + 1, parent);
        out.insert(out.end(), expanded.begin(), expanded.end());
        i += 2;
        continue;
      }
      if (t.delimiter)
        t.children = expandTrees(t.children, depth, parent);
      out.push_back(std::move(t));
    }
    return out;
  }

public:
  Expanded run(const std::vector<Token> &tokens) {
    auto trees = tokenTrees(tokens);
    Trees program;
    for (size_t i = 0; i < trees.size(); ++i) {
      if (is(trees[i], "@") && i + 1 < trees.size() && is(trees[i + 1], "macro")) {
        if (i + 3 >= trees.size() || trees[i + 2].token.kind != Token::Identifier ||
            trees[i + 3].delimiter != '{')
          fail(trees[i].token.loc, DiagnosticCode::MacroSyntax, "expected @macro name { rules }");
        MacroDecl m;
        m.id = result.declarations.size();
        m.name = trees[i + 2].token.text;
        m.origin = trees[i].token.loc;
        m.module = m.origin.span.file == NoFile ? 0 : m.origin.span.file;
        if (names.contains(m.name))
          fail(m.origin, DiagnosticCode::MacroSyntax, "duplicate macro `" + m.name + "`");
        auto &rules = trees[i + 3].children;
        for (size_t n = 0; n < rules.size();) {
          if (is(rules[n], ";") || is(rules[n], ",")) {
            ++n;
            continue;
          }
          if (n + 2 >= rules.size() || rules[n].delimiter != '(' || !is(rules[n + 1], "=>") ||
              rules[n + 2].delimiter != '{')
            fail(rules[n].token.loc, DiagnosticCode::MacroSyntax,
                 "expected (pattern) => { expansion }");
          std::set<std::string> captures;
          validatePattern(rules[n].children, captures);
          m.rules.push_back({rules[n].children, rules[n + 2].children});
          n += 3;
        }
        names[m.name] = m.id;
        result.declarations.push_back(std::move(m));
        i += 3;
      } else
        program.push_back(trees[i]);
    }
    auto expanded = expandTrees(program, 0, 0);
    flatten(expanded, result.tokens);
    result.tokens.push_back(tokens.back());
    return std::move(result);
  }
};
}
BuiltinId findBuiltin(const std::string &name) {
  auto it = builtinRegistry.find(name);
  return it == builtinRegistry.end() ? BuiltinId::None : it->second;
}
Trees tokenTrees(const std::vector<Token> &tokens) {
  size_t pos = 0;
  return group(tokens, pos);
}
Expanded expand(const std::vector<Token> &tokens, SourceManager *sources) {
  std::string path = std::string(ISAC_LIBRARY_ROOT) + "/std/macros.isa";
  std::ifstream file(path);
  if (!file)
    fail({}, DiagnosticCode::MacroSyntax, "cannot load std macros");
  std::string text{std::istreambuf_iterator<char>(file), {}};
  auto prelude = lex(text, sources ? sources->add(path, text) : NoFile);
  for (auto &token : prelude)
    token.loc.file = path;
  prelude.pop_back();
  prelude.insert(prelude.end(), tokens.begin(), tokens.end());
  return Expander().run(prelude);
}
void print(const Expanded &e, std::ostream &out) {
  for (auto &x : e.expansions)
    out << "// expansion #" << x.id << " parent=#" << x.parent << " macro=#" << x.macro << "\n";
  for (auto &t : e.tokens) {
    if (t.kind == Token::End)
      break;
    out << (t.kind == Token::String ? quoteText(t.text) : t.text);
    if (t.kind == Token::Identifier && t.loc.context)
      out << "#" << t.loc.context;
    out << (t.text == ";" || t.text == "{" || t.text == "}" ? "\n" : " ");
  }
  out << "\n";
}
}
