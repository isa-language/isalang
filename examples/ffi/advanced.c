#include <stdint.h>
#include <stdarg.h>
typedef int32_t (*Callback)(int32_t);
int32_t apply(Callback callback, int32_t value) {
  return callback(value);
}
int64_t sum(int32_t count, ...) {
  int64_t result = 0;
  va_list arguments;
  va_start(arguments, count);
  for (int32_t i = 0; i < count; ++i)
    result += va_arg(arguments, int);
  va_end(arguments);
  return result;
}
union Number {
  uint64_t integer;
  double decimal;
  uint8_t padding[56];
};
union Number *number(void) {
  static union Number value;
  return &value;
}
