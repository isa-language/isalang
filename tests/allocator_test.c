#include "isac_allocator.h"
#include "isac_runtime.h"
#include <stdint.h>
#include <string.h>

void test_allocator_fail(int);
size_t test_allocator_live(void);
size_t test_allocator_allocations(void);
size_t test_allocator_reallocations(void);
size_t test_allocator_deallocations(void);
int main(int argc, char **argv) {
  if (argc == 2) {
    unsigned char *p = NULL;
    if (!strcmp(argv[1], "realloc"))
      p = isac_rt_alloc(8);
    test_allocator_fail(1);
    if (!strcmp(argv[1], "zeroed"))
      p = isac_rt_alloc_zeroed(8);
    else if (!strcmp(argv[1], "realloc"))
      p = isac_rt_realloc(p, 16);
    else
      p = isac_rt_alloc(8);
    isac_rt_free(p);
    return 1;
  }
  isac_rt_free(NULL);
  unsigned char *p = isac_rt_alloc_zeroed(8);
  if ((uintptr_t)p % _Alignof(max_align_t))
    return 2;
  for (size_t i = 0; i < 8; ++i)
    if (p[i])
      return 3;
  memcpy(p, "storage", 8);
  p = isac_rt_realloc(p, 64);
  if (memcmp(p, "storage", 8))
    return 4;
  p = isac_rt_realloc(p, 8);
  if (memcmp(p, "storage", 8))
    return 5;

  test_allocator_fail(1);
  if (isac_allocator_reallocate(p, 128) != NULL || memcmp(p, "storage", 8))
    return 6;
  test_allocator_fail(0);
  if (isac_rt_realloc(p, 0) != NULL || test_allocator_live())
    return 7;
  p = isac_rt_realloc(NULL, 0);
  if (!p)
    return 8;
  isac_rt_free(p);

  char arg0[] = "test", arg1[] = "Ol\303\241";
  char *args[] = {arg0, arg1};
  isac_rt_env_init(2, args);
  if (isac_rt_env_argument_count() != 2)
    return 9;
  IsacString s;
  isac_rt_string_from(&s, isac_rt_env_argument(1));
  static const uint8_t bytes[] = " grown with the selected provider";
  IsacStr suffix = {bytes, sizeof(bytes) - 1};
  isac_rt_string_push_str(&s, &suffix);
  isac_rt_string_drop(&s);
  void *vec = isac_rt_vec_new(sizeof(int32_t), 0);
  for (int32_t i = 0; i < 128; ++i)
    *(int32_t *)isac_rt_vec_push_slot(vec) = i;
  if (*(int32_t *)isac_rt_vec_at(vec, 127) != 127)
    return 10;
  isac_rt_vec_drop_storage(vec);
  isac_rt_env_destroy();
  if (test_allocator_live() || !test_allocator_reallocations() ||
      test_allocator_allocations() != test_allocator_deallocations())
    return 11;
  if (isac_rt_test_allocations() != test_allocator_allocations() ||
      isac_rt_test_frees() != test_allocator_deallocations())
    return 12;
  return 0;
}
