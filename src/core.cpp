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
  std::vector<SymbolId> temporaries;
  std::map<ValueId, SymbolId> referenceResults;
  bool referenceReturn = false;
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
  void borrowSource(const hir::Expr &e, Inst &binding) {
    const auto &source = e.args.at(0);
    if (source.kind == hir::Expr::Call || source.kind == hir::Expr::Method) {
      auto value = expr(source);
      binding.slot = referenceResults.at(value);
      binding.args = {value};
    } else if (source.kind == hir::Expr::Field || source.kind == hir::Expr::Index) {
      binding.slot = source.args.at(0).ref;
      if (source.kind == hir::Expr::Field) {
        binding.placeProjection = Inst::PlaceProjection::Field;
        const auto *part = &source;
        while (part->kind == hir::Expr::Field) {
          const auto &fields = p.semantics.types[part->args[0].type].fields;
          auto field = std::find_if(fields.begin(), fields.end(),
                                    [&](const auto &f) { return f.offset == part->ref; });
          binding.fieldPath.insert(binding.fieldPath.begin(), field - fields.begin());
          part = &part->args[0];
        }
        binding.slot = part->ref;
        binding.fieldIndex = binding.fieldPath.back();
      } else {
        binding.placeProjection = Inst::PlaceProjection::Index;
        if (source.ref != None) {
          auto index = make(Inst::Constant, source.loc, USize);
          index.text = std::to_string(source.ref);
          binding.args = {emit(index)};
        } else
          binding.args = {expr(source.args.at(1))};
      }
    } else
      binding.slot = source.ref;
  }
  SymbolId receiver(const hir::Expr &e) {
    if (e.kind == hir::Expr::Variable)
      return e.ref;
    auto value = e.kind == hir::Expr::Borrow ? None : expr(e);
    Inst source{};
    if (e.kind == hir::Expr::Borrow)
      borrowSource(e, source);
    SymbolId id = p.semantics.symbols.size();
    hir::Symbol symbol{id,   "$temporary" + std::to_string(id), e.type, Access::Own, false, false,
                       e.loc};
    symbol.temporary = true;
    auto binding = make(Inst::Bind, e.loc, e.type);
    binding.slot = id;
    if (e.kind == hir::Expr::Borrow) {
      symbol.reference = true;
      symbol.access = e.borrowKind == BorrowKind::Shared ? Access::Shared : Access::Unique;
      binding.kind = e.borrowKind == BorrowKind::Shared ? Inst::BorrowShared : Inst::BorrowUnique;
      binding.slot = source.slot;
      binding.args = std::move(source.args);
      binding.placeProjection = source.placeProjection;
      binding.fieldIndex = source.fieldIndex;
      binding.fieldPath = std::move(source.fieldPath);
      symbol.region = e.region;
      binding.offset = id;
      binding.region = e.region;
      binding.access = symbol.access;
      binding.quantity = Quantity::One;
    } else
      binding.args = {value};
    p.semantics.symbols.push_back(std::move(symbol));
    emit(binding, false);
    temporaries.push_back(id);
    return id;
  }
  void endTemporaries(Loc loc) {
    for (auto it = temporaries.rbegin(); it != temporaries.rend(); ++it) {
      if (p.semantics.symbols[*it].reference) {
        auto end = make(Inst::EndBorrow, loc);
        end.slot = *it;
        emit(end, false);
      }
    }
    if (!temporaries.empty()) {
      auto end = make(Inst::ScopeEnd, loc);
      end.locals = std::move(temporaries);
      emit(end, false);
      temporaries.clear();
    }
  }
  std::pair<SymbolId, uint32_t> place(const hir::Expr &e) {
    if (e.kind == hir::Expr::Variable)
      return {e.ref, 0};
    if (e.kind != hir::Expr::Field && e.kind != hir::Expr::Index)
      return {receiver(e), 0};
    auto [id, off] = place(e.args[0]);
    return {id, off + e.ref};
  }
  ValueId expr(const hir::Expr &e) {
    using E = hir::Expr;
    Inst i = make(Inst::Constant, e.loc, e.type);
    i.text = e.text;
    i.operatorClass = e.operatorClass;
    switch (e.kind) {
    case E::Match:
    case E::Block: {
      auto instructions = block(e.body);
      auto cleanup = instructions.back();
      instructions.pop_back();
      std::vector<Inst> endReferences;
      for (auto &instruction : instructions) {
        if (instruction.kind == Inst::EndBorrow && instruction.unconditional)
          endReferences.push_back(instruction);
        else
          out->push_back(instruction);
      }
      auto value = expr(e.args.at(0));
      if (e.type != Str)
        emit(make(Inst::EndViews, e.loc), false);
      for (auto &end : endReferences)
        emit(end, false);
      emit(cleanup, false);
      return value;
    }
    case E::Length:
      i.kind = Inst::Length;
      i.slot = receiver(e.args.at(0));
      return emit(i);
    case E::StoreReference:
      i.kind = Inst::StoreReference;
      i.slot = receiver(e.args.at(0));
      i.access = Access::Shared;
      return emit(i);
    case E::Borrow:
      if (scalar(e.type)) {
        i.kind = Inst::Copy;
        i.slot = receiver(e);
        return emit(i);
      }
      fail(e.loc, DiagnosticCode::InvalidCore, "unbound BorrowExpr in Core elaboration");
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
      if (e.kind == E::Index && e.ref == None) {
        i.kind = Inst::Read;
        i.slot = receiver(e.args.at(0));
        i.args = {expr(e.args.at(1))};
        return emit(i);
      }
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
        auto parentTemporaries = std::move(temporaries);
        temporaries.clear();
        std::vector<Inst> rhs;
        out = &rhs;
        auto value = expr(e.args[1]);
        auto write = make(Inst::Write, e.loc, Bool);
        write.slot = slot;
        write.args = {value};
        emit(write, false);
        endTemporaries(e.loc);
        temporaries = std::move(parentTemporaries);
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
      const auto &target = e.args.at(0);
      if (target.kind == E::Field && p.semantics.ownedFieldType(target.type)) {
        i.kind = Inst::FieldStore;
        i.slot = target.args.at(0).ref;
        i.type = target.type;
        i.quantity = Quantity::One;
        const auto &fields = p.semantics.types[target.args[0].type].fields;
        for (uint32_t n = 0; n < fields.size(); ++n)
          if (fields[n].offset == target.ref)
            i.fieldIndex = n;
        i.args = {expr(e.args.at(1))};
        emit(i, false);
        return None;
      }
      if (target.kind == E::Index && target.ref == None) {
        i.kind = Inst::Write;
        i.slot = receiver(target.args.at(0));
        auto index = expr(target.args.at(1));
        i.type = target.type;
        i.args = {expr(e.args.at(1)), index};
        emit(i, false);
        return None;
      }
      auto [id, off] = place(e.args[0]);
      i.kind = Inst::Write;
      i.slot = id;
      i.offset = off;
      i.type = e.args[0].type;
      i.args = {expr(e.args[1])};
      emit(i, false);
      return None;
    }
    case E::PayloadTake:
      i.kind = Inst::PayloadTake;
      i.slot = e.args.at(0).ref;
      i.offset = e.ref;
      return emit(i);
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
    case E::Drop:
      if (e.args[0].kind == E::Field && p.semantics.ownedFieldType(e.args[0].type)) {
        const auto &field = e.args[0];
        i.kind = e.kind == E::Move ? Inst::FieldTake : Inst::FieldDrop;
        i.slot = field.args.at(0).ref;
        i.type = field.type;
        i.quantity = Quantity::One;
        const auto &fields = p.semantics.types[field.args[0].type].fields;
        for (uint32_t n = 0; n < fields.size(); ++n)
          if (fields[n].offset == field.ref)
            i.fieldIndex = n;
        return emit(i, e.kind == E::Move);
      }
      if (e.kind == E::Drop) {
        i.kind = Inst::Drop;
        i.slot = e.args[0].ref;
        emit(i, false);
        return None;
      }
      i.kind = p.semantics.linear(e.type) ? Inst::Move : Inst::Copy;
      i.slot = e.args[0].ref;
      return emit(i);
    case E::Print:
      if (p.semantics.types[e.args[0].type].representation == hir::Representation::StringRecord) {
        i.kind = Inst::TextPrint;
        i.slot = receiver(e.args[0]);
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
        i.slot = receiver(e.args[0]);
        i.access = f.access;
        start = 1;
        if (!f.operations.empty()) {
          i.kind = Inst::ProtocolTransition;
          i.protocol = p.semantics.types[f.owner].protocol;
          i.quantity = e.type == Unit ? Quantity::Zero : Quantity::Omega;
        }
      }
      for (size_t n = start; n < e.args.size(); ++n) {
        if (p.semantics.symbols[f.params[n]].reference) {
          auto argument = make(Inst::ReferenceArgument, e.args[n].loc, e.args[n].type);
          argument.slot = receiver(e.args[n]);
          argument.access = p.semantics.symbols[f.params[n]].access;
          auto value = emit(argument);
          referenceResults[value] = argument.slot;
          i.args.push_back(value);
        } else
          i.args.push_back(expr(e.args[n]));
      }
      if (!f.intrinsic.empty()) {
        i.kind = intrinsicInfo(f.intrinsicId).lowering == IntrinsicLowering::ContainerStorage
                     ? Inst::ContainerCall
                 : intrinsicInfo(f.intrinsicId).lowering == IntrinsicLowering::StringRecord
                     ? Inst::StringCall
                 : intrinsicInfo(f.intrinsicId).lowering == IntrinsicLowering::InputRecord
                     ? Inst::InputCall
                 : intrinsicInfo(f.intrinsicId).lowering == IntrinsicLowering::ExternalCall
                     ? Inst::Call
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
      auto result = emit(i, e.type != Unit || i.kind == Inst::ProtocolTransition);
      if (f.returnsReference()) {
        auto index = std::find(f.params.begin(), f.params.end(), f.resultOrigin) - f.params.begin();
        referenceResults[result] = i.slot != None && index == 0
                                       ? i.slot
                                       : referenceResults.at(i.args.at(index - (i.slot != None)));
      }
      return result;
    }
    }
    fail(e.loc, DiagnosticCode::InternalError, "unhandled HIR expression");
  }
  std::vector<Inst> block(const std::vector<hir::Stmt> &ss) {
    auto enclosingTemporaries = std::move(temporaries);
    temporaries.clear();
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
        if (referenceReturn) {
          auto reference = make(Inst::ReferenceArgument, s.expr.loc, s.expr.type);
          reference.slot = receiver(s.expr);
          reference.access = Access::Shared;
          i.args = {emit(reference)};
        } else if (s.expr.type != Unit)
          i.args = {expr(s.expr)};
        emit(i, false);
        break;
      case hir::Stmt::If:
        i.kind = Inst::If;
        i.args = {expr(s.expr)};
        emit(make(Inst::EndViews, s.loc), false);
        endTemporaries(s.loc);
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
          auto body = block(arm.body);
          if (arm.variant != None) {
            auto fact = make(Inst::RefineVariant, arm.loc);
            fact.slot = s.symbol;
            fact.offset = arm.variant;
            body.insert(body.begin(), fact);
          }
          if (arm.catchall || n + 1 == s.body.size())
            return body;
          std::vector<Inst> branch;
          auto savedOut = out;
          out = &branch;
          Inst cond = make(Inst::If, arm.loc);
          cond.args = {expr(arm.expr)};
          cond.body = std::move(body);
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
          endTemporaries(s.loc);
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
      case hir::Stmt::BorrowEnd:
        i.kind = Inst::EndBorrow;
        i.quantity = p.semantics.linear(i.type) ? Quantity::One : Quantity::Omega;
        i.slot = s.symbol;
        i.unconditional = s.unconditional;
        emit(i, false);
        break;
      case hir::Stmt::BorrowStart:
      case hir::Stmt::Borrow:
        i.kind = s.access == Access::Shared ? Inst::BorrowShared : Inst::BorrowUnique;
        borrowSource(s.expr, i);
        i.region = s.expr.region;
        i.projection = s.expr.ref == None ? 0 : s.expr.ref;
        i.offset = s.symbol;
        i.type = s.expr.type;
        i.quantity = Quantity::One;
        i.access = s.access;
        emit(i, false);
        if (s.kind == hir::Stmt::BorrowStart)
          break;
        {
          auto b = block(s.body);
          out->insert(out->end(), b.begin(), b.end());
        }
        i.kind = Inst::EndBorrow;
        i.quantity = p.semantics.linear(i.type) ? Quantity::One : Quantity::Omega;
        i.slot = s.symbol;
        emit(i, false);
        break;
      }
      emit(make(Inst::EndViews, s.loc), false);
      endTemporaries(s.loc);
    }
    Inst viewsEnd = make(Inst::EndViews, ss.empty() ? Loc{} : ss.back().loc);
    emit(viewsEnd, false);
    Inst end = make(Inst::ScopeEnd, ss.empty() ? Loc{} : ss.back().loc);
    end.locals = std::move(locals);
    emit(end, false);
    out = saved;
    temporaries = std::move(enclosingTemporaries);
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
      referenceReturn = f.returnsReference();
      referenceResults.clear();
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
  std::set<uint32_t> movedPayloads = {};
  std::map<uint32_t, std::set<SymbolId>> loans = {};

  std::set<SymbolId> extraRoots = {};
  std::set<uint32_t> absentFields = {};
  std::map<uint32_t, OwnedFieldState> fields = {};
  std::vector<uint32_t> ownedProjection = {};
  SymbolId loanParent = None;
  VariantId knownVariant = None;
  MemoryState memory() const {
    return !live                      ? MemoryState::Dead
           : access == Access::Shared ? MemoryState::Shared
           : access == Access::Unique ? MemoryState::Unique
                                      : MemoryState::Own;
  }
  bool operator==(const Resource &other) const {
    return memory() == other.memory() && state == other.state && root == other.root &&
           ownedProjection == other.ownedProjection && loanParent == other.loanParent &&
           extraRoots == other.extraRoots && shared == other.shared && unique == other.unique &&
           allocation == other.allocation &&
           (!live || (fields == other.fields && absentFields == other.absentFields &&
                      movedPayloads == other.movedPayloads && loans == other.loans &&
                      knownVariant == other.knownVariant));
  }
};
struct Temporary {
  TypeId type = Unit;
  StateId state = None;
  bool available = true;
  bool allocation = false;
  SymbolId movedFrom = None;
  std::map<uint32_t, std::set<SymbolId>> loans = {};
  std::set<uint32_t> absentFields = {};
  std::map<uint32_t, OwnedFieldState> fields = {};
};
struct VariantTest {
  SymbolId symbol;
  ValueId version;
  VariantId variant;
};
struct Context {
  std::map<ValueId, VariantId> constructedVariants;

  std::map<SymbolId, std::set<VariantId>> variantSets;
  std::map<ValueId, std::set<VariantId>> valueVariantSets;
  std::map<ValueId, std::pair<SymbolId, ValueId>> tagSources;
  std::map<ValueId, VariantTest> variantTests;
  std::map<ValueId, int64_t> constants, tags;
  std::map<ValueId, bool> conditions;

  std::map<SymbolId, VariantId> variants;
  std::map<SymbolId, Resource> vars;
  std::map<ValueId, Temporary> values;
  std::map<ValueId, SymbolId> pointers;
  std::map<SymbolId, SymbolId> pointerSlots;
  std::map<ValueId, SymbolId> references;
  std::map<ValueId, std::set<SymbolId>> views;
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
  std::map<FunctionId, core::Function *> definitions;
  std::set<FunctionId> checked, checking;
  bool hasFieldContract(const hir::Function &f) {
    for (auto id : f.params) {
      const auto &s = symbol(id);
      if ((s.reference || (s.receiver && !f.consumesSelf)) &&
          !p.semantics.ownedFields(s.type).empty())
        return true;
    }
    return false;
  }
  void recordFieldPostStates(Context &c, Loc loc) {
    auto &summary = definitions.at(function->id)->fieldPostStates;
    for (auto id : function->params) {
      const auto &s = symbol(id);
      if (!(s.reference || (s.receiver && !function->consumesSelf)) ||
          p.semantics.ownedFields(s.type).empty())
        continue;
      auto &r = live(c, id, loc);
      if (!r.absentFields.empty())
        DiagnosticEngine()
            .error(DiagnosticCode::BorrowConflict, loc,
                   "borrowed struct must have all owned fields initialized before return")
            .secondary(s.loc, "caller retains ownership of this struct")
            .help("restore every extracted field before returning")
            .emit();
      completeFields(s.type, r.fields, loc);
      if (s.access == Access::Shared && r.fields != initialFields(s.type))
        fail(loc, DiagnosticCode::SharedMutation,
             "shared field contract cannot change owned fields");
      auto found = std::find_if(summary.begin(), summary.end(),
                                [&](const auto &state) { return state.parameter == id; });
      if (found == summary.end())
        summary.push_back({id, r.fields, loc});
      else if (found->fields != r.fields)
        DiagnosticEngine()
            .error(DiagnosticCode::ProtocolReturn, loc,
                   "incompatible owned-field post-states between return paths")
            .secondary(found->origin, "different field post-state returned here")
            .help("return the same field states and allocation responsibilities on every path")
            .emit();
    }
  }
  void applyFieldPostStates(Inst &i, Context &c) {
    if (!definitions.contains(i.function))
      return;
    const auto &f = p.semantics.functions[i.function];
    const size_t skip = i.slot == None ? 0 : 1;
    for (const auto &post : definitions.at(i.function)->fieldPostStates) {
      auto pos = std::find(f.params.begin(), f.params.end(), post.parameter) - f.params.begin();
      SymbolId actual =
          i.slot != None && pos == 0 ? i.slot : c.references.at(i.args.at(pos - skip));
      auto &fields = placeFields(c, actual, i.loc);
      fields = post.fields;
      for (auto &[index, state] : fields)
        state.origin = i.loc;
    }
  }
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
      if (it != c.vars.end() && it->second.movedTo != None &&
          !symbol(it->second.movedTo).name.starts_with("$"))
        d.help("use `" + symbol(it->second.movedTo).name + "`, or borrow `" + symbol(id).name +
               "` instead of moving it");
      else
        d.help("borrow the value if ownership transfer is not required");
      d.emit();
    }
    return it->second;
  }
  std::set<SymbolId> origins(Context &c, SymbolId id, Loc loc) {
    const auto &r = live(c, id, loc);
    auto result = r.extraRoots;
    result.insert(r.root == None ? id : r.root);
    return result;
  }
  void retainOrigins(Context &c, const std::set<SymbolId> &owners, Loc loc) {
    for (auto owner : owners) {
      auto &r = live(c, owner, loc);
      ++r.shared;
      r.borrowAt = loc;
    }
  }
  void releaseOrigins(Context &c, const std::set<SymbolId> &owners, Loc loc) {
    for (auto owner : owners) {
      auto &r = live(c, owner, loc);
      if (!r.shared)
        fail(loc, DiagnosticCode::InvalidCore, "unbalanced shared provenance carrier");
      --r.shared;
    }
  }
  void setAliasOrigins(Resource &alias, const std::set<SymbolId> &owners) {
    alias.root = *owners.begin();
    alias.extraRoots = owners;
    alias.extraRoots.erase(alias.root);
  }
  void complete(const Resource &resource, Loc loc) {
    if (!resource.absentFields.empty())
      fail(loc, DiagnosticCode::ConsumedResource,
           "struct has uninitialized or moved owned fields; initialize them before moving or "
           "borrowing the whole value");
  }
  std::map<uint32_t, OwnedFieldState> initialFields(TypeId type) {
    std::map<uint32_t, OwnedFieldState> fields;
    for (auto index : p.semantics.ownedFields(type)) {
      auto t = p.semantics.types[type].fields[index].type;
      fields[index] = {initial(t), p.semantics.needsDrop(t), initialFields(t), {}};
    }
    return fields;
  }
  OwnedFieldState *projectedState(Context &c, SymbolId id, Loc loc) {
    const auto &alias = live(c, id, loc);
    auto &owner = alias.root == None ? live(c, id, loc) : live(c, alias.root, loc);
    auto *fields = &owner.fields;
    OwnedFieldState *state = nullptr;
    for (auto index : alias.ownedProjection) {
      if (!fields->contains(index))
        fail(loc, DiagnosticCode::ConsumedResource, "cannot access an absent owned projection");
      state = &fields->at(index);
      fields = &state->fields;
    }
    return state;
  }
  std::map<uint32_t, OwnedFieldState> &placeFields(Context &c, SymbolId id, Loc loc) {
    if (auto *state = projectedState(c, id, loc))
      return state->fields;
    auto &alias = live(c, id, loc);
    return c.vars.at(alias.root == None ? id : alias.root).fields;
  }
  std::set<uint32_t> missingFields(TypeId type, const std::map<uint32_t, OwnedFieldState> &fields) {
    auto missing = p.semantics.ownedFields(type);
    for (const auto &[index, state] : fields)
      missing.erase(index);
    return missing;
  }
  void completeFields(TypeId type, const std::map<uint32_t, OwnedFieldState> &fields, Loc loc) {
    if (!missingFields(type, fields).empty())
      fail(loc, DiagnosticCode::BorrowConflict,
           "borrowed aggregate must restore all extracted owned fields before return or end of "
           "borrow");
    for (const auto &[index, field] : fields)
      completeFields(p.semantics.types[type].fields.at(index).type, field.fields, loc);
  }
  void initialFieldContract(TypeId type, const std::map<uint32_t, OwnedFieldState> &fields,
                            Loc loc) {
    for (auto index : p.semantics.ownedFields(type)) {
      if (!fields.contains(index))
        fail(loc, DiagnosticCode::InvalidCore, "incomplete owned-field input contract");
      const auto &value = fields.at(index);
      auto t = p.semantics.types[type].fields.at(index).type;
      if (value.state != initial(t) || value.allocation != p.semantics.needsDrop(t))
        fail(loc, DiagnosticCode::ProtocolReturn,
             "owned field `" + p.semantics.types[type].fields[index].name +
                 "` does not satisfy the function's initial-state contract");
      initialFieldContract(t, value.fields, loc);
    }
  }
  FieldDropPlan fieldDropPlan(TypeId type, const std::map<uint32_t, OwnedFieldState> &fields,
                              const std::set<uint32_t> &absent, const std::string &name, Loc loc) {
    FieldDropPlan plan;
    for (auto index : p.semantics.ownedFields(type)) {
      if (absent.contains(index)) {
        plan.fields[index].skip = true;
        continue;
      }
      if (!fields.contains(index))
        fail(loc, DiagnosticCode::InvalidCore, "missing owned-field state");
      const auto &value = fields.at(index);
      const auto &field = p.semantics.types[type].fields[index];
      final(field.type, value.state, name + "." + field.name, loc, value.origin);
      auto child = fieldDropPlan(field.type, value.fields, {}, name + "." + field.name, loc);
      child.skip = !value.allocation;
      plan.fields[index] = std::move(child);
    }
    return plan;
  }
  Resource &access(Context &c, SymbolId id, Access requested, Loc l) {
    auto &r = live(c, id, l);
    for (auto owner : r.extraRoots)
      live(c, owner, l);
    auto &root = r.root == None ? r : live(c, r.root, l);
    if (r.root != None || symbol(id).reference) {
      if (r.unique || (requested != Access::Shared && r.shared))
        fail(l, DiagnosticCode::InvalidReborrow,
             "reference parameter is unavailable while a derived borrow is active");
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
        Loc lastUse;
        for (auto &[alias, resource] : c.vars)
          if (resource.live && (resource.root == id || resource.extraRoots.contains(id))) {
            activeOrigin = resource.borrowAt;
            lastUse = symbol(alias).lastUse;
            break;
          }
        auto diagnostic =
            DiagnosticEngine()
                .error(DiagnosticCode::InvalidReborrow, l,
                       reason + " for `" + symbol(id).name + "`")
                .label(l, "incompatible access requested here")
                .secondary(activeOrigin, "active borrow starts here")
                .note("unique access requires exclusive access to the resource")
                .help("end the active borrow before moving, dropping or mutating this resource");
        for (const auto &[container, state] : c.vars)
          if (state.live && state.root == None)
            for (const auto &[offset, owner] : state.loans) {
              (void)offset;
              if (owner.contains(id))
                diagnostic.secondary(symbol(container).loc,
                                     "this container retains a shared reference to the resource");
            }
        if (lastUse.span.file != NoFile)
          diagnostic.secondary(lastUse, "reference last used in this statement");
        diagnostic.emit();
      }
    }
    if (r.access == Access::Shared && requested != Access::Shared)
      fail(l, DiagnosticCode::SharedMutation,
           "shared self/resource is read-only: `" + symbol(id).name + "`");
    if (r.access == Access::Unique && requested == Access::Own)
      fail(l, DiagnosticCode::BorrowConflict, "unique borrow does not grant ownership");
    return root;
  }
  void forgetVariantSets(Context &c, SymbolId root) {
    c.variantSets.erase(root);
    for (auto it = c.tagSources.begin(); it != c.tagSources.end();)
      if (it->second.first == root)
        it = c.tagSources.erase(it);
      else
        ++it;
    for (auto it = c.variantTests.begin(); it != c.variantTests.end();)
      if (it->second.symbol == root)
        it = c.variantTests.erase(it);
      else
        ++it;
  }
  void mergeVariantSets(Context &into, const Context &other) {
    for (auto it = into.variantSets.begin(); it != into.variantSets.end();) {
      auto found = other.variantSets.find(it->first);
      if (found == other.variantSets.end())
        it = into.variantSets.erase(it);
      else {
        it->second.insert(found->second.begin(), found->second.end());
        ++it;
      }
    }
  }
  void refineTest(Context &c, ValueId condition, bool selected, Loc loc) {
    auto found = c.variantTests.find(condition);
    if (found == c.variantTests.end())
      return;
    const auto &test = found->second;
    auto resource = c.vars.find(test.symbol);
    if (resource == c.vars.end() || !resource->second.live ||
        resource->second.version != test.version)
      return;
    if (!c.variantSets.contains(test.symbol)) {
      for (const auto &variant : p.semantics.types[symbol(test.symbol).type].variants)
        c.variantSets[test.symbol].insert(variant.id);
    }
    auto &variants = c.variantSets.at(test.symbol);
    if (selected) {
      if (!variants.contains(test.variant))
        fail(loc, DiagnosticCode::InvalidCore, "impossible variant branch");
      variants = {test.variant};
    } else
      variants.erase(test.variant);
    if (variants.empty())
      fail(loc, DiagnosticCode::InvalidCore, "empty variant set on a reachable branch");
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
    if (it != c.vars.end() && symbol(id).temporary && it->second.shared)
      DiagnosticEngine()
          .error(DiagnosticCode::EscapingView, l, "borrowed value outlives temporary owner")
          .secondary(symbol(id).loc, "temporary owner created here")
          .secondary(it->second.borrowAt, "borrow created here")
          .help("bind the owner to a local variable before borrowing it")
          .emit();
    auto &r = access(c, id, Access::Own, l);
    if (symbol(id).receiver && !function->consumesSelf) {
      if (implicit)
        return;
      fail(l, DiagnosticCode::BorrowConflict,
           "method self cannot be dropped or moved; it is returned by the protocol transition");
    }
    final(symbol(id).type, r.state, symbol(id).name, l, r.stateAt);
    site.fieldCleanup[id] =
        fieldDropPlan(symbol(id).type, r.fields, r.absentFields, symbol(id).name, l);
    if (r.allocation) {
      site.cleanup.push_back(id);
      site.movedPayloads[id] = {r.movedPayloads.begin(), r.movedPayloads.end()};
      site.absentFields[id] = {r.absentFields.begin(), r.absentFields.end()};
    }
    for (auto [offset, owner] : r.loans) {
      (void)offset;
      releaseOrigins(c, owner, l);
    }
    r.loans.clear();
    r.allocation = false;
    c.vars[id].live = false;
    c.vars[id].consumedBy = "drop";
    c.vars[id].consumedAt = l;
    c.variants.erase(id);
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
    for (auto &[child, loan] : c.vars)
      if (loan.live && loan.loanParent == id)
        endBorrow(c, child);
    auto &root = c.vars.at(r.root);
    if (r.access == Access::Shared) {
      releaseOrigins(c, origins(c, id, {}), {});
      if (r.loanParent != None)
        --c.vars.at(r.loanParent).shared;
    } else {
      completeFields(symbol(id).type, placeFields(c, id, r.borrowAt), r.borrowAt);
      if (!missingFields(symbol(id).type, placeFields(c, id, r.borrowAt)).empty())
        fail(
            r.borrowAt, DiagnosticCode::BorrowConflict,
            "unique borrow cannot end while owned fields are absent; restore the extracted fields");
      if (r.loanParent != None)
        c.vars.at(r.loanParent).unique = false;
      else
        root.unique = false;
    }
    r.live = false;
  }
  void endViews(Context &c) {
    for (auto &[v, owners] : c.views)
      releaseOrigins(c, owners, {});
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
      if (r.live && r.root == None && (!symbol(id).receiver || function->consumesSelf) &&
          !symbol(id).reference)
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
  std::set<SymbolId> call(Inst &i, Context &c) {
    std::set<SymbolId> payloadResultOrigin;
    auto &f = p.semantics.functions[i.function];
    if (hasFieldContract(f))
      checkFunction(f.id);
    size_t skip = i.slot == None ? 0 : 1;
    if (i.slot != None) {
      auto &r = access(c, i.slot, i.access, i.loc);
      const auto &receiver = live(c, i.slot, i.loc);
      const auto *field = projectedState(c, i.slot, i.loc);
      if (!field)
        complete(r, i.loc);
      auto &fields = placeFields(c, i.slot, i.loc);
      if (!missingFields(symbol(i.slot).type, fields).empty())
        fail(i.loc, DiagnosticCode::ConsumedResource, "method receiver has absent owned fields");
      initialFieldContract(symbol(i.slot).type, fields, i.loc);
      auto receiverState = field ? field->state : r.state;
      auto receiverAllocation = field ? field->allocation : r.allocation;
      auto receiverName = symbol(i.slot).name;
      if (field) {
        auto type = symbol(receiver.root).type;
        receiverName = symbol(receiver.root).name;
        for (auto index : receiver.ownedProjection) {
          const auto &member = p.semantics.types[type].fields.at(index);
          receiverName += "." + member.name;
          type = member.type;
        }
      }
      if (i.kind == Inst::ProtocolTransition) {
        auto &q = p.semantics.protocols[i.protocol];
        auto it = std::find_if(q.operations.begin(), q.operations.end(), [&](auto &op) {
          return op.from == receiverState &&
                 std::find(f.operations.begin(), f.operations.end(), op.id) != f.operations.end();
        });
        if (it == q.operations.end()) {
          std::string name = f.name.substr(f.name.find("::") + 2);
          std::string m = "operation `" + name +
                          "` is not valid in the current state\n\nresource:\n    " + receiverName +
                          "\ncurrent state:\n    " + q.states[receiverState] +
                          "\noperation:\n    " + name + "\navailable in:";
          for (auto &op : q.operations)
            if (op.name == name)
              m += "\n    " + q.states[op.from];
          DiagnosticEngine()
              .error(DiagnosticCode::InvalidProtocolOperation, i.loc, m)
              .label(i.loc, "operation is unavailable in state `" + q.states[receiverState] + "`")
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
        if (field) {
          if (i.access == Access::Own || f.releases || it->from != it->to)
            fail(i.loc, DiagnosticCode::BorrowConflict,
                 "borrowed field protocol operations must preserve state and allocation");
        } else {
          r.state = it->to;
          r.stateAt = i.loc;
        }
        if (i.access == Access::Own)
          r.version = i.result;
      }
      if (p.semantics.heap(symbol(i.slot).type)) {
        if (!receiverAllocation)
          fail(i.loc, DiagnosticCode::AllocationContract,
               "operation requires an allocation that was already released");
        if (f.releases) {
          if (field)
            fail(i.loc, DiagnosticCode::BorrowConflict,
                 "a borrowed field cannot release its allocation");
          r.allocation = false;
        }
      }
    }
    if (i.slot != None && i.access == Access::Unique)
      forgetVariantSets(c, c.vars.at(i.slot).root == None ? i.slot : c.vars.at(i.slot).root);
    std::map<SymbolId, Access> argumentRoots;
    if (i.slot != None) {
      for (auto root : origins(c, i.slot, i.loc))
        argumentRoots[root] = i.access;
    }
    for (size_t n = 0; n < i.args.size(); ++n) {
      const auto &parameter = symbol(f.params[n + skip]);
      auto t = parameter.type;
      if (parameter.reference) {
        auto found = c.references.find(i.args[n]);
        if (found == c.references.end())
          fail(i.loc, DiagnosticCode::InvalidCore, "missing reference argument region");
        access(c, found->second, parameter.access, i.loc);
        initialFieldContract(parameter.type, placeFields(c, found->second, i.loc), i.loc);
        for (auto root : origins(c, found->second, i.loc)) {
          if (argumentRoots.contains(root) &&
              (parameter.access == Access::Unique || argumentRoots.at(root) == Access::Unique))
            fail(i.loc, DiagnosticCode::InvalidReborrow,
                 "overlapping mutable reference arguments in the same call");
          argumentRoots[root] = parameter.access;
          if (parameter.access == Access::Unique)
            forgetVariantSets(c, root);
        }
      } else if (p.semantics.linear(t)) {
        auto &v = take(c, i.args[n], i.loc);
        if (!v.absentFields.empty())
          fail(i.loc, DiagnosticCode::ConsumedResource,
               "cannot pass a struct with uninitialized owned fields");
        initialFieldContract(t, v.fields, i.loc);
        if (v.state != initial(t))
          fail(i.loc, DiagnosticCode::ProtocolReturn,
               "owned resource arguments must be in their initial protocol state");
      }
    }
    applyFieldPostStates(i, c);

    for (size_t n = 0; n < i.args.size(); ++n) {
      const auto &parameter = symbol(f.params[n + skip]);
      if (!parameter.reference && p.semantics.containsReferences(parameter.type)) {
        auto &value = c.values.at(i.args[n]);
        for (const auto &source : f.payloadResultSources()) {
          if (source.parameter != parameter.id)
            continue;
          const auto &path = source.path;
          auto offset = p.semantics.payloadOffset(parameter.type, path.variant, path.field);
          if (value.loans.contains(offset)) {
            const auto &owners = value.loans.at(offset);
            payloadResultOrigin.insert(owners.begin(), owners.end());
          } else if (!value.loans.empty())
            fail(i.loc, DiagnosticCode::InvalidCore,
                 "argument loans do not match result source path");
        }
        for (auto [offset, owner] : value.loans)
          releaseOrigins(c, owner, i.loc);
        value.loans.clear();
      }
    }
    if (f.consumesSelf && i.slot != None) {
      auto &owner = access(c, i.slot, Access::Own, i.loc);
      if (!owner.movedPayloads.empty())
        fail(i.loc, DiagnosticCode::ConsumedResource,
             "cannot consume an enum with extracted payloads");
      if (!owner.loans.empty())
        fail(i.loc, DiagnosticCode::EscapingView,
             "consuming reference payloads requires a return-region contract");
      owner.live = false;
      owner.allocation = false;
      owner.consumedBy = "move";
      owner.consumedAt = i.loc;
      c.variants.erase(i.slot);
    }
    return payloadResultOrigin;
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
      auto inferredEnds = std::move(i.inferredEnds);
      i.inferredEnds.clear();
      for (auto alias : inferredEnds) {
        auto found = c.vars.find(alias);
        if (found != c.vars.end() && found->second.live && found->second.root != None) {
          endBorrow(c, alias);
          i.inferredEnds.push_back(alias);
        }
      }
      i.possibleVariants.clear();
      i.storedPayloadReference = false;
      i.cleanup.clear();
      i.movedPayloads.clear();
      i.freeTemporary = false;
      i.replaceField = false;
      i.absentFields.clear();
      i.temporaryAbsentFields.clear();
      i.fieldCleanup.clear();
      i.replacedFieldPlan = {};
      i.temporaryFieldPlan = {};
      if (i.kind == Inst::FieldStore || i.kind == Inst::FieldTake || i.kind == Inst::FieldDrop) {
        i.protocol = None;
        i.from = i.to = None;
      }
      switch (i.kind) {
      case Inst::Constant:
        if (i.type == I64)
          c.constants[i.result] = std::stoll(i.text);
        break;
      case Inst::Binary:
        if (i.text == "==" && c.constants.contains(i.args[1])) {
          if (c.tags.contains(i.args[0]))
            c.conditions[i.result] = c.tags.at(i.args[0]) == c.constants.at(i.args[1]);
          if (c.tagSources.contains(i.args[0])) {
            auto [id, version] = c.tagSources.at(i.args[0]);
            auto resource = c.vars.find(id);
            if (resource != c.vars.end() && resource->second.live &&
                resource->second.version == version) {
              auto variant = static_cast<VariantId>(c.constants.at(i.args[1]));
              c.variantTests[i.result] = {id, version, variant};
              if (c.variantSets.contains(id)) {
                const auto &possible = c.variantSets.at(id);
                if (!possible.contains(variant))
                  c.conditions[i.result] = false;
                else if (possible.size() == 1)
                  c.conditions[i.result] = true;
              }
            }
          }
        }
        break;
      case Inst::StoreReference: {
        access(c, i.slot, Access::Shared, i.loc);
        auto owner = origins(c, i.slot, i.loc);
        retainOrigins(c, owner, i.loc);
        c.values[i.result] = {i.type, None, true, false, None, {{0, owner}}};
        break;
      }
      case Inst::ReferenceArgument:
        if (!symbol(i.slot).reference)
          fail(i.loc, DiagnosticCode::InvalidCore, "argument must refer to an active borrow");
        access(c, i.slot, i.access, i.loc);
        c.references[i.result] = i.slot;
        break;
      case Inst::RefineVariant: {
        const auto &resource = access(c, i.slot, Access::Shared, i.loc);
        auto root = c.vars.at(i.slot).root == None ? i.slot : c.vars.at(i.slot).root;
        if ((resource.knownVariant != None && resource.knownVariant != i.offset) ||
            (c.variantSets.contains(root) && !c.variantSets.at(root).contains(i.offset)))
          fail(i.loc, DiagnosticCode::InvalidCore, "variant refinement contradicts flow facts");
        c.variantSets[root] = {i.offset};
        i.possibleVariants = {i.offset};
        c.variants[i.slot] = i.offset;
        break;
      }
      case Inst::EndViews:
        endViews(c);
        break;
      case Inst::Length:
      case Inst::TextPrint:
        access(c, i.slot, Access::Shared, i.loc);
        break;
      case Inst::InputCall:
      case Inst::StringCall: {
        if (i.slot != None) {
          access(c, i.slot, i.access, i.loc);
          if (i.intrinsicId == IntrinsicId::String_as_str ||
              i.intrinsicId == IntrinsicId::Input_result_str) {
            auto owners = origins(c, i.slot, i.loc);
            retainOrigins(c, owners, i.loc);
            c.views[i.result] = std::move(owners);
          }
        }
        if (p.semantics.linear(i.type))
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
      case Inst::PayloadTake: {
        auto &owner = access(c, i.slot, Access::Own, i.loc);
        auto tid = symbol(i.slot).type;
        if (!c.variants.contains(i.slot))
          fail(i.loc, DiagnosticCode::InvalidCore, "payload transfer requires a proven variant");
        auto variant = c.variants.at(i.slot);
        bool found = false;
        const auto &payloads = p.semantics.types[tid].variants.at(variant).payload;
        for (size_t n = 0; n < payloads.size(); ++n)
          if (p.semantics.payloadOffset(tid, variant, n) == i.offset && payloads[n] == i.type)
            found = true;
        if (!found || !owner.movedPayloads.insert(i.offset).second)
          fail(i.loc, DiagnosticCode::ConsumedResource, "enum payload has already been consumed");
        c.values[i.result] = {i.type, initial(i.type), true, p.semantics.needsDrop(i.type)};
        c.values[i.result].fields = initialFields(i.type);
        break;
      }
      case Inst::EnumConstruct: {
        const auto &payloads = p.semantics.types[i.type].variants.at(i.offset).payload;
        std::map<uint32_t, std::set<SymbolId>> loans;
        if (p.semantics.containsReferences(i.type))
          c.constructedVariants[i.result] = i.offset;
        for (size_t n = 0; n < i.args.size(); ++n) {
          auto tid = payloads[n];
          if (!p.semantics.linear(tid))
            continue;
          if (!p.semantics.heap(tid) && p.semantics.containsProtocolOwnedFields(tid))
            fail(i.loc, DiagnosticCode::AllocationContract,
                 "enum payloads with protocolled fields require a payload state contract");
          auto &payload = take(c, i.args[n], i.loc);
          if (!payload.absentFields.empty())
            fail(i.loc, DiagnosticCode::ConsumedResource,
                 "cannot store a struct with uninitialized owned fields");
          if (p.semantics.reference(tid)) {
            if (payload.loans.size() != 1 || !payload.loans.contains(0))
              fail(i.loc, DiagnosticCode::InvalidCore, "stored reference has no origin");
            loans[p.semantics.payloadOffset(i.type, i.offset, n)] = payload.loans.at(0);
          }
          final(tid, payload.state, "enum payload", i.loc);
          if (payload.state != initial(tid) || (p.semantics.heap(tid) && !payload.allocation))
            fail(i.loc, DiagnosticCode::AllocationContract,
                 "enum payload must retain its allocation and be in its initial final protocol "
                 "state");
        }
        c.values[i.result] = {i.type, initial(i.type), true, p.semantics.needsDrop(i.type),
                              None,   std::move(loans)};
        break;
      }
      case Inst::Create:
      case Inst::Array:
        c.values[i.result] = {i.type, initial(i.type), true, p.semantics.needsDrop(i.type)};
        c.values[i.result].absentFields = p.semantics.ownedFields(i.type);
        break;
      case Inst::FieldStore:
      case Inst::FieldTake:
      case Inst::FieldDrop: {
        const auto &binding = symbol(i.slot);
        bool borrowed = binding.reference || (binding.receiver && !function->consumesSelf) ||
                        live(c, i.slot, i.loc).root != None;
        auto &owner =
            access(c, i.slot, i.kind == Inst::FieldStore || borrowed ? Access::Unique : Access::Own,
                   i.loc);
        auto &fields = placeFields(c, i.slot, i.loc);
        auto fieldName = symbol(i.slot).name + "." +
                         p.semantics.types[symbol(i.slot).type].fields[i.fieldIndex].name;
        auto oldPlan = [&] {
          if (!fields.contains(i.fieldIndex))
            fail(i.loc, DiagnosticCode::InvalidCore, "missing field protocol state");
          const auto &old = fields.at(i.fieldIndex);
          final(i.type, old.state, fieldName, i.loc, old.origin);
          auto plan = fieldDropPlan(i.type, old.fields, {}, fieldName, i.loc);
          plan.skip = !old.allocation;
          return plan;
        };
        if (i.kind == Inst::FieldStore) {
          auto &value = take(c, i.args.at(0), i.loc);
          if (!value.loans.empty() || !value.absentFields.empty() ||
              (!value.allocation && !p.semantics.containsProtocolFields(i.type)))
            fail(i.loc, DiagnosticCode::AllocationContract,
                 "owned field requires a fully initialized, non-borrowed owner");
          i.replaceField = fields.contains(i.fieldIndex);
          if (i.replaceField)
            i.replacedFieldPlan = oldPlan();
          if (live(c, i.slot, i.loc).ownedProjection.empty())
            owner.absentFields.erase(i.fieldIndex);
          fields[i.fieldIndex] = {value.state, value.allocation, value.fields, i.loc};
          i.to = value.state;
          i.protocol = p.semantics.types[i.type].protocol;
        } else {
          if (!fields.contains(i.fieldIndex))
            fail(i.loc, DiagnosticCode::ConsumedResource,
                 "owned field is uninitialized or has already been consumed");
          auto state = fields.at(i.fieldIndex);
          i.from = state.state;
          i.protocol = p.semantics.types[i.type].protocol;
          if (i.kind == Inst::FieldTake) {
            c.values[i.result] = {i.type, state.state, true, state.allocation};
            c.values[i.result].fields = std::move(state.fields);
          } else {
            i.replacedFieldPlan = oldPlan();
            i.replaceField = true;
          }
          if (live(c, i.slot, i.loc).ownedProjection.empty())
            owner.absentFields.insert(i.fieldIndex);
          fields.erase(i.fieldIndex);
        }
        break;
      }
      case Inst::Move: {
        c.variants.erase(i.slot);
        auto &r = access(c, i.slot, Access::Own, i.loc);
        complete(r, i.loc);
        if (symbol(i.slot).receiver && !function->consumesSelf)
          fail(i.loc, DiagnosticCode::BorrowConflict, "method self cannot be moved");
        if (!r.movedPayloads.empty())
          fail(i.loc, DiagnosticCode::ConsumedResource,
               "cannot move an enum with consumed payloads");
        i.resourceInput = r.version;
        if (r.knownVariant != None)
          c.constructedVariants[i.result] = r.knownVariant;
        if (c.variantSets.contains(i.slot)) {
          c.valueVariantSets[i.result] = c.variantSets.at(i.slot);
          c.variantSets.erase(i.slot);
        }
        c.values[i.result] = {i.type, r.state, true, r.allocation, i.slot, std::move(r.loans)};
        c.values[i.result].fields = std::move(r.fields);
        r.fields.clear();
        r.loans.clear();
        c.vars[i.slot].allocation = false;
        c.vars[i.slot].live = false;
        c.vars[i.slot].consumedBy = "move";
        c.vars[i.slot].consumedAt = i.loc;
        break;
      }
      case Inst::Copy:
      case Inst::Read: {
        auto &resource = access(c, i.slot, Access::Shared, i.loc);
        if (i.kind == Inst::Read && i.offset == 0 && i.type == I64 &&
            p.semantics.types[symbol(i.slot).type].kind == hir::Type::Enum &&
            resource.knownVariant != None)
          c.tags[i.result] = resource.knownVariant;
        if (i.kind == Inst::Read && i.offset == 0 && i.type == I64 &&
            p.semantics.types[symbol(i.slot).type].kind == hir::Type::Enum) {
          auto root = c.vars.at(i.slot).root == None ? i.slot : c.vars.at(i.slot).root;
          c.tagSources[i.result] = {root, resource.version};
        }
        if (i.type == Str && c.vars.at(i.slot).root != None) {
          auto owners = origins(c, i.slot, i.loc);
          retainOrigins(c, owners, i.loc);
          c.views[i.result] = std::move(owners);
        }
        if (i.type == RawPtr) {
          if (p.semantics.heap(symbol(i.slot).type))
            c.pointers[i.result] = i.slot;
          else if (c.pointerSlots.contains(i.slot))
            c.pointers[i.result] = c.pointerSlots.at(i.slot);
        }
        break;
      }
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
        c.variantSets.erase(i.slot);
        if (c.valueVariantSets.contains(i.args[0]))
          c.variantSets[i.slot] = c.valueVariantSets.at(i.args[0]);
        if (c.constructedVariants.contains(i.args[0])) {
          c.vars[i.slot].knownVariant = c.constructedVariants.at(i.args[0]);
          c.variantSets[i.slot] = {c.constructedVariants.at(i.args[0])};
        }
        if (p.semantics.linear(i.type)) {
          c.vars[i.slot].loans = c.values.at(i.args[0]).loans;
          c.vars[i.slot].absentFields = c.values.at(i.args[0]).absentFields;
          c.vars[i.slot].fields = c.values.at(i.args[0]).fields;
        }
        if (i.type == Str && c.views.contains(i.args[0])) {
          setAliasOrigins(c.vars[i.slot], c.views.at(i.args[0]));
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
          i.temporaryFieldPlan =
              fieldDropPlan(t.type, t.fields, t.absentFields, "temporary", i.loc);
          i.freeTemporary = t.allocation;
          i.temporaryAbsentFields = {t.absentFields.begin(), t.absentFields.end()};
          for (auto [offset, owner] : t.loans) {
            (void)offset;
            releaseOrigins(c, owner, i.loc);
          }
          t.loans.clear();
        }
        break;
      case Inst::BorrowShared:
      case Inst::BorrowUnique: {
        if (i.placeProjection == Inst::PlaceProjection::None && !i.args.empty() &&
            (!c.references.contains(i.args[0]) || c.references.at(i.args[0]) != i.slot))
          fail(i.loc, DiagnosticCode::InvalidCore,
               "returned borrow origin does not match call contract");
        if (i.projection) {
          if (!c.variants.contains(i.slot))
            fail(i.loc, DiagnosticCode::InvalidCore, "payload borrow requires a proven variant");
          auto tid = symbol(i.slot).type;
          auto variant = c.variants.at(i.slot);
          const auto &payloads = p.semantics.types[tid].variants.at(variant).payload;
          bool found = false, stored = false;
          for (size_t n = 0; n < payloads.size(); ++n)
            if (p.semantics.payloadOffset(tid, variant, n) == i.projection) {
              stored = p.semantics.reference(payloads[n]) &&
                       p.semantics.types[payloads[n]].element == i.type;
              found = stored || payloads[n] == i.type;
            }
          if (stored) {
            i.storedPayloadReference = true;
            access(c, i.slot, Access::Shared, i.loc);
            const auto &container = c.vars.at(i.slot);
            const auto &loans =
                container.root == None ? container.loans : c.vars.at(container.root).loans;
            if (i.kind != Inst::BorrowShared || !loans.contains(i.projection))
              fail(i.loc, DiagnosticCode::InvalidCore, "missing stored-reference origin");
            auto owners = loans.at(i.projection);
            if (owners.empty())
              fail(i.loc, DiagnosticCode::InvalidCore, "empty stored-reference origins");
            auto owner = *owners.begin();
            auto &root = live(c, owner, i.loc);
            retainOrigins(c, owners, i.loc);
            c.vars[i.offset] = {true,  root.state,   Access::Shared, owner, 0,
                                false, root.version, false,          "",    None};
            setAliasOrigins(c.vars[i.offset], owners);
            c.vars[i.offset].borrowAt = i.loc;
            break;
          }
          if (!found || c.vars.at(i.slot).movedPayloads.contains(i.projection))
            fail(i.loc, DiagnosticCode::ConsumedResource, "cannot borrow a consumed enum payload");
        }
        auto &source = live(c, i.slot, i.loc);
        bool ownedProjection =
            i.placeProjection == Inst::PlaceProjection::Field && p.semantics.ownedFieldType(i.type);
        if (!ownedProjection)
          complete(source, i.loc);
        bool shared = i.kind == Inst::BorrowShared;
        bool derivedUnique = source.root != None && source.access == Access::Unique &&
                             i.placeProjection == Inst::PlaceProjection::Field;
        if (source.root != None && !derivedUnique && (!shared || source.access != Access::Shared))
          fail(i.loc, DiagnosticCode::InvalidReborrow, "exclusive reborrowing is outside this PoC");
        auto &root = access(c, i.slot, shared ? Access::Shared : Access::Unique, i.loc);
        auto path = source.ownedProjection;
        if (i.placeProjection == Inst::PlaceProjection::Field) {
          auto type = symbol(i.slot).type;
          auto *fields = &placeFields(c, i.slot, i.loc);
          for (auto index : i.fieldPath) {
            const auto &member = p.semantics.types[type].fields.at(index);
            if (p.semantics.ownedFieldType(member.type)) {
              if (!fields->contains(index))
                fail(i.loc, DiagnosticCode::ConsumedResource,
                     "cannot borrow an uninitialized or consumed owned field");
              fields = &fields->at(index).fields;
              path.push_back(index);
              if (!missingFields(member.type, *fields).empty())
                fail(i.loc, DiagnosticCode::ConsumedResource,
                     "borrowed aggregate has absent owned fields");
            }
            type = member.type;
          }

          if (!ownedProjection)
            path.clear();
        }
        SymbolId rid = source.root == None ? i.slot : source.root;
        root.borrowAt = i.loc;
        auto owners = origins(c, i.slot, i.loc);
        if (shared) {
          retainOrigins(c, owners, i.loc);
          if (derivedUnique)
            ++source.shared;
        } else if (derivedUnique)
          source.unique = true;
        else
          root.unique = true;
        c.vars[i.offset] = {true,         root.state, shared ? Access::Shared : Access::Unique,
                            rid,          0,          false,
                            root.version, false,      "",
                            None};
        if (shared)
          setAliasOrigins(c.vars[i.offset], owners);
        c.vars[i.offset].borrowAt = i.loc;
        c.vars[i.offset].ownedProjection = std::move(path);
        if (derivedUnique)
          c.vars[i.offset].loanParent = i.slot;
        if (auto *field = projectedState(c, i.offset, i.loc))
          c.vars[i.offset].state = field->state;
        break;
      }
      case Inst::EndBorrow:
        endBorrow(c, i.slot);
        break;
      case Inst::ContainerCall:
      case Inst::Call:
      case Inst::ProtocolTransition: {
        auto payloadOrigin = call(i, c);
        if (p.semantics.functions[i.function].returnsReference()) {
          const auto &target = p.semantics.functions[i.function];
          auto index = std::find(target.params.begin(), target.params.end(), target.resultOrigin) -
                       target.params.begin();
          c.references[i.result] = i.slot != None && index == 0
                                       ? i.slot
                                       : c.references.at(i.args.at(index - (i.slot != None)));
        } else if ((i.kind == Inst::Call || i.kind == Inst::ContainerCall) &&
                   p.semantics.linear(i.type)) {
          c.values[i.result] = {i.type, initial(i.type), true, p.semantics.needsDrop(i.type)};
          const auto &target = p.semantics.functions[i.function];
          if (i.kind == Inst::ContainerCall)
            c.values[i.result].fields = initialFields(i.type);
          if (target.resultBorrowedContainer) {
            auto resultOrigins = payloadOrigin;
            if (!target.resultPayloadSource) {
              auto index =
                  std::find(target.params.begin(), target.params.end(), target.resultOrigin) -
                  target.params.begin();
              auto alias = i.slot != None && index == 0
                               ? i.slot
                               : c.references.at(i.args.at(index - (i.slot != None)));
              auto more = origins(c, alias, i.loc);
              resultOrigins.insert(more.begin(), more.end());
            }
            for (auto input : target.additionalResultOrigins) {
              if (std::find(target.params.begin(), target.params.end(), input) ==
                  target.params.end())
                continue;
              auto index = std::find(target.params.begin(), target.params.end(), input) -
                           target.params.begin();
              auto more = origins(c, c.references.at(i.args.at(index)), i.loc);
              resultOrigins.insert(more.begin(), more.end());
            }
            if (resultOrigins.empty()) {

              auto possible = p.semantics.unborrowedVariants(i.type);
              i.possibleVariants.assign(possible.begin(), possible.end());
              c.valueVariantSets[i.result] = std::move(possible);
              break;
            }
            for (auto offset : p.semantics.borrowedPayloadOffsets(i.type)) {
              c.values[i.result].loans[offset] = resultOrigins;
              retainOrigins(c, resultOrigins, i.loc);
            }
          }
        }
        break;
      }
      case Inst::ScopeEnd:
        for (auto it = i.locals.rbegin(); it != i.locals.rend(); ++it)
          drop(c, *it, i.loc, true, i);
        for (auto id : i.locals) {
          c.vars.erase(id);
          c.variantSets.erase(id);
        }
        break;
      case Inst::Return:
        if (function->returnsReference()) {
          auto alias = c.references.at(i.args.at(0));
          const auto &borrow = live(c, alias, i.loc);
          auto origin = borrow.root == None ? alias : borrow.root;
          if (origin != function->resultOrigin || !borrow.extraRoots.empty()) {
            DiagnosticEngine()
                .error(DiagnosticCode::EscapingView, i.loc,
                       "reference to local value escapes the function")
                .label(i.loc, "reference returned here")
                .secondary(symbol(origin).loc, "owner `" + symbol(origin).name + "` created here")
                .note("the result must borrow from input `" + symbol(function->resultOrigin).name +
                      "`")
                .help("return a reference to the input, or transfer ownership of the local value")
                .emit();
          }
        } else if (p.semantics.linear(function->result)) {
          auto &result = take(c, i.args.at(0), i.loc);
          if (function->resultBorrowedContainer) {
            const auto allowed = p.semantics.borrowedPayloadOffsets(function->result);
            for (const auto &[offset, owners] : result.loans) {
              for (auto origin : owners) {
                if (!allowed.contains(offset))
                  fail(i.loc, DiagnosticCode::InvalidCore, "invalid returned payload path");
                if (!function->acceptsResultOrigin(origin))
                  DiagnosticEngine()
                      .error(
                          DiagnosticCode::EscapingView, i.loc,
                          symbol(origin).parameter
                              ? "borrowed payload cannot escape through an unrelated result region"
                              : "reference stored in returned container escapes its local owner")
                      .label(i.loc, "container returned here")
                      .secondary(
                          symbol(origin).loc,
                          (symbol(origin).parameter ? "borrowed input `" : "owner `") +
                              symbol(origin).name +
                              (symbol(origin).parameter ? "` received here" : "` created here"))
                      .note("returned payloads may borrow only from the contracted input origins")
                      .help("borrow from the input or return an owned payload")
                      .emit();
              }
            }
          }
          if ((!function->resultBorrowedContainer && !result.loans.empty()) ||
              result.state != None || result.allocation != p.semantics.needsDrop(function->result))
            fail(i.loc, DiagnosticCode::InvalidCore, "unsupported resource return contract");
        }
        recordFieldPostStates(c, i.loc);
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
            for (auto alias : i.exitEnds) {
              auto found = edge.vars.find(alias);
              if (found != edge.vars.end() && found->second.live && found->second.root != None)
                endBorrow(edge, alias);
            }
          for (auto &edge : exits)
            compatible(exits[0], edge, i.loc, "loop exit");
          for (size_t n = 1; n < exits.size(); ++n)
            mergeVariantSets(exits[0], exits[n]);
          c = std::move(exits[0]);
        }
        break;
      }
      case Inst::If: {
        if (c.conditions.contains(i.args[0])) {

          bool selected = c.conditions.at(i.args[0]);
          refineTest(c, i.args[0], selected, i.loc);
          block(selected ? i.body : i.otherwise, c);
          (selected ? i.otherwise : i.body).clear();
          break;
        }
        Context then = c, other = c;
        refineTest(then, i.args[0], true, i.loc);
        refineTest(other, i.args[0], false, i.loc);
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
        mergeVariantSets(then, other);
        c = std::move(then);
        for (auto it = c.variants.begin(); it != c.variants.end();) {
          auto fact = other.variants.find(it->first);
          if (fact == other.variants.end() || fact->second != it->second)
            it = c.variants.erase(it);
          else
            ++it;
        }
        break;
      }
      default:
        break;
      }
    }
  }

  void checkFunction(FunctionId id) {
    if (checked.contains(id))
      return;
    if (!checking.insert(id).second)
      fail(p.semantics.functions[id].loc, DiagnosticCode::ProtocolReturn,
           "recursive owned-field contracts require explicit fixed-point summaries, outside this "
           "subset");
    auto *previousFunction = function;
    auto previousLoops = std::move(loops);
    loops.clear();
    auto &cf = *definitions.at(id);
    function = &p.semantics.functions[id];
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
                      p.semantics.needsDrop(s.type),
                      "",
                      None};
        c.vars[id].fields = initialFields(s.type);
      }
      for (const auto &contract : function->payloadParameterRegions) {
        auto &origin = c.vars[contract.origin];
        origin.live = true;
        origin.access = Access::Shared;
        origin.state = initial(symbol(contract.origin).type);
        origin.allocation = p.semantics.heap(symbol(contract.origin).type);
        origin.borrowAt = symbol(contract.parameter).loc;
        for (auto offset : p.semantics.borrowedPayloadOffsets(symbol(contract.parameter).type)) {
          c.vars[contract.parameter].loans[offset] = {contract.origin};
          ++origin.shared;
        }
      }
      block(cf.body, c);
      if (!c.returned) {
        if (function->result != Unit)
          fail(function->loc, DiagnosticCode::MissingReturn, "not all paths return a value");
        fail(function->loc, DiagnosticCode::InvalidCore, "missing Core fallthrough cleanup point");
      }
    }
    function = previousFunction;
    loops = std::move(previousLoops);
    checking.erase(id);
    checked.insert(id);
  }

public:
  explicit Checker(Program x) : p(std::move(x)) {}
  Program run() {
    for (auto &cf : p.functions) {
      cf.fieldPostStates.clear();
      definitions[cf.id] = &cf;
    }
    for (auto &cf : p.functions)
      checkFunction(cf.id);
    return std::move(p);
  }
};
void dump(const std::vector<Inst> &ops, const hir::Program &p, std::ostream &o, int d) {
  static const char *names[] = {"constant",
                                "copy",
                                "read",
                                "write",
                                "create",
                                "array",
                                "move",
                                "bind",
                                "drop",
                                "borrow.shared",
                                "borrow.unique",
                                "end_borrow",
                                "call",
                                "protocol.transition",
                                "return",
                                "if",
                                "scope.end",
                                "binary",
                                "unary",
                                "print",
                                "discard",
                                "heap.alloc",
                                "heap.free",
                                "heap.load",
                                "heap.store",
                                "memcpy",
                                "memmove",
                                "cast",
                                "loop",
                                "break",
                                "continue",
                                "enum.construct",
                                "inspect",
                                "static_str",
                                "string.call",
                                "print.text",
                                "end_views",
                                "input.call",
                                "refine.variant",
                                "payload.take",
                                "reference.argument",
                                "reference.store",
                                "length",
                                "field.store",
                                "field.take",
                                "field.drop",
                                "container.call"};
  for (auto &i : ops) {
    if (i.kind == Inst::Loop)
      dump(i.condition, p, o, d + 2);
    for (auto id : i.cleanup)
      o << std::string(d, ' ') << "drop #v" << id << " DropGlue(#T" << p.symbols[id].type
        << (p.dynamicContainer(p.symbols[id].type) ? ") => drop owned elements, then free storage\n"
            : p.types[p.symbols[id].type].dropGlue == hir::DropGlue::StringAllocation
                ? ") => string.drop (free payload)\n"
            : p.types[p.symbols[id].type].dropGlue == hir::DropGlue::InputAllocation
                ? ") => input.drop (free successful text payload)\n"
            : p.types[p.symbols[id].type].kind == hir::Type::Enum
                ? ") => drop active variant payloads\n"
            : !p.ownedFields(p.symbols[id].type).empty()
                ? ") => drop initialized fields in reverse order\n"
                : ") => heap.free\n");
    if (i.freeTemporary)
      o << std::string(d, ' ') << "drop temporary => heap.free\n";
    for (auto alias : i.inferredEnds)
      o << std::string(d, ' ') << "end_borrow #v" << alias << " [CFG last-use]\n";
    if (!i.exitEnds.empty()) {
      o << std::string(d, ' ') << "loop.exit ends={";
      for (auto alias : i.exitEnds)
        o << " #v" << alias;
      o << " } [CFG edge]\n";
    }
    o << std::string(d, ' ');
    if (i.result != None)
      o << "%" << i.result << " = ";
    o << names[i.kind];
    if (i.kind == Inst::FieldStore || i.kind == Inst::FieldTake || i.kind == Inst::FieldDrop)
      o << " field=#" << i.fieldIndex << (i.replaceField ? " drop-previous" : "");
    if ((i.kind == Inst::FieldStore || i.kind == Inst::FieldTake || i.kind == Inst::FieldDrop) &&
        i.protocol != None) {
      auto state = i.kind == Inst::FieldStore ? i.to : i.from;
      o << " protocol=" << p.protocols[i.protocol].name
        << " state=" << p.protocols[i.protocol].states[state];
    }
    if (i.projection)
      o << " payload.offset=" << i.projection;
    if (i.storedPayloadReference)
      o << " stored-reference";
    if (!i.possibleVariants.empty()) {
      o << " possible-variants={";
      for (auto variant : i.possibleVariants)
        o << "#V" << variant << " ";
      o << "}";
    }
    if (i.kind == Inst::RefineVariant)
      o << " variant=#V" << i.offset;
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
    if (i.placeProjection == Inst::PlaceProjection::Field)
      for (auto field : i.fieldPath)
        o << " place.field=#" << field;
    if (i.placeProjection == Inst::PlaceProjection::Index)
      o << " place.index";
    if (i.region != None)
      o << " region=#R" << i.region;
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
  regions::infer(p);
  return Verified(Checker(std::move(p)).run());
}
void print(const Program &p, std::ostream &o) {
  o << "Verified Core (0 / 1 / omega; MemoryState independent of ProtocolState)\n";
  regions::print(p, o);
  regions::printConstraints(p, o);
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
    for (const auto &post : f.fieldPostStates) {
      o << "  field.post-state #v" << post.parameter << " complete\n";
      auto show = [&](auto &&self, TypeId type, const auto &fields, std::string path) -> void {
        for (const auto &[index, state] : fields) {
          const auto &member = p.semantics.types[type].fields.at(index);
          auto name = path + "." + member.name;
          o << "    " << name << " allocation=" << (state.allocation ? "retained" : "released");
          auto protocol = p.semantics.types[member.type].protocol;
          if (protocol != None)
            o << " state=" << p.semantics.protocols[protocol].states.at(state.state);
          o << "\n";
          self(self, member.type, state.fields, name);
        }
      };
      show(show, p.semantics.symbols[post.parameter].type, post.fields,
           p.semantics.symbols[post.parameter].name);
    }
    for (const auto &region : hf.parameterRegions)
      o << "  region.contract #v" << region.parameter << " #R" << region.region << " caller-owned"
        << (hf.acceptsResultOrigin(region.parameter) ? " result-bound\n" : " noescape\n");
    for (const auto &region : hf.payloadParameterRegions)
      o << "  payload.region #v" << region.parameter << " #R" << region.region << " origin=#v"
        << region.origin << " Shared caller-owned"
        << (hf.acceptsResultOrigin(region.origin) ? " result-bound\n" : " noescape\n");
    for (const auto &source : hf.payloadResultSources())
      o << "  return.payload-source #v" << source.parameter << " variant=#V" << source.path.variant
        << " field=" << source.path.field << "\n";
    if (hf.resultBorrowedContainer)
      o << "  result.payload-regions: Shared borrowed container\n";
    if (hf.resultOrigin != None)
      o << "  return.region #R" << hf.resultRegion << " <= #R"
        << p.semantics.symbols[hf.resultOrigin].region << " origin=#v" << hf.resultOrigin << "\n";
    for (auto origin : hf.additionalResultOrigins)
      o << "  return.region #R" << hf.resultRegion << " <= #R" << p.semantics.symbols[origin].region
        << " origin=#v" << origin << "\n";
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
