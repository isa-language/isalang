#include "isac/hir.hpp"
#include <bit>
#include <limits>
namespace isac {
namespace {
using E = hir::Expr;
void allowed(const E &e) {
  if (e.kind != E::Constant && e.kind != E::StaticStr && e.kind != E::Unary &&
      e.kind != E::Binary && e.kind != E::Cast)
    fail(e.loc, DiagnosticCode::InvalidConstant,
         "constant initializer requires a compile-time expression; function calls and runtime "
         "values are not allowed");
  if (floating(e.type))
    fail(e.loc, DiagnosticCode::InvalidConstant,
         "floating-point constant evaluation is not implemented");
  for (const auto &arg : e.args)
    allowed(arg);
}
uint64_t mask(unsigned width) {
  return width == 64 ? UINT64_MAX : (uint64_t(1) << width) - 1;
}
uint64_t number(const E &e) {
  return e.text.starts_with('-') ? uint64_t(0) - std::stoull(e.text.substr(1))
                                 : std::stoull(e.text);
}
int64_t sign(uint64_t value, unsigned width) {
  value &= mask(width);
  if (value & (uint64_t(1) << (width - 1)))
    value |= ~mask(width);
  return std::bit_cast<int64_t>(value);
}
E evaluate(const E &e, unsigned pointerWidth) {
  if (e.kind == E::Constant || e.kind == E::StaticStr)
    return e;
  auto a = evaluate(e.args.at(0), pointerWidth);
  uint64_t x = number(a), result = 0;
  auto width = bitWidth(a.type, pointerWidth);
  x &= mask(width);
  if (e.kind == E::Cast) {
    result = signedType(a.type) ? static_cast<uint64_t>(sign(x, width)) : x;
  } else if (e.kind == E::Unary) {
    result = e.text == "!" ? !x : uint64_t(0) - x;
  } else if (e.text == "&&" && x == 0)
    result = 0;
  else if (e.text == "||" && x != 0)
    result = 1;
  else {
    auto b = evaluate(e.args.at(1), pointerWidth);
    auto y = number(b) & mask(width);
    const auto &op = e.text;
    if (op == "+")
      result = x + y;
    else if (op == "-")
      result = x - y;
    else if (op == "*")
      result = x * y;
    else if (op == "&")
      result = x & y;
    else if (op == "|")
      result = x | y;
    else if (op == "^")
      result = x ^ y;
    else if (op == "<<")
      result = x << (y % width);
    else if (op == ">>")
      result = signedType(a.type) ? static_cast<uint64_t>(sign(x, width) >> (y % width))
                                  : x >> (y % width);
    else if (op == "&&")
      result = x && y;
    else if (op == "||")
      result = x || y;
    else if (op == "==")
      result = x == y;
    else if (op == "!=")
      result = x != y;
    else if (op == "<")
      result = signedType(a.type) ? sign(x, width) < sign(y, width) : x < y;
    else if (op == ">")
      result = signedType(a.type) ? sign(x, width) > sign(y, width) : x > y;
    else if (op == "<=")
      result = signedType(a.type) ? sign(x, width) <= sign(y, width) : x <= y;
    else if (op == ">=")
      result = signedType(a.type) ? sign(x, width) >= sign(y, width) : x >= y;
    else if (op == "/" || op == "%") {
      if (!y)
        fail(e.loc, DiagnosticCode::InvalidConstant, "division by zero in constant expression");
      if (signedType(a.type)) {
        auto sx = sign(x, width), sy = sign(y, width);
        auto minimum =
            width == 64 ? std::numeric_limits<int64_t>::min() : -(int64_t(1) << (width - 1));
        if (sx == minimum && sy == -1)
          fail(e.loc, DiagnosticCode::InvalidConstant, "division overflow in constant expression");
        result = static_cast<uint64_t>(op == "/" ? sx / sy : sx % sy);
      } else
        result = op == "/" ? x / y : x % y;
    } else
      fail(e.loc, DiagnosticCode::InvalidConstant, "unsupported constant operator");
  }
  E value{};
  value.kind = E::Constant;
  value.type = e.type;
  value.loc = e.loc;
  auto resultWidth = bitWidth(e.type, pointerWidth);
  result &= mask(resultWidth);
  value.text =
      signedType(e.type) ? std::to_string(sign(result, resultWidth)) : std::to_string(result);
  return value;
}
}
hir::Expr evaluateConstant(const hir::Expr &e, unsigned pointerWidth) {
  allowed(e);
  return evaluate(e, pointerWidth);
}
}
