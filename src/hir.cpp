#include "isac/hir.hpp"
#include "isac/macros.hpp"
#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
namespace isac {
namespace {
class Resolver {
  const ast::Program &ast;
  hir::Program p;
  std::unordered_map<std::string, TypeId> types;
  std::unordered_map<std::string, GenericDeclId> genericNames;
  std::map<std::pair<GenericDeclId, std::vector<TypeId>>, TypeId> instanceCache;
  TypeParamId nextTypeParam = 0;
  std::unordered_map<std::string, GenericDeclId> genericStructNames;
  std::unordered_map<std::string, TypeParamId> activeTypeParameters;
  hir::Substitution activeSubstitution;
  TypeId activeSelf = None;
  std::string activeOwner;
  struct FunctionSource {
    const ast::Function *source;
    std::unordered_map<std::string, TypeParamId> parameters;
    hir::Substitution substitution;
    TypeId owner;
  };
  std::vector<FunctionSource> functionSources;
  std::set<std::pair<TypeId, std::string>> methodsDeclared;
  TypeId substitute(hir::GenericPayload payload, const hir::Substitution &sub) {
    if (!payload.parameter)
      return payload.id;
    for (auto [parameter, type] : sub.types)
      if (parameter == payload.id)
        return type;
    fail({}, DiagnosticCode::TypeMismatch, "unbound generic type parameter");
  }
  TypeId instantiateStruct(GenericDeclId id, const std::vector<TypeId> &arguments, Loc loc) {
    const auto decl = p.genericStructs.at(id - p.genericEnums.size());
    if (arguments.size() != decl.parameters.size())
      fail(loc, DiagnosticCode::TypeMismatch,
           "wrong number of type arguments for `" + decl.name + "`");
    auto key = std::make_pair(id, arguments);
    if (instanceCache.contains(key))
      return instanceCache.at(key);
    if (p.instances.size() >= 1024)
      fail(loc, DiagnosticCode::TypeMismatch, "generic instantiation limit reached (1024)");
    hir::GenericInstance instance{
        static_cast<InstanceId>(p.instances.size()), id, {}, static_cast<TypeId>(p.types.size())};
    std::string name = decl.name + "<";
    for (size_t i = 0; i < arguments.size(); ++i) {
      if (i)
        name += ",";
      name += p.types.at(arguments[i]).name;
      instance.substitution.types.push_back({decl.parameters[i], arguments[i]});
    }
    hir::Type type{hir::Type::Structure, name + ">"};
    uint32_t offset = 0;
    for (const auto &field : decl.fields) {
      auto tid = substitute(field.type, instance.substitution);
      if (!scalar(tid) && p.types[tid].kind != hir::Type::Array && !p.ownedFieldType(tid))
        fail(loc, DiagnosticCode::InvalidPlace,
             "generic struct field `" + field.name +
                 "` requires aggregate element ownership support");
      if (p.storageWidth(tid) > 65536 - offset)
        fail(loc, DiagnosticCode::InvalidPlace, "generic struct layout exceeds 65536 cells");
      type.fields.push_back({field.name, tid, offset});
      offset += p.storageWidth(tid);
    }
    type.width = std::max(1u, offset);
    auto tid = instance.type;
    types[type.name] = tid;
    p.types.push_back(std::move(type));
    p.instances.push_back(std::move(instance));
    instanceCache.emplace(std::move(key), tid);
    return tid;
  }
  void ensureMethods(TypeId owner, const std::string &name) {
    if (!methodsDeclared.insert({owner, name}).second)
      return;
    std::optional<hir::GenericInstance> instance;
    for (const auto &candidate : p.instances)
      if (candidate.type == owner)
        instance = candidate;
    if (!instance)
      return;
    const auto declarationName =
        instance->declaration < p.genericEnums.size()
            ? p.genericEnums[instance->declaration].name
            : p.genericStructs[instance->declaration - p.genericEnums.size()].name;
    auto savedParameters = activeTypeParameters;
    auto savedSubstitution = activeSubstitution;
    auto savedSelf = activeSelf;
    auto savedOwner = activeOwner;
    auto savedPrivilege = privileged;
    for (const auto &impl : ast.impls) {
      if (impl.parameters.empty() || impl.owner != declarationName)
        continue;
      activeTypeParameters.clear();
      activeSubstitution = instance->substitution;
      activeSelf = owner;
      activeOwner = impl.owner;
      for (size_t n = 0; n < impl.parameters.size(); ++n)
        activeTypeParameters[impl.parameters[n]] = instance->substitution.types[n].first;
      for (auto method : impl.methods)
        if (ast.functions.at(method).name == name)
          declareFunction(ast.functions.at(method), owner);
    }
    activeTypeParameters = std::move(savedParameters);
    activeSubstitution = std::move(savedSubstitution);
    activeSelf = savedSelf;
    activeOwner = std::move(savedOwner);
    privileged = savedPrivilege;
  }
  std::map<TypeId, TypeId> sharedReferenceTypes;
  TypeId storedReferenceType(TypeId element, Loc loc) {
    if (!p.supportsStoredSharedReference(element))
      fail(
          loc, DiagnosticCode::EscapingView,
          "stored references require a scalar, plain fixed stack aggregate or non-protocol heap wrapper");
    if (sharedReferenceTypes.contains(element))
      return sharedReferenceTypes.at(element);
    auto id = static_cast<TypeId>(p.types.size());
    hir::Type type{hir::Type::Reference, "&" + p.types[element].name};
    type.element = element;
    p.types.push_back(std::move(type));
    sharedReferenceTypes[element] = id;
    return id;
  }
  void storeReference(hir::Expr &expression) {
    bool alias = expression.kind == hir::Expr::Variable && p.symbols[expression.ref].reference;
    if (expression.kind != hir::Expr::Borrow && !alias)
      return;
    auto access = alias                                         ? p.symbols[expression.ref].access
                  : expression.borrowKind == BorrowKind::Shared ? Access::Shared
                                                                : Access::Unique;
    if (access != Access::Shared)
      fail(expression.loc, DiagnosticCode::EscapingView,
           "storing unique references requires exclusive payload region contracts");
    hir::Expr stored{};
    stored.kind = hir::Expr::StoreReference;
    stored.loc = expression.loc;
    stored.type = storedReferenceType(expression.type, expression.loc);
    stored.args.push_back(std::move(expression));
    expression = std::move(stored);
  }
  TypeId instantiate(GenericDeclId id, const std::vector<TypeId> &arguments, Loc loc) {
    const auto decl = p.genericEnums.at(id);
    if (arguments.size() != decl.parameters.size())
      fail(loc, DiagnosticCode::TypeMismatch,
           "wrong number of type arguments for `" + decl.name + "`");
    auto key = std::make_pair(id, arguments);
    if (auto found = instanceCache.find(key); found != instanceCache.end())
      return found->second;
    if (p.instances.size() >= 1024)
      fail(loc, DiagnosticCode::TypeMismatch, "generic instantiation limit reached (1024)");
    hir::GenericInstance instance{
        static_cast<InstanceId>(p.instances.size()), id, {}, static_cast<TypeId>(p.types.size())};
    std::string name = decl.name + "<";
    for (size_t i = 0; i < arguments.size(); ++i) {
      if (i)
        name += ",";
      name += p.types.at(arguments[i]).name;
      instance.substitution.types.push_back({decl.parameters[i], arguments[i]});
    }
    hir::Type type{hir::Type::Enum, name + ">"};
    for (const auto &source : decl.variants) {
      hir::Variant variant{static_cast<VariantId>(type.variants.size()), source.name, {}};
      for (auto payload : source.payload) {
        TypeId tid = payload.id;
        if (payload.parameter) {
          auto found =
              std::find_if(instance.substitution.types.begin(), instance.substitution.types.end(),
                           [&](auto pair) { return pair.first == payload.id; });
          tid = found->second;
        }
        if (!p.reference(tid) && p.containsReferences(tid))
          fail(loc, DiagnosticCode::EscapingView,
               "nested containers with borrowed payloads require recursive region contracts");
        if (!scalar(tid) && !p.linear(tid))
          fail(loc, DiagnosticCode::InvalidPayload,
               "enum payload views require reference region contracts");
        variant.payload.push_back(tid);
      }
      uint32_t width = 1;
      for (auto payload : variant.payload) {
        auto cells = p.storageWidth(payload);
        if (cells > 65536 || width > 65536 - cells)
          fail(loc, DiagnosticCode::InvalidPayload,
               "enum layout exceeds the PoC limit of 65536 storage cells");
        width += cells;
      }
      type.width = std::max(type.width, width);
      type.variants.push_back(std::move(variant));
    }
    auto tid = instance.type;
    types[type.name] = tid;
    p.types.push_back(std::move(type));
    p.instances.push_back(std::move(instance));
    instanceCache.emplace(std::move(key), tid);
    return tid;
  }

  std::unordered_map<std::string, ProtocolId> protocols;
  std::unordered_map<std::string, FunctionId> functions;
  struct Binding {
    uint32_t id;
    bool constant;
    Binding(uint32_t id = None, bool constant = false) : id(id), constant(constant) {}
  };
  std::vector<std::unordered_map<std::string, Binding>> scopes;
  std::unordered_map<std::string, Binding> globalConstants;
  std::optional<Binding> binding(const std::string &name, Loc loc) {
    auto key = name + "#" + std::to_string(loc.context);
    for (auto i = scopes.rbegin(); i != scopes.rend(); ++i)
      if (i->contains(key))
        return i->at(key);
    if (globalConstants.contains(key))
      return globalConstants.at(key);
    return {};
  }
  void declareConstant(const ast::Stmt &a, bool global = false) {
    auto key = a.name + "#" + std::to_string(a.loc.context);
    auto &scope = global ? globalConstants : scopes.back();
    if (scope.contains(key))
      duplicate(a.loc, a.name);
    auto expression = expr(a.expr);
    coerce(getType(a.annotation), expression);
    auto value = evaluateConstant(expression, p.target.pointerWidth);
    value.inferredLiteral = false;
    ConstId id = p.constants.size();
    value.constant = id;
    p.constants.push_back({id, a.name, value.type, expression, value, a.loc});
    scope[key] = Binding{id, true};
  }
  hir::Function *current = nullptr;
  bool privileged = false;
  uint32_t nextLoop = 0;
  RegionId nextRegion = 0;
  std::vector<uint32_t> loops;
  void duplicate(Loc l, const std::string &n) {
    fail(l, DiagnosticCode::DuplicateDeclaration, "duplicate declaration `" + n + "`");
  }
  TypeId getType(const TypeSyntax &s, bool allowReference = false) {
    if (!s.countName.empty()) {
      auto resolved = s;
      auto entry = binding(s.countName, s.loc);
      if (!entry || !entry->constant)
        fail(s.loc, DiagnosticCode::InvalidConstant,
             "array size requires a previously declared constant");
      const auto &value = p.constants[entry->id].value;
      if (!integer(value.type) || value.text.starts_with('-'))
        fail(s.loc, DiagnosticCode::InvalidConstant,
             "array size must be a positive integer constant");
      auto count = std::stoull(value.text);
      if (count == 0 || count > 65536)
        fail(s.loc, DiagnosticCode::InvalidConstant, "array size must be in 1..65536");
      resolved.count = static_cast<int>(count);
      resolved.countName.clear();
      return getType(resolved, allowReference);
    }
    if (s.reference && !allowReference)
      fail(s.loc, DiagnosticCode::EscapingView,
           "reference types here require a local borrow binding; aggregate and return "
           "region contracts are not implemented");
    if (!s.count && s.arguments.empty() && activeTypeParameters.contains(s.name))
      return substitute({true, activeTypeParameters.at(s.name)}, activeSubstitution);
    if (activeSelf != None && !s.count && s.arguments.empty() &&
        (s.name == "self" || s.name == activeOwner))
      return activeSelf;
    if (genericNames.contains(s.name) || genericStructNames.contains(s.name)) {
      std::vector<TypeId> arguments;
      for (const auto &argument : s.arguments) {
        if (argument.reference) {
          if (argument.mutableReference)
            fail(argument.loc, DiagnosticCode::EscapingView,
                 "stored unique references are not supported yet");
          arguments.push_back(storedReferenceType(getType(argument, true), argument.loc));
        } else
          arguments.push_back(getType(argument));
      }
      if (s.count)
        fail(s.loc, DiagnosticCode::InvalidPayload,
             "arrays of enum values are not implemented yet");
      return genericNames.contains(s.name)
                 ? instantiate(genericNames.at(s.name), arguments, s.loc)
                 : instantiateStruct(genericStructNames.at(s.name), arguments, s.loc);
    }
    if (!s.arguments.empty())
      fail(s.loc, DiagnosticCode::TypeMismatch,
           "type `" + s.name + "` does not accept type arguments");
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
      fail(s.loc, DiagnosticCode::InvalidReceiver,
           "arrays and slices require scalar elements in this increment");
    if (s.count == -1) {
      if (!s.reference || !allowReference)
        fail(s.loc, DiagnosticCode::EscapingView, "slice types require &T[] or &mut T[]");
      auto key = s.name + "[]";
      if (types.contains(key))
        return types.at(key);
      auto id = static_cast<TypeId>(p.types.size());
      types[key] = id;
      p.types.push_back({hir::Type::Slice, key, None, t, 0, 0, {}});
      return id;
    }
    std::string key = s.name + "[" + std::to_string(s.count) + "]";
    if (types.contains(key))
      return types[key];
    TypeId id = p.types.size();
    types[key] = id;
    p.types.push_back({hir::Type::Array, key, None, t, (uint32_t)s.count, (uint32_t)s.count, {}});
    return id;
  }
  void declareFunction(const ast::Function &a, TypeId ownerOverride = None) {
    if (ownerOverride != None && (a.external || !a.intrinsic.empty() || a.releases))
      fail(a.loc, DiagnosticCode::RestrictedOperation,
           "generic impl methods currently require ordinary checked bodies");
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
    f.name = ownerOverride != None ? p.types[ownerOverride].name + "::" + a.name
             : a.owner.empty()     ? a.name
                                   : a.owner + "::" + a.name;
    if (functions.contains(f.name) || f.name == "print")
      duplicate(a.loc, f.name);
    functions[f.name] = f.id;
    if (ownerOverride != None)
      f.owner = ownerOverride;
    else if (!a.owner.empty()) {
      if (!types.contains(a.owner) || (p.types[types[a.owner]].kind != hir::Type::Structure &&
                                       types[a.owner] != Str && !scalar(types[a.owner])))
        fail(a.loc, DiagnosticCode::UnknownName, "unknown impl struct");
      f.owner = types[a.owner];
      if ((p.heap(f.owner) || f.owner == Str) && !a.trusted)
        fail(a.loc, DiagnosticCode::RestrictedOperation,
             "bundled heap types cannot be extended by application impls");
    }
    if (a.result.mutableReference)
      fail(a.result.loc, DiagnosticCode::EscapingView,
           "unique reference returns require exclusive result-region contracts");
    f.result = getType(a.result, a.result.reference);
    if (f.result == Str && f.intrinsic.empty())
      fail(f.loc, DiagnosticCode::EscapingView,
           "returning str views requires lifetime contracts, outside this PoC");
    f.resultBorrowedContainer = !a.result.reference && p.containsReferences(f.result);
    if (!a.result.reference && p.linear(f.result) && f.intrinsic.empty() &&
        ((!p.heap(f.result) && p.needsDrop(f.result)) || p.types[f.result].protocol != None))
      fail(a.loc, DiagnosticCode::InvalidPlace,
           "this resource return requires an allocation or region transfer contract");
    scopes.emplace_back();
    for (size_t i = 0; i < a.params.size(); ++i) {
      auto &ap = a.params[i];
      bool self = ap.name == "self";
      if (self && i != 0)
        fail(ap.loc, DiagnosticCode::TypeMismatch, "self must be first");
      if (!self && ap.access != Access::Own)
        fail(ap.loc, DiagnosticCode::InvalidPlace,
             "shared/unique parameters are supported only for self");
      auto t = getType(ap.type, ap.type.reference);
      if (ap.type.reference && (f.external || !f.intrinsic.empty() || p.types[t].protocol != None ||
                                t == Str || t == RawPtr))
        fail(ap.loc, DiagnosticCode::EscapingView,
             "this reference parameter requires an unsupported ABI or protocol contract");
      if (p.containsReferences(t) &&
          (ap.type.reference || self || f.external || !f.intrinsic.empty() ||
           p.borrowedPayloadInputType(t) == None))
        fail(ap.loc, DiagnosticCode::EscapingView,
             "borrowed container parameters require payload origin contracts");
      if (t == Unit)
        fail(ap.loc, DiagnosticCode::TypeMismatch, "unit parameter is not supported");
      auto access = ap.type.reference ? (ap.type.mutableReference ? Access::Unique : Access::Shared)
                                      : ap.access;
      auto id = add(ap.name, t, ap.loc, access, true, self);
      f.params.push_back(id);
      if (p.containsReferences(t)) {
        auto origin = static_cast<SymbolId>(p.symbols.size());
        auto region = nextRegion++;
        hir::Symbol symbol{origin, ap.name, p.borrowedPayloadInputType(t), Access::Shared, true,
                           false,  ap.loc};
        symbol.reference = true;
        symbol.region = region;
        p.symbols.push_back(std::move(symbol));
        f.payloadParameterRegions.push_back({id, origin, region});
      }
      if (ap.type.reference ||
          (self && a.result.reference && access == Access::Shared && p.types[t].protocol == None)) {
        auto &symbol = p.symbols[id];
        symbol.reference = true;
        symbol.region = nextRegion++;
        f.parameterRegions.push_back(
            {id, symbol.region,
             access == Access::Shared ? BorrowKind::Shared : BorrowKind::Unique});
      }
      if (self)
        f.access = ap.access;
    }
    if (f.owner != None && scalar(f.owner) && !f.params.empty() &&
        p.symbols[f.params[0]].receiver && f.access != Access::Shared)
      fail(f.loc, DiagnosticCode::InvalidReceiver,
           "primitive extension methods currently require shared self");
    if (a.result.reference || f.resultBorrowedContainer) {
      for (const auto &region : f.parameterRegions) {
        const auto &parameter = p.symbols[region.parameter];
        if (parameter.access == Access::Shared &&
            (f.resultBorrowedContainer ? p.borrowedPayloadResultCompatible(parameter.type, f.result)
                                       : p.borrowResultCompatible(parameter.type, f.result))) {
          if (f.resultOrigin != None) {
            if (!f.resultBorrowedContainer)
              fail(a.result.loc, DiagnosticCode::EscapingView,
                   "direct reference return requires one input origin in this subset");
            f.additionalResultOrigins.push_back(parameter.id);
          } else {
            f.resultOrigin = parameter.id;
          }
        }
      }
      if (f.resultBorrowedContainer)
        for (const auto &region : f.payloadParameterRegions) {
          const auto &origin = p.symbols[region.origin];
          if (!p.borrowedPayloadResultCompatible(origin.type, f.result))
            continue;
          auto path = p.singleBorrowedPayloadPath(p.symbols[region.parameter].type);
          if (!path)
            fail(a.result.loc, DiagnosticCode::EscapingView,
                 "returned references require a single borrowed payload path in the input "
                 "container");
          if (p.unborrowedVariants(f.result).empty())
            fail(
                a.result.loc, DiagnosticCode::EscapingView,
                "payload-bound results require at least one reference-free variant in this subset");
          if (f.resultOrigin == None) {
            f.resultOrigin = region.origin;
            f.resultPayloadSource = hir::PayloadResultSource{region.parameter, *path};
          } else {
            f.additionalResultOrigins.push_back(region.origin);
            f.additionalPayloadSources.push_back({region.parameter, *path});
          }
        }
      if (f.resultOrigin == None || f.external || !f.intrinsic.empty() ||
          (f.owner != None && (f.resultBorrowedContainer || p.types[f.owner].protocol != None)))
        fail(a.result.loc, DiagnosticCode::EscapingView,
             "reference return requires a compatible shared input-region contract; local owners "
             "cannot escape");
      f.resultRegion = nextRegion++;
    }
    scopes.pop_back();
    f.consumesSelf = f.owner != None && p.types[f.owner].kind == hir::Type::Enum &&
                     !f.params.empty() && p.symbols[f.params[0]].receiver &&
                     f.access == Access::Own;
    p.functions.push_back(f);
    functionSources.push_back({&a, activeTypeParameters, activeSubstitution, ownerOverride});
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
        return i->at(key).id;
    fail(l, DiagnosticCode::UnknownName, "unknown name `" + n + "`");
  }
  void same(TypeId a, TypeId b, Loc l) {
    if (a != b)
      fail(l, DiagnosticCode::TypeMismatch,
           "expected `" + p.types[a].name + "`, found `" + p.types[b].name + "`");
  }
  void coerce(TypeId wanted, hir::Expr &e) {
    if (p.types[wanted].kind == hir::Type::Slice && e.kind == hir::Expr::Borrow &&
        p.types[e.type].kind == hir::Type::Array &&
        p.types[wanted].element == p.types[e.type].element) {
      e.type = wanted;
      return;
    }
    if (p.reference(wanted))
      storeReference(e);
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
    for (size_t i = skip; i < e.args.size(); ++i) {
      auto &parameter = p.symbols[f.params[i]];
      auto &argument = e.args[i];
      bool alias = argument.kind == hir::Expr::Variable && p.symbols[argument.ref].reference;
      if (parameter.reference && !parameter.receiver) {
        if (argument.kind != hir::Expr::Borrow && !alias)
          fail(argument.loc, DiagnosticCode::ArgumentMismatch,
               "reference parameter requires an explicit borrow or reference binding");
        Access access = alias                                       ? p.symbols[argument.ref].access
                        : argument.borrowKind == BorrowKind::Shared ? Access::Shared
                                                                    : Access::Unique;
        if (access != parameter.access)
          fail(argument.loc, DiagnosticCode::ArgumentMismatch,
               "reference argument capability does not match parameter");
      } else if (argument.kind == hir::Expr::Borrow && !parameter.receiver)
        fail(argument.loc, DiagnosticCode::ArgumentMismatch,
             "borrow cannot be passed to an owned parameter");
      coerce(parameter.type, argument);
    }
  }
  bool place(const hir::Expr &e) {
    return e.kind == hir::Expr::Variable || e.kind == hir::Expr::Field ||
           e.kind == hir::Expr::Index;
  }
  hir::Expr expr(const ast::Expr &a, TypeId expected = None) {
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
      const auto &source = e.args[0];
      bool projection = (source.kind == H::Field || source.kind == H::Index) &&
                        source.args[0].kind == H::Variable && scalar(source.type);
      if (source.kind != H::Variable && !projection)
        fail(a.loc, DiagnosticCode::InvalidPlace,
             "borrow requires a named owner or a direct scalar field/element projection");
      if (projection && a.text == "unique")
        fail(a.loc, DiagnosticCode::InvalidPlace,
             "unique projection borrows require exclusive place contracts");
      e.type = e.args[0].type;
      e.borrowKind = a.text == "unique" ? BorrowKind::Unique : BorrowKind::Shared;
      e.region = nextRegion++;
      return e;
    }
    if (a.kind == A::Match) {
      ast::Stmt match{};
      match.kind = ast::Stmt::Match;
      match.loc = a.loc;
      match.expr = a.args.at(0);
      match.body = a.body;
      match.expressionMatch = true;
      e.kind = H::Match;
      e.body = block({match});
      auto id = e.body.at(0).body.back().matchResult;
      auto resultBinding = e.body.at(0).body.front();
      e.body.at(0).body.erase(e.body.at(0).body.begin());
      e.body.insert(e.body.begin(), std::move(resultBinding));
      e.type = p.symbols.at(id).type;
      H value{};
      value.kind = H::Variable;
      value.loc = a.loc;
      value.type = e.type;
      value.ref = id;
      e.args.push_back(value);
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
    if (!a.typeArguments.empty()) {
      auto sep = a.text.find("::");
      TypeSyntax owner{a.text.substr(0, sep), 0, a.loc};
      owner.arguments = a.typeArguments;
      auto tid = getType(owner);
      ensureMethods(tid, a.text.substr(sep + 2));
      auto concrete = a;
      concrete.typeArguments.clear();
      concrete.text = p.types[tid].name + a.text.substr(sep);
      return expr(concrete, expected);
    }
    if ((a.kind == A::Name || a.kind == A::Call) && a.text.find("::") != std::string::npos) {
      auto sep = a.text.find("::");
      auto tn = a.text.substr(0, sep);
      auto vn = a.text.substr(sep + 2);
      std::vector<hir::Expr> inferredArguments;
      TypeId enumType = types.contains(tn) ? types[tn] : None;
      if (genericNames.contains(tn)) {
        auto gid = genericNames.at(tn);
        enumType = None;
        for (const auto &instance : p.instances)
          if (instance.type == expected && instance.declaration == gid)
            enumType = expected;
        if (enumType == None) {
          const auto &decl = p.genericEnums[gid];
          const auto &variants = p.genericEnums[gid].variants;
          auto variant = std::find_if(variants.begin(), variants.end(),
                                      [&](const auto &v) { return v.name == vn; });
          if (variant == variants.end())
            fail(a.loc, DiagnosticCode::InvalidVariant, "unknown enum variant `" + a.text + "`");
          if (variant->payload.size() != a.args.size())
            fail(a.loc, DiagnosticCode::InvalidPayload, "wrong enum payload arity");
          std::vector<TypeId> arguments(decl.parameters.size(), None);
          for (size_t i = 0; i < a.args.size(); ++i) {
            auto value = expr(a.args[i]);
            storeReference(value);
            auto payload = variant->payload[i];
            if (payload.parameter) {
              auto index = std::find(decl.parameters.begin(), decl.parameters.end(), payload.id) -
                           decl.parameters.begin();
              if (arguments[index] != None)
                same(arguments[index], value.type, value.loc);
              arguments[index] = value.type;
            }
            inferredArguments.push_back(std::move(value));
          }
          if (std::find(arguments.begin(), arguments.end(), None) != arguments.end())
            fail(a.loc, DiagnosticCode::TypeMismatch,
                 "cannot infer all type arguments of `" + tn + "`; add a type annotation");
          enumType = instantiate(gid, arguments, a.loc);
        }
      }
      if (enumType != None && p.types[enumType].kind == hir::Type::Enum &&
          !functions.contains(a.text)) {
        e.type = enumType;
        e.kind = H::EnumConstruct;
        auto &vs = p.types[e.type].variants;
        auto it = std::find_if(vs.begin(), vs.end(), [&](auto &v) { return v.name == vn; });
        if (it == vs.end())
          fail(a.loc, DiagnosticCode::InvalidVariant, "unknown enum variant `" + a.text + "`");
        auto payloadTypes = it->payload;
        e.ref = it->id;
        if (a.args.size() != payloadTypes.size())
          fail(a.loc, DiagnosticCode::InvalidPayload, "wrong enum payload arity");
        for (size_t i = 0; i < a.args.size(); ++i) {
          auto arg = inferredArguments.empty() ? expr(a.args[i], payloadTypes[i])
                                               : std::move(inferredArguments[i]);
          coerce(payloadTypes[i], arg);
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
      if (auto found = binding(a.text, a.loc); found && found->constant) {
        auto value = p.constants[found->id].value;
        value.loc = a.loc;
        return value;
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
    if (a.kind == A::Assign && a.args[0].kind == A::Name) {
      auto found = binding(a.args[0].text, a.args[0].loc);
      if (found && found->constant)
        fail(a.loc, DiagnosticCode::ConstantAssignment,
             "cannot assign to constant `" + a.args[0].text + "`");
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
      break;
    }
    case A::Index: {
      auto &t = p.types[e.args[0].type];
      if (t.kind != hir::Type::Array && t.kind != hir::Type::Slice)
        fail(a.loc, DiagnosticCode::TypeMismatch, "indexing requires an array or slice");
      bool fixed = t.kind == hir::Type::Array && e.args[1].kind == H::Constant;
      if (fixed) {
        if (!integer(e.args[1].type))
          fail(a.loc, DiagnosticCode::TypeMismatch, "array index must be an integer");
        auto n = std::stoll(e.args[1].text);
        if (n < 0 || n >= t.count)
          fail(a.loc, DiagnosticCode::InvalidIndex, "array index out of bounds");
        e.ref = static_cast<uint32_t>(n);
      } else {
        coerce(USize, e.args[1]);
        e.ref = None;
      }
      e.kind = H::Index;
      e.type = t.element;
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
      if (p.linear(e.args[0].type) &&
          !(e.args[0].kind == H::Field && e.args[0].args[0].kind == H::Variable &&
            p.ownedFieldType(e.args[0].type)))
        fail(a.loc, DiagnosticCode::InvalidPlace,
             "PoC assignment supports scalar places; use a fresh let for resource moves");
      break;
    case A::Method: {
      auto &recv = e.args[0];
      if ((p.types[recv.type].kind == hir::Type::Slice ||
           p.types[recv.type].kind == hir::Type::Array) &&
          (a.text == "len" || a.text == "is_empty")) {
        if (e.args.size() != 1)
          fail(a.loc, DiagnosticCode::ArgumentMismatch, "length methods take no arguments");
        e.kind = H::Length;
        e.type = USize;
        if (a.text == "is_empty") {
          auto length = e;
          hir::Expr zero{};
          zero.kind = H::Constant;
          zero.type = USize;
          zero.text = "0";
          zero.loc = a.loc;
          e.kind = H::Binary;
          e.text = "==";
          e.type = Bool;
          e.args = {length, zero};
        }
        return e;
      }
      if (a.text == "move" || a.text == "drop") {
        if (recv.kind != H::Variable &&
            !(recv.kind == H::Field && recv.args[0].kind == H::Variable &&
              p.ownedFieldType(recv.type)))
          fail(a.loc, DiagnosticCode::InvalidPlace, "explicit move/drop requires a named place");
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
      ensureMethods(recv.type, method);
      auto key = p.types[recv.type].name + "::" + method;
      if (!functions.contains(key))
        DiagnosticEngine()
            .error(DiagnosticCode::UnknownName, a.loc,
                   "no method `" + method + "` for type `" + p.types[recv.type].name + "`")
            .secondary(recv.loc, "receiver expression produces `" + p.types[recv.type].name + "`")
            .emit();
      e.ref = functions[key];
      auto &f = p.functions[e.ref];
      if (!privileged && f.trusted && !f.pub)
        fail(a.loc, DiagnosticCode::RestrictedOperation, "library-internal method is not public");
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
    if ((e.kind == H::Call || e.kind == H::Method) && p.functions[e.ref].returnsReference()) {
      hir::Expr borrowed{};
      borrowed.kind = H::Borrow;
      borrowed.loc = e.loc;
      borrowed.type = e.type;
      borrowed.borrowKind = BorrowKind::Shared;
      borrowed.region = nextRegion++;
      borrowed.args.push_back(std::move(e));
      return borrowed;
    }
    return e;
  }
  bool uses(const hir::Expr &e, SymbolId symbol) {
    if (e.kind == hir::Expr::Variable && e.ref == symbol)
      return true;
    for (const auto &arg : e.args)
      if (uses(arg, symbol))
        return true;
    for (const auto &statement : e.body)
      if (uses(statement, symbol))
        return true;
    return false;
  }
  bool uses(const hir::Stmt &s, SymbolId symbol) {
    if (uses(s.expr, symbol))
      return true;
    for (const auto *body : {&s.body, &s.otherwise, &s.latch})
      for (const auto &statement : *body)
        if (uses(statement, symbol))
          return true;
    return false;
  }
  std::vector<hir::Stmt> block(const std::vector<ast::Stmt> &ss, bool keepScope = false) {
    scopes.emplace_back();
    std::vector<hir::Stmt> out;
    std::vector<SymbolId> localReferences;
    for (size_t position = 0; position < ss.size(); ++position) {

      auto a = ss[position];
      if (a.conditionalPattern) {
        ast::Stmt selected{}, fallback{};
        selected.kind = fallback.kind = ast::Stmt::Expression;
        selected.loc = fallback.loc = a.loc;
        selected.pattern = a.pattern;
        selected.body = a.body;
        fallback.pattern.kind = ast::Pattern::Wildcard;
        fallback.pattern.loc = a.loc;
        fallback.body = a.otherwise;
        a.kind = ast::Stmt::Match;
        a.body = {std::move(selected), std::move(fallback)};
        a.otherwise.clear();
      }
      if (a.kind == ast::Stmt::Const) {
        declareConstant(a);
        continue;
      }
      std::optional<hir::Expr> initializer;
      if (a.kind == ast::Stmt::Let)
        initializer =
            expr(a.expr, a.annotated ? getType(a.annotation, a.annotation.reference) : None);
      if (initializer && initializer->kind == hir::Expr::Borrow) {
        hir::Stmt borrowed{};
        borrowed.kind = hir::Stmt::BorrowStart;
        borrowed.loc = a.loc;
        borrowed.expr = std::move(*initializer);
        borrowed.access =
            borrowed.expr.borrowKind == BorrowKind::Unique ? Access::Unique : Access::Shared;
        if (a.annotated && (!a.annotation.reference ||
                            a.annotation.mutableReference != (borrowed.access == Access::Unique)))
          fail(a.loc, DiagnosticCode::TypeMismatch,
               "reference binding requires matching & or &mut annotation");
        if (a.annotated)
          coerce(getType(a.annotation, true), borrowed.expr);
        borrowed.symbol = add(a.name, borrowed.expr.type, a.loc, borrowed.access);
        p.symbols[borrowed.symbol].reference = true;
        p.symbols[borrowed.symbol].inferredRegion = !keepScope;
        p.symbols[borrowed.symbol].region = borrowed.expr.region;
        localReferences.push_back(borrowed.symbol);
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
        Access matchAccess = Access::Own;
        if (s.expr.kind == hir::Expr::Borrow) {
          matchAccess = s.expr.borrowKind == BorrowKind::Shared ? Access::Shared : Access::Unique;
          auto source = s.expr.args.at(0);
          s.expr = std::move(source);
        }
        auto tid = s.expr.type;
        auto t = p.types[tid];
        if (t.kind != hir::Type::Enum && tid != Bool && tid != Char && !integer(tid))
          fail(a.loc, DiagnosticCode::InvalidPattern, "match requires enum, bool or integer");
        bool ownedPayloads = false;
        for (const auto &variant : t.variants)
          for (auto payload : variant.payload)
            ownedPayloads |= p.linear(payload);
        ownedPayloads = ownedPayloads && matchAccess == Access::Own;
        if (ownedPayloads && s.expr.kind == hir::Expr::Variable) {
          hir::Expr moved{};
          moved.kind = hir::Expr::Move;
          moved.loc = s.expr.loc;
          moved.type = tid;
          moved.args = {s.expr};
          s.expr = std::move(moved);
        }
        scopes.emplace_back();
        std::vector<H> prefix;
        TypeId armType = None;
        if (a.expressionMatch)
          s.matchResult = add("$match_result" + std::to_string(p.symbols.size()), Unit, a.loc);
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
          auto pat = arm.pattern;
          if (pat.kind == ast::Pattern::Binding) {
            auto found = binding(pat.name, pat.loc);
            if (found && found->constant) {
              const auto &value = p.constants[found->id].value;
              same(tid, value.type, pat.loc);
              pat.kind = tid == Bool   ? ast::Pattern::Boolean
                         : tid == Char ? ast::Pattern::Character
                                       : ast::Pattern::Integer;
              pat.name = tid == Bool ? (value.text == "0" ? "false" : "true") : value.text;
            } else if (t.kind == hir::Type::Enum &&
                       std::any_of(t.variants.begin(), t.variants.end(),
                                   [&](const auto &v) { return v.name == pat.name; })) {
              pat.kind = ast::Pattern::Variant;
            }
          }
          if (all) {
            if (!a.expressionMatch && !a.conditionalPattern)
              fail(pat.loc, DiagnosticCode::UnreachableArm, "unreachable match arm");
            auto warning = DiagnosticEngine()
                               .error(DiagnosticCode::UnreachableMatchArmWarning, pat.loc,
                                      "unreachable match arm")
                               .build();
            warning.severity = DiagnosticSeverity::Warning;
            p.warnings.push_back(std::move(warning));
            continue;
          }
          scopes.emplace_back();
          SymbolId alias = None;
          std::vector<std::pair<SymbolId, uint32_t>> payloadBorrows;
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
                return pat.name == v.name ||
                       pat.name == t.name.substr(0, t.name.find('<')) + "::" + v.name;
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
                  if (p.reference(found->payload[j])) {
                    if (matchAccess == Access::Unique)
                      fail(pat.loc, DiagnosticCode::SharedMutation,
                           "a stored shared reference cannot become unique through match");
                    auto pt = p.types[found->payload[j]].element;
                    auto id = add(pat.bindings[j], pt, pat.loc, Access::Shared);
                    p.symbols[id].reference = true;
                    p.symbols[id].region = nextRegion++;
                    payloadBorrows.push_back({id, p.payloadOffset(tid, found->id, j)});
                    continue;
                  }
                  if (matchAccess != Access::Own) {
                    auto pt = found->payload[j];
                    if (p.types[pt].protocol != None ||
                        (matchAccess == Access::Unique && !p.heap(pt)))
                      fail(pat.loc, DiagnosticCode::InvalidReborrow,
                           "this payload projection requires a shared non-protocol value or a "
                           "unique heap wrapper");
                    auto id = add(pat.bindings[j], pt, pat.loc, matchAccess);
                    p.symbols[id].reference = true;
                    p.symbols[id].region = nextRegion++;
                    payloadBorrows.push_back({id, p.payloadOffset(tid, found->id, j)});
                    continue;
                  }
                  H let{};
                  let.kind = H::Let;
                  let.loc = pat.loc;
                  let.symbol = add(pat.bindings[j], found->payload[j], pat.loc);
                  let.expr.kind =
                      p.linear(found->payload[j]) ? hir::Expr::PayloadTake : hir::Expr::Field;
                  let.expr.type = found->payload[j];
                  let.expr.loc = pat.loc;
                  let.expr.ref = p.payloadOffset(tid, found->id, j);
                  let.expr.args = {base};
                  h.body.push_back(let);
                }
            } else {
              if ((tid == Bool) != (pat.kind == ast::Pattern::Boolean) ||
                  (tid == Char) != (pat.kind == ast::Pattern::Character) ||
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
          if (ownedPayloads && alias == None) {
            H drop{};
            drop.kind = H::Expression;
            drop.loc = arm.loc;
            drop.expr.kind = hir::Expr::Drop;
            drop.expr.loc = arm.loc;
            drop.expr.args = {base};
            h.body.push_back(std::move(drop));
          }
          auto body = block(arm.body);
          if (ownedPayloads && alias == None && !payloadBorrows.empty()) {
            body.insert(body.begin(), std::move(h.body.back()));
            h.body.pop_back();
          }
          if (a.expressionMatch) {
            auto value = expr(arm.expr);
            if (armType == None) {
              armType = value.type;
              if (!scalar(armType))
                fail(arm.loc, DiagnosticCode::TypeMismatch,
                     "this match expression increment requires scalar arm results");
              p.symbols[s.matchResult].type = armType;
            } else if (value.type != armType) {
              DiagnosticEngine()
                  .error(DiagnosticCode::TypeMismatch, value.loc, "incompatible match arm types")
                  .note("first arm: " + p.types[armType].name)
                  .note("this arm: " + p.types[value.type].name)
                  .emit();
            }
            H assignment{};
            assignment.kind = H::Expression;
            assignment.loc = arm.loc;
            assignment.expr.kind = hir::Expr::Assign;
            assignment.expr.loc = arm.loc;
            assignment.expr.type = Unit;
            hir::Expr result{};
            result.kind = hir::Expr::Variable;
            result.type = armType;
            result.ref = s.matchResult;
            result.loc = arm.loc;
            assignment.expr.args = {result, value};
            body.push_back(std::move(assignment));
          }
          for (auto it = payloadBorrows.rbegin(); it != payloadBorrows.rend(); ++it) {
            H borrow{};
            borrow.kind = H::Borrow;
            borrow.loc = pat.loc;
            borrow.access = p.symbols[it->first].access;
            borrow.symbol = it->first;
            borrow.expr.kind = hir::Expr::Borrow;
            borrow.expr.loc = pat.loc;
            borrow.expr.type = p.symbols[it->first].type;
            borrow.expr.ref = it->second;
            borrow.expr.region = p.symbols[it->first].region;
            borrow.expr.args = {base};
            borrow.body = std::move(body);
            body = {std::move(borrow)};
          }
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
        if (a.expressionMatch) {
          if (armType == None)
            fail(a.loc, DiagnosticCode::NonExhaustiveMatch, "match expression requires an arm");
          H result{};
          result.kind = H::Let;
          result.loc = a.loc;
          result.symbol = s.matchResult;
          result.expr.kind = hir::Expr::Constant;
          result.expr.loc = a.loc;
          result.expr.type = armType;
          result.expr.text = "0";
          prefix.insert(prefix.begin(), std::move(result));
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
          s.expr = expr(a.expr, a.kind == ast::Stmt::Let && a.annotated
                                    ? getType(a.annotation, a.annotation.reference)
                                    : None);
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
      s.expr = initializer ? std::move(*initializer)
                           : expr(a.expr, a.kind == ast::Stmt::Return ? current->result
                                          : a.kind == ast::Stmt::Let && a.annotated
                                              ? getType(a.annotation, a.annotation.reference)
                                              : None);
      if (s.expr.kind == hir::Expr::Borrow &&
          !(a.kind == ast::Stmt::Return && current->returnsReference()))
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
      if (a.kind == ast::Stmt::Return) {
        if (current->returnsReference() &&
            !((s.expr.kind == hir::Expr::Variable && p.symbols[s.expr.ref].reference &&
               p.symbols[s.expr.ref].access == Access::Shared) ||
              (s.expr.kind == hir::Expr::Borrow && s.expr.borrowKind == BorrowKind::Shared)))
          fail(s.expr.loc, DiagnosticCode::EscapingView,
               "shared reference return requires a shared reference binding with an input origin");
        coerce(current->result, s.expr);
      }
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

    std::map<size_t, std::vector<hir::Stmt>> endings;
    for (auto symbol : localReferences) {
      size_t last = 0;
      for (size_t n = 0; n < out.size(); ++n)
        if (out[n].symbol == symbol || uses(out[n], symbol))
          last = n;
      if (keepScope)
        last = out.size() - 1;
      p.symbols[symbol].lastUse = out[last].loc;
      hir::Stmt end{};
      end.kind = hir::Stmt::BorrowEnd;
      end.unconditional = keepScope;
      end.symbol = symbol;
      end.loc = out[last].loc;
      endings[last].push_back(end);
    }
    for (auto it = endings.rbegin(); it != endings.rend(); ++it)
      out.insert(out.begin() + it->first + 1, it->second.begin(), it->second.end());
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
    for (const auto &constant : ast.constants)
      declareConstant(constant, true);
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
      if (!a.parameters.empty())
        continue;
      if (types.contains(a.name) || protocols.contains(a.name))
        duplicate(a.loc, a.name);
      TypeId id = p.types.size();
      types[a.name] = id;
      p.types.push_back({hir::Type::Structure, a.name, None, None, 0, 1, {}});
    }
    for (auto &a : ast.enums) {
      if (!a.parameters.empty()) {
        if (types.contains(a.name) || protocols.contains(a.name) || genericNames.contains(a.name))
          duplicate(a.loc, a.name);
        GenericDeclId id = p.genericEnums.size();
        genericNames[a.name] = id;
        hir::GenericEnum decl{id, a.name, {}, {}};
        std::unordered_map<std::string, TypeParamId> parameters;
        for (const auto &name : a.parameters) {
          if (parameters.contains(name))
            duplicate(a.loc, name);
          parameters[name] = nextTypeParam++;
          decl.parameters.push_back(parameters[name]);
        }
        std::vector<hir::GenericVariant> variants;
        std::set<std::string> names;
        if (a.variants.empty())
          fail(a.loc, DiagnosticCode::InvalidVariant, "enum must declare at least one variant");
        for (const auto &variant : a.variants) {
          if (!names.insert(variant.name).second)
            duplicate(variant.loc, variant.name);
          hir::GenericVariant resolved{variant.name, {}};
          for (const auto &payload : variant.payload) {
            if (payload.reference || payload.count || !payload.countName.empty() ||
                !payload.arguments.empty())
              fail(payload.loc, DiagnosticCode::InvalidPayload,
                   "compound generic payload types are not implemented yet");
            if (parameters.contains(payload.name))
              resolved.payload.push_back({true, parameters.at(payload.name)});
            else
              resolved.payload.push_back({false, getType(payload)});
          }
          variants.push_back(std::move(resolved));
        }
        decl.variants = std::move(variants);
        p.genericEnums.push_back(std::move(decl));
        continue;
      }
      if (types.contains(a.name) || protocols.contains(a.name) || genericNames.contains(a.name))
        duplicate(a.loc, a.name);
      types[a.name] = p.types.size();
      p.types.push_back({hir::Type::Enum, a.name});
    }
    for (auto &a : ast.enums) {
      if (!a.parameters.empty())
        continue;
      auto enumId = types[a.name];
      std::set<std::string> names;
      if (a.variants.empty())
        fail(a.loc, DiagnosticCode::InvalidVariant, "enum must declare at least one variant");
      for (auto &v : a.variants) {
        if (!names.insert(v.name).second)
          duplicate(v.loc, v.name);
        hir::Variant variant{static_cast<VariantId>(p.types[enumId].variants.size()), v.name, {}};
        for (auto &ts : v.payload) {
          auto tid = getType(ts);
          if (!scalar(tid) && !p.heap(tid))
            fail(ts.loc, DiagnosticCode::InvalidPayload,
                 "aggregate payload declarations require completed type layout");
          variant.payload.push_back(tid);
        }
        auto &t = p.types[enumId];
        uint32_t width = 1;
        for (auto payload : variant.payload)
          width += p.storageWidth(payload);
        t.width = std::max(t.width, width);
        t.variants.push_back(variant);
      }
    }

    for (const auto &a : ast.structs) {
      if (a.parameters.empty())
        continue;
      if (types.contains(a.name) || genericNames.contains(a.name) ||
          genericStructNames.contains(a.name) || protocols.contains(a.name))
        duplicate(a.loc, a.name);
      if (!a.protocol.empty() || !a.representation.empty())
        fail(a.loc, DiagnosticCode::InvalidPlace,
             "generic protocol/heap struct contracts are not implemented yet");
      GenericDeclId id = p.genericEnums.size() + p.genericStructs.size();
      genericStructNames[a.name] = id;
      hir::GenericStruct decl{id, a.name, {}, {}};
      std::unordered_map<std::string, TypeParamId> parameters;
      for (const auto &name : a.parameters) {
        if (parameters.contains(name))
          duplicate(a.loc, name);
        parameters[name] = nextTypeParam++;
        decl.parameters.push_back(parameters[name]);
      }
      std::set<std::string> fields;
      for (const auto &field : a.fields) {
        if (!fields.insert(field.name).second)
          duplicate(field.loc, field.name);
        const auto &type = field.type;
        if (type.reference || type.count || !type.arguments.empty() || !type.countName.empty())
          fail(type.loc, DiagnosticCode::InvalidPayload,
               "compound generic field templates are not implemented yet");
        decl.fields.push_back({field.name, parameters.contains(type.name)
                                               ? hir::GenericPayload{true, parameters.at(type.name)}
                                               : hir::GenericPayload{false, getType(type)}});
      }
      p.genericStructs.push_back(std::move(decl));
    }

    for (auto &a : ast.structs) {
      if (!a.parameters.empty())
        continue;
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
    }

    for (auto &a : ast.structs) {
      if (!a.parameters.empty())
        continue;
      privileged = a.trusted;
      auto id = types[a.name];
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
        if (t == Unit || (p.types[t].kind == hir::Type::Structure && !p.ownedFieldType(t)) ||
            p.types[t].kind == hir::Type::Enum ||
            (p.ownedFieldType(t) && p.types[id].protocol != None))
          fail(f.loc, DiagnosticCode::InvalidPlace,
               "PoC struct fields require scalars, scalar arrays, or non-protocol heap owners");
        p.types[id].fields.push_back({f.name, t, offset});
        offset += p.storageWidth(t);
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
    for (const auto &impl : ast.impls) {
      if (!impl.parameters.empty()) {
        size_t arity =
            genericNames.contains(impl.owner)
                ? p.genericEnums[genericNames.at(impl.owner)].parameters.size()
            : genericStructNames.contains(impl.owner)
                ? p.genericStructs[genericStructNames.at(impl.owner) - p.genericEnums.size()]
                      .parameters.size()
                : 0;
        if (arity != impl.parameters.size() || arity != impl.target.arguments.size())
          fail(impl.loc, DiagnosticCode::TypeMismatch,
               "generic impl must bind all owner type parameters");
        std::set<std::string> names;
        for (size_t n = 0; n < arity; ++n) {
          if (!names.insert(impl.parameters[n]).second)
            duplicate(impl.loc, impl.parameters[n]);
          const auto &arg = impl.target.arguments[n];
          if (arg.name != impl.parameters[n] || arg.reference || arg.count ||
              !arg.arguments.empty())
            fail(impl.loc, DiagnosticCode::TypeMismatch,
                 "generic impl requires owner parameters in declaration order");
        }
      } else if (!types.contains(impl.owner) || !impl.target.arguments.empty() ||
                 (p.types[types.at(impl.owner)].kind != hir::Type::Structure &&
                  types.at(impl.owner) != Str && !scalar(types.at(impl.owner))))
        fail(impl.loc, DiagnosticCode::UnknownName, "unknown impl struct `" + impl.owner + "`");
    }
    std::set<uint32_t> genericMethods;
    for (const auto &impl : ast.impls)
      if (!impl.parameters.empty())
        genericMethods.insert(impl.methods.begin(), impl.methods.end());
    for (size_t n = 0; n < ast.functions.size(); ++n)
      if (!genericMethods.contains(n))
        declareFunction(ast.functions[n]);
    for (auto &f : p.functions) {
      if (!f.intrinsic.empty()) {
        if (f.intrinsicId == IntrinsicId::Env_argument ||
            f.intrinsicId == IntrinsicId::Env_argument_count) {
          std::vector<TypeId> signature{f.result};
          for (auto param : f.params)
            signature.push_back(p.symbols[param].type);
          if (!f.trusted || f.owner != None ||
              signature != intrinsicSignature(f.intrinsicId, None, f.result))
            fail(f.loc, DiagnosticCode::RestrictedOperation,
                 "invalid environment intrinsic signature");
          continue;
        }
        if (intrinsicInfo(f.intrinsicId).lowering == IntrinsicLowering::InputRecord) {
          if (!f.trusted)
            fail(f.loc, DiagnosticCode::RestrictedOperation,
                 "input intrinsics are private library implementation");
          if (f.intrinsic.starts_with("io.parse_")) {
            bool view = f.intrinsic.starts_with("io.parse_view_");
            auto suffix = f.intrinsic.substr(view ? 14 : 9);
            auto cap = suffix;
            cap[0] = static_cast<char>(std::toupper(cap[0]));
            if (f.owner == None ||
                (view ? f.owner != Str
                      : p.types[f.owner].representation != hir::Representation::StringRecord) ||
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
    for (size_t i = 0; i < functionSources.size(); ++i) {
      auto source = functionSources[i];
      auto f = p.functions[i];
      activeTypeParameters = source.parameters;
      activeSubstitution = source.substitution;
      activeSelf = source.owner;
      activeOwner = source.source->owner;
      current = &f;
      privileged = f.trusted;
      if (f.external || !f.intrinsic.empty())
        continue;
      scopes.emplace_back();
      for (auto id : f.params)
        scopes.back()[p.symbols[id].name + "#" + std::to_string(p.symbols[id].loc.context)] = id;
      try {
        f.body = block(source.source->body);
        p.functions[i] = std::move(f);
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
    if ((f.result != I32 && f.result != Unit) || !f.params.empty())
      DiagnosticEngine()
          .error(DiagnosticCode::TypeMismatch, f.loc, "invalid entrypoint signature")
          .note("supported signatures: fn main() -> i32; fn main()")
          .help(
              "use std.env.argument_count() and std.env.argument(index) for command-line arguments")
          .emit();
    p.entryPoint = {f.id, f.result == Unit ? hir::MainSignatureKind::NoArgsUnit
                                           : hir::MainSignatureKind::NoArgsI32};
    ExprId nextExpression = 0;
    std::function<void(hir::Expr &)> numberExpression;
    std::function<void(std::vector<hir::Stmt> &)> numberBlock;
    numberExpression = [&](hir::Expr &expression) {
      expression.id = nextExpression++;
      for (auto &argument : expression.args)
        numberExpression(argument);
      numberBlock(expression.body);
    };
    numberBlock = [&](std::vector<hir::Stmt> &statements) {
      for (auto &statement : statements) {
        numberExpression(statement.expr);
        numberBlock(statement.body);
        numberBlock(statement.otherwise);
        numberBlock(statement.latch);
      }
    };
    for (auto &constant : p.constants) {
      numberExpression(constant.expression);
      numberExpression(constant.value);
    }
    for (auto &function : p.functions)
      numberBlock(function.body);
    return std::move(p);
  }
};
void printS(const std::vector<hir::Stmt> &, std::ostream &, int);
void printE(const hir::Expr &e, std::ostream &o) {
  static const char *names[] = {"constant", "symbol",         "binary",          "unary", "call",
                                "method",   "field",          "index",           "array", "assign",
                                "move",     "drop",           "construct",       "print", "unit",
                                "cast",     "enum.construct", "static_str",      "block", "borrow",
                                "match",    "payload.take",   "reference.store", "length"};
  o << names[e.kind] << "<T" << e.type << ">";
  if (e.id != None)
    o << " expr=#E" << e.id;
  if (e.constant != None)
    o << " const=#C" << e.constant;
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
  o << "entrypoint #F" << p.entryPoint.function
    << (p.entryPoint.signature == hir::MainSignatureKind::NoArgsUnit ? " NoArgsUnit" : " NoArgsI32")
    << "\n";
  for (const auto &decl : p.genericStructs) {
    o << "generic struct #G" << decl.id << " " << decl.name;
    for (auto parameter : decl.parameters)
      o << " #TP" << parameter;
    o << "\n";
  }
  for (const auto &decl : p.genericEnums) {
    o << "generic enum #G" << decl.id << " " << decl.name;
    for (auto parameter : decl.parameters)
      o << " #TP" << parameter;
    o << "\n";
    for (const auto &variant : decl.variants) {
      o << "  variant " << variant.name;
      for (auto payload : variant.payload)
        o << (payload.parameter ? " #TP" : " #T") << payload.id;
      o << "\n";
    }
  }
  for (const auto &instance : p.instances) {
    o << "instance #I" << instance.id << " generic=#G" << instance.declaration << " type=#T"
      << instance.type;
    for (auto [parameter, type] : instance.substitution.types)
      o << " #TP" << parameter << "=#T" << type;
    o << "\n";
  }
  for (const auto &constant : p.constants) {
    o << "const #C" << constant.id << " " << constant.name << " : #T" << constant.type << " = ";
    printE(constant.value, o);
    o << "\n";
  }
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
    if (f.consumesSelf)
      o << " receiver-effect=consume";
    if (f.owner != None)
      o << " impl=#T" << f.owner;
    for (auto op : f.operations)
      o << " operation=#O" << op;
    o << "\n";
    for (const auto &region : f.parameterRegions)
      o << "  region.contract #v" << region.parameter << " #R" << region.region << " "
        << (region.kind == BorrowKind::Shared ? "Shared" : "Unique") << " caller-owned"
        << (f.acceptsResultOrigin(region.parameter) ? " result-bound\n" : " noescape\n");
    for (const auto &region : f.payloadParameterRegions)
      o << "  payload.region #v" << region.parameter << " #R" << region.region << " origin=#v"
        << region.origin << " Shared caller-owned"
        << (f.acceptsResultOrigin(region.origin) ? " result-bound\n" : " noescape\n");
    for (const auto &source : f.payloadResultSources())
      o << "  return.payload-source #v" << source.parameter << " variant=#V" << source.path.variant
        << " field=" << source.path.field << "\n";
    if (f.resultBorrowedContainer)
      o << "  result.payload-regions: Shared borrowed container\n";
    if (f.resultOrigin != None)
      o << "  return.region #R" << f.resultRegion << " <= #R" << p.symbols[f.resultOrigin].region
        << " origin=#v" << f.resultOrigin << "\n";
    for (auto origin : f.additionalResultOrigins)
      o << "  return.region #R" << f.resultRegion << " <= #R" << p.symbols[origin].region
        << " origin=#v" << origin << "\n";
    printS(f.body, o, 2);
  }
}
}
