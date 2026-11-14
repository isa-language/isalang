#include "isac_runtime.h"
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

enum { Workers = 8, Iterations = 1000 };
static void *worker(void *argument) {
  uintptr_t id = (uintptr_t)argument;
  for (size_t i = 0; i < Iterations; ++i) {
    unsigned char *bytes = isac_rt_alloc_zeroed(64);
    for (size_t j = 0; j < 64; ++j)
      if (bytes[j])
        abort();
    bytes[0] = (unsigned char)id;
    isac_rt_free(bytes);
    int32_t *value = isac_rt_alloc(sizeof(*value));
    *value = (int32_t)(id + i);
    if (*value != (int32_t)(id + i))
      abort();
    isac_rt_free(value);
  }
  return NULL;
}
int main(void) {
  pthread_t workers[Workers];
  for (uintptr_t n = 0; n < Workers; ++n)
    if (pthread_create(&workers[n], NULL, worker, (void *)n))
      abort();
  for (size_t n = 0; n < Workers; ++n)
    if (pthread_join(workers[n], NULL))
      abort();

  return isac_rt_test_allocations() != Workers * Iterations * 2 ||
         isac_rt_test_frees() != Workers * Iterations * 2;
}
