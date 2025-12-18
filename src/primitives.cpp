#include "isac/hir.hpp"
#include <cctype>
#include <cmath>
#include <limits>
namespace isac {
NumericLiteral parseNumericLiteral(const std::string &source, Loc loc) {
  auto invalid = [&]() {
    fail(loc, DiagnosticCode::InvalidLiteral, "invalid numeric literal `" + source + "`");
  };
  NumericLiteral result;
  size_t start = source.starts_with('-') ? 1 : 0, pos = start;
  unsigned base = 10;
  if (pos + 1 < source.size() && source[pos] == '0') {
    switch (source[pos + 1]) {
    case 'x':
    case 'X':
      base = 16;
      break;
    case 'b':
    case 'B':
      base = 2;
      break;
    case 'o':
    case 'O':
      base = 8;
      break;
    default:
      break;
    }
    if (base != 10)
      pos += 2;
  }
  auto digit = [](char c) -> unsigned {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return 100;
  };
  uint64_t magnitude = 0;
  bool overflow = false;
  bool previousDigit = false, anyDigit = false;
  std::string decimal;
  while (pos < source.size()) {
    char c = source[pos];
    if (c == '_') {
      if (!previousDigit || pos + 1 == source.size() || digit(source[pos + 1]) >= base)
        invalid();
      previousDigit = false;
      ++pos;
      continue;
    }
    if (base == 10 && c == '.' && !result.floating) {
      if (!previousDigit || pos + 1 == source.size() || digit(source[pos + 1]) >= 10)
        invalid();
      result.floating = true;
      decimal += c;
      previousDigit = false;
      ++pos;
      continue;
    }
    unsigned d = digit(c);
    if (d >= base)
      break;
    if (!result.floating) {
      overflow = overflow || magnitude > (UINT64_MAX - d) / base;
      if (!overflow)
        magnitude = magnitude * base + d;
    }
    decimal += c;
    previousDigit = anyDigit = true;
    ++pos;
  }
  if (!anyDigit || !previousDigit)
    invalid();
  result.suffix = source.substr(pos);
  if (!result.suffix.empty() && result.suffix != "i8" && result.suffix != "i16" &&
      result.suffix != "i32" && result.suffix != "i64" && result.suffix != "u8" &&
      result.suffix != "u16" && result.suffix != "u32" && result.suffix != "u64" &&
      result.suffix != "usize" && result.suffix != "isize" &&
      !(base == 10 && (result.suffix == "f32" || result.suffix == "f64")))
    invalid();
  bool floatValue = result.floating || result.suffix == "f32" || result.suffix == "f64";
  if (overflow && !floatValue)
    fail(loc, DiagnosticCode::InvalidLiteral, "numeric literal out of range");
  result.value = (start ? "-" : "") + (floatValue ? decimal : std::to_string(magnitude));
  return result;
}
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
