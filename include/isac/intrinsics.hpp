#pragma once
#include "isac/effects.hpp"
#include <cstdint>
#include <string_view>
#include <string>
#include <vector>
namespace isac {
enum class IntrinsicCategory { Memory, Arithmetic, IO, Compiler, Platform };
enum class IntrinsicLowering { ExternalCall, StringRecord, InputRecord, HeapConstructor };
enum class IntrinsicId : uint32_t {
  None,
#define ISAC_INTRINSIC(id, name, category, lowering, io, allocation) id,
#include "isac/intrinsics.def"
#undef ISAC_INTRINSIC
};
struct IntrinsicInfo {
  IntrinsicId id;
  std::string_view name;
  IntrinsicCategory category;
  IntrinsicLowering lowering;
  FunctionEffects effects;
};
const IntrinsicInfo &intrinsicInfo(IntrinsicId);
IntrinsicId findIntrinsic(std::string_view);

std::vector<uint32_t> intrinsicSignature(IntrinsicId, uint32_t owner, uint32_t result);
std::string intrinsicRuntimeSymbol(IntrinsicId);

}
