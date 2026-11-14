#include "isac/hir.hpp"

namespace isac::hir {
TypeId instantiateHeapContainer(Program &p, GenericDeclId id, const std::vector<TypeId> &arguments,
                                Loc loc) {
  if (id < p.genericEnums.size() || id - p.genericEnums.size() >= p.genericStructs.size())
    fail(loc, DiagnosticCode::InvalidCore, "invalid generic container declaration identity");
  const auto &decl = p.genericStructs[id - p.genericEnums.size()];
  if (decl.representation != Representation::GenericBox &&
      decl.representation != Representation::GenericVec)
    fail(loc, DiagnosticCode::InvalidCore,
         "heap instantiation requires a registered container representation");
  if (arguments.size() != decl.parameters.size())
    fail(loc, DiagnosticCode::TypeMismatch,
         "wrong number of type arguments for `" + decl.name + "`");
  if (arguments.size() != 1 || arguments[0] >= p.types.size() || !p.containerElement(arguments[0]))
    fail(
        loc, DiagnosticCode::InvalidPayload,
        "generic heap containers require reference-free owned elements with a final initial protocol state");
  Substitution sub{{{decl.parameters[0], arguments[0]}}};
  for (const auto &instance : p.instances)
    if (instance.declaration == id && instance.substitution.types == sub.types)
      return instance.type;
  if (p.instances.size() >= 1024)
    fail(loc, DiagnosticCode::TypeMismatch, "generic instantiation limit reached (1024)");
  Type type{};
  type.kind = Type::Structure;
  type.name = decl.name + "<" + p.types[arguments[0]].name + ">";
  type.representation = decl.representation;
  type.dropGlue = DropGlue::ContainerElements;
  type.element = arguments[0];
  auto tid = static_cast<TypeId>(p.types.size());
  p.types.push_back(std::move(type));
  p.instances.push_back({static_cast<InstanceId>(p.instances.size()), id, std::move(sub), tid});
  return tid;
}
FunctionId owningBoxIntrinsic(Program &p, TypeId owner, IntrinsicId intrinsic, Loc loc) {
  if (owner >= p.types.size() || p.types[owner].representation != Representation::GenericBox ||
      (intrinsic != IntrinsicId::Box_new_generic && intrinsic != IntrinsicId::Box_into_inner))
    fail(loc, DiagnosticCode::InvalidCore, "invalid owning Box intrinsic specialization");
  for (const auto &f : p.functions)
    if (f.owner == owner && f.intrinsicId == intrinsic && validContainerIntrinsic(p, f))
      return f.id;
  bool consume = intrinsic == IntrinsicId::Box_into_inner;
  Function f{};
  f.id = static_cast<FunctionId>(p.functions.size());
  f.name = p.types[owner].name + (consume ? "::into_inner" : "::new");
  f.owner = owner;
  f.result = consume ? p.types[owner].element : owner;
  f.intrinsicId = intrinsic;
  f.intrinsic = std::string(intrinsicInfo(intrinsic).name);
  f.effects = intrinsicInfo(intrinsic).effects;
  f.trusted = true;
  f.consumesSelf = consume;
  f.loc = loc;
  Symbol symbol{};
  symbol.id = static_cast<SymbolId>(p.symbols.size());
  symbol.type = consume ? owner : p.types[owner].element;
  symbol.parameter = true;
  symbol.receiver = consume;
  symbol.name = consume ? "self" : "value";
  symbol.loc = loc;
  f.params.push_back(symbol.id);
  p.symbols.push_back(std::move(symbol));
  if (!validContainerIntrinsic(p, f))
    fail(loc, DiagnosticCode::InvalidCore, "invalid specialized Box signature");
  p.functions.push_back(std::move(f));
  return p.functions.back().id;
}
}
