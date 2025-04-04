#include "isac_runtime.h"
#include <stdint.h>
#include <string.h>
int main(void) {
  unsigned char *a = isac_rt_alloc(16), *b = isac_rt_alloc_zeroed(16);
  for (size_t i = 0; i < 16; ++i)
    if (b[i] != 0)
      return 1;
  for (size_t i = 0; i < 16; ++i)
    a[i] = (unsigned char)i;
  if (isac_rt_memcpy(b, a, 16) != b || memcmp(a, b, 16))
    return 2;
  isac_rt_memmove(b + 1, b, 15);
  for (size_t i = 1; i < 16; ++i)
    if (b[i] != (unsigned char)(i - 1))
      return 3;
  isac_rt_free(a);
  isac_rt_free(b);
  if (isac_rt_test_allocations() != 2 || isac_rt_test_frees() != 2)
    return 4;
  isac_rt_println("runtime ok");
  return 0;
}
