#pragma once
#include <string>
namespace isac {
enum class FunctionKind { Normal, Async };
struct FunctionEffects {
  bool io = false, allocation = false, asynchronous = false, unsafeEffect = false;
  bool threading = false;
  bool operator==(const FunctionEffects &) const = default;
  void merge(const FunctionEffects &other) {
    io |= other.io;
    allocation |= other.allocation;
    asynchronous |= other.asynchronous;
    unsafeEffect |= other.unsafeEffect;
    threading |= other.threading;
  }
  std::string describe() const {
    std::string result;
    if (io)
      result += " IO";
    if (allocation)
      result += " Allocation";
    if (asynchronous)
      result += " Async";
    if (unsafeEffect)
      result += " Unsafe";
    if (threading)
      result += " Thread";
    return result.empty() ? " Pure" : result;
  }
};
}
