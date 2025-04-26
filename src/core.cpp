#include "isac/core.hpp"
#include <algorithm>
#include <functional>
#include <map>
#include <set>
namespace isac::core {
namespace {
class Builder {
  Program p;
  std::vector<Inst> *out = nullptr;
  Inst make(Inst::Kind k, Loc l, TypeId t = Unit) {
    Inst i{};
    i.kind = k;
    i.loc = l;
    i.type = t;
    i.quantity = p.semantics.linear(t) ? Quantity::One : Quantity::Omega;
    return i;
  }
  ValueId emit(Inst i, bool result = true) {
    if (result)
      i.result = p.values++;
    auto v = i.result;
    out->push_back(std::move(i));
    return v;
  }
  std::pair<SymbolId, uint32_t> place(const hir::Expr &e) {
    if (e.kind == hir::Expr::Variable)
      return {e.ref, 0};
    auto [id, off] = place(e.args[0]);
    return {id, off + e.ref};
  }
  ValueId expr(const hir::Expr &e) {
    using E = hir::Expr;
    Inst i = make(Inst::Constant, e.loc, e.type);
    i.text = e.text;
    i.operatorClass = e.operatorClass;
    switch (e.kind) {
    case E::Block: {
      auto instructions = block(e.body);
      auto cleanup = instructions.back();
      instructions.pop_back();
      out->insert(out->end(), instructions.begin(), instructions.end());
      auto value = expr(e.args.at(0));
      emit(cleanup, false);
      return value;
    }
    case E::Cast:
      i.kind = Inst::Cast;
      i.args = {expr(e.args[0])};
      return emit(i);
    case E::StaticStr:
      i.kind = Inst::StaticStr;
      return emit(i);
    case E::Constant:
      return emit(i);
    case E::UnitValue:
      return None;
    case E::Variable:
      if (p.semantics.linear(e.type))
        fail(e.loc, DiagnosticCode::ImplicitCopy,
             "resource `" + p.semantics.symbols[e.ref].name + "` cannot be copied; use .move()");
      i.kind = Inst::Copy;
      i.slot = e.ref;
      return emit(i);
    case E::Field:
    case E::Index: {
      if (p.semantics.linear(e.type))
        fail(e.loc, DiagnosticCode::ImplicitCopy,
             "partial resource moves/copies are outside this PoC");
      auto [id, off] = place(e);
      i.kind = Inst::Read;
      i.slot = id;
      i.offset = off;
      return emit(i);
    }
    case E::Binary:
    case E::Unary:
      if (e.kind == E::Binary && (e.text == "&&" || e.text == "||")) {

        auto lhs = expr(e.args[0]);
        SymbolId slot = p.semantics.symbols.size();
        p.semantics.symbols.push_back(
            {slot, "$logic" + std::to_string(slot), Bool, Access::Own, false, false, e.loc});
        auto bind = make(Inst::Bind, e.loc, Bool);
        bind.slot = slot;
        bind.args = {lhs};
        emit(bind, false);
        auto branch = make(Inst::If, e.loc);
        branch.args = {lhs};
        auto *saved = out;
        std::vector<Inst> rhs;
        out = &rhs;
        auto value = expr(e.args[1]);
        auto write = make(Inst::Write, e.loc, Bool);
        write.slot = slot;
        write.args = {value};
        emit(write, false);
        out = saved;
        if (e.text == "&&")
          branch.body = std::move(rhs);
        else
          branch.otherwise = std::move(rhs);
        emit(branch, false);
        auto read = make(Inst::Copy, e.loc, Bool);
        read.slot = slot;
        return emit(read);
      }
      i.kind = e.kind == E::Binary ? Inst::Binary : Inst::Unary;
      for (auto &a : e.args)
        i.args.push_back(expr(a));
      return emit(i);
    case E::Assign: {
      auto [id, off] = place(e.args[0]);
      i.kind = Inst::Write;
      i.slot = id;
      i.offset = off;
      i.type = e.args[0].type;
      i.args = {expr(e.args[1])};
      emit(i, false);
      return None;
    }
    case E::EnumConstruct:
      i.kind = Inst::EnumConstruct;
      i.offset = e.ref;
      for (auto &a : e.args)
        i.args.push_back(expr(a));
      return emit(i);
    case E::Construct:
      i.kind = Inst::Create;
      return emit(i);
    case E::Array:
      i.kind = Inst::Array;
      for (auto &a : e.args)
        i.args.push_back(expr(a));
      return emit(i);
    case E::Move:
      i.kind = p.semantics.linear(e.type) ? Inst::Move : Inst::Copy;
      i.slot = e.args[0].ref;
      return emit(i);
    case E::Drop:
      i.kind = Inst::Drop;
      i.slot = e.args[0].ref;
      emit(i, false);
      return None;
    case E::Print:
      if (p.semantics.types[e.args[0].type].representation == hir::Representation::StringRecord) {
        if (e.args[0].kind != E::Variable)
          fail(e.loc, DiagnosticCode::InvalidStringOperation,
               "print(String) requires a named owner or borrow");
        i.kind = Inst::TextPrint;
        i.slot = e.args[0].ref;
        i.loc = e.args[0].loc;
        emit(i, false);
        return None;
      }
      i.kind = Inst::Print;
      i.args = {expr(e.args[0])};
      emit(i, false);
      return None;
    case E::Call:
    case E::Method: {
      i.kind = Inst::Call;
      i.function = e.ref;
      auto &f = p.semantics.functions[e.ref];
      i.intrinsicId = f.intrinsicId;
      size_t start = 0;
      if (e.kind == E::Method) {
        i.slot = e.args[0].ref;
        i.access = f.access;
        start = 1;
        if (!f.operations.empty()) {
          i.kind = Inst::ProtocolTransition;
          i.protocol = p.semantics.types[f.owner].protocol;
          i.quantity = e.type == Unit ? Quantity::Zero : Quantity::Omega;
        }
      }
      for (size_t n = start; n < e.args.size(); ++n)
        i.args.push_back(expr(e.args[n]));
      if (!f.intrinsic.empty()) {
        i.kind = intrinsicInfo(f.intrinsicId).lowering == IntrinsicLowering::StringRecord
                     ? Inst::StringCall
                 : intrinsicInfo(f.intrinsicId).lowering == IntrinsicLowering::InputRecord
                     ? Inst::InputCall
                     : Inst::HeapAlloc;
        i.text = f.intrinsic;
      }
      if (f.external) {
        if (f.name == "isac_rt_alloc" || f.name == "isac_rt_alloc_zeroed")
          i.kind = Inst::HeapAlloc;
        else if (f.name == "isac_rt_free")
          i.kind = Inst::HeapFree;
        else if (f.name == "isac_rt_load_i32" || f.name == "isac_rt_buffer_read" ||
                 f.name == "isac_rt_buffer_len")
          i.kind = Inst::HeapLoad;
        else if (f.name == "isac_rt_store_i32" || f.name == "isac_rt_buffer_write" ||
                 f.name == "isac_rt_buffer_init")
          i.kind = Inst::HeapStore;
        else if (f.name == "isac_rt_memcpy")
          i.kind = Inst::MemCopy;
        else if (f.name == "isac_rt_memmove")
          i.kind = Inst::MemMove;
      }
      return emit(i, e.type != Unit || i.kind == Inst::ProtocolTransition);
    }
    }
    fail(e.loc, DiagnosticCode::InternalError, "unhandled HIR expression");
  }
  std::vector<Inst> block(const std::vector<hir::Stmt> &ss) {
    std::vector<Inst> result;
    auto *saved = out;
    out = &result;
    std::vector<SymbolId> locals;
    for (auto &s : ss) {
      Inst i = make(Inst::ScopeEnd, s.loc);
      switch (s.kind) {
      case hir::Stmt::Let:
        i.kind = Inst::Bind;
        i.slot = s.symbol;
        i.type = p.semantics.symbols[s.symbol].type;
        i.quantity = p.semantics.linear(i.type) ? Quantity::One : Quantity::Omega;
        i.args = {expr(s.expr)};
        emit(i, false);
        locals.push_back(s.symbol);
        break;
      case hir::Stmt::Expression: {
        auto v = expr(s.expr);
        if (v != None && s.expr.type != Unit) {
          i.kind = Inst::Discard;
          i.type = s.expr.type;
          i.quantity = p.semantics.linear(i.type) ? Quantity::One : Quantity::Omega;
          i.args = {v};
          emit(i, false);
        }
        break;
      }
      case hir::Stmt::Return:
        i.kind = Inst::Return;
        if (s.expr.type != Unit)
          i.args = {expr(s.expr)};
        emit(i, false);
        break;
      case hir::Stmt::If:
        i.kind = Inst::If;
        i.args = {expr(s.expr)};
        emit(make(Inst::EndViews, s.loc), false);
        i.body = block(s.body);
        i.otherwise = block(s.otherwise);
        emit(i, false);
        break;
      case hir::Stmt::Match: {
        Inst inspect = make(Inst::Inspect, s.loc);
        inspect.slot = s.symbol;
        emit(inspect, false);

        std::function<std::vector<Inst>(size_t)> arms = [&](size_t n) {
          auto &arm = s.body.at(n);
          if (arm.catchall || n + 1 == s.body.size())
            return block(arm.body);
          std::vector<Inst> branch;
          auto savedOut = out;
          out = &branch;
          Inst cond = make(Inst::If, arm.loc);
          cond.args = {expr(arm.expr)};
          cond.body = block(arm.body);
          cond.otherwise = arms(n + 1);
          emit(cond, false);
          out = savedOut;
          return branch;
        };
        auto lowered = arms(0);
        out->insert(out->end(), lowered.begin(), lowered.end());
        break;
      }
      case hir::Stmt::Scope: {
        auto nested = block(s.body);
        out->insert(out->end(), nested.begin(), nested.end());
        break;
      }
      case hir::Stmt::Loop: {
        i.kind = Inst::Loop;
        i.loop = s.loop;
        i.unconditional = s.unconditional;
        if (!s.unconditional) {
          auto savedOut = out;
          out = &i.condition;
          i.args = {expr(s.expr)};
          emit(make(Inst::EndViews, s.loc), false);
          out = savedOut;
        }
        i.body = block(s.body);
        i.latch = block(s.latch);
        emit(i, false);
        break;
      }
      case hir::Stmt::Break:
      case hir::Stmt::Continue:
        i.kind = s.kind == hir::Stmt::Break ? Inst::Break : Inst::Continue;
        i.loop = s.loop;
        emit(i, false);
        break;
      case hir::Stmt::Borrow:
        i.kind = s.access == Access::Shared ? Inst::BorrowShared : Inst::BorrowUnique;
        i.slot = s.expr.ref;
        i.offset = s.symbol;
        i.type = s.expr.type;
        i.quantity = Quantity::One;
        i.access = s.access;
        emit(i, false);
        {
          auto b = block(s.body);
          out->insert(out->end(), b.begin(), b.end());
        }
        i.kind = Inst::EndBorrow;
        i.slot = s.symbol;
        emit(i, false);
        break;
      }
      emit(make(Inst::EndViews, s.loc), false);
    }
    Inst viewsEnd = make(Inst::EndViews, ss.empty() ? Loc{} : ss.back().loc);
    emit(viewsEnd, false);
    Inst end = make(Inst::ScopeEnd, ss.empty() ? Loc{} : ss.back().loc);
    end.locals = std::move(locals);
    emit(end, false);
    out = saved;
    return result;
  }

public:
  explicit Builder(hir::Program h) {
    p.semantics = std::move(h);
  }
  Program run() {
    for (auto &f : p.semantics.functions) {
      if (f.external || !f.intrinsic.empty())
        continue;
      Function cf{f.id, block(f.body), {}};
      for (auto id : f.params) {
        (void)id;
        cf.parameters.push_back(p.values++);
      }
      // An explicit return provides a cleanup point for fallthrough, including owned parameters.
      if (f.result == Unit)
        cf.body.push_back(make(Inst::Return, f.loc));
      p.functions.push_back(std::move(cf));
    }
    return std::move(p);
  }
};
struct Resource {
  bool live = true;
  StateId state = None;
  Access access = Access::Own;
  SymbolId root = None;
  unsigned shared = 0;
  bool unique = false;
  ValueId version = None;
  bool allocation = false;
  std::string consumedBy;
  SymbolId movedTo = None;
  Loc consumedAt = {}, borrowAt = {}, stateAt = {};
  MemoryState memory() const {
    return !live                      ? MemoryState::Dead
           : access == Access::Shared ? MemoryState::Shared
           : access == Access::Unique ? MemoryState::Unique
                                      : MemoryState::Own;
  }
  bool operator==(const Resource &other) const {
    return memory() == other.memory() && state == other.state && root == other.root &&
           shared == other.shared && unique == other.unique && allocation == other.allocation;
  }
};
struct Temporary {
  TypeId type = Unit;
  StateId state = None;
  bool available = true;
  bool allocation = false;
  SymbolId movedFrom = None;
};
struct Context {
  std::map<SymbolId, Resource> vars;
  std::map<ValueId, Temporary> values;
  std::map<ValueId, SymbolId> pointers;
  std::map<SymbolId, SymbolId> pointerSlots;
  std::map<ValueId, SymbolId> views;
  bool returned = false;
};
class Checker {
  Program p;
  hir::Function *function = nullptr;
  struct LoopCheck {
    uint32_t id;
    Context entry;
    std::vector<Context> exits;
  };
  std::vector<LoopCheck> loops;
  void compatible(const Context &entry, const Context &edge, Loc loc, const char *edgeName) {
    for (auto &[id, r] : entry.vars)
      if (!edge.vars.contains(id) || r != edge.vars.at(id))
        fail(loc, DiagnosticCode::LoopInvariant,
             std::string("incompatible resource state at ") + edgeName + ": `" + symbol(id).name +
                 "`");
  }

  const hir::Symbol &symbol(SymbolId id) {
    return p.semantics.symbols.at(id);
  }
  Resource &live(Context &c, SymbolId id, Loc l) {
    auto it = c.vars.find(id);
    if (it == c.vars.end() || !it->second.live) {
      bool moved = it != c.vars.end() && it->second.consumedBy == "move";
      auto d = DiagnosticEngine()
                   .error(DiagnosticCode::ConsumedResource, l,
                          std::string("use of ") + (moved ? "moved" : "dropped") + " value `" +
                              symbol(id).name + "`")
                   .label(l, "value used here after ownership was consumed")
                   .note("the original binding no longer owns this resource");
      if (it != c.vars.end())
        d.secondary(it->second.consumedAt,
                    moved ? "ownership moved here" : "resource dropped here");
      if (it != c.vars.end() && it->second.movedTo != None)
        d.help("use `" + symbol(it->second.movedTo).name + "`, or borrow `" + symbol(id).name +
               "` instead of moving it");
      else
        d.help("borrow the value if ownership transfer is not required");
      d.emit();
    }
    return it->second;
  }
  Resource &access(Context &c, SymbolId id, Access requested, Loc l) {
    auto &r = live(c, id, l);
    auto &root = r.root == None ? r : live(c, r.root, l);
    if (r.root != None) {
      if (requested == Access::Own)
        fail(l, DiagnosticCode::BorrowConflict,
             "cannot move, drop or transition borrowed resource `" + symbol(id).name + "`");
      if (requested == Access::Unique && root.shared && r.access == Access::Unique)
        DiagnosticEngine()
            .error(DiagnosticCode::InvalidReborrow, l,
                   "cannot mutate while a derived str view is active")
            .secondary(root.borrowAt, "view created here")
            .help("end the view scope before mutation")
            .emit();
      if (requested == Access::Unique && r.access == Access::Shared)
        DiagnosticEngine()
            .error(DiagnosticCode::SharedMutation, l,
                   "cannot write through shared borrow `" + symbol(id).name + "`")
            .secondary(r.borrowAt, "shared borrow starts here")
            .note("shared access is read-only")
            .help("use a unique borrow after the shared borrow ends")
            .emit();
    } else {
      if (r.unique || (requested != Access::Shared && r.shared)) {
        std::string reason = r.unique ? "resource already has an active unique borrow"
                                      : "incompatible access while shared borrows are active";
        Loc activeOrigin = r.borrowAt;
        for (auto &[alias, resource] : c.vars)
          if (resource.live && resource.root == id) {
            activeOrigin = resource.borrowAt;
            break;
          }
        DiagnosticEngine()
            .error(DiagnosticCode::InvalidReborrow, l, reason + " for `" + symbol(id).name + "`")
            .label(l, "incompatible access requested here")
            .secondary(activeOrigin, "active borrow starts here")
            .note("unique access requires exclusive access to the resource")
            .help("end the active borrow before moving, dropping or mutating this resource")
            .emit();
      }
    }
    if (r.access == Access::Shared && requested != Access::Shared)
      fail(l, DiagnosticCode::SharedMutation,
           "shared self/resource is read-only: `" + symbol(id).name + "`");
    if (r.access == Access::Unique && requested == Access::Own)
      fail(l, DiagnosticCode::BorrowConflict, "unique borrow does not grant ownership");
    return root;
  }
  void final(TypeId t, StateId s, const std::string &n, Loc l, Loc origin = {}) {
    auto pid = p.semantics.types[t].protocol;
    if (pid == None)
      return;
    auto &q = p.semantics.protocols[pid];
    if (q.finals.empty())
      return;
    if (std::find(q.finals.begin(), q.finals.end(), s) != q.finals.end())
      return;
    std::string m = "resource leaves scope in a non-final protocol state\n\nresource:\n    " + n +
                    "\nprotocol:\n    " + q.name + "\ncurrent state:\n    " + q.states.at(s) +
                    "\nvalid final states:";
    for (auto f : q.finals)
      m += "\n    " + q.states[f];
    auto d = DiagnosticEngine()
                 .error(DiagnosticCode::NonFinalState, l, m)
                 .label(l, "resource leaves its scope here")
                 .help("transition the resource to a final state before leaving this scope");
    if (origin.span.file != NoFile)
      d.secondary(origin, "resource entered this protocol state here");
    d.emit();
  }
  StateId initial(TypeId t) {
    auto pid = p.semantics.types[t].protocol;
    return pid == None ? None : p.semantics.protocols[pid].initial;
  }
  void drop(Context &c, SymbolId id, Loc l, bool implicit, Inst &site) {
    auto it = c.vars.find(id);
    if (implicit && (it == c.vars.end() || !it->second.live))
      return;
    if (!implicit && it != c.vars.end() && !it->second.live)
      DiagnosticEngine()
          .error(DiagnosticCode::ConsumedResource, l,
                 "resource `" + symbol(id).name + "` has already been consumed")
          .label(l, "drop requested here")
          .secondary(it->second.consumedAt, "previous " + it->second.consumedBy + " occurred here")
          .note("a resource may be consumed only once")
          .help("remove the repeated drop")
          .emit();
    if (it != c.vars.end() && symbol(id).type == Str && it->second.root != None) {
      endBorrow(c, id);
      return;
    }
    auto &r = access(c, id, Access::Own, l);
    if (symbol(id).receiver) {
      if (implicit)
        return;
      fail(l, DiagnosticCode::BorrowConflict,
           "method self cannot be dropped or moved; it is returned by the protocol transition");
    }
    final(symbol(id).type, r.state, symbol(id).name, l, r.stateAt);
    if (r.allocation)
      site.cleanup.push_back(id);
    r.allocation = false;
    c.vars[id].live = false;
    c.vars[id].consumedBy = "drop";
    c.vars[id].consumedAt = l;
  }
  Temporary &take(Context &c, ValueId v, Loc l) {
    auto it = c.values.find(v);
    if (it == c.values.end() || !it->second.available)
      fail(l, DiagnosticCode::UnconsumedResource, "linear temporary was already consumed");
    it->second.available = false;
    return it->second;
  }
  void endBorrow(Context &c, SymbolId id) {
    auto it = c.vars.find(id);
    if (it == c.vars.end() || !it->second.live)
      return;
    auto &r = it->second;
    auto &root = c.vars.at(r.root);
    if (r.access == Access::Shared)
      --root.shared;
    else
      root.unique = false;
    r.live = false;
  }
  void endViews(Context &c) {
    for (auto &[v, root] : c.views)
      --c.vars.at(root).shared;
    c.views.clear();
  }
  void finish(Context &c, Loc l, Inst &site) {
    endViews(c);
    for (auto &[id, r] : c.vars)
      if (r.live && r.root != None)
        endBorrow(c, id);
    for (auto it = c.vars.rbegin(); it != c.vars.rend(); ++it) {
      auto id = it->first;
      auto &r = it->second;
      if (r.live && r.root == None && !symbol(id).receiver)
        drop(c, id, l, true, site);
      if (r.live && symbol(id).receiver && p.semantics.heap(symbol(id).type) &&
          r.allocation == function->releases)
        fail(l, DiagnosticCode::AllocationContract,
             "method body does not satisfy its allocation-retention/release contract");
    }
    for (auto &[v, t] : c.values)
      if (t.available && p.semantics.linear(t.type))
        fail(l, DiagnosticCode::UnconsumedResource,
             "unconsumed linear temporary %" + std::to_string(v));
  }
  void call(Inst &i, Context &c) {
    auto &f = p.semantics.functions[i.function];
    size_t skip = i.slot == None ? 0 : 1;
    if (i.slot != None) {
      auto &r = access(c, i.slot, i.access, i.loc);
      if (i.kind == Inst::ProtocolTransition) {
        auto &q = p.semantics.protocols[i.protocol];
        auto it = std::find_if(q.operations.begin(), q.operations.end(), [&](auto &op) {
          return op.from == r.state &&
                 std::find(f.operations.begin(), f.operations.end(), op.id) != f.operations.end();
        });
        if (it == q.operations.end()) {
          std::string name = f.name.substr(f.name.find("::") + 2);
          std::string m = "operation `" + name +
                          "` is not valid in the current state\n\nresource:\n    " +
                          symbol(i.slot).name + "\ncurrent state:\n    " + q.states[r.state] +
                          "\noperation:\n    " + name + "\navailable in:";
          for (auto &op : q.operations)
            if (op.name == name)
              m += "\n    " + q.states[op.from];
          DiagnosticEngine()
              .error(DiagnosticCode::InvalidProtocolOperation, i.loc, m)
              .label(i.loc, "operation is unavailable in state `" + q.states[r.state] + "`")
              .help("perform a valid protocol transition before this operation")
              .emit();
        }

        if (symbol(i.slot).receiver && i.access == Access::Own)
          fail(i.loc, DiagnosticCode::ProtocolMismatch,
               "owning protocol calls on method self are outside this PoC");
        i.operation = it->id;
        i.from = it->from;
        i.to = it->to;
        i.resourceInput = r.version;
        r.state = it->to;
        r.stateAt = i.loc;
        if (i.access == Access::Own)
          r.version = i.result;
      }
      if (p.semantics.heap(symbol(i.slot).type)) {
        if (!r.allocation)
          fail(i.loc, DiagnosticCode::AllocationContract,
               "operation requires an allocation that was already released");
        if (f.releases)
          r.allocation = false;
      }
    }
    for (size_t n = 0; n < i.args.size(); ++n) {
      auto t = symbol(f.params[n + skip]).type;
      if (p.semantics.linear(t)) {
        auto &v = take(c, i.args[n], i.loc);
        if (v.state != initial(t))
          fail(i.loc, DiagnosticCode::ProtocolReturn,
               "owned resource arguments must be in their initial protocol state");
      }
    }
  }
  void pointerAccess(Context &c, ValueId value, Access requested, Loc loc, bool free = false) {
    auto it = c.pointers.find(value);
    if (it == c.pointers.end()) {
      if (!function->trusted)
        fail(loc, DiagnosticCode::RestrictedOperation,
             "raw memory operation outside runtime bindings");
      return;
    }
    auto &r = access(c, it->second, requested, loc);
    if (!r.allocation)
      fail(loc, DiagnosticCode::AllocationContract, "allocation already released");
    if (free)
      r.allocation = false;
  }
  void block(std::vector<Inst> &ops, Context &c) {
    for (auto &i : ops) {
      if (c.returned)
        break;
      i.cleanup.clear();
      i.freeTemporary = false;
      switch (i.kind) {
      case Inst::EndViews:
        endViews(c);
        break;
      case Inst::TextPrint:
        access(c, i.slot, Access::Shared, i.loc);
        break;
      case Inst::InputCall:
      case Inst::StringCall: {
        if (i.slot != None) {
          auto &root = access(c, i.slot, i.access, i.loc);
          if (i.intrinsicId == IntrinsicId::String_as_str ||
              i.intrinsicId == IntrinsicId::Input_result_str) {
            auto &source = c.vars.at(i.slot);
            auto rid = source.root == None ? i.slot : source.root;
            ++root.shared;
            root.borrowAt = i.loc;
            c.views[i.result] = rid;
          }
        } else
          c.values[i.result] = {i.type, None, true, true};
        break;
      }
      case Inst::Inspect:
        access(c, i.slot, Access::Shared, i.loc);
        break;
      case Inst::HeapAlloc:
        if (p.semantics.linear(i.type))
          c.values[i.result] = {i.type, initial(i.type), true, true};
        break;
      case Inst::HeapFree:
        pointerAccess(c, i.args[0], Access::Own, i.loc, true);
        break;
      case Inst::HeapLoad:
        pointerAccess(c, i.args[0], Access::Shared, i.loc);
        break;
      case Inst::HeapStore:
        pointerAccess(c, i.args[0], Access::Unique, i.loc);
        break;
      case Inst::MemCopy:
      case Inst::MemMove:
        pointerAccess(c, i.args[0], Access::Unique, i.loc);
        pointerAccess(c, i.args[1], Access::Shared, i.loc);
        break;
      case Inst::EnumConstruct:
      case Inst::Create:
      case Inst::Array:
        c.values[i.result] = {i.type, initial(i.type), true};
        break;
      case Inst::Move: {
        auto &r = access(c, i.slot, Access::Own, i.loc);
        if (symbol(i.slot).receiver)
          fail(i.loc, DiagnosticCode::BorrowConflict, "method self cannot be moved");
        i.resourceInput = r.version;
        c.values[i.result] = {i.type, r.state, true, r.allocation, i.slot};
        c.vars[i.slot].allocation = false;
        c.vars[i.slot].live = false;
        c.vars[i.slot].consumedBy = "move";
        c.vars[i.slot].consumedAt = i.loc;
        break;
      }
      case Inst::Copy:
      case Inst::Read:
        access(c, i.slot, Access::Shared, i.loc);
        if (i.type == Str && c.vars.at(i.slot).root != None) {
          auto root = c.vars.at(i.slot).root;
          ++c.vars.at(root).shared;
          c.views[i.result] = root;
        }
        if (i.type == RawPtr) {
          if (p.semantics.heap(symbol(i.slot).type))
            c.pointers[i.result] = i.slot;
          else if (c.pointerSlots.contains(i.slot))
            c.pointers[i.result] = c.pointerSlots.at(i.slot);
        }
        break;
      case Inst::Write:
        access(c, i.slot, Access::Unique, i.loc);
        break;
      case Inst::Bind: {
        StateId state = None;
        bool allocation = false;
        if (p.semantics.linear(i.type)) {
          auto &temp = take(c, i.args[0], i.loc);
          state = temp.state;
          allocation = temp.allocation;
          if (temp.movedFrom != None)
            c.vars[temp.movedFrom].movedTo = i.slot;
        }
        c.vars[i.slot] = {true,  state,     Access::Own, None, 0,
                          false, i.args[0], allocation,  "",   None};
        if (i.type == Str && c.views.contains(i.args[0])) {
          c.vars[i.slot].root = c.views.at(i.args[0]);
          c.vars[i.slot].access = Access::Shared;
          c.vars[i.slot].borrowAt = c.vars.at(c.vars[i.slot].root).borrowAt;
          c.views.erase(i.args[0]);
        }
        if (i.type == RawPtr && c.pointers.contains(i.args[0]))
          c.pointerSlots[i.slot] = c.pointers.at(i.args[0]);
        break;
      }
      case Inst::Drop:
        drop(c, i.slot, i.loc, false, i);
        break;
      case Inst::Discard:
        if (p.semantics.linear(i.type)) {
          auto &t = take(c, i.args[0], i.loc);
          final(t.type, t.state, "temporary", i.loc);
          i.freeTemporary = t.allocation;
        }
        break;
      case Inst::BorrowShared:
      case Inst::BorrowUnique: {
        auto &source = live(c, i.slot, i.loc);
        bool shared = i.kind == Inst::BorrowShared;

        if (source.root != None && (!shared || source.access != Access::Shared))
          fail(i.loc, DiagnosticCode::InvalidReborrow, "exclusive reborrowing is outside this PoC");
        auto &root = access(c, i.slot, shared ? Access::Shared : Access::Unique, i.loc);
        SymbolId rid = source.root == None ? i.slot : source.root;
        root.borrowAt = i.loc;
        if (shared)
          ++root.shared;
        else
          root.unique = true;
        c.vars[i.offset] = {true,         root.state, shared ? Access::Shared : Access::Unique,
                            rid,          0,          false,
                            root.version, false,      "",
                            None};
        c.vars[i.offset].borrowAt = i.loc;
        break;
      }
      case Inst::EndBorrow:
        endBorrow(c, i.slot);
        break;
      case Inst::Call:
      case Inst::ProtocolTransition:
        call(i, c);
        break;
      case Inst::ScopeEnd:
        for (auto it = i.locals.rbegin(); it != i.locals.rend(); ++it)
          drop(c, *it, i.loc, true, i);
        for (auto id : i.locals)
          c.vars.erase(id);
        break;
      case Inst::Return:
        finish(c, i.loc, i);
        c.returned = true;
        break;
      case Inst::Break:
      case Inst::Continue: {
        if (loops.empty() || loops.back().id != i.loop)
          fail(i.loc, DiagnosticCode::InvalidCore, "invalid loop target in Core");
        auto &entry = loops.back().entry;
        for (auto &[id, r] : c.vars)
          if (!entry.vars.contains(id) && r.live && r.root != None)
            endBorrow(c, id);
        for (auto it = c.vars.rbegin(); it != c.vars.rend(); ++it)
          if (!entry.vars.contains(it->first) && it->second.root == None)
            drop(c, it->first, i.loc, true, i);
        for (auto it = c.vars.begin(); it != c.vars.end();) {
          if (!entry.vars.contains(it->first))
            it = c.vars.erase(it);
          else
            ++it;
        }
        if (i.kind == Inst::Continue)
          compatible(entry, c, i.loc, "loop backedge");
        else
          loops.back().exits.push_back(c);
        c.returned = true;
        break;
      }
      case Inst::Loop: {
        block(i.condition, c);
        Context entry = c, body = c;
        loops.push_back({i.loop, entry, {}});
        block(i.body, body);
        if (!body.returned) {
          block(i.latch, body);
          compatible(entry, body, i.loc, "loop backedge");
        }
        auto exits = std::move(loops.back().exits);
        loops.pop_back();
        if (!i.unconditional)
          exits.push_back(entry);
        if (exits.empty())
          c.returned = true;
        else {
          for (auto &edge : exits)
            compatible(exits[0], edge, i.loc, "loop exit");
          c = std::move(exits[0]);
        }
        break;
      }
      case Inst::If: {
        Context then = c, other = c;
        block(i.body, then);
        block(i.otherwise, other);
        if (then.returned && other.returned) {
          c.returned = true;
          break;
        }
        if (then.returned) {
          c = std::move(other);
          break;
        }
        if (other.returned) {
          c = std::move(then);
          break;
        }
        for (auto &[id, r] : c.vars) {
          if (then.vars.at(id) != other.vars.at(id))
            fail(i.loc, DiagnosticCode::BranchJoin,
                 "branches leave resource `" + symbol(id).name +
                     "` with different ownership, borrows or protocol states");
        }
        i.merges.clear();
        for (auto &[id, r] : c.vars) {
          if (p.semantics.linear(symbol(id).type) && then.vars.at(id).live &&
              then.vars.at(id).version != other.vars.at(id).version) {
            auto merged = p.values++;
            i.merges.push_back({id, then.vars.at(id).version, other.vars.at(id).version, merged});
            then.vars.at(id).version = merged;
          }
        }
        c = std::move(then);
        break;
      }
      default:
        break;
      }
    }
  }

public:
  explicit Checker(Program x) : p(std::move(x)) {}
  Program run() {
    for (auto &cf : p.functions) {
      function = &p.semantics.functions[cf.id];
      std::vector<StateId> entries{None};
      if (!function->operations.empty()) {
        entries.clear();
        auto &q = p.semantics.protocols[p.semantics.types[function->owner].protocol];
        for (auto id : function->operations)
          for (auto &op : q.operations)
            if (op.id == id)
              entries.push_back(op.from);
      }
      for (auto entry : entries) {
        Context c;
        for (size_t n = 0; n < function->params.size(); ++n) {
          auto id = function->params[n];
          auto &s = symbol(id);
          c.vars[id] = {true,
                        s.receiver ? entry : initial(s.type),
                        s.access,
                        None,
                        0,
                        false,
                        cf.parameters[n],
                        p.semantics.heap(s.type),
                        "",
                        None};
        }
        block(cf.body, c);
        if (!c.returned) {
          if (function->result != Unit)
            fail(function->loc, DiagnosticCode::MissingReturn, "not all paths return a value");
          fail(function->loc, DiagnosticCode::InvalidCore,
               "missing Core fallthrough cleanup point");
        }
      }
    }
    return std::move(p);
  }
};
void dump(const std::vector<Inst> &ops, const hir::Program &p, std::ostream &o, int d) {
  static const char *names[] = {"constant",      "copy",
                                "read",          "write",
                                "create",        "array",
                                "move",          "bind",
                                "drop",          "borrow.shared",
                                "borrow.unique", "end_borrow",
                                "call",          "protocol.transition",
                                "return",        "if",
                                "scope.end",     "binary",
                                "unary",         "print",
                                "discard",       "heap.alloc",
                                "heap.free",     "heap.load",
                                "heap.store",    "memcpy",
                                "memmove",       "cast",
                                "loop",          "break",
                                "continue",      "enum.construct",
                                "inspect",       "static_str",
                                "string.call",   "print.text",
                                "end_views",     "input.call"};
  for (auto &i : ops) {
    if (i.kind == Inst::Loop)
      dump(i.condition, p, o, d + 2);
    for (auto id : i.cleanup)
      o << std::string(d, ' ') << "drop #v" << id << " DropGlue(#T" << p.symbols[id].type
        << (p.types[p.symbols[id].type].dropGlue == hir::DropGlue::StringAllocation
                ? ") => string.drop (free payload)\n"
            : p.types[p.symbols[id].type].dropGlue == hir::DropGlue::InputAllocation
                ? ") => input.drop (free successful text payload)\n"
                : ") => heap.free\n");
    if (i.freeTemporary)
      o << std::string(d, ' ') << "drop temporary => heap.free\n";
    o << std::string(d, ' ');
    if (i.result != None)
      o << "%" << i.result << " = ";
    o << names[i.kind];
    if (i.slot != None)
      o << " #v" << i.slot << "(" << p.symbols[i.slot].name << ")";
    if (i.function != None)
      o << " #F" << i.function;
    if (!i.text.empty())
      o << " " << (i.kind == Inst::StaticStr ? quoteText(i.text) : i.text);
    if (i.kind == Inst::StringCall)
      o << " memory=" << accessName(i.access)
        << (i.text == "string.as_str" ? " [retain shared view]" : "");
    if (i.kind == Inst::TextPrint)
      o << " [temporary Shared access; no consume]";
    for (auto a : i.args)
      o << " %" << a;
    if (i.loop != None)
      o << " target=#L" << i.loop;
    if (i.kind == Inst::EnumConstruct)
      o << " variant=#V" << i.offset;
    if (i.kind == Inst::Read || i.kind == Inst::Write)
      o << " offset=" << i.offset;
    if (i.kind == Inst::BorrowShared || i.kind == Inst::BorrowUnique)
      o << " as #v" << i.offset;
    if (i.kind == Inst::ProtocolTransition) {
      auto &q = p.protocols[i.protocol];
      o << " #P" << i.protocol << " #O" << i.operation << " memory=" << accessName(i.access)
        << " witness:0";
      if (i.from != None)
        o << " " << q.states[i.from] << " -> " << q.states[i.to];
      if (p.functions[i.function].releases)
        o << " allocation=absent";
      if (i.access == Access::Own)
        o << " [consume %" << i.resourceInput << "; produce %" << i.result << "; rebind #v"
          << i.slot << "]";
      else
        o << " [temporary " << accessName(i.access) << " access to %" << i.resourceInput << "]";
    }
    if (i.kind == Inst::Move)
      o << " [consume %" << i.resourceInput << "; source=Dead]";
    if (i.type != Unit)
      o << " : "
        << (i.quantity == Quantity::One    ? "1"
            : i.quantity == Quantity::Zero ? "0"
                                           : "omega")
        << " " << p.types[i.type].name;
    for (auto l : i.locals)
      o << " drop-if-live #v" << l;
    if (i.kind == Inst::HeapAlloc && p.heap(i.type))
      o << " allocation=present memory=Own";
    o << "\n";
    dump(i.body, p, o, d + 2);
    dump(i.latch, p, o, d + 2);
    if (!i.otherwise.empty()) {
      o << std::string(d, ' ') << "else\n";
      dump(i.otherwise, p, o, d + 2);
    }
    for (auto &m : i.merges)
      o << std::string(d, ' ') << "%" << m.result << " = resource.phi %" << m.left << ", %"
        << m.right << " bind #v" << m.slot << "\n";
  }
}
}
Program elaborate(hir::Program h) {
  return Builder(std::move(h)).run();
}
void checkTypes(const Program &);
Verified verify(Program p) {
  checkTypes(p);
  return Verified(Checker(std::move(p)).run());
}
void print(const Program &p, std::ostream &o) {
  o << "Verified Core (0 / 1 / omega; MemoryState independent of ProtocolState)\n";
  for (auto &f : p.functions) {
    auto &hf = p.semantics.functions[f.id];
    o << "fn #F" << f.id << " " << hf.name << " kind=Normal effects={" << hf.effects.describe()
      << " }\n";
    for (size_t n = 0; n < hf.params.size(); ++n) {
      auto id = hf.params[n];
      auto &s = p.semantics.symbols[id];
      o << "  param %" << f.parameters[n] << " #v" << id << " : "
        << (p.semantics.linear(s.type) ? "1" : "omega") << " " << p.semantics.types[s.type].name
        << "<" << accessName(s.access) << ">\n";
    }
    if (!hf.operations.empty()) {
      const auto &protocol = p.semantics.protocols[p.semantics.types[hf.owner].protocol];
      for (auto id : hf.operations)
        for (const auto &op : protocol.operations)
          if (op.id == id)
            o << "  contract #O" << id << ": 1 " << p.semantics.types[hf.owner].name << "<"
              << accessName(op.access) << " | " << protocol.states[op.from] << "> -> 1 "
              << p.semantics.types[hf.owner].name << "<" << accessName(op.access) << " | "
              << protocol.states[op.to] << ">\n";
    }
    dump(f.body, p.semantics, o, 2);
  }
}
}
