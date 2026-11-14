#include "isac/hir.hpp"
#include "isac/async.hpp"
#include "isac/ffi.hpp"
#include "isac/macros.hpp"
#include "isac/iteration.hpp"
#include <algorithm>
#include <deque>
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
  std::set<std::pair<GenericDeclId, std::vector<TypeId>>> resolvingInstances;
  TypeParamId nextTypeParam = 0;
  std::map<TypeId, const ast::Struct *> structSources;
  std::map<TypeId, unsigned> structLayoutState;
  bool structLayoutsEnabled = false;
  void ensureStructLayout(TypeId id, Loc origin) {
    if (!structLayoutsEnabled || !structSources.contains(id))
      return;
    auto state = structLayoutState[id];
    if (state == 2)
      return;
    if (state == 1)
      fail(origin, DiagnosticCode::InvalidPlace,
           "recursive by-value struct layout involving `" + p.types[id].name + "`");
    structLayoutState[id] = 1;
    const auto &source = *structSources.at(id);
    auto savedPrivilege = privileged;
    privileged = source.trusted;
    if (source.foreignC) {
      std::set<std::string> names;
      uint32_t bytes = 0, alignment = 1;
      for (const auto &field : source.fields) {
        if (!names.insert(field.name).second)
          duplicate(field.loc, field.name);
        if (field.type.reference || field.type.count == -1)
          fail(field.loc, DiagnosticCode::InvalidCAbi,
               "C fields require scalars, raw pointers, fixed scalar arrays or nested C records");
        if (field.type.count || !field.type.countName.empty()) {
          auto element = field.type;
          element.count = 0;
          element.countName.clear();
          if (!scalar(getType(element)))
            fail(field.loc, DiagnosticCode::InvalidCAbi, "C array fields require scalar elements");
        }
        auto tid = getType(field.type);
        if (!p.cScalar(tid) && p.types[tid].kind != hir::Type::ForeignRecord &&
            !(p.types[tid].kind == hir::Type::Array && p.cScalar(p.types[tid].element)))
          fail(field.loc, DiagnosticCode::InvalidCAbi, "field has no C-compatible representation");
        auto [size, align] = ffi::layout(p, tid);
        if (source.foreignUnion) {
          p.types[id].fields.push_back({field.name, tid, 0});
          bytes = std::max(bytes, size);
        } else {
          bytes = (bytes + align - 1) / align * align;
          p.types[id].fields.push_back({field.name, tid, bytes});
          bytes += size;
        }
        alignment = std::max(alignment, align);
        if (bytes > 1024 * 1024)
          fail(field.loc, DiagnosticCode::InvalidCAbi, "C struct layout exceeds 1 MiB");
      }
      if (source.fields.empty() || !source.parameters.empty() || !source.protocol.empty())
        fail(source.loc, DiagnosticCode::InvalidCAbi,
             "C structs require fields, without generics/protocols; use an opaque type");
      p.types[id].width = (bytes + alignment - 1) / alignment * alignment;
      structLayoutState[id] = 2;
      privileged = savedPrivilege;
      return;
    }
    std::set<std::string> names;
    std::vector<hir::Field> fields;
    uint32_t offset = 0;
    for (const auto &field : source.fields) {
      if (!names.insert(field.name).second)
        duplicate(field.loc, field.name);
      auto type = getType(field.type, field.type.reference);
      if (field.type.reference) {
        type = storedReferenceType(type, field.loc,
                                   field.type.mutableReference ? Access::Unique : Access::Shared);
      }
      if (p.types[type].kind == hir::Type::ForeignRecord ||
          p.types[type].kind == hir::Type::ForeignOpaque)
        fail(field.loc, DiagnosticCode::InvalidCAbi,
             "C structs cannot be embedded in language aggregates; use a raw pointer");

      if (type == RawPtr && !p.heap(id))
        fail(field.loc, DiagnosticCode::RestrictedOperation,
             "RawPtr fields require a trusted heap representation");
      if (type == Str)
        fail(field.loc, DiagnosticCode::EscapingView,
             "str fields require lifetime contracts, outside this PoC");
      if (type == Unit ||
          (p.types[type].kind == hir::Type::Structure && !p.managedFieldType(type) &&
           !p.supportsStoredReferenceReferent(type)) ||
          (p.types[type].kind == hir::Type::Enum && !p.localCarrierType(type)) ||
          (p.managedFieldType(type) && p.types[id].protocol != None))
        fail(field.loc, DiagnosticCode::InvalidPlace,
             "struct fields require scalars, scalar arrays, or non-protocol owners");
      auto width = p.storageWidth(type);
      if (width > 65536 || offset > 65536 - width)
        fail(field.loc, DiagnosticCode::InvalidPlace,
             "struct layout exceeds the PoC limit of 65536 storage cells");
      fields.push_back({field.name, type, offset});
      offset += width;
    }
    p.types[id].fields = std::move(fields);
    p.types[id].width = std::max(1u, offset);
    if (p.types[id].representation == hir::Representation::StringRecord) {
      const auto &fs = p.types[id].fields;
      if (fs.size() != 3 || fs[0].type != RawPtr || fs[1].type != USize || fs[2].type != USize)
        fail(source.loc, DiagnosticCode::RestrictedOperation,
             "String requires the runtime descriptor {ptr, len, capacity}");
    }
    if (p.heap(id) && p.types[id].representation != hir::Representation::StringRecord &&
        p.types[id].representation != hir::Representation::InputRecord &&
        (p.types[id].fields.size() != 1 || p.types[id].fields[0].type != RawPtr))
      fail(source.loc, DiagnosticCode::RestrictedOperation,
           "heap wrappers require one private RawPtr field");
    structLayoutState[id] = 2;
    privileged = savedPrivilege;
  }
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
  std::deque<ast::Function> synthesizedMethods;
  std::map<std::pair<FunctionId, std::vector<TypeId>>, FunctionId> variadicInstances;
  FunctionId instantiateVariadic(FunctionId declaration, hir::Expr &expression) {
    const auto original = p.functions.at(declaration);
    if (expression.args.size() < original.fixedParams)
      fail(expression.loc, DiagnosticCode::ArgumentMismatch,
           "too few fixed arguments to variadic C function `" + original.name + "`");
    std::vector<TypeId> extra;
    for (size_t n = original.fixedParams; n < expression.args.size(); ++n) {
      auto &argument = expression.args[n];
      auto type = argument.type;
      bool reference = argument.kind == hir::Expr::Borrow ||
                       (argument.kind == hir::Expr::Variable && p.symbols[argument.ref].reference);
      if (reference || !p.cScalar(type))
        fail(argument.loc, DiagnosticCode::InvalidCAbi,
             "variadic C arguments require native scalars or typed pointers; convert buffers "
             "explicitly inside unsafe");
      auto promoted = type == F32 ? F64
                      : type == Bool || type == I8 || type == U8 || type == I16 || type == U16
                          ? I32
                          : type;
      if (promoted != type) {
        hir::Expr cast{};
        cast.kind = hir::Expr::Cast;
        cast.type = promoted;
        cast.loc = argument.loc;
        cast.text = "c.default.promotion";
        cast.args.push_back(std::move(argument));
        argument = std::move(cast);
      }
      extra.push_back(promoted);
    }
    if (extra.empty())
      return declaration;
    auto key = std::make_pair(declaration, extra);
    if (variadicInstances.contains(key))
      return variadicInstances.at(key);
    auto instance = original;
    instance.id = p.functions.size();
    instance.name += "__varargs_" + std::to_string(instance.id);
    instance.cLinkName = original.cLinkName.empty() ? original.name : original.cLinkName;
    instance.variadicDeclaration = declaration;
    for (size_t n = 0; n < extra.size(); ++n) {
      SymbolId id = p.symbols.size();
      p.symbols.push_back({id, "$vararg" + std::to_string(n), extra[n], Access::Own, true, false,
                           expression.args[n + original.fixedParams].loc});
      instance.params.push_back(id);
    }
    p.functions.push_back(std::move(instance));
    functionSources.push_back(functionSources.at(declaration));
    variadicInstances[key] = p.functions.back().id;
    return p.functions.back().id;
  }
  std::set<std::pair<TypeId, std::string>> methodsDeclared;
  TypeId substitute(hir::GenericPayload payload, const hir::Substitution &sub) {
    if (payload.referenceAccess) {
      auto access = *payload.referenceAccess;
      payload.referenceAccess.reset();
      return storedReferenceType(substitute(payload, sub), {}, access);
    }
    if (!payload.arguments.empty()) {
      std::vector<TypeId> arguments;
      for (const auto &argument : payload.arguments)
        arguments.push_back(substitute(argument, sub));
      return payload.id < p.genericEnums.size() ? instantiate(payload.id, arguments, {})
                                                : instantiateStruct(payload.id, arguments, {});
    }
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
    for (auto type : arguments)
      ensureStructLayout(type, loc);
    for (const auto &field : decl.fields)
      if (!field.type.parameter && field.type.arguments.empty())
        ensureStructLayout(field.type.id, loc);
    auto key = std::make_pair(id, arguments);
    if (instanceCache.contains(key))
      return instanceCache.at(key);
    if (!resolvingInstances.insert(key).second)
      fail(loc, DiagnosticCode::InvalidPlace,
           "recursive generic field layout involving `" + decl.name + "`");
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
    if (decl.representation != hir::Representation::Stack) {
      auto tid = hir::instantiateHeapContainer(p, id, arguments, loc);
      types[p.types[tid].name] = tid;
      instanceCache.emplace(std::move(key), tid);
      resolvingInstances.erase({id, arguments});
      return tid;
    }
    uint32_t offset = 0;
    for (const auto &field : decl.fields) {
      auto tid = substitute(field.type, instance.substitution);
      if (p.containsReferences(tid) && !p.localCarrierType(tid))
        fail(loc, DiagnosticCode::EscapingView,
             "struct field `" + field.name + "` requires a recursive stored-region contract");
      if (!scalar(tid) && p.types[tid].kind != hir::Type::Array && !p.managedFieldType(tid) &&
          !p.supportsStoredReferenceReferent(tid))
        fail(loc, DiagnosticCode::InvalidPlace,
             "generic struct field `" + field.name +
                 "` requires aggregate element ownership support");
      if (p.storageWidth(tid) > 65536 - offset)
        fail(loc, DiagnosticCode::InvalidPlace, "generic struct layout exceeds 65536 cells");
      type.fields.push_back({field.name, tid, offset});
      offset += p.storageWidth(tid);
    }
    type.width = std::max(1u, offset);
    instance.id = static_cast<InstanceId>(p.instances.size());
    auto tid = instance.type = static_cast<TypeId>(p.types.size());
    types[type.name] = tid;
    p.types.push_back(std::move(type));
    p.instances.push_back(std::move(instance));
    instanceCache.emplace(std::move(key), tid);
    resolvingInstances.erase({id, arguments});
    return tid;
  }
  TypeId fixedArray(TypeId element, uint32_t count, Loc loc) {
    if ((!scalar(element) && p.types[element].kind != hir::Type::Array) || !count ||
        count > 65536 || p.storageWidth(element) > 65536 / count)
      fail(loc, DiagnosticCode::InvalidIndex,
           "fixed arrays require scalar leaves and at most 65536 storage cells");
    auto name = p.types[element].name;
    auto bracket = name.find('[');
    name.insert(bracket == std::string::npos ? name.size() : bracket,
                "[" + std::to_string(count) + "]");
    if (types.contains(name))
      return types.at(name);
    auto id = static_cast<TypeId>(p.types.size());
    auto width = count * p.storageWidth(element);
    types[name] = id;
    p.types.push_back({hir::Type::Array, name, None, element, count, width, {}});
    return id;
  }
  const ast::Struct *structDeclaration(TypeId type) const {
    if (structSources.contains(type))
      return structSources.at(type);
    for (const auto &instance : p.instances) {
      if (instance.type != type || instance.declaration < p.genericEnums.size())
        continue;
      const auto &decl = p.genericStructs.at(instance.declaration - p.genericEnums.size());
      for (const auto &source : ast.structs)
        if (source.name == decl.name)
          return &source;
    }
    return nullptr;
  }
  const std::vector<std::string> *initializerMethods(TypeId type) const {
    auto source = structDeclaration(type);
    return source && !source->initializerMethods.empty() ? &source->initializerMethods : nullptr;
  }
  struct IterationProtocol {
    TypeId element;
    std::string length, get;
  };
  struct ViewIteration {
    struct Transformation {
      ViewOperation kind;
      Loc loc;
      std::vector<ast::Expr> args;
    };
    ast::Expr source;
    std::vector<Transformation> operations;
    bool sourceUnique = false, unique = false, elements = false, reverse = false;
  };
  std::optional<ViewIteration> viewIteration(const ast::Expr &expression) {
    const ast::Expr *base = &expression;
    std::vector<const ast::Expr *> chain;
    while (base->kind == ast::Expr::Method && !base->args.empty()) {
      chain.push_back(base);
      base = &base->args.front();
    }
    std::reverse(chain.begin(), chain.end());
    if (chain.empty())
      return std::nullopt;

    bool elements = false;
    size_t start = chain.size();
    for (size_t n = 0; n < chain.size(); ++n) {
      auto operation = findViewOperation(chain[n]->text);
      if (!operation)
        continue;
      if (*operation == ViewOperation::View || *operation == ViewOperation::ViewMut)
        start = n;
      if (*operation == ViewOperation::Elements || *operation == ViewOperation::ElementsMut)
        elements = true;
    }
    if (!elements || start == chain.size())
      return std::nullopt;
    auto root = *findViewOperation(chain[start]->text);
    if (chain[start]->args.size() != 1)
      fail(chain[start]->loc, DiagnosticCode::InvalidRange, "view()/view_mut() take no arguments");
    ViewIteration plan;
    plan.source = chain[start]->args.front();
    plan.unique = root == ViewOperation::ViewMut;
    plan.sourceUnique = plan.unique;
    for (size_t n = start + 1; n < chain.size(); ++n) {
      const auto &call = *chain[n];
      auto operation = findViewOperation(call.text);
      if (!operation)
        fail(call.loc, DiagnosticCode::InvalidRange,
             "unknown view/iterator operation `" + call.text + "`");
      if (*operation == ViewOperation::Elements || *operation == ViewOperation::ElementsMut) {
        if (plan.elements || call.args.size() != 1)
          fail(call.loc, DiagnosticCode::InvalidRange,
               "elements()/elements_mut() take no arguments and end the view chain");
        if (*operation == ViewOperation::ElementsMut && !plan.unique)
          fail(call.loc, DiagnosticCode::InvalidRange, "elements_mut() requires view_mut()");
        plan.unique = *operation == ViewOperation::ElementsMut;
        plan.elements = true;
      } else if (*operation == ViewOperation::Reverse) {
        if (!plan.elements || call.args.size() != 1)
          fail(call.loc, DiagnosticCode::InvalidRange,
               "rev() requires an elements iterator and takes no arguments");
        plan.reverse = !plan.reverse;
      } else {
        if (plan.elements || *operation == ViewOperation::View ||
            *operation == ViewOperation::ViewMut)
          fail(call.loc, DiagnosticCode::InvalidRange,
               "view transformations must precede elements()/elements_mut()");
        plan.operations.push_back({*operation, call.loc, {call.args.begin() + 1, call.args.end()}});
      }
    }
    if (!plan.elements)
      fail(expression.loc, DiagnosticCode::InvalidRange,
           "a for view requires elements()/elements_mut()");
    return plan;
  }

  std::pair<ast::Expr, std::vector<ast::Expr>>
  viewIndices(const ViewIteration &plan, TypeId owner, const ast::Expr &source,
              const ast::Expr &index, const std::string &lengthMethod,
              std::vector<ast::Stmt> &bindings, Loc loc, const std::string &prefix) {
    using A = ast::Expr;
    auto name = [&](const std::string &text) { return A{A::Name, loc, text, {}}; };
    auto number = [&](uint64_t value) {
      return A{A::Int, loc, std::to_string(value) + "usize", {}};
    };
    auto binary = [&](std::string op, A left, A right) {
      return A{A::Binary, loc, std::move(op), {std::move(left), std::move(right)}};
    };
    size_t serial = 0;
    auto bind = [&](A expression) {
      auto text = prefix + std::to_string(serial++);
      ast::Stmt statement{};
      statement.kind = ast::Stmt::Let;
      statement.loc = expression.loc;
      statement.name = text;
      statement.annotated = true;
      statement.annotation = {"usize", 0, expression.loc};
      statement.expr = std::move(expression);
      bindings.push_back(std::move(statement));
      return name(text);
    };
    struct Axis {
      size_t original;
      A start, length;
      bool reverse = false;
    };
    std::vector<Axis> axes;
    auto type = owner;
    while (p.types.at(type).kind == hir::Type::Array) {
      axes.push_back({axes.size(), number(0), number(p.types.at(type).count)});
      type = p.types.at(type).element;
    }
    if (axes.empty())
      axes.push_back({0, number(0), bind(A{A::Method, loc, lengthMethod, {source}})});
    auto rank = axes.size();
    auto axisNumber = [&](const A &argument) -> size_t {
      auto value = expr(argument);
      if (value.kind != hir::Expr::Constant || !integer(value.type) || value.text.starts_with('-'))
        fail(argument.loc, DiagnosticCode::InvalidRange,
             "view axes must be nonnegative compile-time integer constants");
      auto axis = std::stoull(value.text);
      if (axis >= rank)
        fail(argument.loc, DiagnosticCode::InvalidRange, "view axis is outside the source rank");
      return static_cast<size_t>(axis);
    };
    for (const auto &call : plan.operations) {
      auto operation = call.kind;
      auto arity = call.args.size();
      if (operation == ViewOperation::Transpose) {
        if (arity)
          fail(call.loc, DiagnosticCode::InvalidRange, "transpose() takes no arguments");
        std::reverse(axes.begin(), axes.end());
      } else if (operation == ViewOperation::ReverseAxis) {
        if (arity != 1)
          fail(call.loc, DiagnosticCode::InvalidRange, "reverse_axis() requires one axis");
        auto &axis = axes.at(axisNumber(call.args.at(0)));
        axis.reverse = !axis.reverse;
      } else if (operation == ViewOperation::PermuteAxes) {
        if (arity != rank)
          fail(call.loc, DiagnosticCode::InvalidRange,
               "permute_axes() requires one distinct axis per dimension");
        std::vector<Axis> reordered;
        std::set<size_t> seen;
        for (size_t n = 0; n < call.args.size(); ++n) {
          auto axis = axisNumber(call.args[n]);
          if (!seen.insert(axis).second)
            fail(call.args[n].loc, DiagnosticCode::InvalidRange,
                 "permute_axes() cannot duplicate an axis");
          reordered.push_back(axes.at(axis));
        }
        axes = std::move(reordered);
      } else if (operation == ViewOperation::Slice) {
        if (arity != 3)
          fail(call.loc, DiagnosticCode::InvalidRange, "slice() requires axis, start and end");
        auto &axis = axes.at(axisNumber(call.args.at(0)));
        auto start = bind(call.args.at(1)), end = bind(call.args.at(2));
        ast::Stmt check{};
        check.kind = ast::Stmt::If;
        check.loc = call.loc;
        check.expr = binary("||", binary(">", start, end), binary(">", end, axis.length));
        ast::Stmt panic{};
        panic.kind = ast::Stmt::Expression;
        panic.loc = call.loc;
        panic.expr = {A::Call,
                      call.loc,
                      "panic",
                      {{A::String, call.loc, "view slice bounds out of range", {}}}};
        check.body.push_back(std::move(panic));
        bindings.push_back(std::move(check));
        axis.start =
            bind(binary("+", axis.start, axis.reverse ? binary("-", axis.length, end) : start));
        axis.length = bind(binary("-", end, start));
      }
    }
    A total = number(1);
    for (const auto &axis : axes)
      total = binary("*", total, axis.length);
    total = bind(std::move(total));
    A flat = plan.reverse ? binary("-", binary("-", total, number(1)), index) : index;
    std::vector<A> coordinates(rank);
    for (size_t axis = 0; axis < rank; ++axis) {
      A stride = number(1);
      for (size_t next = axis + 1; next < rank; ++next)
        stride = binary("*", stride, axes[next].length);
      auto coordinate = binary("%", binary("/", flat, stride), axes[axis].length);
      if (axes[axis].reverse)
        coordinate = binary("-", binary("-", axes[axis].length, number(1)), coordinate);
      coordinates[axes[axis].original] = binary("+", axes[axis].start, coordinate);
    }
    return {std::move(total), std::move(coordinates)};
  }
  IterationProtocol iterationProtocol(TypeId owner, bool unique, Loc loc) {
    auto shape = p.types.at(owner);
    if (shape.kind == hir::Type::Array || shape.kind == hir::Type::Slice)
      return {shape.element, "len", unique ? "get_mut" : "get"};
    auto declaration = structDeclaration(owner);
    if (!declaration || declaration->iterationMethods.empty())
      fail(loc, DiagnosticCode::InvalidRange,
           "collection for requires an array, slice or a type declaring @iteration");
    const auto recipe = declaration->iterationMethods;
    if (unique && recipe.size() != 3)
      fail(loc, DiagnosticCode::InvalidRange, "@iteration does not declare mutable element access");
    auto lengthName = recipe.at(0), getName = recipe.at(unique ? 2 : 1);
    ensureMethods(owner, lengthName);
    ensureMethods(owner, getName);
    auto key = p.types.at(owner).name + "::";
    if (!functions.contains(key + lengthName) || !functions.contains(key + getName))
      fail(loc, DiagnosticCode::InvalidRange, "@iteration names an unknown method");
    auto length = p.functions.at(functions.at(key + lengthName));
    auto get = p.functions.at(functions.at(key + getName));
    auto access = unique ? Access::Unique : Access::Shared;
    const auto result = p.types.at(get.result);
    bool valid = length.pub && get.pub && length.params.size() == 1 && length.owner == owner &&
                 length.access == Access::Shared && length.result == USize &&
                 length.resultOrigin == None && get.params.size() == 2 && get.owner == owner &&
                 get.access == access && p.symbols.at(get.params.at(1)).type == USize &&
                 !p.symbols.at(get.params.at(1)).reference && get.resultBorrowedContainer &&
                 get.resultAccess == access && get.resultOrigin == get.params.front() &&
                 get.additionalResultOrigins.empty() && result.kind == hir::Type::Enum &&
                 result.variants.size() == 2;
    TypeId element = None;
    bool some = false, none = false;
    for (const auto &variant : result.variants) {
      if (variant.name == "None" && variant.payload.empty())
        none = true;
      if (variant.name == "Some" && variant.payload.size() == 1) {
        const auto &payload = p.types.at(variant.payload.front());
        some = payload.kind == hir::Type::Reference && payload.referenceAccess == access;
        if (some)
          element = payload.element;
      }
    }
    if (!valid || !some || !none)
      fail(loc, DiagnosticCode::InvalidRange,
           "@iteration requires len(shared self) -> usize and get(shared/unique self, usize) "
           "-> Option<&T>/Option<&mut T> bounded by self");
    return {element, std::move(lengthName), std::move(getName)};
  }
  void ensureMethods(TypeId owner, const std::string &name) {
    if (!methodsDeclared.insert({owner, name}).second)
      return;
    if (p.types[owner].kind == hir::Type::Array || p.types[owner].kind == hir::Type::Slice) {
      if (!scalar(p.types[owner].element))
        return;
      auto intrinsic = findIntrinsic("slice." + name);
      if (!elementSliceIntrinsic(intrinsic))
        return;

      ast::Function method{};
      method.name = name;
      method.trusted = method.pub = true;
      method.intrinsic = std::string(intrinsicInfo(intrinsic).name);
      auto capability =
          containerMutableBorrowIntrinsic(intrinsic) ? Access::Unique : Access::Shared;
      TypeSyntax receiverType{};
      receiverType.name = p.types[owner].name;
      method.params.push_back({"self", receiverType, capability, {}});
      if (intrinsic == IntrinsicId::Slice_get || intrinsic == IntrinsicId::Slice_get_mut)
        method.params.push_back({"index", TypeSyntax{"usize", 0, {}}, Access::Own, {}});
      TypeSyntax payload{};
      payload.name = p.types[p.types[owner].element].name;
      payload.reference = true;
      payload.mutableReference = capability == Access::Unique;
      method.result.name = "Option";
      method.result.arguments = {payload};
      synthesizedMethods.push_back(std::move(method));
      auto savedPrivilege = privileged;
      declareFunction(synthesizedMethods.back(), owner);
      privileged = savedPrivilege;
      return;
    }
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
  std::map<std::pair<TypeId, Access>, TypeId> referenceTypes;
  TypeId storedReferenceType(TypeId element, Loc loc, Access access = Access::Shared) {
    if (!p.supportsStoredReferenceReferent(element))
      fail(loc, DiagnosticCode::EscapingView,
           "stored references require a scalar, plain fixed stack aggregate or non-protocol heap "
           "wrapper");
    auto key = std::make_pair(element, access);
    if (referenceTypes.contains(key))
      return referenceTypes.at(key);
    auto id = static_cast<TypeId>(p.types.size());
    hir::Type type{hir::Type::Reference,
                   (access == Access::Unique ? "&mut " : "&") + p.types[element].name};
    type.element = element;
    type.referenceAccess = access;
    p.types.push_back(std::move(type));
    referenceTypes[key] = id;
    return id;
  }
  void storeReference(hir::Expr &expression) {
    bool alias = expression.kind == hir::Expr::Variable && p.symbols[expression.ref].reference;
    if (expression.kind != hir::Expr::Borrow && !alias)
      return;
    auto access = alias                                         ? p.symbols[expression.ref].access
                  : expression.borrowKind == BorrowKind::Shared ? Access::Shared
                                                                : Access::Unique;
    hir::Expr stored{};
    stored.kind = hir::Expr::StoreReference;
    stored.loc = expression.loc;
    stored.type = storedReferenceType(expression.type, expression.loc, access);
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
        if (!p.heap(tid) && p.containsProtocolOwnedFields(tid))
          fail(loc, DiagnosticCode::AllocationContract,
               "enum payloads with protocolled fields require a payload state contract");
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
  unsigned unsafeDepth = 0;
  std::map<std::string, const ast::ForeignType *> foreignAliases;
  std::set<std::string> resolvingAliases;
  void requireUnsafe(Loc loc, const std::string &operation) {
    if (current)
      current->effects.unsafeEffect = true;
    if (!unsafeDepth)
      DiagnosticEngine()
          .error(DiagnosticCode::UnsafeRequired, loc, operation + " requires unsafe")
          .help("isolate this operation in unsafe { ... } and uphold its memory contract")
          .emit();
  }
  uint32_t nextLoop = 0;
  RegionId nextRegion = 0;
  std::vector<uint32_t> loops;
  void duplicate(Loc l, const std::string &n) {
    fail(l, DiagnosticCode::DuplicateDeclaration, "duplicate declaration `" + n + "`");
  }
  TypeId getType(const TypeSyntax &s, bool allowReference = false) {
    if (!s.dimensions.empty()) {
      auto base = s;
      base.count = 0;
      base.countName.clear();
      base.dimensions.clear();
      base.reference = false;
      base.mutableReference = false;
      auto element = getType(base);
      std::vector<std::pair<int, std::string>> extents{{s.count, s.countName}};
      extents.insert(extents.end(), s.dimensions.begin(), s.dimensions.end());
      for (auto it = extents.rbegin(); it != extents.rend(); ++it) {
        auto count = it->first;
        if (!it->second.empty()) {
          auto entry = binding(it->second, s.loc);
          if (!entry || !entry->constant)
            fail(s.loc, DiagnosticCode::InvalidConstant,
                 "array size requires a previously declared constant");
          const auto &value = p.constants[entry->id].value;
          if (!integer(value.type) || value.text.starts_with('-') ||
              std::stoull(value.text) > 65536)
            fail(s.loc, DiagnosticCode::InvalidConstant, "array size must be in 1..65536");
          count = static_cast<int>(std::stoull(value.text));
        }
        element = fixedArray(element, count, s.loc);
      }
      if (s.reference && !allowReference)
        fail(s.loc, DiagnosticCode::EscapingView, "array references require a borrow contract");
      return element;
    }
    if (s.functionPointer) {
      std::vector<TypeId> parameters;
      for (size_t n = 0; n < s.arguments.size(); ++n) {
        const auto &syntax = s.arguments[n];
        if (syntax.reference || syntax.count)
          fail(syntax.loc, DiagnosticCode::InvalidCAbi,
               "C callbacks require native scalars or raw pointers, without safe references");
        auto tid = getType(syntax);
        if (!(p.cScalar(tid) || (n + 1 == s.arguments.size() && tid == Unit)))
          fail(syntax.loc, DiagnosticCode::InvalidCAbi, "invalid C callback signature");
        parameters.push_back(tid);
      }
      auto result = parameters.back();
      parameters.pop_back();
      for (TypeId id = 0; id < p.types.size(); ++id)
        if (p.functionPointer(id) && p.types[id].element == result &&
            p.types[id].parameters == parameters)
          return id;
      std::string name = "extern C fn(";
      for (auto tid : parameters)
        name += p.types[tid].name + ",";
      name += ")->" + p.types[result].name;
      TypeId id = p.types.size();
      hir::Type pointer{hir::Type::FunctionPointer, name};
      pointer.element = result;
      pointer.parameters = std::move(parameters);
      p.types.push_back(std::move(pointer));
      return id;
    }
    if (s.rawPointer) {
      const auto &referent = s.arguments.at(0);
      TypeId element =
          !referent.rawPointer && referent.arguments.empty() && types.contains(referent.name)
              ? types.at(referent.name)
              : getType(referent);
      if (!p.cScalar(element) && element != Unit &&
          p.types[element].kind != hir::Type::ForeignRecord &&
          p.types[element].kind != hir::Type::ForeignOpaque)
        fail(s.loc, DiagnosticCode::InvalidRawPointer,
             "raw pointers require C-compatible referents");
      auto name = std::string(s.mutablePointer ? "*mut " : "*const ") + p.types[element].name;
      if (types.contains(name))
        return types.at(name);
      TypeId id = p.types.size();
      types[name] = id;
      hir::Type pointer{hir::Type::RawPointer, name};
      pointer.element = element;
      pointer.mutablePointer = s.mutablePointer;
      p.types.push_back(std::move(pointer));
      return id;
    }
    if (foreignAliases.contains(s.name)) {
      if (!resolvingAliases.insert(s.name).second)
        fail(s.loc, DiagnosticCode::InvalidCAbi, "cyclic C type alias");
      auto target = getType(foreignAliases.at(s.name)->target);
      resolvingAliases.erase(s.name);
      if (!p.cScalar(target))
        fail(s.loc, DiagnosticCode::InvalidCAbi, "C aliases require scalar or pointer types");
      types[s.name] = target;
      return target;
    }
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
    if (s.count && s.arguments.empty() && activeTypeParameters.contains(s.name)) {
      auto resolved = s;
      resolved.name =
          p.types[substitute({true, activeTypeParameters.at(s.name)}, activeSubstitution)].name;
      return getType(resolved, allowReference);
    }
    if (activeSelf != None && !s.count && s.arguments.empty() &&
        (s.name == "self" || s.name == activeOwner))
      return activeSelf;
    if (genericNames.contains(s.name) || genericStructNames.contains(s.name)) {
      std::vector<TypeId> arguments;
      for (const auto &argument : s.arguments) {
        if (argument.reference) {
          arguments.push_back(
              storedReferenceType(getType(argument, true), argument.loc,
                                  argument.mutableReference ? Access::Unique : Access::Shared));
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
    ensureStructLayout(t, s.loc);
    if (t == RawPtr && !privileged)
      fail(s.loc, DiagnosticCode::RestrictedOperation,
           "RawPtr is restricted to bundled runtime bindings");
    if (!s.count)
      return t;
    if (!scalar(t) && !(s.count > 0 && p.types[t].kind == hir::Type::Array))
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
    return fixedArray(t, static_cast<uint32_t>(s.count), s.loc);
  }
  void declareFunction(const ast::Function &a, TypeId ownerOverride = None) {
    if ((a.kind != FunctionKind::Normal && a.kind != FunctionKind::Async) ||
        (a.kind == FunctionKind::Normal && a.effects.asynchronous))
      fail(
          a.loc, DiagnosticCode::FutureFeature,
          "async/coroutine function metadata requires suspension and frame verification, not yet implemented");
    if (ownerOverride != None &&
        (a.external || a.releases ||
         (!a.intrinsic.empty() &&
          (!a.trusted || intrinsicInfo(findIntrinsic(a.intrinsic)).lowering !=
                             IntrinsicLowering::ContainerStorage))))
      fail(a.loc, DiagnosticCode::RestrictedOperation,
           "generic impl methods currently require ordinary checked bodies");
    privileged = a.trusted;
    if (a.foreignC && (a.name.starts_with("isac_") || a.name.starts_with("__isac_")))
      fail(a.loc, DiagnosticCode::InvalidCAbi, "runtime and compiler C symbols are reserved");
    hir::Function f{};
    f.trusted = a.trusted;
    f.external = a.external;
    f.foreignC = a.foreignC;
    f.variadic = a.variadic;
    f.fixedParams = a.params.size();
    f.cLinkName = a.cLinkName;
    f.unsafeFunction = a.unsafeFunction;
    f.intrinsic = a.intrinsic;
    f.kind = a.kind;
    f.intrinsicId =
        a.foreignC ? IntrinsicId::None : findIntrinsic(a.external ? a.name : a.intrinsic);
    if (!a.intrinsic.empty() && f.intrinsicId == IntrinsicId::None)
      fail(a.loc, DiagnosticCode::RestrictedOperation, "unknown intrinsic `" + a.intrinsic + "`");
    f.effects = intrinsicInfo(f.intrinsicId).effects;
    f.effects.asynchronous = a.kind == FunctionKind::Async;
    f.effects.unsafeEffect = a.foreignC || a.unsafeFunction;
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
    if (ownerOverride != None) {
      f.owner = ownerOverride;
      if (p.dynamicContainer(f.owner) && !a.trusted)
        fail(a.loc, DiagnosticCode::RestrictedOperation,
             "bundled heap types cannot be extended by application impls");
    } else if (!a.owner.empty()) {
      if (!types.contains(a.owner) || (p.types[types[a.owner]].kind != hir::Type::Structure &&
                                       types[a.owner] != Str && !scalar(types[a.owner])))
        fail(a.loc, DiagnosticCode::UnknownName, "unknown impl struct");
      f.owner = types[a.owner];
      if ((p.heap(f.owner) || f.owner == Str) && !a.trusted)
        fail(a.loc, DiagnosticCode::RestrictedOperation,
             "bundled heap types cannot be extended by application impls");
    }
    if (a.foreignC && a.result.reference)
      fail(a.loc, DiagnosticCode::InvalidCAbi,
           "C borrowed returns require an explicit origin contract");
    f.resultAccess =
        containerMutableBorrowIntrinsic(f.intrinsicId) ? Access::Unique : Access::Shared;
    if (a.result.mutableReference && !containerMutableBorrowIntrinsic(f.intrinsicId))
      fail(a.result.loc, DiagnosticCode::EscapingView,
           "unique reference returns require exclusive result-region contracts");
    f.result = getType(a.result, a.result.reference);

    if (!a.result.reference && f.intrinsic.empty() && !f.external &&
        p.containsReferences(f.result) && !p.referenceCapability(f.result, Access::Shared) &&
        p.borrowedResultLayoutSupported(f.result) &&
        std::any_of(a.params.begin(), a.params.end(), [&](const auto &parameter) {
          if (parameter.type.mutableReference ||
              (parameter.name == "self" && parameter.access == Access::Unique))
            return true;

          if (parameter.name == "self" || parameter.type.reference)
            return false;
          auto type = getType(parameter.type);
          if (p.types[type].kind != hir::Type::Structure)
            return false;
          const auto leaves = p.borrowedReferences(type);
          return std::any_of(leaves.begin(), leaves.end(), [](const auto &leaf) {
            return leaf.reference.capability == Access::Unique;
          });
        }))
      f.resultAccess = Access::Unique;
    if (p.types[f.result].kind == hir::Type::ForeignRecord ||
        p.types[f.result].kind == hir::Type::ForeignOpaque)
      fail(a.loc, DiagnosticCode::InvalidCAbi,
           "C records and opaque types may only be used through raw pointers");
    if (f.result == Str && f.intrinsic.empty())
      fail(f.loc, DiagnosticCode::EscapingView,
           "returning str views requires lifetime contracts, outside this PoC");
    f.resultBorrowedContainer = !a.result.reference && p.containsReferences(f.result);
    if (f.resultBorrowedContainer && f.resultAccess == Access::Shared &&
        !p.referenceCapability(f.result, Access::Shared))
      fail(a.result.loc, DiagnosticCode::EscapingView,
           "unique payload returns require an exclusive storage result contract");
    if (f.resultBorrowedContainer && !p.borrowedResultLayoutSupported(f.result))
      fail(a.result.loc, DiagnosticCode::EscapingView,
           "borrowed result requires a supported positional reference and owned-field contract");
    if (!a.result.reference && p.linear(f.result) && f.intrinsic.empty() &&
        ((!p.heap(f.result) && p.needsDrop(f.result) &&
          p.types[f.result].kind != hir::Type::Structure &&
          p.types[f.result].kind != hir::Type::Enum) ||
         p.types[f.result].protocol != None))
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
      if (p.types[t].kind == hir::Type::ForeignRecord ||
          p.types[t].kind == hir::Type::ForeignOpaque || (ap.type.reference && p.rawPointer(t)))
        fail(ap.loc, DiagnosticCode::InvalidCAbi,
             "C types require a raw-pointer ABI, without safe references");
      if (ap.type.reference && ((f.external && !f.foreignC) || !f.intrinsic.empty() ||
                                p.types[t].protocol != None || t == Str || t == RawPtr))
        fail(ap.loc, DiagnosticCode::EscapingView,
             "this reference parameter requires an unsupported ABI or protocol contract");
      bool inspectPayload = self && ap.access == Access::Shared && !ap.type.reference &&
                            !f.external && f.intrinsic.empty() && p.opaqueBorrowedPayload(t);
      if (inspectPayload && (a.result.reference || f.resultBorrowedContainer))
        fail(a.result.loc, DiagnosticCode::EscapingView,
             "a tag-only payload inspection cannot return borrowed values");
      if (self && p.containsReferences(t) && !p.referenceCapability(t, Access::Shared) &&
          (a.result.reference || f.resultBorrowedContainer))
        fail(a.result.loc, DiagnosticCode::EscapingView,
             "a Unique-bearing receiver requires a separate borrowed-result transfer contract");
      if (p.containsReferences(t) && !inspectPayload &&
          (ap.type.reference || (self && !p.borrowedReceiverSupported(t, ap.access)) ||
           f.external || !f.intrinsic.empty() || !p.borrowedInputSupported(t)))
        fail(ap.loc, DiagnosticCode::EscapingView,
             "borrowed container parameters require payload origin contracts");
      if (t == Unit)
        fail(ap.loc, DiagnosticCode::TypeMismatch, "unit parameter is not supported");
      auto access = ap.type.reference ? (ap.type.mutableReference ? Access::Unique : Access::Shared)
                                      : ap.access;
      auto id = add(ap.name, t, ap.loc, access, true, self);
      f.params.push_back(id);
      if (inspectPayload)
        f.payloadInspectionParameters.push_back(id);
      else if (p.containsReferences(t)) {
        for (const auto &leaf : p.borrowedReferences(t)) {
          auto origin = static_cast<SymbolId>(p.symbols.size());
          auto region = nextRegion++;
          hir::Symbol symbol{
              origin, ap.name, leaf.reference.referent, leaf.reference.capability, true,
              false,  ap.loc};
          symbol.reference = true;
          symbol.region = region;
          p.symbols.push_back(std::move(symbol));
          f.payloadParameterRegions.push_back({id, origin, region, leaf.path});
        }
      }
      if (ap.type.reference ||
          (self && !p.containsReferences(t) && (a.result.reference || f.resultBorrowedContainer) &&
           (access == Access::Shared ||
            (f.resultBorrowedContainer && f.resultAccess == Access::Unique &&
             access == Access::Unique) ||
            containerMutableBorrowIntrinsic(f.intrinsicId)) &&
           p.types[t].protocol == None)) {
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
      if (a.result.reference)
        for (const auto &region : f.parameterRegions) {
          const auto &parameter = p.symbols[region.parameter];
          if (parameter.access == f.resultAccess &&
              p.borrowResultCompatible(parameter.type, f.result)) {
            if (f.resultOrigin != None)
              fail(a.result.loc, DiagnosticCode::EscapingView,
                   "direct reference return requires one input origin in this subset");
            f.resultOrigin = parameter.id;
          }
        }
      if (f.resultBorrowedContainer) {
        std::set<hir::ReferenceSource> sources;
        bool missingPosition = false;
        const auto candidates = p.referenceTransferCandidates(f);
        bool ambiguous = f.resultAccess == Access::Unique &&
                         std::any_of(candidates.begin(), candidates.end(),
                                     [](const auto &entry) { return entry.second.size() > 1; });
        for (const auto &[path, inputs] : candidates) {
          missingPosition |= inputs.empty();
          if (f.resultAccess == Access::Unique) {
            if (inputs.empty())
              fail(
                  a.result.loc, DiagnosticCode::EscapingView,
                  "exclusive payload returns require a compatible unique input for every output position");
            if (!ambiguous && p.referenceResultAccess(f, path) == Access::Unique &&
                sources.contains(*inputs.begin()))
              fail(a.result.loc, DiagnosticCode::EscapingView,
                   "exclusive result positions cannot duplicate an input capability");
          }
          sources.insert(inputs.begin(), inputs.end());
        }
        if (ambiguous) {
          for (const auto &source : sources) {
            const auto &parameter = p.symbols[source.parameter];
            if (source.kind != hir::ReferenceSourceKind::Carrier || parameter.reference ||
                parameter.receiver || p.types[parameter.type].kind != hir::Type::Structure ||
                !p.borrowedInputSupported(parameter.type))
              fail(
                  a.result.loc, DiagnosticCode::EscapingView,
                  "exclusive payload returns require exactly one compatible unique input origin per output position");
          }
          f.transferInferencePending = true;
        }
        if (missingPosition && !sources.empty())
          fail(a.result.loc, DiagnosticCode::EscapingView,
               "a returned reference position has no compatible input-region contract");
        if (!f.transferInferencePending && !p.referenceResultPresenceSupported(f, candidates))
          fail(a.result.loc, DiagnosticCode::EscapingView,
               "result presence requires references not guaranteed by the input contract");
        for (const auto &source : sources) {
          std::optional<hir::PayloadResultSource> payload;
          if (source.kind == hir::ReferenceSourceKind::Carrier)
            payload = hir::PayloadResultSource{source.parameter, source.path};
          if (f.resultOrigin == None) {
            f.resultOrigin = source.origin;
            f.resultPayloadSource = payload;
          } else {
            f.additionalResultOrigins.push_back(source.origin);
            if (payload)
              f.additionalPayloadSources.push_back(*payload);
          }
        }
      }
      if (f.resultOrigin == None || f.external ||
          (!f.intrinsic.empty() && !containerBorrowIntrinsic(f.intrinsicId)) ||
          (f.owner != None && p.types[f.owner].protocol != None))
        fail(a.result.loc, DiagnosticCode::EscapingView,
             "reference return requires a compatible shared input-region contract; local owners "
             "cannot escape");
      f.resultRegion = nextRegion++;
      if (f.resultAccess == Access::Unique && f.intrinsic.empty() && !f.transferInferencePending &&
          !p.exclusivePayloadResult(f))
        fail(
            a.result.loc, DiagnosticCode::EscapingView,
            "exclusive payload returns require one direct unique input/output or distinct owned input leaves for every output");
    }
    scopes.pop_back();
    f.consumesSelf = f.owner != None &&
                     (p.types[f.owner].kind == hir::Type::Enum ||
                      f.intrinsicId == IntrinsicId::Box_into_inner) &&
                     !f.params.empty() && p.symbols[f.params[0]].receiver &&
                     f.access == Access::Own;
    if (intrinsicInfo(f.intrinsicId).lowering == IntrinsicLowering::ContainerStorage &&
        !validContainerIntrinsic(p, f))
      fail(a.loc, DiagnosticCode::RestrictedOperation,
           "invalid generic container intrinsic contract");
    if (f.foreignC && !ffi::signature(p, f))
      fail(f.loc, DiagnosticCode::InvalidCAbi,
           "unsupported C ABI signature; use scalars, raw pointers, borrowed scalar "
           "arrays/slices, str or &String; owned values and borrowed results require wrappers");
    if (f.kind == FunctionKind::Async)
      f.futureType = asyncs::declareFuture(p, f);
    p.functions.push_back(f);
    functionSources.push_back({&a, activeTypeParameters, activeSubstitution, ownerOverride});
  }
  SymbolId add(std::string n, TypeId t, Loc l, Access a = Access::Own, bool param = false,
               bool self = false, bool shadow = false) {
    auto key = n + "#" + std::to_string(l.context);
    if (!shadow && scopes.back().contains(key))
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
  bool fieldPlace(const hir::Expr &e) {
    if (e.kind == hir::Expr::Variable)
      return true;
    return e.kind == hir::Expr::Field && !p.heap(e.args[0].type) && fieldPlace(e.args[0]);
  }
  void fieldTransferContract(const hir::Expr &e) {

    const auto *part = &e.args.at(0);
    while (part->kind == hir::Expr::Field) {
      const auto parent = part->args[0].type;
      if (p.types[parent].protocol != None || p.containsReferences(parent))
        fail(e.loc, DiagnosticCode::FutureFeature,
             "deep owned-field transfer through `" + p.types[parent].name +
                 "` requires a recursive reference/protocol projection contract");
      part = &part->args[0];
    }
  }
  hir::Expr initializerExpr(const ast::Expr &value, TypeId target) {
    auto argument = expr(value, target);
    if (argument.type == Str &&
        p.types[target].representation == hir::Representation::StringRecord) {

      ensureMethods(target, "from");
      auto name = p.types[target].name + "::from";
      if (!functions.contains(name))
        fail(value.loc, DiagnosticCode::UnknownName, "String initializer requires String::from");
      auto factory = p.functions.at(functions.at(name));
      if (factory.params.size() != 1 || factory.result != target ||
          p.symbols[factory.params[0]].type != Str || p.symbols[factory.params[0]].reference ||
          p.symbols[factory.params[0]].receiver || factory.resultOrigin != None ||
          factory.unsafeFunction || (factory.trusted && !factory.pub))
        fail(value.loc, DiagnosticCode::ArgumentMismatch,
             "String initializer requires a safe public str -> String constructor");
      hir::Expr call{};
      call.kind = hir::Expr::Call;
      call.ref = factory.id;
      call.loc = value.loc;
      call.type = target;
      call.args.push_back(std::move(argument));
      args(call, factory, 0);
      return call;
    }
    const auto from = argument.type;
    bool lossless = false;
    const auto bits = p.target.pointerWidth;
    if (integer(from) && integer(target)) {
      auto sourceBits = bitWidth(from, bits), targetBits = bitWidth(target, bits);
      lossless = signedType(from) == signedType(target)
                     ? targetBits >= sourceBits
                     : !signedType(from) && signedType(target) && targetBits > sourceBits;
    } else if (from == F32 && target == F64)
      lossless = true;
    else if (integer(from) && floating(target)) {
      auto precision = target == F32 ? 24u : 53u;
      lossless = bitWidth(from, bits) - unsigned(signedType(from)) <= precision;
    }
    const bool reference =
        argument.kind == hir::Expr::Borrow ||
        (argument.kind == hir::Expr::Variable && p.symbols[argument.ref].reference);
    if (from != target && lossless && !reference && !argument.inferredLiteral) {
      hir::Expr cast{};
      cast.kind = hir::Expr::Cast;
      cast.type = target;
      cast.loc = value.loc;
      cast.text = "initializer.lossless";
      cast.args.push_back(std::move(argument));
      return cast;
    }
    coerce(target, argument);
    return argument;
  }
  hir::Expr adjustReference(hir::Expr value, bool storedPlace = false) {
    if (storedPlace || !p.reference(value.type))
      return value;
    hir::Expr reference{};
    reference.kind = hir::Expr::Borrow;
    reference.loc = value.loc;
    reference.type = p.types[value.type].element;
    reference.borrowKind = p.types[value.type].referenceAccess == Access::Shared
                               ? BorrowKind::Shared
                               : BorrowKind::Unique;
    reference.region = nextRegion++;
    reference.args.push_back(std::move(value));
    return reference;
  }
  hir::Expr expr(const ast::Expr &a, TypeId expected = None, bool storedPlace = false) {
    return adjustReference(unadjustedExpr(a, expected), storedPlace);
  }
  hir::Expr unadjustedExpr(const ast::Expr &a, TypeId expected) {
    hir::Expr e{};
    e.loc = a.loc;
    e.text = a.text;
    using A = ast::Expr;
    using H = hir::Expr;
    if (a.kind == A::AtCall && macros::findBuiltin(a.text) == macros::BuiltinId::Thread) {
      if (a.args.size() != 1 || !a.typeArguments.empty())
        fail(a.loc, DiagnosticCode::ArgumentMismatch, "@thread expects one resolved function call");
      auto call = expr(a.args[0]);
      if (call.kind != H::Call)
        fail(a.loc, DiagnosticCode::ArgumentMismatch,
             "@thread requires an ordinary top-level function call");
      const auto &target = p.functions.at(call.ref);
      if (target.owner != None || target.external || target.foreignC || target.unsafeFunction ||
          target.intrinsicId != IntrinsicId::None)
        fail(a.loc, DiagnosticCode::ArgumentMismatch,
             "@thread requires an ordinary safe Isalang function");
      call.kind = H::ThreadCall;
      call.loc = a.loc;
      return call;
    }
    if (a.kind == A::Await ||
        (a.kind == A::AtCall && macros::findBuiltin(a.text) == macros::BuiltinId::BlockOn)) {
      const bool awaiting = a.kind == A::Await;
      if (awaiting && (!current || current->kind != FunctionKind::Async))
        fail(a.loc, DiagnosticCode::FutureFeature,
             "`await` can only be used inside an async function");
      if (a.args.size() != 1 || !a.typeArguments.empty())
        fail(a.loc, DiagnosticCode::ArgumentMismatch,
             "Future consumption expects one value argument");
      auto value = expr(a.args[0]);
      if (value.kind == H::Borrow ||
          (value.kind == H::Variable && p.symbols.at(value.ref).reference))
        fail(a.loc, DiagnosticCode::TypeMismatch,
             "await/block_on consumes an owned Future, not a borrowed Future");
      const auto producer = p.types.at(value.type).futureFunction;
      if (producer == None)
        fail(a.loc, DiagnosticCode::TypeMismatch,
             "await/block_on requires a Future produced by an async function");
      if (value.kind == H::Variable) {
        hir::Expr move{};
        move.kind = H::Move;
        move.type = value.type;
        move.loc = value.loc;
        move.args.push_back(std::move(value));
        value = std::move(move);
      }
      e.kind = awaiting ? H::Await : H::BlockOn;
      e.ref = producer;
      e.type = p.functions.at(producer).result;
      e.args.push_back(std::move(value));
      return e;
    }
    if (a.kind == A::Unsafe) {
      ++unsafeDepth;
      e.kind = H::Unsafe;
      e.args.push_back(expr(a.args.at(0), expected));
      e.type = e.args[0].type;
      --unsafeDepth;
      return e;
    }
    if (a.kind == A::Dereference) {
      auto pointer = expr(a.args.at(0));

      if ((pointer.kind == H::Variable && p.symbols[pointer.ref].reference) ||
          pointer.kind == H::Borrow) {
        pointer.loc = a.loc;
        return pointer;
      }
      requireUnsafe(a.loc, "raw pointer dereference");
      if (!p.rawPointer(pointer.type) || p.types[pointer.type].element == Unit ||
          p.types[p.types[pointer.type].element].kind == hir::Type::ForeignOpaque)
        fail(a.loc, DiagnosticCode::InvalidRawPointer,
             "dereference requires a pointer to a sized C type");
      e.kind = H::RawPlace;
      e.type = p.types[pointer.type].element;
      ensureStructLayout(e.type, a.loc);
      e.args.push_back(std::move(pointer));
      return e;
    }
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
      auto id = getType(a.typeArguments.empty() ? syntax : a.typeArguments.at(0));
      auto &t = p.types[id];
      unsigned word = p.target.pointerWidth / 8;
      uint64_t size = 0;
      unsigned alignment = 1;
      if (id == Unit)
        size = 0;
      else if (p.rawPointer(id) || p.functionPointer(id) || t.kind == hir::Type::ForeignRecord) {
        auto native = ffi::layout(p, id);
        size = native.first;
        alignment = native.second;
      } else if (t.kind == hir::Type::ForeignOpaque)
        fail(a.loc, DiagnosticCode::InvalidCAbi, "opaque C type has no known size or alignment");
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
      if (a.args[0].kind == A::Name && functions.contains(a.args[0].text) &&
          !binding(a.args[0].text, a.args[0].loc)) {
        const auto &function = p.functions[functions.at(a.args[0].text)];
        if (a.text != "shared" || !function.foreignC || function.variadic ||
            !ffi::signature(p, function))
          fail(a.loc, DiagnosticCode::InvalidCAbi,
               "function addresses require an extern C function and shared address syntax");
        for (auto id : function.params) {
          const auto &param = p.symbols[id];
          if (param.reference || !p.cScalar(param.type))
            fail(a.loc, DiagnosticCode::InvalidCAbi,
                 "callbacks cannot use adapted safe-reference parameters");
        }

        TypeId tid = None;
        std::vector<TypeId> params;
        for (auto id : function.params)
          params.push_back(p.symbols[id].type);
        for (TypeId id = 0; id < p.types.size(); ++id)
          if (p.functionPointer(id) && p.types[id].element == function.result &&
              p.types[id].parameters == params)
            tid = id;
        if (tid == None) {
          tid = p.types.size();
          hir::Type pointer{hir::Type::FunctionPointer, "extern C fn " + function.name};
          pointer.element = function.result;
          pointer.parameters = std::move(params);
          p.types.push_back(std::move(pointer));
        }
        e.kind = H::FunctionAddress;
        e.type = tid;
        e.ref = function.id;
        return e;
      }
      e.kind = H::Borrow;
      auto stored = expr(a.args.at(0), None, true);
      if (stored.kind == H::Field && p.reference(stored.type) &&
          p.types[stored.type].referenceAccess == Access::Unique && fieldPlace(stored)) {
        e.type = p.types[stored.type].element;
        e.args.push_back(std::move(stored));
        e.borrowKind = a.text == "unique" ? BorrowKind::Unique : BorrowKind::Shared;
        e.region = nextRegion++;
        return e;
      }
      e.args.push_back(adjustReference(std::move(stored)));
      const auto &source = e.args[0];
      bool projection = (source.kind == H::Field && fieldPlace(source) &&
                         (scalar(source.type) || p.ownedFieldType(source.type) ||
                          p.types[source.type].kind == hir::Type::Array)) ||
                        (source.kind == H::Index &&
                         (scalar(source.type) || p.types[source.type].kind == hir::Type::Array));
      if (source.kind != H::Variable && !projection)
        fail(a.loc, DiagnosticCode::InvalidPlace,
             "borrow requires a named owner or a supported direct field/element projection");
      if (projection && a.text == "unique" && !p.ownedFieldType(source.type) &&
          p.types[source.type].kind != hir::Type::Array && source.kind != H::Index)
        fail(a.loc, DiagnosticCode::InvalidPlace,
             "unique projection borrows require exclusive place contracts");
      e.type = e.args[0].type;
      e.borrowKind = a.text == "unique" ? BorrowKind::Unique : BorrowKind::Shared;
      e.region = nextRegion++;
      return e;
    }
    if (a.kind == A::BraceInit ||
        (a.kind == A::Block && a.body.empty() && expected != None &&
         (p.types[expected].kind == hir::Type::Array || initializerMethods(expected)))) {
      TypeId target = expected;
      if (!a.text.empty()) {
        TypeSyntax syntax{a.text, 0, a.loc};
        syntax.arguments = a.typeArguments;
        target = getType(syntax);
        if (expected != None)
          same(expected, target, a.loc);
      }
      if (target == None)
        fail(a.loc, DiagnosticCode::TypeMismatch,
             "brace initialization requires a type name or an expected array/collection type");
      auto values = a.args;
      auto names = a.initializerNames;
      if (a.kind == A::Block && values.size() == 1 && values[0].kind == A::Name &&
          values[0].text == "unit")
        values.clear();
      if (p.types[target].kind == hir::Type::Array) {
        if (std::any_of(names.begin(), names.end(), [](const auto &name) { return !name.empty(); }))
          fail(a.loc, DiagnosticCode::TypeMismatch,
               "array initialization requires positional elements");
        if (values.size() != p.types[target].count)
          fail(a.loc, DiagnosticCode::TypeMismatch,
               "array initializer length does not match its type");
        e.kind = H::Array;
        e.type = target;
        auto element = p.types[target].element;
        for (const auto &value : values) {
          auto argument = initializerExpr(value, element);
          e.args.push_back(std::move(argument));
        }
        return e;
      }
      if (auto recipe = initializerMethods(target)) {
        auto factoryName = recipe->at(0), insertionName = recipe->at(1);
        ensureMethods(target, factoryName);
        ensureMethods(target, insertionName);
        auto key = p.types[target].name + "::";
        if (!functions.contains(key + factoryName) || !functions.contains(key + insertionName))
          fail(a.loc, DiagnosticCode::UnknownName,
               "@initializer names an unknown factory or insertion method");
        auto factory = p.functions.at(functions.at(key + factoryName));
        auto insertion = p.functions.at(functions.at(key + insertionName));
        if (!factory.params.empty() || factory.result != target || factory.resultOrigin != None ||
            insertion.params.empty() || !p.symbols[insertion.params[0]].receiver ||
            insertion.access != Access::Unique ||
            (insertion.params.size() != 2 && insertion.params.size() != 3))
          fail(
              a.loc, DiagnosticCode::ArgumentMismatch,
              "@initializer requires a zero-argument owned factory and a unique insertion with one or two owned arguments");
        for (size_t n = 1; n < insertion.params.size(); ++n)
          if (p.symbols[insertion.params[n]].reference)
            fail(a.loc, DiagnosticCode::ArgumentMismatch,
                 "@initializer insertion arguments must be owned");

        e.kind = H::Block;
        e.type = target;
        scopes.emplace_back();
        auto id = add("$initializer" + std::to_string(p.symbols.size()), target, a.loc, Access::Own,
                      false, false, true);
        hir::Stmt binding{};
        binding.kind = hir::Stmt::Let;
        binding.symbol = id;
        binding.loc = a.loc;
        binding.expr = expr(ast::Expr{A::Call, a.loc, key + factoryName, {}});
        e.body.push_back(std::move(binding));
        H receiver{};
        receiver.kind = H::Variable;
        receiver.ref = id;
        receiver.type = target;
        receiver.loc = a.loc;
        size_t at = 0;
        for (size_t n = 0; n < values.size(); ++n) {
          const auto &name = names.empty() ? std::string{} : names.at(at++);
          std::vector<ast::Expr> entry;
          if (insertion.params.size() == 2) {
            if (!name.empty())
              fail(values[n].loc, DiagnosticCode::TypeMismatch,
                   "sequence initializer requires positional elements");
            entry.push_back(values[n]);
          } else {
            if (name != ":")
              fail(values[n].loc, DiagnosticCode::TypeMismatch,
                   "map initializer requires key => value entries");
            if (name == ":") {
              entry.push_back(values[n++]);
              entry.push_back(values.at(n));
            }
          }
          H call{};
          call.kind = H::Method;
          call.ref = insertion.id;
          call.type = insertion.result;
          call.loc = entry[0].loc;
          call.args.push_back(receiver);
          for (size_t arg = 0; arg < entry.size(); ++arg)
            call.args.push_back(
                initializerExpr(entry[arg], p.symbols[insertion.params[arg + 1]].type));
          args(call, insertion, 0);
          if (insertion.unsafeFunction || (factory.foreignC && factory.external))
            requireUnsafe(a.loc, "unsafe initializer call");
          if (!privileged && insertion.trusted && !insertion.pub)
            fail(a.loc, DiagnosticCode::RestrictedOperation, "initializer insertion is not public");
          hir::Stmt statement{};
          statement.kind = hir::Stmt::Expression;
          statement.loc = call.loc;
          statement.expr = std::move(call);
          e.body.push_back(std::move(statement));
        }
        H moved{};
        moved.kind = H::Move;
        moved.type = target;
        moved.loc = a.loc;
        moved.args.push_back(receiver);
        e.args.push_back(std::move(moved));
        scopes.pop_back();
        return e;
      }
      if (p.types[target].kind != hir::Type::Structure || p.heap(target))
        fail(a.loc, DiagnosticCode::TypeMismatch, "type does not support brace initialization");
      const auto fields = p.types[target].fields;
      if (values.size() != fields.size() || names.size() != fields.size())
        fail(a.loc, DiagnosticCode::TypeMismatch,
             "struct initializer must specify every field exactly once");
      std::set<uint32_t> initialized;
      e.kind = H::StructInit;
      e.type = target;
      for (size_t n = 0; n < values.size(); ++n) {
        auto field = std::find_if(fields.begin(), fields.end(),
                                  [&](const auto &field) { return field.name == names[n]; });
        if (field == fields.end())
          fail(values[n].loc, DiagnosticCode::UnknownName,
               "unknown initializer field `" + names[n] + "`");
        auto index = static_cast<uint32_t>(field - fields.begin());
        if (!initialized.insert(index).second)
          fail(values[n].loc, DiagnosticCode::DuplicateDeclaration,
               "duplicate initializer field `" + names[n] + "`");
        auto value = initializerExpr(values[n], field->type);
        if (value.kind == H::Borrow)
          fail(value.loc, DiagnosticCode::EscapingView,
               "struct initializer requires owned field values");
        e.fieldPath.push_back(index);
        e.args.push_back(std::move(value));
      }
      return e;
    }
    if (a.kind == A::ArrayRepeat) {
      e.kind = H::ArrayRepeat;
      TypeId element = expected != None && p.types[expected].kind == hir::Type::Array
                           ? p.types[expected].element
                           : None;
      uint32_t count = 0;
      if (a.text == "zero") {
        while (element != None && p.types[element].kind == hir::Type::Array)
          element = p.types[element].element;
        if (element == None || !scalar(element))
          fail(a.loc, DiagnosticCode::TypeMismatch,
               "default array initialization requires scalar elements");
        hir::Expr zero{};
        zero.kind = H::Constant;
        zero.loc = a.loc;
        zero.type = element;
        zero.text = "0";
        e.args = {zero};
        count = p.types[expected].width;
      } else {
        auto value = expr(a.args.at(0), element);
        if (element != None)
          coerce(element, value);
        element = value.type;
        if (!scalar(element))
          fail(a.loc, DiagnosticCode::ImplicitCopy,
               "array repetition requires Copy scalar elements");
        auto length = expr(a.args.at(1));
        coerce(USize, length);
        auto constant = evaluateConstant(length, p.target.pointerWidth);
        auto size = std::stoull(constant.text);
        if (!size || size > 65536)
          fail(a.loc, DiagnosticCode::InvalidIndex, "array size must be in 1..65536");
        count = size;
        e.args = {std::move(value)};
      }
      e.type = a.text == "zero" ? expected
                                : getType({p.types[element].name, static_cast<int>(count), a.loc});
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
      auto literal = parseNumericLiteral(e.text, a.loc);
      auto name = literal.suffix;
      e.text = literal.value;
      if (!name.empty()) {
        if (!types.contains(name) || !numeric(types[name]))
          fail(a.loc, DiagnosticCode::InvalidLiteral, "unknown numeric suffix");
        e.type = types[name];

      } else {
        e.type = expected != None && numeric(expected) && floating(expected) == literal.floating
                     ? expected
                 : literal.floating ? F64
                                    : I32;
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
    if (!a.typeArguments.empty() && a.kind != A::Cast) {
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
      return expr(n, expected);
    }
    if (a.kind == A::Unary && (a.text == "post++" || a.text == "post--")) {
      const auto &operand = a.args.at(0);
      if (operand.kind != A::Name)
        fail(a.loc, DiagnosticCode::InvalidPlace,
             "postfix increment/decrement requires a named integer place");
      auto found = binding(operand.text, operand.loc);
      if (found && found->constant)
        fail(a.loc, DiagnosticCode::ConstantAssignment,
             "cannot assign to constant `" + operand.text + "`");
      auto place = expr(operand);
      if (!integer(place.type))
        fail(a.loc, DiagnosticCode::TypeMismatch,
             "postfix increment/decrement requires an integer");

      e.kind = H::Block;
      e.type = place.type;
      scopes.emplace_back();
      auto old = add("$postfix" + std::to_string(p.symbols.size()), place.type, a.loc);
      hir::Stmt snapshot{};
      snapshot.kind = hir::Stmt::Let;
      snapshot.loc = a.loc;
      snapshot.symbol = old;
      snapshot.expr = place;
      e.body.push_back(std::move(snapshot));
      H value = place;
      value.ref = old;
      H one{};
      one.kind = H::Constant;
      one.type = place.type;
      one.loc = a.loc;
      one.text = "1";
      H sum{};
      sum.kind = H::Binary;
      sum.type = place.type;
      sum.loc = a.loc;
      sum.text = a.text == "post++" ? "+" : "-";
      sum.args = {value, one};
      hir::Stmt update{};
      update.kind = hir::Stmt::Expression;
      update.loc = a.loc;
      update.expr.kind = H::Assign;
      update.expr.type = Unit;
      update.expr.loc = a.loc;
      update.expr.args = {place, std::move(sum)};
      e.body.push_back(std::move(update));
      e.args.push_back(value);
      scopes.pop_back();
      return e;
    }
    if (a.kind == A::Assign && a.args[0].kind == A::Name) {
      auto found = binding(a.args[0].text, a.args[0].loc);
      if (found && found->constant)
        fail(a.loc, DiagnosticCode::ConstantAssignment,
             "cannot assign to constant `" + a.args[0].text + "`");
    }
    for (size_t n = 0; n < a.args.size(); ++n) {
      const auto &x = a.args[n];
      e.args.push_back(
          expr(x,
               a.kind == A::Array && expected != None && p.types[expected].kind == hir::Type::Array
                   ? p.types[expected].element
                   : None,
               n == 0 && (a.kind == A::Assign ||
                          (a.kind == A::Method && (a.text == "move" || a.text == "drop")))));
    }
    switch (a.kind) {
    case A::Cast: {
      auto target =
          a.typeArguments.empty() ? getType({a.text, 0, a.loc}) : getType(a.typeArguments[0]);
      auto &source = e.args[0];
      bool reference = source.kind == H::Borrow ||
                       (source.kind == H::Variable && p.symbols[source.ref].reference);
      if (p.rawPointer(target) || p.rawPointer(source.type)) {
        requireUnsafe(a.loc, "raw pointer cast");
        e.type = target;
        if (reference && p.rawPointer(target)) {
          if (!p.target.littleEndian || source.type != p.types[target].element ||
              !scalar(source.type))
            fail(a.loc, DiagnosticCode::InvalidRawPointer,
                 "raw address conversion requires a same-type scalar reference on a little-endian "
                 "target");
          auto access =
              source.kind == H::Borrow
                  ? (source.borrowKind == BorrowKind::Unique ? Access::Unique : Access::Shared)
                  : p.symbols[source.ref].access;
          if (p.types[target].mutablePointer && access != Access::Unique)
            fail(a.loc, DiagnosticCode::InvalidRawPointer,
                 "mutable pointer requires a unique reference");
          e.kind = H::RawAddress;
        } else {
          if (!((p.rawPointer(target) && (p.rawPointer(source.type) || source.type == USize)) ||
                (target == USize && p.rawPointer(source.type))))
            fail(a.loc, DiagnosticCode::InvalidRawPointer,
                 "pointer casts require a raw pointer or usize");
          e.kind = H::PointerCast;
        }
        return e;
      }
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
      auto text = [&](TypeId t) {
        return t == Str || p.types[t].representation == hir::Representation::StringRecord;
      };
      if ((a.text == "==" || a.text == "!=") && text(e.args[0].type) && text(e.args[1].type)) {
        for (auto &operand : e.args) {
          if (operand.type == Str)
            continue;
          H view{};
          view.kind = H::Method;
          view.loc = operand.loc;
          view.type = Str;
          view.ref = functions.at("String::as_str");
          view.args = {std::move(operand)};
          operand = std::move(view);
        }
        e.kind = H::Call;
        e.ref = functions.at("isac_rt_str_equal");
        e.type = Bool;
        if (a.text == "!=") {
          auto equal = std::move(e);
          e = {};
          e.kind = H::Unary;
          e.loc = a.loc;
          e.text = "!";
          e.type = Bool;
          e.args = {std::move(equal)};
        }
        return e;
      }
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
      e.kind = H::Array;
      e.type = fixedArray(t, static_cast<uint32_t>(e.args.size()), a.loc);
      break;
    }
    case A::Member: {
      auto &t = p.types[e.args[0].type];
      if (e.args[0].kind == H::RawPlace && t.kind == hir::Type::ForeignRecord) {
        auto source = std::move(e.args[0]);
        auto it = std::find_if(t.fields.begin(), t.fields.end(),
                               [&](auto &f) { return f.name == a.text; });
        if (it == t.fields.end())
          fail(a.loc, DiagnosticCode::UnknownName, "unknown C field `" + a.text + "`");
        e.kind = H::RawPlace;
        e.type = it->type;
        e.args = std::move(source.args);
        e.fieldPath = std::move(source.fieldPath);
        e.fieldPath.push_back(it - t.fields.begin());
        return e;
      }
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
        if (e.args[1].text.starts_with('-'))
          fail(a.loc, DiagnosticCode::InvalidIndex, "array index out of bounds");
        auto n = std::stoull(e.args[1].text);
        if (n >= t.count)
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
      if (e.args[0].kind == H::RawPlace) {
        requireUnsafe(a.loc, "raw pointer write");
        if (!p.types[e.args[0].args[0].type].mutablePointer || !p.cScalar(e.args[0].type))
          fail(a.loc, DiagnosticCode::InvalidRawPointer,
               "raw writes require *mut and a scalar C place");
        coerce(e.args[0].type, e.args[1]);
        e.kind = H::Assign;
        return e;
      }
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
      if (p.linear(e.args[0].type) && !(e.args[0].kind == H::Field && fieldPlace(e.args[0]) &&
                                        p.managedFieldType(e.args[0].type)))
        fail(a.loc, DiagnosticCode::InvalidPlace,
             "PoC assignment supports scalar places; use a fresh let for resource moves");
      if (e.args[0].kind == H::Field && p.managedFieldType(e.args[0].type))
        fieldTransferContract(e.args[0]);
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
            !(recv.kind == H::Field && fieldPlace(recv) && p.managedFieldType(recv.type)))
          fail(a.loc, DiagnosticCode::InvalidPlace, "explicit move/drop requires a named place");
        if (e.args.size() != 1)
          fail(a.loc, DiagnosticCode::ArgumentMismatch, "move/drop take no arguments");
        if (recv.kind == H::Field)
          fieldTransferContract(recv);
        e.kind = a.text == "move" ? H::Move : H::Drop;
        e.type = a.text == "move" ? recv.type : Unit;
        break;
      }
      if (recv.kind == H::Field && p.containsReferences(recv.type))
        fail(
            a.loc, DiagnosticCode::EscapingView,
            "borrowed field receivers require projection contracts; extract the field with move()");
      if (p.types[recv.type].futureFunction != None)
        fail(a.loc, DiagnosticCode::TypeMismatch,
             "Future storage is private; use await, @block_on, move() or drop()");
      auto method = a.text;
      std::vector<hir::Stmt> formattingTemps;
      if (method == "append_display" || method == "append_debug") {
        if (p.types[recv.type].representation != hir::Representation::StringRecord ||
            e.args.size() != 2)
          fail(a.loc, DiagnosticCode::FormatType, "format append requires String and one argument");
        auto &value = e.args[1];
        if (p.types[value.type].representation == hir::Representation::StringRecord) {

          if (value.kind != H::Variable && value.kind != H::Borrow) {
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
      if (f.unsafeFunction)
        requireUnsafe(a.loc, "call to unsafe method");
      if (f.params.empty() || !p.symbols[f.params[0]].receiver)
        fail(a.loc, DiagnosticCode::TypeMismatch, "method needs self");
      e.kind = H::Method;
      e.type = f.result;
      args(e, f, 0);
      if (e.args[0].kind == H::Field && fieldPlace(e.args[0]) && p.ownedFieldType(e.args[0].type)) {
        if (f.access == Access::Own)
          fail(a.loc, DiagnosticCode::BorrowConflict,
               "owning method on a field requires extracting it with move()");
        H adapted{};
        adapted.kind = H::Borrow;
        adapted.loc = e.args[0].loc;
        adapted.type = e.args[0].type;
        adapted.borrowKind = f.access == Access::Unique ? BorrowKind::Unique : BorrowKind::Shared;
        adapted.region = nextRegion++;
        adapted.args.push_back(std::move(e.args[0]));
        e.args[0] = std::move(adapted);
      }
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
    case A::Invoke: {
      requireUnsafe(a.loc, "C function pointer call");
      if (e.args.empty() || !p.functionPointer(e.args[0].type))
        fail(a.loc, DiagnosticCode::InvalidCAbi, "expression is not a C function pointer");
      const auto &signature = p.types[e.args[0].type];
      if (e.args.size() != signature.parameters.size() + 1)
        fail(a.loc, DiagnosticCode::ArgumentMismatch, "wrong number of callback arguments");
      for (size_t n = 0; n < signature.parameters.size(); ++n)
        coerce(signature.parameters[n], e.args[n + 1]);
      e.kind = H::IndirectCall;
      e.type = signature.element;
      break;
    }
    case A::Call: {
      if (auto local = binding(a.text, a.loc);
          local && !local->constant && p.functionPointer(p.symbols[local->id].type)) {
        requireUnsafe(a.loc, "C function pointer call");
        auto pointer = expr(ast::Expr{A::Name, a.loc, a.text, {}});
        const auto &signature = p.types[pointer.type];
        if (e.args.size() != signature.parameters.size())
          fail(a.loc, DiagnosticCode::ArgumentMismatch, "wrong number of callback arguments");
        for (size_t n = 0; n < e.args.size(); ++n)
          coerce(signature.parameters[n], e.args[n]);
        e.kind = H::IndirectCall;
        e.type = signature.element;
        e.args.insert(e.args.begin(), std::move(pointer));
        break;
      }
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
      if (p.functions[e.ref].variadic)
        e.ref = instantiateVariadic(e.ref, e);
      auto &f = p.functions[e.ref];
      if (!f.params.empty() && p.symbols[f.params[0]].receiver)
        fail(a.loc, DiagnosticCode::InvalidPlace, "use receiver.method() for instance methods");
      if (!privileged && f.trusted && !f.pub)
        fail(a.loc, DiagnosticCode::RestrictedOperation,
             "internal runtime function is not part of the public API");
      if ((f.foreignC && f.external) || f.unsafeFunction)
        requireUnsafe(a.loc, "call to `" + f.name + "`");
      e.kind = f.kind == FunctionKind::Async ? H::FutureCreate : H::Call;
      e.type = f.kind == FunctionKind::Async ? f.futureType : f.result;
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
      borrowed.borrowKind = p.functions[e.ref].resultAccess == Access::Unique ? BorrowKind::Unique
                                                                              : BorrowKind::Shared;
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
        borrowed.symbol =
            add(a.name, borrowed.expr.type, a.loc, borrowed.access, false, false, true);
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
          std::vector<std::pair<SymbolId, hir::ReferencePath>> payloadBorrows;
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
                    auto capability = p.types[found->payload[j]].referenceAccess;
                    if (capability == Access::Unique && matchAccess != Access::Own)
                      fail(pat.loc, DiagnosticCode::InvalidReborrow,
                           "match a unique reference result by value to transfer exclusive access");
                    if (matchAccess == Access::Unique)
                      fail(pat.loc, DiagnosticCode::SharedMutation,
                           "a stored shared reference cannot become unique through match");
                    auto pt = p.types[found->payload[j]].element;
                    auto id = add(pat.bindings[j], pt, pat.loc, capability);
                    p.symbols[id].reference = true;
                    p.symbols[id].region = nextRegion++;
                    payloadBorrows.push_back({id, hir::payloadPosition(found->id, j)});
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
                    payloadBorrows.push_back({id, hir::payloadPosition(found->id, j)});
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
                  let.expr.ref =
                      let.expr.kind == hir::Expr::Field ? p.payloadOffset(tid, found->id, j) : None;
                  let.expr.payloadPath = hir::payloadPosition(found->id, j);
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
              if (pat.kind == ast::Pattern::Integer) {
                auto literal = parseNumericLiteral(rhs.text, pat.loc);
                if (literal.floating ||
                    (!literal.suffix.empty() && types.at(literal.suffix) != tid))
                  fail(pat.loc, DiagnosticCode::InvalidPattern,
                       "numeric pattern type does not match scrutinee");
                rhs.text = literal.value;
              }
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
            borrow.expr.payloadPath = it->second;
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
      if (a.kind == A::For && a.classicFor) {
        ast::Expr expansion{
            ast::Expr::Block, a.loc, "classic-for", {{ast::Expr::Name, a.loc, "unit", {}}}};
        expansion.body = a.headerInit;
        auto loop = a;
        loop.kind = A::While;
        loop.headerInit.clear();
        expansion.body.push_back(std::move(loop));
        s.kind = H::Expression;
        s.expr = expr(expansion);
        out.push_back(std::move(s));
        continue;
      }
      if (a.kind == A::For && a.collectionIteration) {
        auto viewPlan = viewIteration(a.expr);
        const auto &sourceExpression = viewPlan ? viewPlan->source : a.expr;
        auto source = expr(sourceExpression);
        const auto shape = p.types[source.type];
        bool unique = viewPlan ? viewPlan->unique
                      : source.kind == hir::Expr::Borrow
                          ? source.borrowKind == BorrowKind::Unique
                          : source.kind == hir::Expr::Variable && p.symbols[source.ref].reference &&
                                p.symbols[source.ref].access == Access::Unique;
        bool sourceUnique = viewPlan ? viewPlan->sourceUnique : unique;
        if (sourceUnique &&
            ((source.kind == hir::Expr::Borrow && source.borrowKind != BorrowKind::Unique) ||
             (source.kind == hir::Expr::Variable && p.symbols[source.ref].reference &&
              p.symbols[source.ref].access != Access::Unique)))
          fail(a.loc, DiagnosticCode::InvalidRange,
               "mutable iteration requires unique access to the source");
        const auto protocol = iterationProtocol(source.type, unique, a.loc);
        auto suffix = std::to_string(p.symbols.size());
        auto owner = "$for_owner" + suffix, view = "$for_view" + suffix,
             index = "$for_index" + suffix;
        auto name = [&](const std::string &text) {
          return ast::Expr{ast::Expr::Name, a.loc, text, {}};
        };
        auto viewExpression = name(view);
        ast::Expr expansion{ast::Expr::Block, a.loc, "collection-for", {name("unit")}};
        ast::Stmt input{};
        input.kind = ast::Stmt::Let;
        input.loc = a.loc;
        input.name = view;
        if (sourceExpression.kind == ast::Expr::Borrow)
          input.expr = sourceExpression;
        else if (source.kind == hir::Expr::Borrow) {

          ast::Stmt storage{};
          storage.kind = ast::Stmt::Let;
          storage.name = owner;
          storage.loc = a.loc;
          storage.expr = sourceExpression;
          expansion.body.push_back(std::move(storage));
          viewExpression = name(owner);
        } else {
          auto borrowed = sourceExpression;
          if (source.kind != hir::Expr::Variable && source.kind != hir::Expr::Field) {
            ast::Stmt storage{};
            storage.kind = ast::Stmt::Let;
            storage.name = owner;
            storage.loc = a.loc;
            storage.expr = sourceExpression;
            expansion.body.push_back(std::move(storage));
            borrowed = name(owner);
          }
          input.expr = {ast::Expr::Borrow, a.loc, sourceUnique ? "unique" : "shared", {borrowed}};
        }
        if (source.kind == hir::Expr::Variable && p.symbols[source.ref].reference)
          viewExpression = sourceExpression;
        else if (viewExpression.text == view)
          expansion.body.push_back(std::move(input));
        ast::Stmt loop = a;
        loop.collectionIteration = false;
        loop.name = index;
        loop.expr = {ast::Expr::Int, a.loc, "0usize", {}};
        loop.limit = {ast::Expr::Method, a.loc, protocol.length, {viewExpression}};
        ast::Expr selectionIndex = name(index);
        if (viewPlan) {
          auto [length, coordinates] =
              viewIndices(*viewPlan, source.type, viewExpression, name(index), protocol.length,
                          expansion.body, a.loc, "$view_axis" + suffix + "_");
          expansion.text = "view-for rank=" + std::to_string(coordinates.size());
          loop.limit = std::move(length);
          if (shape.kind == hir::Type::Array) {
            auto projection = viewExpression;
            for (auto &coordinate : coordinates)
              projection = {
                  ast::Expr::Index, a.loc, "", {std::move(projection), std::move(coordinate)}};
            ast::Stmt element{};
            element.kind = ast::Stmt::Let;
            element.loc = a.loc;
            element.name = a.name;
            element.expr = {
                ast::Expr::Borrow, a.loc, unique ? "unique" : "shared", {std::move(projection)}};
            loop.body.insert(loop.body.begin(), std::move(element));
            expansion.body.push_back(std::move(loop));
            s.kind = H::Expression;
            s.expr = expr(expansion);
            s.expr.text = expansion.text;
            out.push_back(std::move(s));
            continue;
          }
          selectionIndex = std::move(coordinates.front());
        }
        if (!viewPlan && shape.kind == hir::Type::Array &&
            p.types[shape.element].kind == hir::Type::Array) {
          ast::Stmt row{};
          row.kind = ast::Stmt::Let;
          row.loc = a.loc;
          row.name = a.name;
          ast::Expr projection{ast::Expr::Index, a.loc, "", {viewExpression, name(index)}};
          row.expr = {
              ast::Expr::Borrow, a.loc, unique ? "unique" : "shared", {std::move(projection)}};
          loop.body.insert(loop.body.begin(), std::move(row));
          expansion.body.push_back(std::move(loop));
          s.kind = H::Expression;
          s.expr = expr(expansion);
          out.push_back(std::move(s));
          continue;
        }
        ast::Stmt selection{};
        selection.kind = ast::Stmt::Match;
        selection.loc = a.loc;
        selection.expr = {ast::Expr::Method, a.loc, protocol.get, {viewExpression, selectionIndex}};
        ast::Stmt present{}, absent{};
        present.kind = absent.kind = ast::Stmt::Expression;
        present.loc = absent.loc = a.loc;
        present.pattern = {ast::Pattern::Variant, a.loc, "Some", {a.name}};
        present.body = a.body;
        absent.pattern = {ast::Pattern::Variant, a.loc, "None", {}};
        selection.body = {std::move(present), std::move(absent)};
        loop.body = {std::move(selection)};
        expansion.body.push_back(std::move(loop));
        s.kind = H::Expression;
        s.expr = expr(expansion);
        if (viewPlan)
          s.expr.text = expansion.text;
        out.push_back(std::move(s));
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
        if (a.classicFor) {
          H step{};
          step.kind = H::Expression;
          step.loc = a.limit.loc;
          step.expr = expr(a.limit);
          s.latch.push_back(std::move(step));
        }
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
                           : expr(a.expr,
                                  a.kind == ast::Stmt::Return ? current->result
                                  : a.kind == ast::Stmt::Let && a.annotated
                                      ? getType(a.annotation, a.annotation.reference)
                                      : None,
                                  a.kind == ast::Stmt::Borrow);
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
        s.symbol = add(a.name, s.expr.type, a.loc, Access::Own, false, false, true);
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
        bool field = s.expr.kind == hir::Expr::Field && fieldPlace(s.expr) &&
                     p.reference(s.expr.type) &&
                     p.types[s.expr.type].referenceAccess == Access::Unique;
        if (s.expr.kind != hir::Expr::Variable && !field)
          fail(a.loc, DiagnosticCode::InvalidPlace, "borrow requires a named resource");
        scopes.emplace_back();
        auto type = field ? p.types[s.expr.type].element : s.expr.type;
        s.symbol = add(a.name, type, a.loc, a.access);
        hir::Expr borrowed{};
        borrowed.kind = hir::Expr::Borrow;
        borrowed.loc = a.loc;
        borrowed.type = type;
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
    p.library = ast.library;
    for (const auto &a : ast.foreignTypes) {
      if (types.contains(a.name) || foreignAliases.contains(a.name))
        duplicate(a.loc, a.name);
      if (a.alias)
        foreignAliases[a.name] = &a;
      else {
        types[a.name] = p.types.size();
        p.types.push_back({hir::Type::ForeignOpaque, a.name});
        p.types.back().cName = a.cName;
      }
    }
    for (auto &a : ast.structs) {
      if (a.foreignC && !a.parameters.empty())
        fail(a.loc, DiagnosticCode::InvalidCAbi, "C structs cannot be generic");
      if (!a.parameters.empty())
        continue;
      if (types.contains(a.name) || protocols.contains(a.name) || foreignAliases.contains(a.name))
        duplicate(a.loc, a.name);
      TypeId id = p.types.size();
      types[a.name] = id;
      structSources[id] = &a;
      p.types.push_back({a.foreignC ? hir::Type::ForeignRecord : hir::Type::Structure,
                         a.name,
                         None,
                         None,
                         0,
                         1,
                         {}});
      p.types.back().cName = a.cName;
      p.types.back().foreignUnion = a.foreignUnion;
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
      if (!a.protocol.empty() ||
          (!a.representation.empty() && (!a.trusted || (a.representation != "generic_box" &&
                                                        a.representation != "generic_vec"))))
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
        if (type.count || !type.countName.empty())
          fail(type.loc, DiagnosticCode::InvalidPayload,
               "compound generic field templates are not implemented yet");
        std::function<hir::GenericPayload(const TypeSyntax &)> templateType = [&](const auto &ts) {
          if (ts.reference) {
            auto referent = ts;
            referent.reference = false;
            referent.mutableReference = false;
            auto result = templateType(referent);
            result.referenceAccess = ts.mutableReference ? Access::Unique : Access::Shared;
            return result;
          }
          if (ts.count || !ts.countName.empty() || ts.rawPointer || ts.functionPointer)
            fail(ts.loc, DiagnosticCode::InvalidPayload, "unsupported generic field type shape");
          if (parameters.contains(ts.name)) {
            if (!ts.arguments.empty())
              fail(ts.loc, DiagnosticCode::TypeMismatch, "a type parameter cannot take arguments");
            return hir::GenericPayload{true, parameters.at(ts.name)};
          }
          if (ts.arguments.empty())
            return hir::GenericPayload{false, getType(ts)};
          GenericDeclId target;
          if (genericNames.contains(ts.name))
            target = genericNames.at(ts.name);
          else if (genericStructNames.contains(ts.name))
            target = genericStructNames.at(ts.name);
          else
            fail(ts.loc, DiagnosticCode::UnknownName,
                 "unknown generic field type `" + ts.name + "`");
          hir::GenericPayload application{false, target};
          for (const auto &arg : ts.arguments)
            application.arguments.push_back(templateType(arg));
          return application;
        };
        decl.fields.push_back({field.name, templateType(type)});
      }
      if (!a.representation.empty()) {
        if (decl.parameters.size() != 1 || decl.fields.size() != 1 ||
            !decl.fields[0].type.parameter || decl.fields[0].type.id != decl.parameters[0])
          fail(a.loc, DiagnosticCode::RestrictedOperation, "invalid generic storage declaration");
        decl.representation = a.representation == "generic_box" ? hir::Representation::GenericBox
                                                                : hir::Representation::GenericVec;
      }
      p.genericStructs.push_back(std::move(decl));
    }
    structLayoutsEnabled = true;
    for (const auto &[id, source] : structSources)
      ensureStructLayout(id, source->loc);
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
        if (intrinsicInfo(f.intrinsicId).lowering == IntrinsicLowering::ContainerStorage) {
          if (!validContainerIntrinsic(p, f))
            fail(f.loc, DiagnosticCode::RestrictedOperation, "invalid generic storage signature");
          continue;
        }
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
                      f.intrinsicId == IntrinsicId::String_read_file ||
                      f.intrinsicId == IntrinsicId::String_env_var ||
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
      if (f.external && !f.foreignC) {

        std::vector<TypeId> signature{f.result};
        for (auto param : f.params)
          signature.push_back(p.symbols[param].type);
        if (f.intrinsicId == IntrinsicId::None ||
            intrinsicSignature(f.intrinsicId, f.owner, f.result) != signature)
          fail(f.loc, DiagnosticCode::RestrictedOperation,
               "runtime C ABI signature mismatch for `" + f.name + "`");
      }
      if (f.external && !f.foreignC) {
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
      unsafeDepth = f.unsafeFunction ? 1 : 0;
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
          if (e.kind == hir::Expr::ThreadCall) {
            f.effects.threading = true;
            f.effects.merge(p.functions.at(e.ref).effects);
          }
          if (e.kind == hir::Expr::Call || e.kind == hir::Expr::Method)
            f.effects.merge(p.functions.at(e.ref).effects);
          if (e.kind == hir::Expr::Await || e.kind == hir::Expr::BlockOn) {
            auto effects = p.functions.at(e.ref).effects;
            effects.asynchronous = e.kind == hir::Expr::Await;
            f.effects.merge(effects);
          }
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
    if (!p.library) {
      auto main = functions.find("main");
      if (main == functions.end())
        fail({}, DiagnosticCode::UnknownName, "missing fn main() -> i32");
      auto &f = p.functions[main->second];
      if ((f.result != I32 && f.result != Unit) || !f.params.empty() || f.unsafeFunction)
        DiagnosticEngine()
            .error(DiagnosticCode::TypeMismatch, f.loc, "invalid entrypoint signature")
            .note("supported signatures: fn main() -> i32; fn main()")
            .help("use std.env.argument_count() and std.env.argument(index) for command-line "
                  "arguments")
            .emit();
      p.entryPoint = {f.id, f.result == Unit ? hir::MainSignatureKind::NoArgsUnit
                                             : hir::MainSignatureKind::NoArgsI32};
    }
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
    hir::inferReferenceTransfers(p);
    return std::move(p);
  }
};
void printS(const std::vector<hir::Stmt> &, std::ostream &, int);
void printE(const hir::Expr &e, std::ostream &o) {
  static const char *names[] = {"constant",
                                "symbol",
                                "binary",
                                "unary",
                                "call",
                                "method",
                                "field",
                                "index",
                                "array",
                                "assign",
                                "move",
                                "drop",
                                "construct",
                                "print",
                                "unit",
                                "cast",
                                "enum.construct",
                                "static_str",
                                "block",
                                "borrow",
                                "match",
                                "payload.take",
                                "reference.store",
                                "length",
                                "unsafe",
                                "raw.place",
                                "raw.address",
                                "pointer.cast",
                                "array.repeat",
                                "function.address",
                                "foreign.call.indirect",
                                "struct.init",
                                "future.create",
                                "await",
                                "block_on",
                                "thread.call"};
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
  for (const auto &position : e.payloadPath)
    o << " payload.variant=#V" << position.variant << " payload.field=" << position.index;
  if (e.kind == hir::Expr::StructInit)
    for (auto field : e.fieldPath)
      o << " field=#" << field;
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
  if (p.library)
    o << "library (no native entrypoint)\n";
  else
    o << "entrypoint #F" << p.entryPoint.function
      << (p.entryPoint.signature == hir::MainSignatureKind::NoArgsUnit ? " NoArgsUnit"
                                                                       : " NoArgsI32")
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
    if (p.types[i].kind == hir::Type::ForeignRecord) {
      auto [size, align] = ffi::layout(p, i);
      o << (p.types[i].foreignUnion ? " representation=CUnion size="
                                    : " representation=CRecord size=")
        << size << " align=" << align << " offsets=bytes";
    } else if (p.types[i].kind == hir::Type::ForeignOpaque)
      o << " representation=COpaque unsized";
    else if (p.rawPointer(i))
      o << " representation=RawPointer referent=#T" << p.types[i].element
        << " bits=" << p.target.pointerWidth;
    else if (p.functionPointer(i)) {
      o << " representation=CFunctionPointer parameters={";
      for (auto parameter : p.types[i].parameters)
        o << " #T" << parameter;
      o << " } result=#T" << p.types[i].element << " bits=" << p.target.pointerWidth;
    } else if (p.types[i].futureFunction != None)
      o << " representation=FutureFramePointer producer=#F" << p.types[i].futureFunction
        << " inputs=#T" << p.types[i].element << " DropGlue=FutureFrame";
    else if (p.dynamicContainer(i))
      o << " representation="
        << (p.types[i].representation == hir::Representation::GenericBox ? "BoxStorage"
                                                                         : "VecStorage")
        << "<" << p.types[p.types[i].element].name << "> DropGlue=Elements+Allocation";
    else if (p.types[i].representation == hir::Representation::StringRecord)
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
    if (f.external || f.foreignC)
      o << " extern=C";
    if (f.variadic)
      o << " variadic fixed=" << f.fixedParams;
    if (f.variadicDeclaration != None)
      o << " declaration=#F" << f.variadicDeclaration;
    if (f.unsafeFunction)
      o << " unsafe";
    o << " kind=" << (f.kind == FunctionKind::Async ? "Async" : "Normal") << " effects={"
      << f.effects.describe() << " }";
    if (f.futureType != None)
      o << " call-result=#T" << f.futureType;
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
        << region.origin << " " << accessName(p.symbols[region.origin].access) << " caller-owned"
        << (f.acceptsResultOrigin(region.origin) ? " result-bound\n" : " noescape\n");
    for (auto id : f.payloadInspectionParameters)
      o << "  payload.inspect #v" << id << " Shared tag-only noescape\n";
    for (const auto &source : f.payloadResultSources()) {
      o << "  return.payload-source #v" << source.parameter;
      for (const auto &part : source.path)
        if (part.kind == hir::ReferenceProjectionKind::Field)
          o << " field=#" << part.index;
        else
          o << " variant=#V" << part.variant << " field=" << part.index;
      o << "\n";
    }
    if (f.resultBorrowedContainer)
      o << "  result.payload-regions: "
        << (!p.referenceCapability(f.result, Access::Shared) &&
                    !p.referenceCapability(f.result, Access::Unique)
                ? "Mixed"
                : accessName(f.resultAccess))
        << " borrowed container\n";
    if (f.resultOrigin != None)
      o << "  return.region #R" << f.resultRegion << " <= #R" << p.symbols[f.resultOrigin].region
        << " origin=#v" << f.resultOrigin << "\n";
    for (auto origin : f.additionalResultOrigins)
      o << "  return.region #R" << f.resultRegion << " <= #R" << p.symbols[origin].region
        << " origin=#v" << origin << "\n";
    hir::printReferenceContracts(p, f, o);
    printS(f.body, o, 2);
  }
}
}
