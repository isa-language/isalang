#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
extern int32_t isalang_add(int32_t, int32_t);
extern double isalang_half(double);
extern void isalang_increment(int32_t *);
extern bool isalang_bool(bool);
extern int8_t isalang_i8(int8_t);
extern uint8_t isalang_u8(uint8_t);
int main(void) {
  int32_t value = 41;
  isalang_increment(&value);
  printf("%d %.1f %d\n", isalang_add(20, 22), isalang_half(5.0), value);
  return value == 42 && isalang_bool(false) && !isalang_bool(true) && isalang_i8(-7) == -7 &&
                 isalang_u8(250) == 250
             ? 0
             : 1;
}
