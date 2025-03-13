#include "isac_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ISAC_RT_TESTING
static size_t allocations, frees;
static int reporting;
static void report(void) {
  fprintf(stderr, "ISAC_RT allocations=%zu frees=%zu\n", allocations, frees);
}
static void track_alloc(void) {
  if (!reporting) {
    if (atexit(report))
      abort();
    reporting = 1;
  }
  ++allocations;
}
size_t isac_rt_test_allocations(void) {
  return allocations;
}
size_t isac_rt_test_frees(void) {
  return frees;
}
#endif
_Noreturn void isac_rt_panic(const char *message) {
  fprintf(stderr, "isac runtime panic: %s\n", message);
  abort();
}
void *isac_rt_alloc(size_t size) {
  void *p = malloc(size ? size : 1);
  if (!p)
    isac_rt_panic("allocation failed");
#ifdef ISAC_RT_TESTING
  track_alloc();
#endif
  return p;
}
void *isac_rt_alloc_zeroed(size_t size) {
  void *p = calloc(1, size ? size : 1);
  if (!p)
    isac_rt_panic("allocation failed");
#ifdef ISAC_RT_TESTING
  track_alloc();
#endif
  return p;
}
void isac_rt_free(void *ptr) {
#ifdef ISAC_RT_TESTING
  if (ptr)
    ++frees;
#endif
  free(ptr);
}
void *isac_rt_memcpy(void *d, const void *s, size_t n) {
  return memcpy(d, s, n);
}
void *isac_rt_memmove(void *d, const void *s, size_t n) {
  return memmove(d, s, n);
}
void isac_rt_print_i32(int32_t n) {
  printf("%d\n", (int)n);
}
void isac_rt_print_usize(size_t n) {
  printf("%zu\n", n);
}
void isac_rt_println(const char *s) {
  puts(s);
}
int32_t isac_rt_div_i32(int32_t a, int32_t b) {
  if (!b || (a == INT32_MIN && b == -1))
    isac_rt_panic("invalid division");
  return a / b;
}
size_t isac_rt_div_usize(size_t a, size_t b) {
  if (!b)
    isac_rt_panic("division by zero");
  return a / b;
}
int32_t isac_rt_load_i32(const void *p) {
  return *(const int32_t *)p;
}
void isac_rt_store_i32(void *p, int32_t v) {
  *(int32_t *)p = v;
}
typedef struct {
  size_t len;
  int32_t data[];
} IsacBuffer;
size_t isac_rt_buffer_size(size_t n) {
  if (n > (SIZE_MAX - sizeof(IsacBuffer)) / sizeof(int32_t))
    isac_rt_panic("buffer allocation size overflow");
  return sizeof(IsacBuffer) + n * sizeof(int32_t);
}
void isac_rt_buffer_init(void *p, size_t n) {
  ((IsacBuffer *)p)->len = n;
}
size_t isac_rt_buffer_len(const void *p) {
  return ((const IsacBuffer *)p)->len;
}
int32_t isac_rt_buffer_read(const void *p, size_t i) {
  const IsacBuffer *b = p;
  if (i >= b->len)
    isac_rt_panic("buffer index out of bounds");
  return b->data[i];
}
void isac_rt_buffer_write(void *p, size_t i, int32_t v) {
  IsacBuffer *b = p;
  if (i >= b->len)
    isac_rt_panic("buffer index out of bounds");
  b->data[i] = v;
}
