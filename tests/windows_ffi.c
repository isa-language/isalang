
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
typedef int32_t (*Callback)(int32_t);
int32_t apply(Callback callback, int32_t value) {
  return callback(value);
}
static int32_t twice(int32_t value) {
  return value * 2;
}
Callback get_callback(void) {
  return twice;
}
union Number {
  uint64_t integer;
  double decimal;
  uint8_t padding[56];
};
struct Data {
  uint8_t tag;
  union Number number;
  int32_t tail;
};
struct Data *get_data(void) {
  static struct Data data = {7, {0}, 9};
  return &data;
}
int64_t sum(int32_t count, ...) {
  va_list arguments;
  va_start(arguments, count);
  int64_t result = 0;
  for (int32_t n = 0; n < count; ++n)
    result += va_arg(arguments, int);
  va_end(arguments);
  return result;
}
double average(int32_t count, ...) {
  va_list arguments;
  va_start(arguments, count);
  double result = 0;
  for (int32_t n = 0; n < count; ++n)
    result += va_arg(arguments, double);
  va_end(arguments);
  return count ? result / count : 0;
}
int32_t *fixture_allocate(void) {
  int32_t *value = malloc(sizeof *value);
  if (!value)
    abort();
  *value = 42;
  return value;
}
void fixture_free(int32_t *value) {
  free(value);
}
