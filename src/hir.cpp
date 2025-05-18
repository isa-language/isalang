#include "isac/hir.hpp"
#include "isac/macros.hpp"
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
namespace isac {
namespace {
class Resolver {
  const ast::Program &ast;
  hir::Program p;
  std::unordered_map<std::string, TypeId> types;
  std::unordered_map<std::string, ProtocolId> protocols;
  std::unordered_map<std::string, FunctionId> functions;
  std::vector<std::unordered_map<std::string, SymbolId>> scopes;
  hir::Function *current = nullptr;
  bool privileged = false;
  uint32_t nextLoop = 0;
  RegionId nextRegion = 0;
  std::vector<uint32_t> loops;
  void duplicate(Loc l, const std::string &n) {
    fail(l, DiagnosticCode::DuplicateDeclaration, "duplicate declaration `" + n + "`");
  }
  TypeId getType(const TypeSyntax &s, bool allowReference = false) {
    if (s.reference && !allowReference)
      fail(s.loc, DiagnosticCode::EscapingView,
           "reference types currently require a local borrow binding; aggregate and function "
           "region contracts are not implemented");
    auto it = types.find(s.name);
    if (it == types.end())
      fail(s.loc, DiagnosticCode::UnknownName, "unknown type `" + s.name + "`");
    TypeId t = it->second;
    if (t == RawPtr && !privileged)
      fail(s.loc, DiagnosticCode::RestrictedOperation,
           "RawPtr is restricted to bundled runtime bindings");
    if (!s.count)
      return t;
    if (!scalar(t))
      fail(s.loc, DiagnosticCode::InvalidReceiver, "PoC arrays require i32 or bool elements");
    std::string key = s.name + "[" + std::to_string(s.count) + "]";
    if (types.contains(key))
      return types[key];
    TypeId id = p.types.size();
    types[key] = id;
    p.types.push_back({hir::Type::Array, key, None, t, (uint32_t)s.count, (uint32_t)s.count, {}});
    return id;
  }
  SymbolId add(std::string n, TypeId t, Loc l, Access a = Access::Own, bool param = false,
               bool self = false) {
    auto key = n + "#" + std::to_string(l.context);
    if (scopes.back().contains(key))
      duplicate(l, n);
    SymbolId id = p.symbols.size();
    p.symbols.push_back({id, n, t, a, param, self, l});
    scopes.back()[key] = id;
    return id;
  }
  SymbolId lookup(std::string n, Loc l) {
    auto key = n + "#" + std::to_string(l.context);
    for (auto i = scopes.rbegin(); i != scopes.rend(); ++i)
      if (i->contains(key))
        return i->at(key);
    fail(l, DiagnosticCode::UnknownName, "unknown name `" + n + "`");
  }
  void same(TypeId a, TypeId b, Loc l) {
    if (a != b)
      fail(l, DiagnosticCode::TypeMismatch,
           "expected `" + p.types[a].name + "`, found `" + p.types[b].name + "`");
  }
  void coerce(TypeId wanted, hir::Expr &e) {
    if (e.inferredLiteral && numeric(wanted) && numeric(e.type) &&
        floating(wanted) == floating(e.type)) {
      validateLiteral(wanted, e.text, e.loc, p.target.pointerWidth);
      e.type = wanted;
    }
    same(wanted, e.type, e.loc);
  }
  void args(hir::Expr &e, hir::Function &f, size_t skip) {
    if (e.args.size() != f.params.size())
      fail(e.loc, DiagnosticCode::ArgumentMismatch,
           "wrong number of arguments to `" + f.name + "`");
    for (size_t i = skip; i < e.args.size(); ++i)
      coerce(p.symbols[f.params[i]].type, e.args[i]);
  }
  bool place(const hir::Expr &e) {
    return e.kind == hir::Expr::Variable || e.kind == hir::Expr::Field ||
           e.kind == hir::Expr::Index;
  }
  hir::Expr expr(const ast::Expr &a) {
    hir::Expr e{};
    e.loc = a.loc;
    e.text = a.text;
    using A = ast::Expr;
    using H = hir::Expr;
    if (a.kind == A::AtCall) {
      auto builtin = macros::findBuiltin(a.text);
      if (builtin != macros::BuiltinId::SizeOf && builtin != macros::BuiltinId::AlignOf)
        fail(a.loc, DiagnosticCode::MacroMatch,
             "unresolved @ construct `" + a.text + "` after macro expansion");
      if (a.args.size() != 1 || a.args[0].kind != A::Name)
        fail(a.loc, DiagnosticCode::ArgumentMismatch,
             "layout builtin requires one type argument and no value arguments");
      TypeSyntax syntax{a.args[0].text, 0, a.args[0].loc};
      auto bracket = syntax.name.find('[');
      if (bracket != std::string::npos) {
        syntax.count = std::stoi(syntax.name.substr(bracket + 1));
        syntax.name.resize(bracket);
      }
      auto id = getType(syntax);
      auto &t = p.types[id];
      unsigned word = p.target.pointerWidth / 8;
      uint64_t size = 0;
      unsigned alignment = 1;
      if (id == Unit)
        size = 0;
      else if (id == Str || p.heap(id)) {
        size = word * (id == Str                                               ? 2
                       : t.representation == hir::Representation::StringRecord ? 3
                       : t.representation == hir::Representation::InputRecord  ? 6
                                                                               : 1);
        alignment = p.target.pointerAlignment;
      } else if (p.linear(id)) {
        size = 8 * t.width;
        alignment = p.target.integerAlignment[3];
      } else {
        size = bitWidth(id, p.target.pointerWidth) / 8;
        alignment = floating(id) ? p.target.floatAlignment[id == F64]
                                 : p.target.integerAlignment[size == 1   ? 0
                                                             : size == 2 ? 1
                                                             : size == 4 ? 2
                                                                         : 3];
      }
      e.kind = H::Constant;
      e.type = USize;
      e.text = std::to_string(builtin == macros::BuiltinId::SizeOf ? size : alignment);
      return e;
    }
    if (a.kind == A::Borrow) {
      e.kind = H::Borrow;
      e.args.push_back(expr(a.args.at(0)));
      if (e.args[0].kind != H::Variable)
        fail(a.loc, DiagnosticCode::InvalidPlace,
             "this reference increment requires a named owner or borrow");
      e.type = e.args[0].type;
      e.borrowKind = a.text == "unique" ? BorrowKind::Unique : BorrowKind::Shared;
      e.region = nextRegion++;
      return e;
    }
    if (a.kind == A::Block) {
      e.kind = H::Block;
      e.body = block(a.body, true);
      e.args.push_back(expr(a.args.at(0)));
      e.type = e.args[0].type;
      scopes.pop_back();
      return e;
    }
    if (a.kind == A::String) {
      e.kind = H::StaticStr;
      e.type = Str;
      return e;
    }
    if (a.kind == A::Char) {
      e.kind = H::Constant;
      e.type = Char;
      return e;
    }
    if (a.kind == A::Int) {
      e.kind = H::Constant;
      size_t suffix = e.text.find_first_not_of("0123456789.-");
      std::string name = suffix == std::string::npos ? "" : e.text.substr(suffix);
      if (!name.empty()) {
        if (!types.contains(name) || !numeric(types[name]))
          fail(a.loc, DiagnosticCode::InvalidLiteral, "unknown numeric suffix");
        e.type = types[name];
        e.text.resize(suffix);
      } else {
        e.type = e.text.find('.') == std::string::npos ? I32 : F64;
        e.inferredLiteral = true;
      }
      validateLiteral(e.type, e.text, e.loc, p.target.pointerWidth);
      return e;
    }
    if (a.kind == A::Bool) {
      e.kind = H::Constant;
      e.type = Bool;
      e.text = a.text == "true" ? "1" : "0";
      return e;
    }
    if ((a.kind == A::Name || a.kind == A::Call) && a.text.find("::") != std::string::npos) {
      auto sep = a.text.find("::");
      auto tn = a.text.substr(0, sep);
      auto vn = a.text.substr(sep + 2);
      if (types.contains(tn) && p.types[types[tn]].kind == hir::Type::Enum) {
        e.type = types[tn];
        e.kind = H::EnumConstruct;
        auto &vs = p.types[e.type].variants;
        auto it = std::find_if(vs.begin(), vs.end(), [&](auto &v) { return v.name == vn; });
        if (it == vs.end())
          fail(a.loc, DiagnosticCode::InvalidVariant, "unknown enum variant `" + a.text + "`");
        e.ref = it->id;
        if (a.args.size() != it->payload.size())
          fail(a.loc, DiagnosticCode::InvalidPayload, "wrong enum payload arity");
        for (size_t i = 0; i < a.args.size(); ++i) {
          auto arg = expr(a.args[i]);
          coerce(it->payload[i], arg);
          e.args.push_back(arg);
        }
        return e;
      }
    }
    if (a.kind == A::Name) {
      if (a.text == "unit") {
        e.kind = H::UnitValue;
        return e;
      }
      e.kind = H::Variable;
      e.ref = lookup(a.text, a.loc);
      e.type = p.symbols[e.ref].type;
      return e;
    }
    if (a.kind == A::Unary && a.text == "-" && a.args[0].kind == A::Int) {
      auto n = a.args[0];
      n.text = "-" + n.text;
      return expr(n);
    }
    for (auto &x : a.args)
      e.args.push_back(expr(x));
    switch (a.kind) {
    case A::Cast: {
      if (e.args[0].kind == H::Borrow ||
          (e.args[0].kind == H::Variable && p.symbols[e.args[0].ref].reference))
        fail(a.loc, DiagnosticCode::InvalidCast,
             "cannot cast a reference; reference addresses are not exposed to safe code");
      e.kind = H::Cast;
      e.type = getType({a.text, 0, a.loc});
      auto from = e.args[0].type;
      if (e.type == from)
        break;
      if (from == Char && integer(e.type))
        break;
      if (!numeric(from) || !numeric(e.type) || (floating(from) && integer(e.type)))
        fail(a.loc, DiagnosticCode::InvalidCast,
             "unsupported cast; float-to-integer and raw/char casts are not supported");
      break;
    }
    case A::Unary:
      e.kind = H::Unary;
      e.type = e.args[0].type;
      if (a.text == "!")
        same(Bool, e.type, a.loc);
      else if (!numeric(e.type) || (!signedType(e.type) && !floating(e.type)))
        fail(a.loc, DiagnosticCode::TypeMismatch, "negation requires signed integer or float");
      break;
    case A::Binary: {
      e.kind = H::Binary;
      if (e.args[0].inferredLiteral && !e.args[1].inferredLiteral)
        coerce(e.args[1].type, e.args[0]);
      else if (e.args[1].inferredLiteral)
        coerce(e.args[0].type, e.args[1]);
      auto t = e.args[0].type;
      same(t, e.args[1].type, a.loc);
      bool logical = a.text == "&&" || a.text == "||", bitwise = a.text == "&" || a.text == "|" ||
                                                                 a.text == "^" || a.text == "<<" ||
                                                                 a.text == ">>";
      bool compare = a.text == "==" || a.text == "!=" || a.text == "<" || a.text == ">" ||
                     a.text == "<=" || a.text == ">=";
      if (logical)
        same(Bool, t, a.loc);
      else if (bitwise) {
        if (!integer(t))
          fail(a.loc, DiagnosticCode::TypeMismatch, "bitwise operators require integers");
      } else if (compare) {
        if (!scalar(t))
          fail(a.loc, DiagnosticCode::TypeMismatch, "comparison requires scalar operands");
      } else if (!numeric(t))
        fail(a.loc, DiagnosticCode::TypeMismatch, "arithmetic requires numeric operands");
      e.operatorClass = logical   ? H::OperatorClass::Logical
                        : bitwise ? H::OperatorClass::Bitwise
                        : compare ? H::OperatorClass::Comparison
                                  : H::OperatorClass::Arithmetic;
      e.type = (logical || compare) ? Bool : t;
      break;
    }
    case A::Array: {
      if (e.args.empty())
        fail(a.loc, DiagnosticCode::TypeMismatch, "empty arrays are not supported");
      TypeId t = e.args[0].type;
      for (auto &x : e.args)
        same(t, x.type, x.loc);
      TypeSyntax s{p.types[t].name, (int)e.args.size(), a.loc};
      e.kind = H::Array;
      e.type = getType(s);
      break;
    }
    case A::Member: {
      auto &t = p.types[e.args[0].type];
      if (t.representation != hir::Representation::Stack && !privileged)
        fail(a.loc, DiagnosticCode::RestrictedOperation,
             "heap representation is private; use its methods");
      if (t.kind != hir::Type::Structure)
        fail(a.loc, DiagnosticCode::TypeMismatch, "field access requires a struct");
      auto it =
          std::find_if(t.fields.begin(), t.fields.end(), [&](auto &f) { return f.name == a.text; });
      if (it == t.fields.end())
        fail(a.loc, DiagnosticCode::UnknownName, "unknown field `" + a.text + "`");
      e.kind = H::Field;
      e.ref = it->offset;
      e.type = it->type;
      if (!place(e.args[0]))
        fail(a.loc, DiagnosticCode::TypeMismatch, "field base must be a local place");
      break;
    }
    case A::Index: {
      auto &t = p.types[e.args[0].type];
      if (t.kind != hir::Type::Array)
        fail(a.loc, DiagnosticCode::TypeMismatch, "indexing requires an array");
      same(I32, e.args[1].type, a.loc);
      if (e.args[1].kind != H::Constant)
        fail(a.loc, DiagnosticCode::InvalidIndex, "PoC array indices must be constant");
      auto n = std::stoll(e.args[1].text);
      if (n < 0 || n >= t.count)
        fail(a.loc, DiagnosticCode::InvalidIndex, "array index out of bounds");
      e.kind = H::Index;
      e.ref = n;
      e.type = t.element;
      if (!place(e.args[0]))
        fail(a.loc, DiagnosticCode::TypeMismatch, "array base must be a local place");
      break;
    }
    case A::Assign:
      e.kind = H::Assign;
      if (!place(e.args[0]))
        fail(a.loc, DiagnosticCode::TypeMismatch, "assignment requires a place");
      coerce(e.args[0].type, e.args[1]);
      if (e.args[0].type == Str)
        fail(a.loc, DiagnosticCode::EscapingView,
             "str bindings cannot be reassigned; introduce a new lexical binding");
      if (e.args[0].type == RawPtr)
        fail(a.loc, DiagnosticCode::RestrictedOperation,
             "raw pointer reassignment is not supported");
      if (p.linear(e.args[0].type))
        fail(a.loc, DiagnosticCode::InvalidPlace,
             "PoC assignment supports scalar places; use a fresh let for resource moves");
      break;
    case A::Method: {
      auto &recv = e.args[0];
      if (recv.kind != H::Variable)
        fail(a.loc, DiagnosticCode::InvalidPlace, "method receiver must be a local name");
      if (a.text == "move" || a.text == "drop") {
        if (e.args.size() != 1)
          fail(a.loc, DiagnosticCode::ArgumentMismatch, "move/drop take no arguments");
        e.kind = a.text == "move" ? H::Move : H::Drop;
        e.type = a.text == "move" ? recv.type : Unit;
        break;
      }
      auto method = a.text;
      std::vector<hir::Stmt> formattingTemps;
      if (method == "append_display" || method == "append_debug") {
        if (p.types[recv.type].representation != hir::Representation::StringRecord ||
            e.args.size() != 2)
          fail(a.loc, DiagnosticCode::FormatType, "format append requires String and one argument");
        auto &value = e.args[1];
        if (p.types[value.type].representation == hir::Representation::StringRecord) {
          if (value.kind != H::Variable) {
            SymbolId id = p.symbols.size();
            p.symbols.push_back({id, "$format_value" + std::to_string(id), value.type, Access::Own,
                                 false, false, value.loc});
            hir::Stmt binding{};
            binding.kind = hir::Stmt::Let;
            binding.symbol = id;
            binding.loc = value.loc;
            binding.expr = value;
            formattingTemps.push_back(std::move(binding));
            value = {};
            value.kind = H::Variable;
            value.ref = id;
            value.type = p.symbols[id].type;
            value.loc = a.loc;
          }
          H view{};
          view.kind = H::Method;
          view.loc = value.loc;
          view.type = Str;
          view.ref = functions.at("String::as_str");
          view.args.push_back(value);
          value = std::move(view);
        }
        if (!scalar(value.type) && value.type != Str)
          DiagnosticEngine()
              .error(DiagnosticCode::FormatType, value.loc,
                     "type `" + p.types[value.type].name + "` cannot be formatted")
              .note(method == "append_debug" ? "{:?} requires Debug" : "{} requires Display")
              .emit();
        method = std::string(method == "append_debug" && (value.type == Str || value.type == Char)
                                 ? "fmt_debug_"
                                 : "fmt_") +
                 p.types[value.type].name;
      }
      auto key = p.types[recv.type].name + "::" + method;
      if (!functions.contains(key))
        fail(a.loc, DiagnosticCode::UnknownName, "unknown method `" + key + "`");
      e.ref = functions[key];
      auto &f = p.functions[e.ref];
      if (f.params.empty() || !p.symbols[f.params[0]].receiver)
        fail(a.loc, DiagnosticCode::TypeMismatch, "method needs self");
      e.kind = H::Method;
      e.type = f.result;
      args(e, f, 0);
      if (!formattingTemps.empty()) {
        H block{};
        block.kind = H::Block;
        block.loc = a.loc;
        block.type = e.type;
        block.body = std::move(formattingTemps);
        block.args.push_back(std::move(e));
        return block;
      }
      break;
    }
    case A::Call: {
      if (a.text == "print") {
        if (e.args.size() != 1 ||
            (!scalar(e.args[0].type) && e.args[0].type != Str &&
             p.types[e.args[0].type].representation != hir::Representation::StringRecord))
          fail(a.loc, DiagnosticCode::ArgumentMismatch, "print expects one i32, bool or usize");
        e.kind = H::Print;
        break;
      }
      auto sep = a.text.find("::");
      if (sep != std::string::npos && a.text.substr(sep + 2) == "new" &&
          !functions.contains(a.text)) {
        auto n = a.text.substr(0, sep);
        if (!types.contains(n) || p.types[types[n]].kind != hir::Type::Structure)
          fail(a.loc, DiagnosticCode::UnknownName, "unknown struct `" + n + "`");
        if (!e.args.empty())
          fail(a.loc, DiagnosticCode::ArgumentMismatch, "synthesized new() takes no arguments");
        e.kind = H::Construct;
        e.type = types[n];
        break;
      }
      if (!functions.contains(a.text))
        fail(a.loc, DiagnosticCode::UnknownName, "unknown function `" + a.text + "`");
      e.ref = functions[a.text];
      auto &f = p.functions[e.ref];
      if (!f.params.empty() && p.symbols[f.params[0]].receiver)
        fail(a.loc, DiagnosticCode::InvalidPlace, "use receiver.method() for instance methods");
      if (!privileged && f.trusted && !f.pub)
        fail(a.loc, DiagnosticCode::RestrictedOperation,
             "internal runtime function is not part of the public API");
      e.kind = H::Call;
      e.type = f.result;
      args(e, f, 0);
      break;
    }
    default:
      fail(a.loc, DiagnosticCode::TypeMismatch, "unsupported expression");
    }
    return e;
  }
  bool uses(const ast::Expr &e, const std::string &name, SyntaxContext context) {
    if (e.kind == ast::Expr::Name && e.text == name && e.loc.context == context)
      return true;
    for (auto &a : e.args)
      if (uses(a, name, context))
        return true;
    for (auto &s : e.body)
      if (uses(s, name, context))
        return true;
    return false;
  }
  bool uses(const ast::Stmt &s, const std::string &name, SyntaxContext context) {
    if (uses(s.expr, name, context) || uses(s.limit, name, context))
      return true;
    for (auto &b : s.body)
      if (uses(b, name, context))
        return true;
    for (auto &b : s.otherwise)
      if (uses(b, name, context))
        return true;
    return false;
  }
  std::vector<hir::Stmt> block(const std::vector<ast::Stmt> &ss, bool keepScope = false) {
    scopes.emplace_back();
    std::vector<hir::Stmt> out;
    std::map<size_t, std::vector<hir::Stmt>> endings;
    for (size_t position = 0; position < ss.size(); ++position) {
      struct EndRegions {
        std::function<void()> action;
        ~EndRegions() {
          action();
        }
      } endRegions{[&] {
        auto found = endings.find(position);
        if (found != endings.end())
          out.insert(out.end(), found->second.begin(), found->second.end());
      }};
      auto &a = ss[position];
      if (a.kind == ast::Stmt::Let && a.expr.kind == ast::Expr::Borrow) {
        hir::Stmt borrowed{};
        borrowed.kind = hir::Stmt::BorrowStart;
        borrowed.loc = a.loc;
        borrowed.expr = expr(a.expr);
        borrowed.access =
            borrowed.expr.borrowKind == BorrowKind::Unique ? Access::Unique : Access::Shared;
        if (a.annotated && (!a.annotation.reference ||
                            a.annotation.mutableReference != (borrowed.access == Access::Unique)))
          fail(a.loc, DiagnosticCode::TypeMismatch,
               "reference binding requires matching & or &mut annotation");
        if (a.annotated)
          same(getType(a.annotation, true), borrowed.expr.type, a.annotation.loc);
        size_t last = position;
        for (size_t n = position + 1; n < ss.size(); ++n)
          if (uses(ss[n], a.name, a.loc.context))
            last = n;
        borrowed.symbol = add(a.name, borrowed.expr.type, a.loc, borrowed.access);
        p.symbols[borrowed.symbol].reference = true;
        p.symbols[borrowed.symbol].region = borrowed.expr.region;
        p.symbols[borrowed.symbol].lastUse = ss[last].loc;
        if (keepScope)
          last = ss.size() - 1;
        hir::Stmt end{};
        end.unconditional = keepScope;
        end.kind = hir::Stmt::BorrowEnd;
        end.symbol = borrowed.symbol;
        end.loc = ss[last].loc;
        endings[last].push_back(end);
        out.push_back(std::move(borrowed));
        continue;
      }
      hir::Stmt s{};
      s.kind = static_cast<hir::Stmt::Kind>(a.kind);
      s.loc = a.loc;
      s.access = a.access;
      using A = ast::Stmt;
      using H = hir::Stmt;
      if (a.kind == A::Match) {
        s.kind = H::Match;
        s.expr = expr(a.expr);
        auto tid = s.expr.type;
        auto t = p.types[tid];
        if (t.kind != hir::Type::Enum && tid != Bool && !integer(tid))
          fail(a.loc, DiagnosticCode::InvalidPattern, "match requires enum, bool or integer");
        scopes.emplace_back();
        std::vector<H> prefix;
        if (t.kind == hir::Type::Enum && s.expr.kind == hir::Expr::Variable)
          s.symbol = s.expr.ref;
        else {
          H let{};
          let.kind = H::Let;
          let.loc = a.loc;
          let.symbol = add("$match" + std::to_string(p.symbols.size()), tid, a.loc);
          let.expr = s.expr;
          s.symbol = let.symbol;
          prefix.push_back(let);
        }
        hir::Expr base{};
        base.kind = hir::Expr::Variable;
        base.loc = a.loc;
        base.type = tid;
        base.ref = s.symbol;
        std::set<std::string> covered;
        bool all = false;
        for (auto &arm : a.body) {
          H h{};
          h.kind = H::Scope;
          h.loc = arm.loc;
          auto &pat = arm.pattern;
          if (all)
            fail(pat.loc, DiagnosticCode::UnreachableArm, "unreachable match arm");
          scopes.emplace_back();
          SymbolId alias = None;
          h.catchall = pat.kind == ast::Pattern::Wildcard || pat.kind == ast::Pattern::Binding;
          if (h.catchall) {
            all = true;
            if (pat.kind == ast::Pattern::Binding) {
              if (t.kind == hir::Type::Enum)
                alias = add(pat.name, tid, pat.loc, Access::Shared);
              else {
                H let{};
                let.kind = H::Let;
                let.loc = pat.loc;
                let.symbol = add(pat.name, tid, pat.loc);
                let.expr = base;
                h.body.push_back(let);
              }
            }
          } else {
            hir::Expr lhs = base, rhs{};
            rhs.kind = hir::Expr::Constant;
            rhs.loc = pat.loc;
            std::string key;
            if (t.kind == hir::Type::Enum) {
              if (pat.kind != ast::Pattern::Variant)
                fail(pat.loc, DiagnosticCode::InvalidPattern, "pattern type does not match enum");
              auto found = std::find_if(t.variants.begin(), t.variants.end(), [&](auto &v) {
                return pat.name == t.name + "::" + v.name;
              });
              if (found == t.variants.end())
                fail(pat.loc, DiagnosticCode::InvalidVariant,
                     "unknown variant in pattern `" + pat.name + "`");
              h.variant = found->id;
              key = std::to_string(h.variant);
              if (pat.bindings.size() != found->payload.size())
                fail(pat.loc, DiagnosticCode::InvalidPayload, "wrong payload pattern arity");
              lhs.kind = hir::Expr::Field;
              lhs.type = I64;
              lhs.ref = 0;
              lhs.args = {base};
              rhs.type = I64;
              rhs.text = key;
              for (size_t j = 0; j < pat.bindings.size(); ++j)
                if (pat.bindings[j] != "_") {
                  H let{};
                  let.kind = H::Let;
                  let.loc = pat.loc;
                  let.symbol = add(pat.bindings[j], found->payload[j], pat.loc);
                  let.expr.kind = hir::Expr::Field;
                  let.expr.type = found->payload[j];
                  let.expr.loc = pat.loc;
                  let.expr.ref = j + 1;
                  let.expr.args = {base};
                  h.body.push_back(let);
                }
            } else {
              if ((tid == Bool) != (pat.kind == ast::Pattern::Boolean) ||
                  pat.kind == ast::Pattern::Variant)
                fail(pat.loc, DiagnosticCode::InvalidPattern,
                     "pattern type does not match scrutinee");
              rhs.type = tid;
              rhs.text = tid == Bool ? (pat.name == "true" ? "1" : "0") : pat.name;
              if (tid != Bool)
                validateLiteral(tid, rhs.text, pat.loc, p.target.pointerWidth);
              key = rhs.text;
            }
            if (!covered.insert(key).second)
              fail(pat.loc, DiagnosticCode::UnreachableArm, "unreachable duplicate match arm");
            h.expr.kind = hir::Expr::Binary;
            h.expr.loc = pat.loc;
            h.expr.type = Bool;
            h.expr.text = "==";
            h.expr.args = {lhs, rhs};
          }
          auto body = block(arm.body);
          if (alias != None) {
            H borrow{};
            borrow.kind = H::Borrow;
            borrow.loc = pat.loc;
            borrow.access = Access::Shared;
            borrow.symbol = alias;
            borrow.expr.kind = hir::Expr::Borrow;
            borrow.expr.loc = pat.loc;
            borrow.expr.type = base.type;
            borrow.expr.args = {base};
            borrow.expr.borrowKind = BorrowKind::Shared;
            borrow.expr.region = nextRegion++;
            p.symbols[alias].reference = true;
            p.symbols[alias].region = borrow.expr.region;
            borrow.body = std::move(body);
            h.body.push_back(borrow);
          } else
            h.body.insert(h.body.end(), body.begin(), body.end());
          scopes.pop_back();
          s.body.push_back(std::move(h));
        }
        if (!all) {
          std::string missing;
          if (t.kind == hir::Type::Enum) {
            for (auto &v : t.variants)
              if (!covered.contains(std::to_string(v.id)))
                missing += "\n    " + t.name + "::" + v.name;
          } else if (tid == Bool) {
            if (!covered.contains("0"))
              missing += "\n    false";
            if (!covered.contains("1"))
              missing += "\n    true";
          } else
            missing = "\n    _";
          if (!missing.empty())
            fail(a.loc, DiagnosticCode::NonExhaustiveMatch,
                 "non-exhaustive match\nmissing pattern:" + missing);
        }
        scopes.pop_back();
        prefix.push_back(s);
        H scope{};
        scope.kind = H::Scope;
        scope.loc = a.loc;
        scope.body = std::move(prefix);
        out.push_back(std::move(scope));
        continue;
      }
      if (a.kind == A::Break || a.kind == A::Continue) {
        if (loops.empty())
          fail(a.loc, DiagnosticCode::InvalidLoopControl,
               "break/continue requires an enclosing loop");
        s.kind = a.kind == A::Break ? H::Break : H::Continue;
        s.loop = loops.back();
        out.push_back(s);
        continue;
      }
      if (a.kind == A::While || a.kind == A::Loop || a.kind == A::For) {
        s.kind = H::Loop;
        s.loop = nextLoop++;
        s.unconditional = a.kind == A::Loop;
        scopes.emplace_back();
        std::vector<H> init;
        if (a.kind == A::For) {
          auto first = expr(a.expr), last = expr(a.limit);
          coerce(first.type, last);
          if (!integer(first.type))
            fail(a.loc, DiagnosticCode::InvalidRange,
                 "range bounds must have the same integer type");
          auto id = add(a.name, first.type, a.loc),
               end = add("$range_end" + std::to_string(s.loop), first.type, a.loc);
          H begin{};
          begin.kind = H::Let;
          begin.loc = a.loc;
          begin.symbol = id;
          begin.expr = first;
          init.push_back(begin);
          begin.symbol = end;
          begin.expr = last;
          init.push_back(begin);
          hir::Expr var{};
          var.kind = hir::Expr::Variable;
          var.loc = a.loc;
          var.type = first.type;
          var.ref = id;
          auto bound = var;
          bound.ref = end;
          s.expr.kind = hir::Expr::Binary;
          s.expr.loc = a.loc;
          s.expr.type = Bool;
          s.expr.text = "<";
          s.expr.args = {var, bound};
          auto one = var;
          one.kind = hir::Expr::Constant;
          one.ref = None;
          one.text = "1";
          auto sum = var;
          sum.kind = hir::Expr::Binary;
          sum.text = "+";
          sum.args = {var, one};
          H inc{};
          inc.kind = H::Expression;
          inc.loc = a.loc;
          inc.expr.kind = hir::Expr::Assign;
          inc.expr.loc = a.loc;
          inc.expr.type = Unit;
          inc.expr.args = {var, sum};
          s.latch = {inc};
        } else if (!s.unconditional) {
          s.expr = expr(a.expr);
          same(Bool, s.expr.type, a.loc);
        }
        loops.push_back(s.loop);
        s.body = block(a.body);
        loops.pop_back();
        scopes.pop_back();
        if (a.kind == A::For) {
          init.push_back(s);
          H scope{};
          scope.kind = H::Scope;
          scope.loc = a.loc;
          scope.body = std::move(init);
          out.push_back(std::move(scope));
        } else
          out.push_back(std::move(s));
        continue;
      }
      s.expr = expr(a.expr);
      if (s.expr.kind == hir::Expr::Borrow)
        fail(a.loc, DiagnosticCode::EscapingView,
             "reference expression currently requires a local let binding; reference calls and "
             "returns need region contracts");
      if (a.kind == ast::Stmt::Let) {
        if (a.annotated && a.annotation.reference)
          fail(a.loc, DiagnosticCode::TypeMismatch,
               "a reference annotation requires a borrow initializer");
        if (s.expr.type == Unit)
          fail(a.loc, DiagnosticCode::TypeMismatch, "cannot bind unit");
        if (a.annotated) {
          try {
            coerce(getType(a.annotation), s.expr);
          } catch (Error &e) {
            e.diagnostic.labels.push_back(
                {a.annotation.loc, "expected because of this annotation", false});
            throw;
          }
        }
        s.symbol = add(a.name, s.expr.type, a.loc);
      }
      if (a.kind == ast::Stmt::Return)
        coerce(current->result, s.expr);
      if (a.kind == ast::Stmt::If)
        same(Bool, s.expr.type, a.loc);
      if (a.kind == ast::Stmt::Borrow) {
        if (s.expr.kind != hir::Expr::Variable)
          fail(a.loc, DiagnosticCode::InvalidPlace, "borrow requires a named resource");
        scopes.emplace_back();
        s.symbol = add(a.name, s.expr.type, a.loc, a.access);
        hir::Expr borrowed{};
        borrowed.kind = hir::Expr::Borrow;
        borrowed.loc = a.loc;
        borrowed.type = s.expr.type;
        borrowed.args.push_back(s.expr);
        borrowed.borrowKind = a.access == Access::Unique ? BorrowKind::Unique : BorrowKind::Shared;
        borrowed.region = nextRegion++;
        s.expr = std::move(borrowed);
        p.symbols[s.symbol].reference = true;
        p.symbols[s.symbol].region = s.expr.region;
        s.body = block(a.body);
        scopes.pop_back();
      } else
        s.body = blockNested(a.body);
      s.otherwise = blockNested(a.otherwise);
      out.push_back(std::move(s));
    }
    if (!keepScope)
      scopes.pop_back();
    return out;
  }
  std::vector<hir::Stmt> blockNested(const std::vector<ast::Stmt> &b) {
    return b.empty() ? std::vector<hir::Stmt>{} : block(b);
  }

public:
  explicit Resolver(const ast::Program &a, TargetInfo target) : ast(a) {
    p.target = std::move(target);
  }
  hir::Program run() {
    p.types = {{hir::Type::UnitType, "unit", None, None, 0, 0, {}},
               {hir::Type::Integer, "i32", None, None, 0, 1, {}},
               {hir::Type::Boolean, "bool", None, None, 0, 1, {}},
               {hir::Type::Size, "usize", None, None, 0, 1, {}},
               {hir::Type::Pointer, "RawPtr", None, None, 0, 1, {}}};
    for (auto [name, kind] :
         std::vector<std::pair<std::string, hir::Type::Kind>>{{"i8", hir::Type::Integer},
                                                              {"i16", hir::Type::Integer},
                                                              {"i64", hir::Type::Integer},
                                                              {"u8", hir::Type::Integer},
                                                              {"u16", hir::Type::Integer},
                                                              {"u32", hir::Type::Integer},
                                                              {"u64", hir::Type::Integer},
                                                              {"isize", hir::Type::Integer},
                                                              {"f32", hir::Type::Float},
                                                              {"f64", hir::Type::Float},
                                                              {"char", hir::Type::Character}})
      p.types.push_back({kind, name, None, None, 0, 1, {}});
    p.types.push_back({hir::Type::Pointer, "str"});
    types = {{"unit", Unit}, {"i32", I32}, {"bool", Bool}, {"usize", USize}, {"RawPtr", RawPtr}};
    for (TypeId t = 0; t < p.types.size(); ++t)
      types[p.types[t].name] = t;
    OperationId nextOp = 0;
    for (auto &a : ast.protocols) {
      if (protocols.contains(a.name) || types.contains(a.name))
        duplicate(a.loc, a.name);
      ProtocolId id = p.protocols.size();
      protocols[a.name] = id;
      hir::Protocol q{id, a.name, None, {}, {}, {}};
      std::unordered_map<std::string, StateId> states;
      for (auto &s : a.states) {
        if (states.contains(s.name))
          duplicate(s.loc, s.name);
        states[s.name] = q.states.size();
        q.states.push_back(s.name);
      }
      auto state = [&](const std::string &n) {
        if (!states.contains(n))
          fail(a.loc, DiagnosticCode::InvalidProtocolDeclaration,
               "unknown or missing state `" + n + "` in protocol `" + a.name + "`");
        return states[n];
      };
      q.initial = state(a.initial);
      for (auto &f : a.finals) {
        auto idf = state(f);
        if (std::find(q.finals.begin(), q.finals.end(), idf) != q.finals.end())
          duplicate(a.loc, f);
        q.finals.push_back(idf);
      }
      std::unordered_map<std::string, Access> caps;
      for (auto &s : a.states) {
        std::set<std::string> names;
        for (auto &op : s.operations) {
          if (!names.insert(op.name).second)
            duplicate(op.loc, op.name);
          if (op.name == "move" || op.name == "drop" || op.name == "new")
            fail(op.loc, DiagnosticCode::InvalidProtocolDeclaration, "reserved resource operation");
          if (caps.contains(op.name) && caps[op.name] != op.access)
            fail(op.loc, DiagnosticCode::InvalidProtocolDeclaration,
                 "operation access must agree across states");
          caps[op.name] = op.access;
          auto from = state(s.name), to = state(op.target);
          if (op.access != Access::Own && from != to)
            fail(op.loc, DiagnosticCode::InvalidProtocolDeclaration,
                 "state-changing operations require owning self");
          q.operations.push_back({nextOp++, op.name, op.access, from, to});
        }
      }
      p.protocols.push_back(q);
    }
    for (auto &a : ast.structs) {
      if (types.contains(a.name) || protocols.contains(a.name))
        duplicate(a.loc, a.name);
      TypeId id = p.types.size();
      types[a.name] = id;
      p.types.push_back({hir::Type::Structure, a.name, None, None, 0, 1, {}});
    }
    for (auto &a : ast.enums) {
      if (types.contains(a.name) || protocols.contains(a.name))
        duplicate(a.loc, a.name);
      types[a.name] = p.types.size();
      p.types.push_back({hir::Type::Enum, a.name});
    }
    for (auto &a : ast.enums) {
      auto &t = p.types[types[a.name]];
      std::set<std::string> names;
      if (a.variants.empty())
        fail(a.loc, DiagnosticCode::InvalidVariant, "enum must declare at least one variant");
      for (auto &v : a.variants) {
        if (!names.insert(v.name).second)
          duplicate(v.loc, v.name);
        hir::Variant variant{static_cast<VariantId>(t.variants.size()), v.name, {}};
        for (auto &ts : v.payload) {
          auto tid = getType(ts);
          if (!scalar(tid))
            fail(ts.loc, DiagnosticCode::InvalidPayload,
                 "enum payloads currently require primitive scalar types");
          variant.payload.push_back(tid);
        }
        t.width = std::max(t.width, static_cast<uint32_t>(1 + variant.payload.size()));
        t.variants.push_back(variant);
      }
    }

    for (auto &a : ast.structs) {
      privileged = a.trusted;
      auto id = types[a.name];
      if (!a.representation.empty()) {
        if (!a.trusted || (a.representation != "box_i32" && a.representation != "buffer_i32" &&
                           a.representation != "string_utf8" && a.representation != "input_result"))
          fail(a.loc, DiagnosticCode::RestrictedOperation, "unknown heap representation");
        p.types[id].representation = a.representation == "box_i32"
                                         ? hir::Representation::BoxPointer
                                         : hir::Representation::BufferPointer;
        p.types[id].dropGlue = hir::DropGlue::FreeAllocation;
        if (a.representation == "input_result") {
          p.types[id].representation = hir::Representation::InputRecord;
          p.types[id].dropGlue = hir::DropGlue::InputAllocation;
        }
        if (a.representation == "string_utf8") {
          p.types[id].representation = hir::Representation::StringRecord;
          p.types[id].dropGlue = hir::DropGlue::StringAllocation;
        }
      }
      if (!a.protocol.empty()) {
        if (!protocols.contains(a.protocol))
          fail(a.loc, DiagnosticCode::UnknownName, "unknown protocol `" + a.protocol + "`");
        p.types[id].protocol = protocols[a.protocol];
      }
      std::set<std::string> names;
      uint32_t offset = 0;
      for (auto &f : a.fields) {
        if (!names.insert(f.name).second)
          duplicate(f.loc, f.name);
        auto t = getType(f.type);

        if (t == RawPtr && !p.heap(id))
          fail(f.loc, DiagnosticCode::RestrictedOperation,
               "RawPtr fields require a trusted heap representation");
        if (t == Str)
          fail(f.loc, DiagnosticCode::EscapingView,
               "str fields require lifetime contracts, outside this PoC");
        if (t == Unit || p.types[t].kind == hir::Type::Structure)
          fail(f.loc, DiagnosticCode::InvalidPlace,
               "PoC struct fields require scalars or static scalar arrays");
        p.types[id].fields.push_back({f.name, t, offset});
        offset += p.types[t].width;
      }
      p.types[id].width = std::max(1u, offset);
      if (p.types[id].representation == hir::Representation::StringRecord) {
        auto &fs = p.types[id].fields;
        if (fs.size() != 3 || fs[0].type != RawPtr || fs[1].type != USize || fs[2].type != USize)
          fail(a.loc, DiagnosticCode::RestrictedOperation,
               "String requires the runtime descriptor {ptr, len, capacity}");
      }

      if (p.heap(id) && p.types[id].representation != hir::Representation::StringRecord &&
          p.types[id].representation != hir::Representation::InputRecord &&
          (p.types[id].fields.size() != 1 || p.types[id].fields[0].type != RawPtr))
        fail(a.loc, DiagnosticCode::RestrictedOperation,
             "heap wrappers require one private RawPtr field");
    }
    for (const auto &impl : ast.impls)
      if (!types.contains(impl.owner) || p.types[types.at(impl.owner)].kind != hir::Type::Structure)
        fail(impl.loc, DiagnosticCode::UnknownName, "unknown impl struct `" + impl.owner + "`");
    for (auto &a : ast.functions) {
      privileged = a.trusted;
      hir::Function f{};
      f.trusted = a.trusted;
      f.external = a.external;
      f.intrinsic = a.intrinsic;
      f.kind = a.kind;
      f.intrinsicId = findIntrinsic(a.external ? a.name : a.intrinsic);
      if (!a.intrinsic.empty() && f.intrinsicId == IntrinsicId::None)
        fail(a.loc, DiagnosticCode::RestrictedOperation, "unknown intrinsic `" + a.intrinsic + "`");
      f.effects = intrinsicInfo(f.intrinsicId).effects;
      f.releases = a.releases;
      f.pub = a.pub;
      f.id = p.functions.size();
      f.loc = a.loc;
      f.name = a.owner.empty() ? a.name : a.owner + "::" + a.name;
      if (functions.contains(f.name) || f.name == "print")
        duplicate(a.loc, f.name);
      functions[f.name] = f.id;
      if (!a.owner.empty()) {
        if (!types.contains(a.owner) || p.types[types[a.owner]].kind != hir::Type::Structure)
          fail(a.loc, DiagnosticCode::UnknownName, "unknown impl struct");
        f.owner = types[a.owner];
        if (p.heap(f.owner) && !a.trusted)
          fail(a.loc, DiagnosticCode::RestrictedOperation,
               "bundled heap types cannot be extended by application impls");
      }
      if (a.result.reference)
        fail(a.result.loc, DiagnosticCode::EscapingView,
             "reference return requires an inferred input-region contract; returning local owners "
             "is forbidden");
      f.result = getType(a.result);
      if (f.result == Str && f.intrinsic.empty())
        fail(f.loc, DiagnosticCode::EscapingView,
             "returning str views requires lifetime contracts, outside this PoC");
      if (p.linear(f.result) && f.intrinsic.empty())
        fail(a.loc, DiagnosticCode::InvalidPlace,
             "resource returns are outside this PoC; use synthesized Type::new()");
      scopes.emplace_back();
      for (size_t i = 0; i < a.params.size(); ++i) {
        auto &ap = a.params[i];
        if (ap.type.reference)
          fail(ap.loc, DiagnosticCode::EscapingView,
               "reference parameters require function-region contracts, not yet implemented in "
               "this increment");
        bool self = ap.name == "self";
        if (self && i != 0)
          fail(ap.loc, DiagnosticCode::TypeMismatch, "self must be first");
        if (!self && ap.access != Access::Own)
          fail(ap.loc, DiagnosticCode::InvalidPlace,
               "shared/unique parameters are supported only for self");
        auto t = getType(ap.type);
        if (t == Unit)
          fail(ap.loc, DiagnosticCode::TypeMismatch, "unit parameter is not supported");
        f.params.push_back(add(ap.name, t, ap.loc, ap.access, true, self));
        if (self)
          f.access = ap.access;
      }
      scopes.pop_back();
      p.functions.push_back(f);
    }
    for (auto &f : p.functions) {
      if (!f.intrinsic.empty()) {
        if (intrinsicInfo(f.intrinsicId).lowering == IntrinsicLowering::InputRecord) {
          if (!f.trusted)
            fail(f.loc, DiagnosticCode::RestrictedOperation,
                 "input intrinsics are private library implementation");
          if (f.intrinsic.starts_with("io.parse_")) {
            auto suffix = f.intrinsic.substr(9);
            auto cap = suffix;
            cap[0] = static_cast<char>(std::toupper(cap[0]));
            if (f.owner == None ||
                p.types[f.owner].representation != hir::Representation::StringRecord ||
                f.params.size() != 1 || !p.symbols[f.params[0]].receiver ||
                f.access != Access::Shared || p.types[f.result].name != "Result" + cap ||
                p.types[f.result].representation != hir::Representation::InputRecord)
              fail(f.loc, DiagnosticCode::RestrictedOperation, "invalid parsing signature");
          } else if (f.intrinsic.starts_with("io.read_")) {
            auto suffix = f.intrinsic.substr(8);
            auto cap = suffix;
            cap[0] = static_cast<char>(std::toupper(cap[0]));
            if (f.owner != None || !f.params.empty() || p.types[f.result].name != "Result" + cap ||
                p.types[f.result].representation != hir::Representation::InputRecord)
              fail(f.loc, DiagnosticCode::RestrictedOperation,
                   "invalid input constructor signature");
          } else {
            auto suffix = f.intrinsic.substr(10);
            TypeId result = suffix == "error" ? I32
                            : suffix == "ok"  ? Bool
                            : suffix == "str" ? Str
                                              : types.at(suffix);
            auto cap = suffix == "str" ? std::string("String") : suffix;
            cap[0] = static_cast<char>(std::toupper(cap[0]));
            if (f.owner == None ||
                p.types[f.owner].representation != hir::Representation::InputRecord ||
                f.params.size() != 1 || !p.symbols[f.params[0]].receiver ||
                p.symbols[f.params[0]].type != f.owner || f.access != Access::Shared ||
                f.result != result ||
                (suffix != "ok" && suffix != "error" && p.types[f.owner].name != "Result" + cap))
              fail(f.loc, DiagnosticCode::RestrictedOperation, "invalid input result accessor");
          }
          continue;
        }
        if (f.intrinsic.starts_with("string.")) {
          if (!f.trusted || f.owner == None ||
              p.types[f.owner].representation != hir::Representation::StringRecord)
            fail(f.loc, DiagnosticCode::RestrictedOperation, "invalid String intrinsic owner");
          auto sig = intrinsicSignature(f.intrinsicId, f.owner, f.result);
          auto access =
              f.intrinsicId == IntrinsicId::String_new || f.intrinsicId == IntrinsicId::String_from
                  ? Access::Own
              : f.intrinsicId == IntrinsicId::String_push_str ||
                      f.intrinsicId == IntrinsicId::String_clear ||
                      f.intrinsic.starts_with("string.fmt_")
                  ? Access::Unique
                  : Access::Shared;
          for (auto &t : sig)
            if (t == None)
              t = f.owner;
          if (f.result != sig[0] || f.params.size() + 1 != sig.size() || f.access != access)
            fail(f.loc, DiagnosticCode::RestrictedOperation, "invalid String intrinsic signature");
          for (size_t n = 0; n < f.params.size(); ++n)
            if (p.symbols[f.params[n]].type != sig[n + 1])
              fail(f.loc, DiagnosticCode::RestrictedOperation,
                   "invalid String intrinsic parameter");
          bool method = f.intrinsic != "string.new" && f.intrinsic != "string.from";
          if (method && (f.params.empty() || !p.symbols[f.params[0]].receiver))
            fail(f.loc, DiagnosticCode::RestrictedOperation, "String method requires self");
          continue;
        }
        bool box = f.intrinsic == "box.new", buffer = f.intrinsic == "buffer.new";
        if (!f.trusted || (!box && !buffer) || f.owner == None || f.result != f.owner ||
            f.params.size() != 1 || p.symbols[f.params[0]].receiver ||
            p.symbols[f.params[0]].type != (box ? I32 : USize) ||
            p.types[f.owner].representation !=
                (box ? hir::Representation::BoxPointer : hir::Representation::BufferPointer))
          fail(f.loc, DiagnosticCode::RestrictedOperation, "invalid heap constructor intrinsic");
      }
      if (f.external) {

        std::vector<TypeId> signature{f.result};
        for (auto param : f.params)
          signature.push_back(p.symbols[param].type);
        if (f.intrinsicId == IntrinsicId::None ||
            intrinsicSignature(f.intrinsicId, f.owner, f.result) != signature)
          fail(f.loc, DiagnosticCode::RestrictedOperation,
               "runtime C ABI signature mismatch for `" + f.name + "`");
      }
      if (f.external) {
        if (!f.trusted || f.owner != None || p.linear(f.result))
          fail(f.loc, DiagnosticCode::RestrictedOperation, "unsupported C ABI signature");
        for (auto id : f.params)
          if (p.linear(p.symbols[id].type))
            fail(f.loc, DiagnosticCode::RestrictedOperation, "C ABI accepts only primitive types");
      }
      if (f.releases &&
          (!f.trusted || f.owner == None || !p.heap(f.owner) || f.access != Access::Own))
        fail(f.loc, DiagnosticCode::RestrictedOperation, "invalid allocation-release contract");
      if (f.owner != None && f.intrinsic.empty()) {
        auto pid = p.types[f.owner].protocol;
        if (pid != None) {
          auto shortName = f.name.substr(f.name.find("::") + 2);
          for (auto &op : p.protocols[pid].operations)
            if (op.name == shortName) {
              if (f.params.empty() || !p.symbols[f.params[0]].receiver || op.access != f.access)
                fail(f.loc, DiagnosticCode::InvalidImplementation,
                     "method receiver does not match protocol operation");
              if (f.result != Unit && op.access == Access::Own)
                fail(f.loc, DiagnosticCode::InvalidImplementation,
                     "owning protocol transitions return unit");
              f.operations.push_back(op.id);
            }
          if (f.operations.empty())
            fail(f.loc, DiagnosticCode::InvalidImplementation,
                 "methods on protocol resources must be declared in the protocol");
        }
      }
    }
    for (auto &t : p.types)
      if (t.protocol != None)
        for (auto &op : p.protocols[t.protocol].operations)
          if (!functions.contains(t.name + "::" + op.name))
            fail({}, DiagnosticCode::InvalidImplementation,
                 "missing implementation `" + t.name + "::" + op.name + "`");
    DiagnosticEngine bodyDiagnostics;
    for (size_t i = 0; i < ast.functions.size(); ++i) {
      auto &f = p.functions[i];
      current = &f;
      privileged = f.trusted;
      if (f.external || !f.intrinsic.empty())
        continue;
      scopes.emplace_back();
      for (auto id : f.params)
        scopes.back()[p.symbols[id].name + "#" + std::to_string(p.symbols[id].loc.context)] = id;
      try {
        f.body = block(ast.functions[i].body);
      } catch (const Error &e) {
        bodyDiagnostics.report(e.diagnostic);
        scopes.resize(1);
        loops.clear();
      }
      scopes.pop_back();
      if (bodyDiagnostics.errorCount() >= 20)
        break;
    }
    bodyDiagnostics.throwIfErrors();

    bool changed = true;
    while (changed) {
      changed = false;
      for (auto &f : p.functions) {
        auto before = f.effects;
        std::function<void(const hir::Expr &)> visitExpr;
        std::function<void(const std::vector<hir::Stmt> &)> visitBlock;
        visitExpr = [&](const hir::Expr &e) {
          if (e.kind == hir::Expr::Print)
            f.effects.io = true;
          if (e.kind == hir::Expr::Call || e.kind == hir::Expr::Method)
            f.effects.merge(p.functions.at(e.ref).effects);
          for (auto &arg : e.args)
            visitExpr(arg);
          visitBlock(e.body);
        };
        visitBlock = [&](const std::vector<hir::Stmt> &ss) {
          for (auto &s : ss) {
            visitExpr(s.expr);
            visitBlock(s.body);
            visitBlock(s.otherwise);
            visitBlock(s.latch);
          }
        };
        visitBlock(f.body);
        changed |= before != f.effects;
      }
    }
    auto main = functions.find("main");
    if (main == functions.end())
      fail({}, DiagnosticCode::UnknownName, "missing fn main() -> i32");
    auto &f = p.functions[main->second];
    if (f.result != I32 || !f.params.empty())
      fail(f.loc, DiagnosticCode::TypeMismatch, "main must have signature fn main() -> i32");
    return std::move(p);
  }
};
void printS(const std::vector<hir::Stmt> &, std::ostream &, int);
void printE(const hir::Expr &e, std::ostream &o) {
  static const char *names[] = {"constant", "symbol",         "binary",     "unary", "call",
                                "method",   "field",          "index",      "array", "assign",
                                "move",     "drop",           "construct",  "print", "unit",
                                "cast",     "enum.construct", "static_str", "block", "borrow"};
  o << names[e.kind] << "<T" << e.type << ">";
  if (e.kind == hir::Expr::Borrow)
    o << " region=#R" << e.region
      << " access=" << (e.borrowKind == BorrowKind::Shared ? "Shared" : "Unique");
  if (e.ref != None)
    o << "#" << e.ref;
  if (!e.text.empty())
    o << " " << (e.kind == hir::Expr::StaticStr ? quoteText(e.text) : e.text);
  if (!e.body.empty()) {
    o << " {\n";
    printS(e.body, o, 4);
    o << "  }";
  }
  for (auto &a : e.args) {
    o << " (";
    printE(a, o);
    o << ")";
  }
}
void printS(const std::vector<hir::Stmt> &ss, std::ostream &o, int d) {
  static const char *names[] = {"let",    "return", "expr",         "if",
                                "borrow", "loop",   "break",        "continue",
                                "scope",  "match",  "borrow.start", "borrow.end"};
  for (auto &s : ss) {
    o << std::string(d, ' ') << names[s.kind];
    if (s.symbol != None)
      o << " bind #v" << s.symbol;
    if (s.loop != None)
      o << " target=#L" << s.loop;
    if (s.variant != None)
      o << " variant=#V" << s.variant;
    if (s.catchall)
      o << " wildcard";
    o << " ";
    printE(s.expr, o);
    o << "\n";
    printS(s.body, o, d + 2);
    if (!s.latch.empty()) {
      o << std::string(d, ' ') << "latch\n";
      printS(s.latch, o, d + 2);
    }
    if (!s.otherwise.empty()) {
      o << std::string(d, ' ') << "else\n";
      printS(s.otherwise, o, d + 2);
    }
  }
}
}
hir::Program resolve(const ast::Program &a, TargetInfo target) {
  return Resolver(a, std::move(target)).run();
}
void printHIR(const hir::Program &p, std::ostream &o) {
  o << "HIR\n";
  for (auto &q : p.protocols) {
    o << "protocol " << q.name << " #P" << q.id << " {\n  initial #S" << q.initial << " "
      << q.states[q.initial] << "\n";
    for (auto f : q.finals)
      o << "  final #S" << f << " " << q.states[f] << "\n";
    for (auto &op : q.operations)
      o << "  #O" << op.id << " " << op.name << " access=" << accessName(op.access)
        << " transition=#S" << op.from << " -> #S" << op.to << "\n";
    o << "}\n";
  }
  for (TypeId i = 0; i < p.types.size(); ++i) {
    for (auto &v : p.types[i].variants)
      o << "enum #E" << i << " variant #V" << v.id << " " << v.name
        << " payload=" << v.payload.size() << "\n";
    o << "type #T" << i << " " << p.types[i].name;
    if (p.types[i].representation == hir::Representation::StringRecord)
      o << " representation={RawPtr,usize,usize} DropGlue=StringAllocation";
    else if (p.types[i].representation == hir::Representation::InputRecord)
      o << " representation=InputResultRecord DropGlue=InputAllocation";
    else if (p.heap(i))
      o << " representation=RawPtr DropGlue=FreeAllocation";
    if (p.types[i].protocol != None)
      o << " protocol=#P" << p.types[i].protocol;
    o << "\n";
    for (const auto &field : p.types[i].fields)
      o << "  field " << field.name << " : #T" << field.type << " offset=" << field.offset << "\n";
  }
  for (auto &s : p.symbols)
    o << "symbol #v" << s.id << " " << s.name << " : #T" << s.type
      << " access=" << accessName(s.access) << "\n";
  for (auto &f : p.functions) {
    o << "fn #F" << f.id << " " << f.name << " -> #T" << f.result;
    if (f.external)
      o << " extern=C";
    o << " kind=Normal effects={" << f.effects.describe() << " }";
    if (!f.intrinsic.empty())
      o << " intrinsic=#" << static_cast<unsigned>(f.intrinsicId) << ":" << f.intrinsic;
    if (f.releases)
      o << " allocation-effect=release";
    if (f.owner != None)
      o << " impl=#T" << f.owner;
    for (auto op : f.operations)
      o << " operation=#O" << op;
    o << "\n";
    printS(f.body, o, 2);
  }
}
}
