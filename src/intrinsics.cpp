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

std::vector<uint32_t> intrinsicSignature(IntrinsicId id, uint32_t owner, uint32_t result) {
  auto &info = intrinsicInfo(id);
  static const std::unordered_map<std::string, TypeId> primitives = {
      {"i8", I8},   {"i16", I16}, {"i32", I32},   {"i64", I64},     {"u8", U8},
      {"u16", U16}, {"u32", U32}, {"u64", U64},   {"isize", ISize}, {"usize", USize},
      {"f32", F32}, {"f64", F64}, {"bool", Bool}, {"char", Char},   {"str", Str}};
  auto name = std::string(info.name);
  if (info.lowering == IntrinsicLowering::StringRecord) {
    switch (id) {
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
