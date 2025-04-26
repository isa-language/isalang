#include "isac/frontend.hpp"
#include <algorithm>
#include <cctype>
namespace isac {
const char *accessName(Access a) {
  return a == Access::Own ? "Own" : a == Access::Shared ? "Shared" : "Unique";
}
std::vector<Token> lex(const std::string &source, FileId file) {
  std::vector<Token> out;
  size_t i = 0;
  Loc l;
  l.span.file = file;
  auto push = [&](Token t) {
    t.loc.span.end = i;
    out.push_back(std::move(t));
  };
  auto advance = [&]() {
    if (source[i++] == '\n') {
      ++l.line;
      l.column = 1;
    } else
      ++l.column;
    l.span.start = i;
    l.span.end = i;
  };
  while (i < source.size()) {
    unsigned char c = source[i];
    if (std::isspace(c)) {
      advance();
      continue;
    }
    if (source.compare(i, 2, "//") == 0) {
      while (i < source.size() && source[i] != '\n')
        advance();
      continue;
    }
    if (source.compare(i, 2, "/*") == 0) {
      Loc start = l;
      advance();
      advance();
      while (i < source.size() && source.compare(i, 2, "*/"))
        advance();
      if (i == source.size())
        fail(start, DiagnosticCode::LexicalError, "unterminated comment");
      advance();
      advance();
      continue;
    }
    Loc start = l;
    size_t begin = i;
    Token::Kind kind = Token::Punctuation;
    if (c == '\'') {
      advance();
      uint32_t cp = 0;
      if (i >= source.size())
        fail(start, DiagnosticCode::LexicalError, "unterminated char");
      if (source[i] == '\\') {
        advance();
        if (i >= source.size())
          fail(start, DiagnosticCode::LexicalError, "unterminated char escape");
        char e = source[i];
        advance();
        switch (e) {
        case 'n':
          cp = 10;
          break;
        case 't':
          cp = 9;
          break;
        case 'r':
          cp = 13;
          break;
        case '0':
          cp = 0;
          break;
        case '\'':
          cp = 39;
          break;
        case '\\':
          cp = 92;
          break;
        default:
          fail(start, DiagnosticCode::LexicalError, "unsupported char escape");
        }
      } else {
        unsigned char lead = source[i];
        unsigned count = lead < 128              ? 1
                         : (lead & 0xe0) == 0xc0 ? 2
                         : (lead & 0xf0) == 0xe0 ? 3
                         : (lead & 0xf8) == 0xf0 ? 4
                                                 : 0;
        if (!count)
          fail(start, DiagnosticCode::LexicalError, "invalid UTF-8 char");
        cp = lead & (count == 1 ? 127 : count == 2 ? 31 : count == 3 ? 15 : 7);
        advance();
        for (unsigned n = 1; n < count; ++n) {
          if (i >= source.size() || ((unsigned char)source[i] & 0xc0) != 0x80)
            fail(start, DiagnosticCode::LexicalError, "invalid UTF-8 char");
          cp = (cp << 6) | ((unsigned char)source[i] & 63);
          advance();
        }
        if ((count == 2 && cp < 128) || (count == 3 && cp < 2048) || (count == 4 && cp < 65536))
          fail(start, DiagnosticCode::LexicalError, "overlong UTF-8 char");
      }
      if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) || i >= source.size() ||
          source[i] != '\'')
        fail(start, DiagnosticCode::LexicalError, "char requires one Unicode scalar");
      advance();
      push({std::to_string(cp), start, Token::Character});
      continue;
    }
    if (c == '"') {
      advance();
      std::string value;
      while (i < source.size() && source[i] != '"') {
        if (source[i] == '\n')
          break;
        if (source[i] == '\\') {
          auto escape = l;
          advance();
          if (i == source.size())
            break;
          char c = source[i];
          advance();
          switch (c) {
          case 'n':
            value += '\n';
            break;
          case 'r':
            value += '\r';
            break;
          case 't':
            value += '\t';
            break;
          case '0':
            value += '\0';
            break;
          case '\\':
            value += '\\';
            break;
          case '"':
            value += '"';
            break;
          default:
            DiagnosticEngine()
                .error(DiagnosticCode::InvalidEscape, escape, "invalid escape sequence")
                .help("use \\n, \\r, \\t, \\0, \\\\ or \\\" ")
                .emit();
          }
        } else {
          value += source[i];
          advance();
        }
      }
      start.span.end = i;
      if (i == source.size() || source[i] != '"')
        DiagnosticEngine()
            .error(DiagnosticCode::UnterminatedString, start, "unterminated string literal")
            .help("add a closing quote to terminate the string")
            .emit();
      advance();
      if (!validUtf8(value))
        fail(start, DiagnosticCode::InvalidUtf8, "string literal contains invalid UTF-8");
      push({value, start, Token::String});
      continue;
    } else if (std::isalpha(c) || c == '_') {
      kind = Token::Identifier;
      do {
        advance();
      } while (i < source.size() && (std::isalnum((unsigned char)source[i]) || source[i] == '_'));
    } else if (std::isdigit(c)) {
      kind = Token::Number;
      do {
        advance();
      } while (i < source.size() && std::isdigit((unsigned char)source[i]));
      if (i + 1 < source.size() && source[i] == '.' && std::isdigit((unsigned char)source[i + 1])) {
        advance();
        while (i < source.size() && std::isdigit((unsigned char)source[i]))
          advance();
      }
      while (i < source.size() && std::isalnum((unsigned char)source[i]))
        advance();
    } else {
      std::string two = source.substr(i, 2);
      if (two == "->" || two == "::" || two == "==" || two == "!=" || two == "<=" || two == ">=" ||
          two == "&&" || two == "||" || two == "<<" || two == ">>" || two == ".." || two == "=>") {
        advance();
        advance();
      } else if (std::string("@$?{}()[];:,.=+-*/%<>!&|^").find(c) != std::string::npos)
        advance();
      else
        fail(l, DiagnosticCode::LexicalError, "unexpected character");
    }
    push({source.substr(begin, i - begin), start, kind});
  }
  push({"<eof>", l, Token::End});
  return out;
}
class Parser {
  bool trusted;
  std::string annotation;
  bool allowCast = true;
  std::string stringLiteral() {
    auto t = peek();
    if (t.kind != Token::String)
      fail(t.loc, DiagnosticCode::UnexpectedToken, "expected string literal");
    ++pos;
    return t.text;
  }
  std::string attribute() {
    if (!eat("@"))
      return "";
    if (!trusted)
      fail(peek().loc, DiagnosticCode::RestrictedOperation,
           "internal annotations are restricted to bundled libraries");
    auto name = ident().text;
    if (name == "releases")
      return name;
    if (name != "intrinsic" && name != "representation")
      fail(peek().loc, DiagnosticCode::RestrictedOperation, "unknown internal annotation");
    return stringLiteral();
  }
  const std::vector<Token> &ts;
  size_t pos = 0;
  const Token &peek() {
    return ts[pos];
  }
  bool eat(const std::string &s) {
    if (peek().text != s || peek().kind == Token::String || peek().kind == Token::Character)
      return false;
    ++pos;
    return true;
  }
  Token need(const std::string &s) {
    Token t = peek();
    if (!eat(s))
      fail(t.loc, DiagnosticCode::UnexpectedToken, "expected `" + s + "`, found `" + t.text + "`");
    return t;
  }
  Token ident() {
    Token t = peek();
    if (t.text == "async" || t.text == "await")
      DiagnosticEngine()
          .error(DiagnosticCode::FutureFeature, t.loc, t.text + " is not supported yet")
          .note("`" + t.text + "` is reserved for a future language feature")
          .emit();
    if (t.kind != Token::Identifier)
      fail(t.loc, DiagnosticCode::UnexpectedToken, "expected identifier");
    ++pos;
    return t;
  }
  TypeSyntax type() {
    auto n = ident();
    TypeSyntax t{n.text, 0, n.loc};
    if (eat("[")) {
      auto v = peek();
      if (v.kind != Token::Number)
        fail(v.loc, DiagnosticCode::UnexpectedToken, "static array size required");
      ++pos;
      try {
        t.count = std::stoi(v.text);
      } catch (...) {
        fail(v.loc, DiagnosticCode::UnexpectedToken, "array size out of range");
      }
      if (t.count <= 0 || t.count > 65536)
        fail(v.loc, DiagnosticCode::UnexpectedToken, "array size must be in 1..65536");
      need("]");
    }
    return t;
  }
  int prec(const std::string &s) {
    if (s == "=")
      return 1;
    if (s == "||")
      return 2;
    if (s == "&&")
      return 3;
    if (s == "|")
      return 4;
    if (s == "^")
      return 5;
    if (s == "&")
      return 6;
    if (s == "==" || s == "!=")
      return 7;
    if (s == "<" || s == ">" || s == "<=" || s == ">=")
      return 8;
    if (s == "<<" || s == ">>")
      return 9;
    if (s == "+" || s == "-")
      return 10;
    if (s == "*" || s == "/" || s == "%")
      return 11;
    return 0;
  }
  ast::Expr primary() {
    Token t = peek();
    ast::Expr e{ast::Expr::Name, t.loc, t.text, {}};
    if (eat("@")) {
      e.kind = ast::Expr::AtCall;
      e.text = ident().text;
      if (eat("<")) {
        auto t = type();
        e.args.push_back({ast::Expr::Name, t.loc, t.name, {}});
        if (t.count)
          e.args.back().text += "[" + std::to_string(t.count) + "]";
        need(">");
      }
      need("(");
      if (!eat(")")) {
        do {
          e.args.push_back(expr());
        } while (eat(","));
        need(")");
      }
    } else if (eat("{")) {
      e.kind = ast::Expr::Block;
      e.text.clear();
      while (!eat("}")) {
        if (peek().text == "let" || peek().text == "if" || peek().text == "while" ||
            peek().text == "loop" || peek().text == "for" || peek().text == "borrow" ||
            peek().text == "return" || peek().text == "match" || peek().text == "break" ||
            peek().text == "continue") {
          e.body.push_back(stmt());
          continue;
        }
        auto value = expr();
        if (eat(";")) {
          ast::Stmt statement{};
          statement.kind = ast::Stmt::Expression;
          statement.loc = value.loc;
          statement.expr = std::move(value);
          e.body.push_back(std::move(statement));
        } else {
          e.args.push_back(std::move(value));
          need("}");
          break;
        }
      }
      if (e.args.empty())
        e.args.push_back({ast::Expr::Name, t.loc, "unit", {}});
    } else if (eat("(")) {
      e = expr();
      need(")");
    } else if (eat("[")) {
      e.kind = ast::Expr::Array;
      e.text = "";
      if (!eat("]")) {
        do {
          e.args.push_back(expr());
        } while (eat(","));
        need("]");
      }
    } else if (eat("-") || eat("!")) {
      e.kind = ast::Expr::Unary;
      e.args.push_back(primary());
    } else if (t.kind == Token::String) {
      ++pos;
      e.kind = ast::Expr::String;
    } else if (t.kind == Token::Character) {
      ++pos;
      e.kind = ast::Expr::Char;
    } else if (t.kind == Token::Number) {
      ++pos;
      e.kind = ast::Expr::Int;
    } else if (eat("true") || eat("false")) {
      e.kind = ast::Expr::Bool;
    } else {
      if (t.kind != Token::Identifier)
        DiagnosticEngine()
            .error(DiagnosticCode::UnexpectedToken, t.loc, "expected expression")
            .label(t.loc, "expected an expression before `" + t.text + "`")
            .help("provide an expression after `=`")
            .emit();
      ident();
      if (eat("::"))
        e.text += "::" + ident().text;
    }
    for (;;) {
      if (peek().text == "?")
        fail(peek().loc, DiagnosticCode::FutureFeature,
             "error propagation with ? is not implemented; inspect is_ok() and error() explicitly");
      if (eat("(")) {
        ast::Expr c{ast::Expr::Call, e.loc, e.text, {}};
        if (e.kind != ast::Expr::Name)
          fail(e.loc, DiagnosticCode::UnexpectedToken, "call target must be a name");
        if (!eat(")")) {
          do {
            c.args.push_back(expr());
          } while (eat(","));
          need(")");
        }
        e = std::move(c);
      } else if (eat(".")) {
        auto n = ident();
        ast::Expr c{ast::Expr::Member, e.loc, n.text, {std::move(e)}};
        if (eat("(")) {
          c.kind = ast::Expr::Method;
          if (!eat(")")) {
            do {
              c.args.push_back(expr());
            } while (eat(","));
            need(")");
          }
        }
        e = std::move(c);
      } else if (eat("[")) {
        ast::Expr c{ast::Expr::Index, e.loc, "", {std::move(e), expr()}};
        need("]");
        e = std::move(c);
      } else
        break;
    }
    if (pos && ts[pos - 1].loc.span.file == e.loc.span.file &&
        ts[pos - 1].loc.span.end >= e.loc.span.start)
      e.loc.span.end = ts[pos - 1].loc.span.end;
    return e;
  }
  ast::Expr expr(int minimum = 1) {
    auto e = primary();
    while (allowCast && eat("as")) {
      auto t = type();
      if (t.count)
        fail(t.loc, DiagnosticCode::InvalidCast, "cast target must be scalar");
      e = {ast::Expr::Cast, t.loc, t.name, {std::move(e)}};
    }
    while (peek().kind == Token::Punctuation && prec(peek().text) >= minimum) {
      auto op = ts[pos++];
      int p = prec(op.text);
      auto rhs = expr(p + (op.text == "=" ? 0 : 1));
      e = {op.text == "=" ? ast::Expr::Assign : ast::Expr::Binary,
           op.loc,
           op.text,
           {std::move(e), std::move(rhs)}};
    }
    if (pos && ts[pos - 1].loc.span.file == e.loc.span.file &&
        ts[pos - 1].loc.span.end >= e.loc.span.start)
      e.loc.span.end = ts[pos - 1].loc.span.end;
    return e;
  }
  std::vector<ast::Stmt> block() {
    need("{");
    std::vector<ast::Stmt> b;
    while (!eat("}")) {
      if (peek().kind == Token::End)
        fail(peek().loc, DiagnosticCode::UnexpectedToken, "unterminated block");
      b.push_back(stmt());
    }
    return b;
  }
  ast::Stmt stmt() {
    ast::Stmt s{};
    s.loc = peek().loc;
    if (eat(";")) {
      s.kind = ast::Stmt::Expression;
      s.expr = {ast::Expr::Name, s.loc, "unit", {}};
    } else if (eat("let")) {
      s.kind = ast::Stmt::Let;
      {
        auto name = ident();
        s.name = name.text;
        s.loc.context = name.loc.context;
      }
      if (eat(":")) {
        s.annotated = true;
        s.annotation = type();
      }
      need("=");
      s.expr = expr();
      need(";");
    } else if (eat("return")) {
      s.kind = ast::Stmt::Return;
      if (!eat(";")) {
        s.expr = expr();
        need(";");
      } else
        s.expr = {ast::Expr::Name, s.loc, "unit", {}};
    } else if (eat("if")) {
      s.kind = ast::Stmt::If;
      s.expr = expr();
      s.body = block();
      if (eat("else")) {
        if (peek().text == "if")
          s.otherwise = {stmt()};
        else
          s.otherwise = block();
      }
    } else if (eat("match")) {
      s.kind = ast::Stmt::Match;
      s.expr = expr();
      need("{");
      while (!eat("}")) {
        ast::Stmt arm{};
        arm.kind = ast::Stmt::Expression;
        arm.loc = peek().loc;
        auto &p = arm.pattern;
        p.loc = peek().loc;
        if (eat("_"))
          p.kind = ast::Pattern::Wildcard;
        else if (peek().text == "true" || peek().text == "false") {
          p.kind = ast::Pattern::Boolean;
          p.name = ts[pos++].text;
        } else if (peek().kind == Token::Number || peek().text == "-") {
          p.kind = ast::Pattern::Integer;
          if (eat("-"))
            p.name = "-";
          p.name += ts[pos++].text;
        } else {
          p.name = ident().text;
          p.kind = ast::Pattern::Binding;
          if (eat("::")) {
            p.kind = ast::Pattern::Variant;
            p.name += "::" + ident().text;
            if (eat("(")) {
              if (!eat(")")) {
                do {
                  p.bindings.push_back(ident().text);
                } while (eat(","));
                need(")");
              }
            }
          }
        }
        need("=>");
        if (peek().text == "{")
          arm.body = block();
        else {
          ast::Stmt body{};
          body.kind = ast::Stmt::Expression;
          body.loc = peek().loc;
          body.expr = expr();
          arm.body.push_back(body);
        }
        s.body.push_back(std::move(arm));
        if (!eat(",") && peek().text != "}")
          fail(peek().loc, DiagnosticCode::UnexpectedToken, "expected comma after match arm");
      }
    } else if (eat("while")) {
      s.kind = ast::Stmt::While;
      s.expr = expr();
      s.body = block();
    } else if (eat("loop")) {
      s.kind = ast::Stmt::Loop;
      s.body = block();
    } else if (eat("for")) {
      s.kind = ast::Stmt::For;
      {
        auto name = ident();
        s.name = name.text;
        s.loc.context = name.loc.context;
      }
      need("in");
      s.expr = expr();
      need("..");
      s.limit = expr();
      s.body = block();
    } else if (eat("break")) {
      s.kind = ast::Stmt::Break;
      need(";");
    } else if (eat("continue")) {
      s.kind = ast::Stmt::Continue;
      need(";");
    } else if (eat("borrow")) {
      s.kind = ast::Stmt::Borrow;
      if (eat("shared"))
        s.access = Access::Shared;
      else {
        need("unique");
        s.access = Access::Unique;
      }
      allowCast = false;
      s.expr = expr();
      allowCast = true;
      need("as");
      {
        auto name = ident();
        s.name = name.text;
        s.loc.context = name.loc.context;
      }
      s.body = block();
    } else {
      s.kind = ast::Stmt::Expression;
      s.expr = expr();
      need(";");
    }
    return s;
  }
  ast::Function function(std::string owner, bool pub, bool external = false,
                         std::string attr = "") {
    auto n = ident();
    ast::Function f;
    f.trusted = trusted;
    f.external = external;
    f.releases = attr == "releases";
    if (!attr.empty() && !f.releases)
      f.intrinsic = attr;
    f.name = n.text;
    f.loc = n.loc;
    f.owner = std::move(owner);
    f.pub = pub;
    need("(");
    if (!eat(")")) {
      do {
        ast::Param p;
        p.loc = peek().loc;
        if (eat("shared"))
          p.access = Access::Shared;
        else if (eat("unique"))
          p.access = Access::Unique;
        p.name = ident().text;
        if (p.name == "self") {
          if (f.owner.empty())
            fail(p.loc, DiagnosticCode::UnexpectedToken, "self requires impl");
          p.type = {f.owner, 0, p.loc};
        } else {
          need(":");
          p.type = type();
        }
        f.params.push_back(p);
      } while (eat(","));
      need(")");
    }
    if (eat("->"))
      f.result = type();
    if (external || !f.intrinsic.empty())
      need(";");
    else
      f.body = block();
    return f;
  }

public:
  explicit Parser(const std::vector<Token> &t, bool trust) : trusted(trust), ts(t) {}
  bool fragment(const std::string &kind) {
    if (kind == "expr")
      expr();
    else if (kind == "type")
      type();
    else
      return false;
    return peek().kind == Token::End;
  }
  ast::Program run() {
    ast::Program p;
    while (peek().kind != Token::End) {
      if (peek().text == "async" || peek().text == "await")
        ident();
      if (peek().text == "@" && ts[pos + 1].text == "import") {
        ++pos;
        ++pos;
        p.imports.push_back(stringLiteral());
        eat(";");
        continue;
      }
      auto attr = attribute();
      bool pub = eat("pub");
      if (peek().text == "async" || peek().text == "await")
        ident();
      auto start = peek().loc;
      if (eat("extern")) {
        if (!trusted)
          fail(start, DiagnosticCode::RestrictedOperation,
               "extern C is restricted to bundled runtime bindings");
        if (stringLiteral() != "C")
          fail(start, DiagnosticCode::RestrictedOperation, "only the C ABI is supported");
        need("fn");
        p.functions.push_back(function("", pub, true));
      } else if (eat("fn"))
        p.functions.push_back(function("", pub, false, attr));
      else if (eat("impl")) {
        std::string owner = ident().text;
        ast::Impl impl{owner, start, {}};
        need("{");
        while (!eat("}")) {
          auto ma = attribute();
          bool mp = eat("pub");
          need("fn");
          impl.methods.push_back(p.functions.size());
          p.functions.push_back(function(owner, mp, false, ma));
        }
        p.impls.push_back(std::move(impl));
      } else if (eat("enum")) {
        ast::Enum e;
        e.name = ident().text;
        e.loc = start;
        need("{");
        while (!eat("}")) {
          ast::EnumVariant v;
          auto n = ident();
          v.name = n.text;
          v.loc = n.loc;
          if (eat("(")) {
            if (!eat(")")) {
              do {
                v.payload.push_back(type());
              } while (eat(","));
              need(")");
            }
          }
          e.variants.push_back(v);
          if (!eat(",")) {
            need("}");
            break;
          }
        }
        p.enums.push_back(std::move(e));
      } else if (eat("struct")) {
        ast::Struct s;
        s.trusted = trusted;
        s.representation = attr;
        s.pub = pub;
        s.loc = start;
        {
          auto name = ident();
          s.name = name.text;
          s.loc.context = name.loc.context;
        }
        if (eat(":"))
          s.protocol = ident().text;
        need("{");
        while (!eat("}")) {
          ast::Field f;
          f.pub = eat("pub");
          auto n = ident();

          f.name = n.text;
          f.loc = n.loc;
          need(":");
          f.type = type();
          s.fields.push_back(f);
          if (!eat(",")) {
            need("}");
            break;
          }
        }
        p.structs.push_back(s);
      } else if (eat("protocol")) {
        ast::Protocol q;
        q.loc = start;
        q.pub = pub;
        q.name = ident().text;
        need("{");
        while (!eat("}")) {
          if (eat("initial")) {
            if (!q.initial.empty())
              fail(peek().loc, DiagnosticCode::InvalidDeclaration, "duplicate initial state");
            q.initial = ident().text;
            need(";");
          } else if (eat("final")) {
            q.finals.push_back(ident().text);
            need(";");
          } else {
            need("state");
            auto n = ident();
            ast::State s{n.text, n.loc, {}};
            need("{");
            while (!eat("}")) {
              auto op = ident();
              need("(");
              Access a = Access::Own;
              if (eat("shared"))
                a = Access::Shared;
              else if (eat("unique"))
                a = Access::Unique;
              need("self");
              need(")");
              std::string target = s.name;
              if (eat("->"))
                target = ident().text;
              need(";");
              s.operations.push_back({op.text, target, a, op.loc});
            }
            q.states.push_back(s);
          }
        }
        p.protocols.push_back(q);
      } else
        fail(peek().loc, DiagnosticCode::UnexpectedToken, "expected fn, struct, impl or protocol");
    }
    return p;
  }
};
ast::Program parse(const std::vector<Token> &ts, bool trusted) {
  return Parser(ts, trusted).run();
}
bool parseFragment(const std::vector<Token> &ts, const std::string &kind) {
  try {
    return Parser(ts, false).fragment(kind);
  } catch (const Error &) {
    return false;
  }
}
static void printStmts(const std::vector<ast::Stmt> &, std::ostream &, int);
static void printExpr(const ast::Expr &e, std::ostream &o) {
  static const char *names[] = {"LiteralInt", "LiteralBool",   "Identifier", "Binary",
                                "Call",       "MethodCall",    "Member",     "Index",
                                "Array",      "Assignment",    "Unary",      "Cast",
                                "Char",       "StringLiteral", "BlockExpr",  "AtCall"};
  o << "(" << names[e.kind] << " " << (e.kind == ast::Expr::String ? quoteText(e.text) : e.text);
  for (auto &a : e.args) {
    o << " ";
    printExpr(a, o);
  }
  if (!e.body.empty())
    printStmts(e.body, o, 2);
  o << ")";
}
static void printStmts(const std::vector<ast::Stmt> &ss, std::ostream &o, int depth) {
  static const char *names[] = {"LetStmt",    "ReturnStmt",   "ExprStmt", "IfStmt",
                                "BorrowStmt", "WhileStmt",    "LoopStmt", "ForStmt",
                                "BreakStmt",  "ContinueStmt", "MatchStmt"};
  for (auto &s : ss) {
    o << std::string(depth, ' ') << names[s.kind] << " " << s.name << " ";
    printExpr(s.expr, o);
    o << "\n";
    printStmts(s.body, o, depth + 2);
    if (!s.otherwise.empty()) {
      o << std::string(depth, ' ') << "else\n";
      printStmts(s.otherwise, o, depth + 2);
    }
  }
}
void printAST(const ast::Program &p, std::ostream &o) {
  for (auto &e : p.enums) {
    o << "EnumDecl " << e.name << "\n";
    for (auto &v : e.variants)
      o << "  Variant " << v.name << " payload=" << v.payload.size() << "\n";
  }
  o << "AST Program\n";
  for (const auto &import : p.imports)
    o << "import " << import << "\n";
  for (auto &q : p.protocols) {
    o << "protocol " << q.name << " initial " << q.initial << "\n";
    for (auto &f : q.finals)
      o << "  final " << f << "\n";
    for (auto &s : q.states) {
      o << "  state " << s.name << "\n";
      for (auto &op : s.operations)
        o << "    " << op.name << "(" << accessName(op.access) << " self) -> " << op.target << "\n";
    }
  }
  for (auto &s : p.structs) {
    o << "struct " << s.name << ": " << s.protocol << "\n";
    for (auto &f : s.fields)
      o << "  " << f.name << ": " << f.type.name << "[" << f.type.count << "]\n";
  }
  for (auto &impl : p.impls) {
    o << "impl " << impl.owner << " {";
    for (auto id : impl.methods)
      o << " fn#" << id;
    o << " }\n";
  }
  for (auto &f : p.functions) {
    if (f.external)
      o << "extern C ";
    if (!f.intrinsic.empty())
      o << "intrinsic " << f.intrinsic << " ";
    o << "fn " << f.owner << "::" << f.name << " -> " << f.result.name << "\n";
    printStmts(f.body, o, 2);
  }
}
}
