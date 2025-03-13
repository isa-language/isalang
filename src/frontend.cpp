#include "isac/frontend.hpp"
#include <algorithm>
#include <cctype>
namespace isac {
const char *accessName(Access a) {
  return a == Access::Own ? "Own" : a == Access::Shared ? "Shared" : "Unique";
}
std::vector<Token> lex(const std::string &source) {
  std::vector<Token> out;
  size_t i = 0;
  Loc l;
  auto advance = [&]() {
    if (source[i++] == '\n') {
      ++l.line;
      l.column = 1;
    } else
      ++l.column;
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
        fail(start, "E0001", "unterminated comment");
      advance();
      advance();
      continue;
    }
    Loc start = l;
    size_t begin = i;
    Token::Kind kind = Token::Punctuation;
    if (c == '"') {
      advance();
      size_t content = i;
      while (i < source.size() && source[i] != '"') {
        if (source[i] == '\n' || source[i] == '\\')
          fail(start, "E0001", "only plain import/ABI string literals are supported");
        advance();
      }
      if (i == source.size())
        fail(start, "E0001", "unterminated string");
      out.push_back({source.substr(content, i - content), start, Token::String});
      advance();
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
      if (source.compare(i, 5, "usize") == 0)
        for (int n = 0; n < 5; ++n)
          advance();
    } else {
      std::string two = source.substr(i, 2);
      if (two == "->" || two == "::" || two == "==" || two == "!=" || two == "<=" || two == ">=" ||
          two == "&&" || two == "||") {
        advance();
        advance();
      } else if (std::string("@{}()[];:,.=+-*/<>!").find(c) != std::string::npos)
        advance();
      else
        fail(l, "E0001", "unexpected character");
    }
    out.push_back({source.substr(begin, i - begin), start, kind});
  }
  out.push_back({"<eof>", l, Token::End});
  return out;
}
class Parser {
  bool trusted;
  std::string annotation;
  std::string stringLiteral() {
    auto t = peek();
    if (t.kind != Token::String)
      fail(t.loc, "E0002", "expected string literal");
    ++pos;
    return t.text;
  }
  std::string attribute() {
    if (!eat("@"))
      return "";
    if (!trusted)
      fail(peek().loc, "E0500", "internal annotations are restricted to bundled libraries");
    auto name = ident().text;
    if (name == "releases")
      return name;
    if (name != "intrinsic" && name != "representation")
      fail(peek().loc, "E0500", "unknown internal annotation");
    return stringLiteral();
  }
  const std::vector<Token> &ts;
  size_t pos = 0;
  const Token &peek() {
    return ts[pos];
  }
  bool eat(const std::string &s) {
    if (peek().text != s)
      return false;
    ++pos;
    return true;
  }
  Token need(const std::string &s) {
    Token t = peek();
    if (!eat(s))
      fail(t.loc, "E0002", "expected `" + s + "`, found `" + t.text + "`");
    return t;
  }
  Token ident() {
    Token t = peek();
    if (t.kind != Token::Identifier)
      fail(t.loc, "E0002", "expected identifier");
    ++pos;
    return t;
  }
  TypeSyntax type() {
    auto n = ident();
    TypeSyntax t{n.text, 0, n.loc};
    if (eat("[")) {
      auto v = peek();
      if (v.kind != Token::Number)
        fail(v.loc, "E0002", "static array size required");
      ++pos;
      try {
        t.count = std::stoi(v.text);
      } catch (...) {
        fail(v.loc, "E0002", "array size out of range");
      }
      if (t.count <= 0 || t.count > 65536)
        fail(v.loc, "E0002", "array size must be in 1..65536");
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
    if (s == "==" || s == "!=")
      return 4;
    if (s == "<" || s == ">" || s == "<=" || s == ">=")
      return 5;
    if (s == "+" || s == "-")
      return 6;
    if (s == "*" || s == "/")
      return 7;
    return 0;
  }
  ast::Expr primary() {
    Token t = peek();
    ast::Expr e{ast::Expr::Name, t.loc, t.text, {}};
    if (eat("(")) {
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
    } else if (t.kind == Token::Number) {
      ++pos;
      e.kind = ast::Expr::Int;
    } else if (eat("true") || eat("false")) {
      e.kind = ast::Expr::Bool;
    } else {
      ident();
      if (eat("::"))
        e.text += "::" + ident().text;
    }
    for (;;) {
      if (eat("(")) {
        ast::Expr c{ast::Expr::Call, e.loc, e.text, {}};
        if (e.kind != ast::Expr::Name)
          fail(e.loc, "E0002", "call target must be a name");
        if (!eat(")")) {
          do {
            c.args.push_back(expr());
          } while (eat(","));
          need(")");
        }
        e = std::move(c);
      } else if (eat(".")) {
        auto n = ident();
        ast::Expr c{ast::Expr::Member, n.loc, n.text, {std::move(e)}};
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
    return e;
  }
  ast::Expr expr(int minimum = 1) {
    auto e = primary();
    while (prec(peek().text) >= minimum) {
      auto op = ts[pos++];
      int p = prec(op.text);
      auto rhs = expr(p + (op.text == "=" ? 0 : 1));
      e = {op.text == "=" ? ast::Expr::Assign : ast::Expr::Binary,
           op.loc,
           op.text,
           {std::move(e), std::move(rhs)}};
    }
    return e;
  }
  std::vector<ast::Stmt> block() {
    need("{");
    std::vector<ast::Stmt> b;
    while (!eat("}")) {
      if (peek().kind == Token::End)
        fail(peek().loc, "E0002", "unterminated block");
      b.push_back(stmt());
    }
    return b;
  }
  ast::Stmt stmt() {
    ast::Stmt s{};
    s.loc = peek().loc;
    if (eat("let")) {
      s.kind = ast::Stmt::Let;
      s.name = ident().text;
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
      if (eat("else"))
        s.otherwise = block();
    } else if (eat("borrow")) {
      s.kind = ast::Stmt::Borrow;
      if (eat("shared"))
        s.access = Access::Shared;
      else {
        need("unique");
        s.access = Access::Unique;
      }
      s.expr = expr();
      need("as");
      s.name = ident().text;
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
            fail(p.loc, "E0002", "self requires impl");
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
  ast::Program run() {
    ast::Program p;
    while (peek().kind != Token::End) {
      if (peek().text == "@" && ts[pos + 1].text == "import") {
        ++pos;
        ++pos;
        p.imports.push_back(stringLiteral());
        eat(";");
        continue;
      }
      auto attr = attribute();
      bool pub = eat("pub");
      auto start = peek().loc;
      if (eat("extern")) {
        if (!trusted)
          fail(start, "E0500", "extern C is restricted to bundled runtime bindings");
        if (stringLiteral() != "C")
          fail(start, "E0500", "only the C ABI is supported");
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
      } else if (eat("struct")) {
        ast::Struct s;
        s.trusted = trusted;
        s.representation = attr;
        s.pub = pub;
        s.loc = start;
        s.name = ident().text;
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
              fail(peek().loc, "E0100", "duplicate initial state");
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
        fail(peek().loc, "E0002", "expected fn, struct, impl or protocol");
    }
    return p;
  }
};
ast::Program parse(const std::vector<Token> &ts, bool trusted) {
  return Parser(ts, trusted).run();
}
static void printExpr(const ast::Expr &e, std::ostream &o) {
  static const char *names[] = {"LiteralInt", "LiteralBool", "Identifier", "Binary",
                                "Call",       "MethodCall",  "Member",     "Index",
                                "Array",      "Assignment",  "Unary"};
  o << "(" << names[e.kind] << " " << e.text;
  for (auto &a : e.args) {
    o << " ";
    printExpr(a, o);
  }
  o << ")";
}
static void printStmts(const std::vector<ast::Stmt> &ss, std::ostream &o, int depth) {
  static const char *names[] = {"LetStmt", "ReturnStmt", "ExprStmt", "IfStmt", "BorrowStmt"};
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
