#include "isac/hir.hpp"
#include <cmath>
namespace isac {
bool integer(TypeId t) {
  return t == I32 || t == USize || (t >= I8 && t <= ISize);
}
bool floating(TypeId t) {
  return t == F32 || t == F64;
}
bool numeric(TypeId t) {
  return integer(t) || floating(t);
}
bool scalar(TypeId t) {
  return numeric(t) || t == Bool || t == Char;
}
bool signedType(TypeId t) {
  return t == I8 || t == I16 || t == I32 || t == I64 || t == ISize;
}
unsigned bitWidth(TypeId t, unsigned pointerBits) {
  if (t == I8 || t == U8)
    return 8;
  if (t == I16 || t == U16)
    return 16;
  if (t == I64 || t == U64 || t == F64)
    return 64;
  if (t == USize || t == ISize || t == RawPtr)
    return pointerBits;
  return 32;
}
void validateLiteral(TypeId t, const std::string &s, Loc loc, unsigned pointerBits) {
  try {
    size_t end = 0;
    if (floating(t)) {
      double n = std::stod(s, &end);
      if (end != s.size() || !std::isfinite(n) ||
          (t == F32 && !std::isfinite(static_cast<float>(n))))
        fail(loc, DiagnosticCode::InvalidLiteral, "floating literal out of range");
      return;
    }
    if (s.find('.') != std::string::npos)
      fail(loc, DiagnosticCode::InvalidLiteral, "integer literal cannot contain a decimal point");
    bool neg = s.starts_with('-');
    auto magnitude = std::stoull(neg ? s.substr(1) : s, &end);
    if (end != (s.size() - (neg ? 1 : 0)))
      fail(loc, DiagnosticCode::InvalidLiteral, "invalid integer literal");
    unsigned bits = bitWidth(t, pointerBits);
    uint64_t max = signedType(t) ? (uint64_t(1) << (bits - 1)) - 1
                                 : (bits == 64 ? UINT64_MAX : (uint64_t(1) << bits) - 1);
    if (neg) {
      if (!signedType(t) || magnitude > max + 1)
        fail(loc, DiagnosticCode::InvalidLiteral, "negative integer literal out of range");
    } else if (magnitude > max)
      fail(loc, DiagnosticCode::InvalidLiteral, "integer literal out of range");
  } catch (const std::out_of_range &) {
    fail(loc, DiagnosticCode::InvalidLiteral, "numeric literal out of range");
  } catch (const std::invalid_argument &) {
    fail(loc, DiagnosticCode::InvalidLiteral, "invalid numeric literal");
  }
}
}
