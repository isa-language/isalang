#include "isac/intrinsics.hpp"
#include "isac/hir.hpp"
#include <stdexcept>
#include <unordered_map>
namespace isac {
static const IntrinsicInfo registry[] = {
    {IntrinsicId::None, "", IntrinsicCategory::Compiler, IntrinsicLowering::ExternalCall, {}},
#define ISAC_INTRINSIC(id, name, category, lowering, io, allocation)                               \
  {IntrinsicId::id,                                                                                \
   name,                                                                                           \
   IntrinsicCategory::category,                                                                    \
   IntrinsicLowering::lowering,                                                                    \
   {io, allocation, false, false}},
#include "isac/intrinsics.def"
#undef ISAC_INTRINSIC
};
const IntrinsicInfo &intrinsicInfo(IntrinsicId id) {
  auto index = static_cast<size_t>(id);
  if (index >= std::size(registry))
    throw std::out_of_range("invalid IntrinsicId");
  return registry[index];
}
IntrinsicId findIntrinsic(std::string_view name) {
  for (auto &entry : registry)
    if (entry.name == name)
      return entry.id;
  return IntrinsicId::None;
}

bool containerBorrowIntrinsic(IntrinsicId id) {
  return id == IntrinsicId::Box_get || id == IntrinsicId::Vec_get || id == IntrinsicId::Vec_first ||
         id == IntrinsicId::Vec_last || containerMutableBorrowIntrinsic(id) ||
         id == IntrinsicId::Vec_as_slice || elementSliceIntrinsic(id);
}
bool containerMutableBorrowIntrinsic(IntrinsicId id) {
  return id == IntrinsicId::Box_get_mut || id == IntrinsicId::Vec_get_mut ||
         id == IntrinsicId::Vec_first_mut || id == IntrinsicId::Vec_last_mut ||
         id == IntrinsicId::Slice_get_mut || id == IntrinsicId::Slice_first_mut ||
         id == IntrinsicId::Slice_last_mut || id == IntrinsicId::Vec_as_mut_slice;
}
bool containerSliceIntrinsic(IntrinsicId id) {
  return id == IntrinsicId::Vec_as_slice || id == IntrinsicId::Vec_as_mut_slice;
}
bool elementSliceIntrinsic(IntrinsicId id) {
  return id == IntrinsicId::Slice_get || id == IntrinsicId::Slice_get_mut ||
         id == IntrinsicId::Slice_first || id == IntrinsicId::Slice_last ||
         id == IntrinsicId::Slice_first_mut || id == IntrinsicId::Slice_last_mut;
}

bool validStringIntrinsic(const hir::Program &p, const hir::Function &f) {
  if (!f.trusted || f.external || f.owner >= p.types.size() ||
      p.types[f.owner].representation != hir::Representation::StringRecord ||
      intrinsicInfo(f.intrinsicId).lowering != IntrinsicLowering::StringRecord ||
      findIntrinsic(f.intrinsic) != f.intrinsicId)
    return false;
  auto signature = intrinsicSignature(f.intrinsicId, f.owner, f.result);
  bool constructor =
      f.intrinsicId == IntrinsicId::String_new || f.intrinsicId == IntrinsicId::String_from;
  auto access = constructor ? Access::Own
                : f.intrinsicId == IntrinsicId::String_push_str ||
                        f.intrinsicId == IntrinsicId::String_clear ||
                        f.intrinsicId == IntrinsicId::String_read_file ||
                        f.intrinsicId == IntrinsicId::String_env_var ||
                        f.intrinsic.starts_with("string.fmt_")
                    ? Access::Unique
                    : Access::Shared;
  if (signature.empty() || signature[0] != f.result || signature.size() != f.params.size() + 1 ||
      f.access != access || f.consumesSelf || f.resultOrigin != None)
    return false;
  for (size_t n = 0; n < f.params.size(); ++n) {
    const auto &param = p.symbols.at(f.params[n]);
    if (param.type != signature[n + 1] || param.reference ||
        param.receiver != (!constructor && n == 0))
      return false;
  }
  return true;
}

bool validContainerIntrinsic(const hir::Program &p, const hir::Function &f) {
  if (!f.trusted || f.external || f.owner >= p.types.size() ||
      intrinsicInfo(f.intrinsicId).lowering != IntrinsicLowering::ContainerStorage)
    return false;
  const auto &owner = p.types[f.owner];
  if (elementSliceIntrinsic(f.intrinsicId)) {
    if ((owner.kind != hir::Type::Array && owner.kind != hir::Type::Slice) ||
        !scalar(owner.element))
      return false;
  } else if (!p.dynamicContainer(f.owner))
    return false;
  auto element = owner.element;
  auto borrowAccess =
      containerMutableBorrowIntrinsic(f.intrinsicId) ? Access::Unique : Access::Shared;
  if (!p.containerElement(element))
    return false;
  std::vector<TypeId> parameters;
  for (auto id : f.params) {
    const auto &param = p.symbols.at(id);
    if (param.reference && !(containerBorrowIntrinsic(f.intrinsicId) && param.receiver &&
                             param.access == borrowAccess))
      return false;
    parameters.push_back(param.type);
  }
  bool receiver = !f.params.empty() && p.symbols[f.params[0]].receiver;
  auto matches = [&](TypeId result, std::vector<TypeId> args, Access access, bool hasReceiver) {
    return f.result == result && parameters == args && receiver == hasReceiver &&
           f.access == access && f.consumesSelf == (f.intrinsicId == IntrinsicId::Box_into_inner);
  };
  if (containerBorrowIntrinsic(f.intrinsicId)) {
    if ((!containerSliceIntrinsic(f.intrinsicId) && !p.supportsStoredSharedReference(element)) ||
        f.params.empty() || f.resultAccess != borrowAccess || f.resultOrigin != f.params[0] ||
        f.resultRegion == None || !f.additionalResultOrigins.empty() ||
        !f.payloadResultSources().empty() || f.parameterRegions.size() != 1 ||
        f.parameterRegions[0].parameter != f.params[0] ||
        f.parameterRegions[0].kind !=
            (borrowAccess == Access::Shared ? BorrowKind::Shared : BorrowKind::Unique) ||
        f.parameterRegions[0].region == None ||
        f.parameterRegions[0].region != p.symbols[f.params[0]].region ||
        !p.symbols[f.params[0]].reference)
      return false;
  }
  if (owner.representation == hir::Representation::GenericBox) {
    if (f.intrinsicId == IntrinsicId::Box_get || f.intrinsicId == IntrinsicId::Box_get_mut)
      return !f.resultBorrowedContainer && matches(element, {f.owner}, borrowAccess, true);
    if (f.intrinsicId == IntrinsicId::Box_new_generic)
      return matches(f.owner, {element}, Access::Own, false);
    if (f.intrinsicId == IntrinsicId::Box_into_inner)
      return matches(element, {f.owner}, Access::Own, true);
    return false;
  }
  switch (f.intrinsicId) {
  case IntrinsicId::Vec_as_slice:
  case IntrinsicId::Vec_as_mut_slice:
    return !f.resultBorrowedContainer && scalar(element) &&
           p.types[f.result].kind == hir::Type::Slice && p.types[f.result].element == element &&
           matches(f.result, {f.owner}, borrowAccess, true);
  case IntrinsicId::Vec_new:
    return matches(f.owner, {}, Access::Own, false);
  case IntrinsicId::Vec_with_capacity:
    return matches(f.owner, {USize}, Access::Own, false);
  case IntrinsicId::Vec_push:
    return matches(Unit, {f.owner, element}, Access::Unique, true);
  case IntrinsicId::Vec_insert:
    return matches(Unit, {f.owner, USize, element}, Access::Unique, true);
  case IntrinsicId::Vec_remove:
    return matches(element, {f.owner, USize}, Access::Unique, true);
  case IntrinsicId::Vec_replace:
    return matches(element, {f.owner, USize, element}, Access::Unique, true);
  case IntrinsicId::Vec_swap:
    return matches(Unit, {f.owner, USize, USize}, Access::Unique, true);
  case IntrinsicId::Vec_clear:
    return matches(Unit, {f.owner}, Access::Unique, true);
  case IntrinsicId::Vec_len:
  case IntrinsicId::Vec_capacity:
    return matches(USize, {f.owner}, Access::Shared, true);
  case IntrinsicId::Vec_get:
  case IntrinsicId::Vec_get_mut:
  case IntrinsicId::Vec_first:
  case IntrinsicId::Vec_last:
  case IntrinsicId::Vec_first_mut:
  case IntrinsicId::Vec_last_mut:
  case IntrinsicId::Slice_get:
  case IntrinsicId::Slice_get_mut:
  case IntrinsicId::Slice_first:
  case IntrinsicId::Slice_last:
  case IntrinsicId::Slice_first_mut:
  case IntrinsicId::Slice_last_mut:
  case IntrinsicId::Vec_pop: {

    TypeId payload = element;
    if (containerBorrowIntrinsic(f.intrinsicId)) {
      const auto &result = p.types.at(f.result);
      if (!f.resultBorrowedContainer || result.kind != hir::Type::Enum || result.variants.empty() ||
          result.variants[0].payload.size() != 1)
        return false;
      payload = result.variants[0].payload[0];
      if (!p.reference(payload) || p.types[payload].element != element ||
          p.types[payload].referenceAccess != borrowAccess)
        return false;
    }
    bool option = false;
    for (const auto &instance : p.instances)
      if (instance.type == f.result && instance.declaration < p.genericEnums.size()) {
        const auto &decl = p.genericEnums[instance.declaration];
        option = decl.name == "Option" && instance.substitution.types.size() == 1 &&
                 instance.substitution.types[0].second == payload;
      }
    if (!option)
      return false;
    const auto &result = p.types.at(f.result);
    return result.kind == hir::Type::Enum && result.variants.size() == 2 &&
           result.variants[0].id == 0 && result.variants[1].id == 1 &&
           result.variants[0].name == "Some" &&
           result.variants[0].payload == std::vector<TypeId>{payload} &&
           result.variants[1].name == "None" && result.variants[1].payload.empty() &&
           (f.intrinsicId == IntrinsicId::Vec_pop
                ? matches(f.result, {f.owner}, Access::Unique, true)
                : matches(f.result,
                          f.intrinsicId == IntrinsicId::Vec_get ||
                                  f.intrinsicId == IntrinsicId::Vec_get_mut ||
                                  f.intrinsicId == IntrinsicId::Slice_get ||
                                  f.intrinsicId == IntrinsicId::Slice_get_mut
                              ? std::vector<TypeId>{f.owner, USize}
                              : std::vector<TypeId>{f.owner},
                          borrowAccess, true));
  }
  default:
    return false;
  }
}

std::vector<uint32_t> intrinsicSignature(IntrinsicId id, uint32_t owner, uint32_t result) {
  auto &info = intrinsicInfo(id);
  static const std::unordered_map<std::string, TypeId> primitives = {
      {"i8", I8},   {"i16", I16}, {"i32", I32},   {"i64", I64},     {"u8", U8},
      {"u16", U16}, {"u32", U32}, {"u64", U64},   {"isize", ISize}, {"usize", USize},
      {"f32", F32}, {"f64", F64}, {"bool", Bool}, {"char", Char},   {"str", Str}};
  auto name = std::string(info.name);
  if (info.lowering == IntrinsicLowering::StringRecord) {
    switch (id) {
    case IntrinsicId::String_read_file:
    case IntrinsicId::String_env_var:
      return {I32, owner, Str};
    case IntrinsicId::String_char_at:
      return {Char, owner, USize};
    case IntrinsicId::String_is_char_boundary:
      return {Bool, owner, USize};
    case IntrinsicId::String_new:
      return {owner};
    case IntrinsicId::String_from:
      return {owner, Str};
    case IntrinsicId::String_len:
    case IntrinsicId::String_capacity:
      return {USize, owner};
    case IntrinsicId::String_is_empty:
      return {Bool, owner};
    case IntrinsicId::String_as_str:
      return {Str, owner};
    case IntrinsicId::String_push_str:
      return {Unit, owner, Str};
    case IntrinsicId::String_clear:
      return {Unit, owner};
    default:
      return {Unit, owner,
              primitives.at(name.substr(name.starts_with("string.fmt_debug_") ? 17 : 11))};
    }
  }
  if (info.lowering == IntrinsicLowering::HeapConstructor)
    return {owner, id == IntrinsicId::box_new ? I32 : USize};
  if (info.lowering == IntrinsicLowering::InputRecord) {
    if (name.starts_with("io.read_"))
      return {result};
    if (name.starts_with("io.parse_"))
      return {result, owner};
    if (id == IntrinsicId::Input_result_error)
      return {I32, owner};
    if (id == IntrinsicId::Input_result_ok)
      return {Bool, owner};
    return {primitives.at(name.substr(10)), owner};
  }
  static const std::unordered_map<std::string, std::vector<TypeId>> abi = {
      {"isac_rt_panic_str", {Unit, Str}},
      {"isac_rt_monotonic_ns", {U64}},
      {"isac_rt_fs_write_text", {I32, Str, Str}},
      {"isac_rt_fs_exists", {I32, Str}},
      {"isac_rt_fs_remove_file", {I32, Str}},
      {"isac_rt_fs_create_dir", {I32, Str}},
      {"isac_rt_process_run", {I64, Str, Str, Bool}},
      {"isac_rt_sleep_ns", {I32, U64}},
      {"isac_rt_unix_ns", {U64}},
      {"isac_rt_env_argument_count", {USize}},
      {"isac_rt_env_argument", {Str, USize}},
      {"isac_rt_str_equal", {Bool, Str, Str}},
      {"isac_rt_stdout_write", {Unit, Str}},
      {"isac_rt_stderr_write", {Unit, Str}},
      {"isac_rt_alloc", {RawPtr, USize}},
      {"isac_rt_alloc_zeroed", {RawPtr, USize}},
      {"isac_rt_free", {Unit, RawPtr}},
      {"isac_rt_memcpy", {RawPtr, RawPtr, RawPtr, USize}},
      {"isac_rt_memmove", {RawPtr, RawPtr, RawPtr, USize}},
      {"isac_rt_load_i32", {I32, RawPtr}},
      {"isac_rt_store_i32", {Unit, RawPtr, I32}},
      {"isac_rt_buffer_size", {USize, USize}},
      {"isac_rt_buffer_init", {Unit, RawPtr, USize}},
      {"isac_rt_buffer_len", {USize, RawPtr}},
      {"isac_rt_buffer_read", {I32, RawPtr, USize}},
      {"isac_rt_buffer_write", {Unit, RawPtr, USize, I32}},
      {"isac_rt_print_i32", {Unit, I32}},
      {"isac_rt_print_bool", {Unit, Bool}},
      {"isac_rt_print_usize", {Unit, USize}},
      {"isac_rt_println", {Unit, RawPtr}},
      {"isac_rt_panic", {Unit, RawPtr}},
      {"isac_rt_div_i32", {I32, I32, I32}},
      {"isac_rt_div_usize", {USize, USize, USize}}};

  auto it = abi.find(name);
  if (it == abi.end())
    return {};
  return it->second;
}
std::string intrinsicRuntimeSymbol(IntrinsicId id) {
  auto &info = intrinsicInfo(id);
  auto name = std::string(info.name);
  if (info.lowering == IntrinsicLowering::StringRecord)
    return "isac_rt_string_" + name.substr(7);
  if (info.lowering == IntrinsicLowering::InputRecord)
    return "isac_rt_input_" + name.substr(3);
  return name;
}
}
