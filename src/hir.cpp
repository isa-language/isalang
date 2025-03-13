#include "isac/hir.hpp"
#include <algorithm>
#include <functional>
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
  void duplicate(Loc l, const std::string &n) {
    fail(l, "E0101", "duplicate declaration `" + n + "`");
  }
  TypeId getType(const TypeSyntax &s) {
    auto it = types.find(s.name);
    if (it == types.end())
      fail(s.loc, "E0102", "unknown type `" + s.name + "`");
    TypeId t = it->second;
    if (t == RawPtr && !privileged)
      fail(s.loc, "E0500", "RawPtr is restricted to bundled runtime bindings");
    if (!s.count)
      return t;
    if (t != I32 && t != Bool)
      fail(s.loc, "E0103", "PoC arrays require i32 or bool elements");
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
    if (scopes.back().contains(n))
      duplicate(l, n);
    SymbolId id = p.symbols.size();
    p.symbols.push_back({id, n, t, a, param, self, l});
    scopes.back()[n] = id;
    return id;
  }
  SymbolId lookup(std::string n, Loc l) {
    for (auto i = scopes.rbegin(); i != scopes.rend(); ++i)
      if (i->contains(n))
        return i->at(n);
    fail(l, "E0102", "unknown name `" + n + "`");
  }
  void same(TypeId a, TypeId b, Loc l) {
    if (a != b)
      fail(l, "E0200", "expected `" + p.types[a].name + "`, found `" + p.types[b].name + "`");
  }
  void coerce(TypeId wanted, hir::Expr &e) {
    if (wanted == USize && e.type == I32 && e.kind == hir::Expr::Constant &&
        !e.text.starts_with("-"))
      e.type = USize;
    same(wanted, e.type, e.loc);
  }
  void args(hir::Expr &e, hir::Function &f, size_t skip) {
    if (e.args.size() != f.params.size())
      fail(e.loc, "E0201", "wrong number of arguments to `" + f.name + "`");
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
    if (a.kind == A::Int) {
      e.kind = H::Constant;
      e.type = I32;
      if (a.text.ends_with("usize")) {
        e.type = USize;
        e.text.resize(e.text.size() - 5);
        try {
          std::stoull(e.text);
        } catch (...) {
          fail(a.loc, "E0202", "usize literal out of range");
        }
        return e;
      }
      try {
        auto v = std::stoll(a.text);
        if (v > 2147483647)
          fail(a.loc, "E0202", "i32 literal out of range");
      } catch (const std::out_of_range &) {
        fail(a.loc, "E0202", "i32 literal out of range");
      }
      return e;
    }
    if (a.kind == A::Bool) {
      e.kind = H::Constant;
      e.type = Bool;
      e.text = a.text == "true" ? "1" : "0";
      return e;
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
    if (a.kind == A::Unary && a.text == "-" && a.args[0].kind == A::Int &&
        a.args[0].text == "2147483648") {
      e.kind = H::Constant;
      e.type = I32;
      e.text = "-2147483648";
      return e;
    }
    for (auto &x : a.args)
      e.args.push_back(expr(x));
    switch (a.kind) {
    case A::Unary:
      e.kind = H::Unary;
      e.type = a.text == "!" ? Bool : I32;
      same(e.type, e.args[0].type, a.loc);
      break;
    case A::Binary: {
      e.kind = H::Binary;
      if (e.args[0].type == USize)
        coerce(USize, e.args[1]);
      if (e.args[1].type == USize)
        coerce(USize, e.args[0]);
      auto t = e.args[0].type;
      same(t, e.args[1].type, a.loc);
      bool logical = a.text == "&&" || a.text == "||";
      bool equal = a.text == "==" || a.text == "!=";
      bool compare = equal || a.text == "<" || a.text == ">" || a.text == "<=" || a.text == ">=";
      if (logical)
        same(Bool, t, a.loc);
      else if (!equal && t != USize)
        same(I32, t, a.loc);
      else if (equal && t != I32 && t != Bool && t != USize)
        fail(a.loc, "E0200", "equality requires scalar operands");
      e.type = (logical || compare) ? Bool : t;
      break;
    }
    case A::Array: {
      if (e.args.empty())
        fail(a.loc, "E0200", "empty arrays are not supported");
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
        fail(a.loc, "E0500", "heap representation is private; use its methods");
      if (t.kind != hir::Type::Structure)
        fail(a.loc, "E0200", "field access requires a struct");
      auto it =
          std::find_if(t.fields.begin(), t.fields.end(), [&](auto &f) { return f.name == a.text; });
      if (it == t.fields.end())
        fail(a.loc, "E0102", "unknown field `" + a.text + "`");
      e.kind = H::Field;
      e.ref = it->offset;
      e.type = it->type;
      if (!place(e.args[0]))
        fail(a.loc, "E0200", "field base must be a local place");
      break;
    }
    case A::Index: {
      auto &t = p.types[e.args[0].type];
      if (t.kind != hir::Type::Array)
        fail(a.loc, "E0200", "indexing requires an array");
      same(I32, e.args[1].type, a.loc);
      if (e.args[1].kind != H::Constant)
        fail(a.loc, "E0203", "PoC array indices must be constant");
      auto n = std::stoll(e.args[1].text);
      if (n < 0 || n >= t.count)
        fail(a.loc, "E0203", "array index out of bounds");
      e.kind = H::Index;
      e.ref = n;
      e.type = t.element;
      if (!place(e.args[0]))
        fail(a.loc, "E0200", "array base must be a local place");
      break;
    }
    case A::Assign:
      e.kind = H::Assign;
      if (!place(e.args[0]))
        fail(a.loc, "E0200", "assignment requires a place");
      coerce(e.args[0].type, e.args[1]);
      if (e.args[0].type == RawPtr)
        fail(a.loc, "E0500", "raw pointer reassignment is not supported");
      if (p.linear(e.args[0].type))
        fail(a.loc, "E0204",
             "PoC assignment supports scalar places; use a fresh let for resource moves");
      break;
    case A::Method: {
      auto &recv = e.args[0];
      if (recv.kind != H::Variable)
        fail(a.loc, "E0204", "method receiver must be a local name");
      if (a.text == "move" || a.text == "drop") {
        if (e.args.size() != 1)
          fail(a.loc, "E0201", "move/drop take no arguments");
        e.kind = a.text == "move" ? H::Move : H::Drop;
        e.type = a.text == "move" ? recv.type : Unit;
        break;
      }
      auto key = p.types[recv.type].name + "::" + a.text;
      if (!functions.contains(key))
        fail(a.loc, "E0102", "unknown method `" + key + "`");
      e.ref = functions[key];
      auto &f = p.functions[e.ref];
      if (f.params.empty() || !p.symbols[f.params[0]].receiver)
        fail(a.loc, "E0200", "method needs self");
      e.kind = H::Method;
      e.type = f.result;
      args(e, f, 0);
      break;
    }
    case A::Call: {
      if (a.text == "print") {
        if (e.args.size() != 1 ||
            (e.args[0].type != I32 && e.args[0].type != Bool && e.args[0].type != USize))
          fail(a.loc, "E0201", "print expects one i32, bool or usize");
        e.kind = H::Print;
        break;
      }
      auto sep = a.text.find("::");
      if (sep != std::string::npos && a.text.substr(sep + 2) == "new" &&
          !functions.contains(a.text)) {
        auto n = a.text.substr(0, sep);
        if (!types.contains(n) || p.types[types[n]].kind != hir::Type::Structure)
          fail(a.loc, "E0102", "unknown struct `" + n + "`");
        if (!e.args.empty())
          fail(a.loc, "E0201", "synthesized new() takes no arguments");
        e.kind = H::Construct;
        e.type = types[n];
        break;
      }
      if (!functions.contains(a.text))
        fail(a.loc, "E0102", "unknown function `" + a.text + "`");
      e.ref = functions[a.text];
      auto &f = p.functions[e.ref];
      if (!f.params.empty() && p.symbols[f.params[0]].receiver)
        fail(a.loc, "E0204", "use receiver.method() for instance methods");
      if (!privileged && f.trusted && !f.pub)
        fail(a.loc, "E0500", "internal runtime function is not part of the public API");
      e.kind = H::Call;
      e.type = f.result;
      args(e, f, 0);
      break;
    }
    default:
      fail(a.loc, "E0200", "unsupported expression");
    }
    return e;
  }
  std::vector<hir::Stmt> block(const std::vector<ast::Stmt> &ss) {
    scopes.emplace_back();
    std::vector<hir::Stmt> out;
    for (auto &a : ss) {
      hir::Stmt s{};
      s.kind = static_cast<hir::Stmt::Kind>(a.kind);
      s.loc = a.loc;
      s.access = a.access;
      s.expr = expr(a.expr);
      if (a.kind == ast::Stmt::Let) {
        if (s.expr.type == Unit)
          fail(a.loc, "E0200", "cannot bind unit");
        if (a.annotated)
          coerce(getType(a.annotation), s.expr);
        s.symbol = add(a.name, s.expr.type, a.loc);
      }
      if (a.kind == ast::Stmt::Return)
        coerce(current->result, s.expr);
      if (a.kind == ast::Stmt::If)
        same(Bool, s.expr.type, a.loc);
      if (a.kind == ast::Stmt::Borrow) {
        if (s.expr.kind != hir::Expr::Variable || !p.linear(s.expr.type))
          fail(a.loc, "E0204", "borrow requires a named resource");
        scopes.emplace_back();
        s.symbol = add(a.name, s.expr.type, a.loc, a.access);
        s.body = block(a.body);
        scopes.pop_back();
      } else
        s.body = blockNested(a.body);
      s.otherwise = blockNested(a.otherwise);
      out.push_back(std::move(s));
    }
    scopes.pop_back();
    return out;
  }
  std::vector<hir::Stmt> blockNested(const std::vector<ast::Stmt> &b) {
    return b.empty() ? std::vector<hir::Stmt>{} : block(b);
  }

public:
  explicit Resolver(const ast::Program &a) : ast(a) {}
  hir::Program run() {
    p.types = {{hir::Type::UnitType, "unit", None, None, 0, 0, {}},
               {hir::Type::Integer, "i32", None, None, 0, 1, {}},
               {hir::Type::Boolean, "bool", None, None, 0, 1, {}},
               {hir::Type::Size, "usize", None, None, 0, 1, {}},
               {hir::Type::Pointer, "RawPtr", None, None, 0, 1, {}}};
    types = {{"unit", Unit}, {"i32", I32}, {"bool", Bool}, {"usize", USize}, {"RawPtr", RawPtr}};
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
          fail(a.loc, "E0104", "unknown or missing state `" + n + "` in protocol `" + a.name + "`");
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
            fail(op.loc, "E0104", "reserved resource operation");
          if (caps.contains(op.name) && caps[op.name] != op.access)
            fail(op.loc, "E0104", "operation access must agree across states");
          caps[op.name] = op.access;
          auto from = state(s.name), to = state(op.target);
          if (op.access != Access::Own && from != to)
            fail(op.loc, "E0104", "state-changing operations require owning self");
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

    for (auto &a : ast.structs) {
      privileged = a.trusted;
      auto id = types[a.name];
      if (!a.representation.empty()) {
        if (!a.trusted || (a.representation != "box_i32" && a.representation != "buffer_i32"))
          fail(a.loc, "E0500", "unknown heap representation");
        p.types[id].representation = a.representation == "box_i32"
                                         ? hir::Representation::BoxPointer
                                         : hir::Representation::BufferPointer;
        p.types[id].dropGlue = hir::DropGlue::FreeAllocation;
      }
      if (!a.protocol.empty()) {
        if (!protocols.contains(a.protocol))
          fail(a.loc, "E0102", "unknown protocol `" + a.protocol + "`");
        p.types[id].protocol = protocols[a.protocol];
      }
      std::set<std::string> names;
      uint32_t offset = 0;
      for (auto &f : a.fields) {
        if (!names.insert(f.name).second)
          duplicate(f.loc, f.name);
        auto t = getType(f.type);
        if (t == USize)
          fail(f.loc, "E0204",
               "usize struct fields are outside the flat i32 layout; use local usize values");
        if (t == RawPtr && !p.heap(id))
          fail(f.loc, "E0500", "RawPtr fields require a trusted heap representation");
        if (t == Unit || p.types[t].kind == hir::Type::Structure)
          fail(f.loc, "E0204", "PoC struct fields require scalars or static scalar arrays");
        p.types[id].fields.push_back({f.name, t, offset});
        offset += p.types[t].width;
      }
      p.types[id].width = std::max(1u, offset);
      if (p.heap(id) && (p.types[id].fields.size() != 1 || p.types[id].fields[0].type != RawPtr))
        fail(a.loc, "E0500", "heap wrappers require one private RawPtr field");
    }
    for (const auto &impl : ast.impls)
      if (!types.contains(impl.owner) || p.types[types.at(impl.owner)].kind != hir::Type::Structure)
        fail(impl.loc, "E0102", "unknown impl struct `" + impl.owner + "`");
    for (auto &a : ast.functions) {
      privileged = a.trusted;
      hir::Function f{};
      f.trusted = a.trusted;
      f.external = a.external;
      f.intrinsic = a.intrinsic;
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
          fail(a.loc, "E0102", "unknown impl struct");
        f.owner = types[a.owner];
        if (p.heap(f.owner) && !a.trusted)
          fail(a.loc, "E0500", "bundled heap types cannot be extended by application impls");
      }
      f.result = getType(a.result);
      if (p.linear(f.result) && f.intrinsic.empty())
        fail(a.loc, "E0204", "resource returns are outside this PoC; use synthesized Type::new()");
      scopes.emplace_back();
      for (size_t i = 0; i < a.params.size(); ++i) {
        auto &ap = a.params[i];
        bool self = ap.name == "self";
        if (self && i != 0)
          fail(ap.loc, "E0200", "self must be first");
        if (!self && ap.access != Access::Own)
          fail(ap.loc, "E0204", "shared/unique parameters are supported only for self");
        auto t = getType(ap.type);
        if (t == Unit)
          fail(ap.loc, "E0200", "unit parameter is not supported");
        f.params.push_back(add(ap.name, t, ap.loc, ap.access, true, self));
        if (self)
          f.access = ap.access;
      }
      scopes.pop_back();
      p.functions.push_back(f);
    }
    for (auto &f : p.functions) {
      if (!f.intrinsic.empty()) {
        bool box = f.intrinsic == "box.new", buffer = f.intrinsic == "buffer.new";
        if (!f.trusted || (!box && !buffer) || f.owner == None || f.result != f.owner ||
            f.params.size() != 1 || p.symbols[f.params[0]].receiver ||
            p.symbols[f.params[0]].type != (box ? I32 : USize) ||
            p.types[f.owner].representation !=
                (box ? hir::Representation::BoxPointer : hir::Representation::BufferPointer))
          fail(f.loc, "E0500", "invalid heap constructor intrinsic");
      }
      if (f.external) {
        if (!f.trusted || f.owner != None || p.linear(f.result))
          fail(f.loc, "E0500", "unsupported C ABI signature");
        for (auto id : f.params)
          if (p.linear(p.symbols[id].type))
            fail(f.loc, "E0500", "C ABI accepts only primitive types");
      }
      if (f.releases &&
          (!f.trusted || f.owner == None || !p.heap(f.owner) || f.access != Access::Own))
        fail(f.loc, "E0500", "invalid allocation-release contract");
      if (f.owner != None && f.intrinsic.empty()) {
        auto pid = p.types[f.owner].protocol;
        if (pid != None) {
          auto shortName = f.name.substr(f.name.find("::") + 2);
          for (auto &op : p.protocols[pid].operations)
            if (op.name == shortName) {
              if (f.params.empty() || !p.symbols[f.params[0]].receiver || op.access != f.access)
                fail(f.loc, "E0105", "method receiver does not match protocol operation");
              if (f.result != Unit && op.access == Access::Own)
                fail(f.loc, "E0105", "owning protocol transitions return unit");
              f.operations.push_back(op.id);
            }
          if (f.operations.empty())
            fail(f.loc, "E0105", "methods on protocol resources must be declared in the protocol");
        }
      }
    }
    for (auto &t : p.types)
      if (t.protocol != None)
        for (auto &op : p.protocols[t.protocol].operations)
          if (!functions.contains(t.name + "::" + op.name))
            fail({}, "E0105", "missing implementation `" + t.name + "::" + op.name + "`");
    for (size_t i = 0; i < ast.functions.size(); ++i) {
      auto &f = p.functions[i];
      current = &f;
      privileged = f.trusted;
      if (f.external || !f.intrinsic.empty())
        continue;
      scopes.emplace_back();
      for (auto id : f.params)
        scopes.back()[p.symbols[id].name] = id;
      f.body = block(ast.functions[i].body);
      scopes.pop_back();
    }
    auto main = functions.find("main");
    if (main == functions.end())
      fail({}, "E0102", "missing fn main() -> i32");
    auto &f = p.functions[main->second];
    if (f.result != I32 || !f.params.empty())
      fail(f.loc, "E0200", "main must have signature fn main() -> i32");
    return std::move(p);
  }
};
void printE(const hir::Expr &e, std::ostream &o) {
  static const char *names[] = {"constant", "symbol", "binary",    "unary", "call",
                                "method",   "field",  "index",     "array", "assign",
                                "move",     "drop",   "construct", "print", "unit"};
  o << names[e.kind] << "<T" << e.type << ">";
  if (e.ref != None)
    o << "#" << e.ref;
  if (!e.text.empty())
    o << " " << e.text;
  for (auto &a : e.args) {
    o << " (";
    printE(a, o);
    o << ")";
  }
}
void printS(const std::vector<hir::Stmt> &ss, std::ostream &o, int d) {
  static const char *names[] = {"let", "return", "expr", "if", "borrow"};
  for (auto &s : ss) {
    o << std::string(d, ' ') << names[s.kind];
    if (s.symbol != None)
      o << " bind #v" << s.symbol;
    o << " ";
    printE(s.expr, o);
    o << "\n";
    printS(s.body, o, d + 2);
    if (!s.otherwise.empty()) {
      o << std::string(d, ' ') << "else\n";
      printS(s.otherwise, o, d + 2);
    }
  }
}
}
hir::Program resolve(const ast::Program &a) {
  return Resolver(a).run();
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
    o << "type #T" << i << " " << p.types[i].name;
    if (p.heap(i))
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
    if (!f.intrinsic.empty())
      o << " intrinsic=" << f.intrinsic;
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
