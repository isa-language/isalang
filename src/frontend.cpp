#include "isac/frontend.hpp"
#include <algorithm>
#include <cctype>
#include <functional>
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
      } while (i < source.size() && (std::isdigit((unsigned char)source[i]) || source[i] == '_'));
      if (i + 1 < source.size() && source[i] == '.' && std::isdigit((unsigned char)source[i + 1])) {
        advance();
        while (i < source.size() && (std::isdigit((unsigned char)source[i]) || source[i] == '_'))
          advance();
      }
      while (i < source.size() && (std::isalnum((unsigned char)source[i]) || source[i] == '_'))
        advance();
    } else {
      std::string two = source.substr(i, 2);
      if (source.substr(i, 3) == "...") {
        advance();
        advance();
        advance();
      } else if (two == "->" || two == "::" || two == "==" || two == "!=" || two == "<=" ||
                 two == ">=" || two == "&&" || two == "||" || two == "<<" || two == ">>" ||
                 two == ".." || two == "=>") {
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
  std::vector<Token> ts;
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
  void typeClose() {

    if (peek().text == ">>") {
      ts[pos].text = ">";
      return;
    }
    need(">");
  }
  TypeSyntax type() {
    if (eat("extern")) {
      auto origin = ts[pos - 1].loc;
      if (stringLiteral() != "C")
        fail(origin, DiagnosticCode::InvalidCAbi, "function pointers require the C ABI");
      need("fn");
      TypeSyntax result{"", 0, origin};
      result.functionPointer = true;
      need("(");
      if (!eat(")")) {
        do {
          result.arguments.push_back(type());
        } while (eat(","));
        need(")");
      }
      result.arguments.push_back(eat("->") ? type() : TypeSyntax{});
      return result;
    }
    if (eat("*")) {
      auto origin = ts[pos - 1].loc;
      bool mutablePointer = eat("mut");
      if (!mutablePointer)
        need("const");
      auto target = type();
      if (target.reference || target.count)
        fail(origin, DiagnosticCode::InvalidCAbi, "raw pointers require a sized referent");
      TypeSyntax result{"", 0, origin};
      result.rawPointer = true;
      result.mutablePointer = mutablePointer;
      result.arguments.push_back(std::move(target));
      return result;
    }
    bool reference = eat("&");
    bool unique = reference && eat("mut");
    auto n = ident();
    TypeSyntax t{n.text, 0, n.loc};
    while (eat("::"))
      t.name += "::" + ident().text;
    t.reference = reference;
    t.mutableReference = unique;
    if (eat("<")) {
      do {
        t.arguments.push_back(type());
      } while (eat(","));
      typeClose();
    }
    if (eat("[")) {
      if (eat("]")) {
        t.count = -1;
        return t;
      }
      auto v = peek();
      if (v.kind == Token::Identifier) {
        t.countName = ident().text;
        need("]");
        return t;
      }
      if (v.kind != Token::Number)
        fail(v.loc, DiagnosticCode::UnexpectedToken, "static array size or constant required");
      ++pos;
      try {
        auto number = parseNumericLiteral(v.text, v.loc);
        if (number.floating || (!number.suffix.empty() && number.suffix != "usize"))
          fail(v.loc, DiagnosticCode::InvalidLiteral,
               "array length requires an integer or usize literal");
        t.count = std::stoi(number.value);
      } catch (const std::out_of_range &) {
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
  ast::Pattern pattern() {
    ast::Pattern p{};
    p.loc = peek().loc;
    if (eat("_"))
      p.kind = ast::Pattern::Wildcard;
    else if (peek().text == "true" || peek().text == "false") {
      p.kind = ast::Pattern::Boolean;
      p.name = ts[pos++].text;
    } else if (peek().kind == Token::Character) {
      p.kind = ast::Pattern::Character;
      p.name = ts[pos++].text;
    } else if (peek().kind == Token::Number || peek().text == "-") {
      p.kind = ast::Pattern::Integer;
      if (eat("-"))
        p.name = "-";
      p.name += ts[pos++].text;
    } else {
      p.name = ident().text;
      p.kind = ast::Pattern::Binding;
      while (eat("::")) {
        p.kind = ast::Pattern::Variant;
        p.name += "::" + ident().text;
      }
      if (eat("(")) {
        p.kind = ast::Pattern::Variant;
        if (!eat(")")) {
          do {
            p.bindings.push_back(ident().text);
          } while (eat(","));
          need(")");
        }
      }
    }
    return p;
  }
  ast::Expr primary() {
    Token t = peek();
    ast::Expr e{ast::Expr::Name, t.loc, t.text, {}};
    if (eat("unsafe")) {
      e.kind = ast::Expr::Unsafe;
      if (peek().text != "{")
        fail(t.loc, DiagnosticCode::UnexpectedToken, "unsafe requires a block");
      e.args.push_back(primary());
    } else if (eat("*")) {
      e.kind = ast::Expr::Dereference;
      e.args.push_back(primary());
    } else if (eat("@")) {
      e.kind = ast::Expr::AtCall;
      e.text = ident().text;
      if (eat("<")) {
        auto t = type();
        if (t.reference)
          fail(t.loc, DiagnosticCode::EscapingView,
               "reference type arguments require complete region contracts");
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
    } else if (eat("match")) {
      e.kind = ast::Expr::Match;
      e.text.clear();
      e.args.push_back(expr());
      need("{");
      while (!eat("}")) {
        ast::Stmt arm{};
        arm.kind = ast::Stmt::Expression;
        arm.loc = peek().loc;
        arm.pattern = pattern();
        need("=>");
        arm.expr = expr();
        e.body.push_back(std::move(arm));
        if (!eat(",") && peek().text != "}")
          fail(peek().loc, DiagnosticCode::UnexpectedToken, "expected comma after match arm");
      }
    } else if (eat("{")) {
      e.kind = ast::Expr::Block;
      e.text.clear();
      while (!eat("}")) {
        if (eat(";"))
          continue;
        if (peek().text == "const" || peek().text == "let" || peek().text == "if" ||
            peek().text == "while" || peek().text == "loop" || peek().text == "for" ||
            peek().text == "borrow" || peek().text == "return" || peek().text == "break" ||
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
        e.args.push_back(expr());
        if (eat(";")) {
          e.kind = ast::Expr::ArrayRepeat;
          e.args.push_back(expr());
        } else {
          while (eat(",")) {
            if (peek().text == "]")
              break;
            e.args.push_back(expr());
          }
        }
        need("]");
      }
    } else if (eat("&")) {
      e.kind = ast::Expr::Borrow;
      e.text = eat("mut") ? "unique" : "shared";
      e.args.push_back(primary());
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
      while (eat("::"))
        e.text += "::" + ident().text;

      if (peek().text == "<") {
        auto saved = pos;
        try {
          ++pos;
          std::vector<TypeSyntax> arguments;
          do {
            arguments.push_back(type());
          } while (eat(","));
          need(">");
          if (peek().text == "::")
            e.typeArguments = std::move(arguments);
          else
            pos = saved;
        } catch (const Error &) {
          pos = saved;
        }
      }
      while (eat("::"))
        e.text += "::" + ident().text;
    }
    for (;;) {
      if (peek().text == "?")
        fail(peek().loc, DiagnosticCode::FutureFeature,
             "error propagation with ? is not implemented; inspect is_ok() and error() explicitly");
      if (eat("(")) {
        ast::Expr c{ast::Expr::Call, e.loc, e.text, {}};
        c.typeArguments = e.typeArguments;
        if (e.kind != ast::Expr::Name) {
          c.kind = ast::Expr::Invoke;
          c.text.clear();
          c.args.push_back(std::move(e));
        }
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
      if (t.count || t.reference)
        fail(t.loc, DiagnosticCode::InvalidCast, "cast target must be scalar");
      e = {ast::Expr::Cast, t.loc, t.name, {std::move(e)}};
      e.typeArguments = {t};
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
    } else if (peek().text == "let" || peek().text == "const") {
      s.kind = eat("const") ? ast::Stmt::Const : ast::Stmt::Let;
      if (s.kind == ast::Stmt::Let)
        need("let");
      {
        auto name = ident();
        s.name = name.text;
        s.loc.context = name.loc.context;
      }
      if (eat(":")) {
        s.annotated = true;
        s.annotation = type();
      }
      if (s.kind == ast::Stmt::Const && !s.annotated)
        fail(s.loc, DiagnosticCode::UnexpectedToken, "const requires a type annotation");
      if (eat("=")) {
        s.expr = expr();
      } else if (s.kind == ast::Stmt::Let && s.annotated &&
                 (s.annotation.count > 0 || !s.annotation.countName.empty())) {

        s.expr = {ast::Expr::ArrayRepeat, s.loc, "zero", {}};
      } else {
        need("=");
      }
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
      if (eat("let")) {
        s.conditionalPattern = true;
        s.pattern = pattern();
        need("=");
      }
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
        arm.pattern = pattern();
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
      if (s.expr.kind == ast::Expr::Unsafe)
        eat(";");
      else
        need(";");
    }
    return s;
  }
  ast::Function function(std::string owner, bool pub, bool external = false, std::string attr = "",
                         bool foreignC = false, bool unsafeFunction = false) {
    auto n = ident();
    ast::Function f;
    f.trusted = trusted;
    f.external = external;
    f.foreignC = foreignC;
    f.unsafeFunction = unsafeFunction;
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
        if (eat("...")) {
          if (!foreignC || f.params.empty())
            fail(p.loc, DiagnosticCode::InvalidCAbi,
                 "variadic functions require extern C and at least one fixed parameter");
          f.variadic = true;
          break;
        }
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
    else if (foreignC && eat(";"))
      f.external = true;
    else
      f.body = block();
    if (f.variadic && !f.external)
      fail(f.loc, DiagnosticCode::InvalidCAbi, "variadic C exports are not supported");
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
        auto origin = peek().loc;
        pos += 2;
        bool parenthesized = eat("(");
        do {
          p.importOrigins.push_back(origin);
          p.imports.push_back(stringLiteral());
          p.importAliases.push_back(eat("as") ? ident().text : "");
        } while (parenthesized && eat(",") && peek().text != ")");
        if (parenthesized)
          need(")");
        eat(";");
        continue;
      }
      auto attr = attribute();
      bool pub = eat("pub");
      if (peek().text == "async" || peek().text == "await")
        ident();
      auto start = peek().loc;
      bool foreign = eat("extern");
      if (foreign && stringLiteral() != "C")
        fail(start, DiagnosticCode::InvalidCAbi, "only the C ABI is supported");
      bool unsafeFunction = eat("unsafe");
      if ((unsafeFunction && peek().text != "fn") ||
          (foreign && peek().text != "fn" && peek().text != "struct" && peek().text != "union" &&
           peek().text != "type"))
        fail(start, DiagnosticCode::InvalidCAbi,
             "extern C supports fn/struct/union/type; unsafe declarations require fn");
      if (foreign && eat("type")) {
        ast::ForeignType declaration;
        auto name = ident();
        declaration.name = name.text;
        declaration.loc = name.loc;
        declaration.alias = eat("=");
        if (declaration.alias)
          declaration.target = type();
        need(";");
        p.foreignTypes.push_back(std::move(declaration));
      } else if (peek().text == "const" && !foreign && !unsafeFunction) {
        p.constants.push_back(stmt());
      } else if (eat("fn"))
        p.functions.push_back(
            function("", pub, foreign && trusted, attr, foreign && !trusted, unsafeFunction));
      else if (eat("impl")) {
        std::vector<std::string> parameters;
        if (eat("<")) {
          do {
            parameters.push_back(ident().text);
          } while (eat(","));
          need(">");
        }
        auto target = type();
        std::string owner = target.name;
        ast::Impl impl{owner, start, {}, std::move(parameters), target};
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
        if (eat("<")) {
          do {
            e.parameters.push_back(ident().text);
          } while (eat(","));
          need(">");
        }
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
      } else if (peek().text == "struct" || (foreign && peek().text == "union")) {
        ast::Struct s;
        s.foreignUnion = eat("union");
        if (!s.foreignUnion)
          need("struct");
        s.trusted = trusted;
        s.foreignC = foreign;
        s.representation = attr;
        s.pub = pub;
        s.loc = start;
        {
          auto name = ident();
          s.name = name.text;
          s.loc.context = name.loc.context;
        }
        if (eat("<")) {
          do {
            s.parameters.push_back(ident().text);
          } while (eat(","));
          need(">");
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
  static const char *names[] = {
      "LiteralInt", "LiteralBool",     "Identifier",      "Binary",     "Call",       "MethodCall",
      "Member",     "Index",           "Array",           "Assignment", "Unary",      "Cast",
      "Char",       "StringLiteral",   "BlockExpr",       "AtCall",     "BorrowExpr", "MatchExpr",
      "UnsafeExpr", "DereferenceExpr", "ArrayRepeatExpr", "InvokeExpr"};
  o << "(" << names[e.kind] << " " << (e.kind == ast::Expr::String ? quoteText(e.text) : e.text);
  if (!e.body.empty())
    printStmts(e.body, o, 2);
  for (auto &a : e.args) {
    o << " ";
    printExpr(a, o);
  }
  o << ")";
}
static void printStmts(const std::vector<ast::Stmt> &ss, std::ostream &o, int depth) {
  static const char *names[] = {"LetStmt",    "ReturnStmt",   "ExprStmt",  "IfStmt",
                                "BorrowStmt", "WhileStmt",    "LoopStmt",  "ForStmt",
                                "BreakStmt",  "ContinueStmt", "MatchStmt", "ConstDecl"};
  for (auto &s : ss) {
    o << std::string(depth, ' ') << (s.conditionalPattern ? "IfLetStmt" : names[s.kind]) << " "
      << s.name << " ";
    if (s.conditionalPattern) {
      o << s.pattern.name << "(";
      for (const auto &binding : s.pattern.bindings)
        o << binding << " ";
      o << ") = ";
    }
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
  std::function<std::string(const TypeSyntax &)> typeName = [&](const TypeSyntax &t) {
    if (t.functionPointer) {
      std::string name = "extern C fn(";
      for (size_t n = 0; n + 1 < t.arguments.size(); ++n)
        name += (n ? "," : "") + typeName(t.arguments[n]);
      return name + ")->" + typeName(t.arguments.back());
    }
    if (t.rawPointer)
      return std::string(t.mutablePointer ? "*mut " : "*const ") + typeName(t.arguments.at(0));
    std::string name = t.reference ? (t.mutableReference ? "&mut " : "&") : "";
    name += t.name;
    if (!t.arguments.empty()) {
      name += "<";
      for (size_t n = 0; n < t.arguments.size(); ++n) {
        if (n)
          name += ",";
        name += typeName(t.arguments[n]);
      }
      name += ">";
    }
    if (t.count)
      name += "[" + std::to_string(t.count) + "]";
    return name;
  };
  for (const auto &type : p.foreignTypes) {
    o << "extern C type " << type.name;
    if (type.alias)
      o << " = " << typeName(type.target);
    o << "\n";
  }
  printStmts(p.constants, o, 0);
  for (auto &e : p.enums) {
    o << "EnumDecl " << e.name;
    if (!e.parameters.empty()) {
      o << "<";
      for (size_t i = 0; i < e.parameters.size(); ++i) {
        if (i)
          o << ", ";
        o << e.parameters[i];
      }
      o << ">";
    }
    o << "\n";
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
    if (s.foreignC)
      o << "extern C ";
    o << (s.foreignUnion ? "union " : "struct ") << s.name;
    if (!s.parameters.empty()) {
      o << "<";
      for (size_t i = 0; i < s.parameters.size(); ++i) {
        if (i)
          o << ",";
        o << s.parameters[i];
      }
      o << ">";
    }
    o << ": " << s.protocol << "\n";
    for (auto &f : s.fields)
      o << "  " << f.name << ": "
        << (s.foreignC ? typeName(f.type) : f.type.name + "[" + std::to_string(f.type.count) + "]")
        << "\n";
  }
  for (auto &impl : p.impls) {
    o << "impl";
    if (!impl.parameters.empty()) {
      o << "<";
      for (size_t i = 0; i < impl.parameters.size(); ++i) {
        if (i)
          o << ",";
        o << impl.parameters[i];
      }
      o << ">";
    }
    o << " " << impl.owner << " {";
    for (auto id : impl.methods)
      o << " fn#" << id;
    o << " }\n";
  }
  for (auto &f : p.functions) {
    if (f.external || f.foreignC)
      o << "extern C ";
    if (f.unsafeFunction)
      o << "unsafe ";
    if (!f.intrinsic.empty())
      o << "intrinsic " << f.intrinsic << " ";
    o << "fn " << f.owner << "::" << f.name << " -> " << typeName(f.result) << "\n";
    if (f.variadic)
      o << "  variadic ...\n";
    if (f.foreignC)
      for (const auto &param : f.params)
        o << "  param " << param.name << ": " << typeName(param.type) << "\n";
    printStmts(f.body, o, 2);
  }
}
}
