#include <stddef.h>
#include <stdint.h>
int32_t sum(const int32_t *values, size_t length) {
  int32_t result = 0;
  for (size_t i = 0; i < length; ++i)
    result += values[i];
  return result;
}
void update(uint8_t *values) {
  values[0] = 10;
  values[1] = 20;
  values[2] = 30;
}
size_t text_length(const uint8_t *text, size_t length) {
  (void)text;
  return length;
}
