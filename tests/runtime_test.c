#include "isac_runtime.h"
#include <stdint.h>
#include <string.h>
int main(int argc, char **argv) {
  if (argc == 2 && strcmp(argv[1], "oom") == 0) {
    void *p = isac_rt_alloc(SIZE_MAX);
    isac_rt_free(p);
    return 5;
  }
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

  void *vec = isac_rt_vec_new(sizeof(int32_t), 0);
  if (isac_rt_vec_len(vec) || isac_rt_vec_capacity(vec) || isac_rt_vec_pop_slot(vec))
    return 6;
  for (int32_t i = 0; i < 4096; ++i) {
    int32_t *slot = isac_rt_vec_push_slot(vec);
    *slot = i;
  }
  if (isac_rt_vec_len(vec) != 4096 || isac_rt_vec_capacity(vec) < 4096)
    return 7;
  for (size_t i = 0; i < 4096; ++i)
    if (*(int32_t *)isac_rt_vec_at(vec, i) != (int32_t)i)
      return 8;
  for (int32_t i = 4095; i >= 0; --i)
    if (*(int32_t *)isac_rt_vec_pop_slot(vec) != i)
      return 9;
  if (isac_rt_vec_pop_slot(vec) != NULL)
    return 10;
  *(int32_t *)isac_rt_vec_push_slot(vec) = 42;
  isac_rt_vec_clear_storage(vec);
  if (isac_rt_vec_len(vec))
    return 11;
  isac_rt_vec_drop_storage(vec);
  if (isac_rt_test_allocations() != 4 || isac_rt_test_frees() != 4)
    return 12;
  isac_rt_println("runtime ok");
  return 0;
}
