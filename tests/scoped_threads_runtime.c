#include "isac_runtime.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

static pthread_t caller;
static uint64_t worker(uint64_t value) {
  assert(!pthread_equal(caller, pthread_self()));
  return value * UINT64_C(3) + UINT64_C(1);
}
typedef struct {
  uint64_t input, output;
} Context;
static void scoped(void *opaque) {
  assert(!pthread_equal(caller, pthread_self()));
  Context *context = opaque;
  context->output = context->input * 7 + 3;
}

int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int __real_pthread_join(pthread_t, void **);
static int failure;
int __wrap_pthread_create(pthread_t *id, const pthread_attr_t *attr, void *(*run)(void *),
                          void *data) {
  if (failure == 1)
    return EAGAIN;
  return __real_pthread_create(id, attr, run, data);
}
int __wrap_pthread_join(pthread_t id, void **result) {

  int status = __real_pthread_join(id, result);
  return failure == 2 ? EINVAL : status;
}
int main(int argc, char **argv) {
  caller = pthread_self();
  int mode = argc > 1 ? atoi(argv[1]) : 0;
  failure = mode > 3 ? mode - 3 : mode;
  if (mode > 3) {
    Context context = {9, 0};
    isac_rt_thread_call(mode == 6 ? NULL : scoped, mode == 7 ? NULL : &context);
    assert(context.output == 66);
    return 0;
  }
  for (uint64_t n = 0; n < 64; ++n)
    assert(isac_rt_thread_run_u64(worker, n) == n * 3 + 1);
  if (failure == 3)
    isac_rt_thread_run_u64(NULL, 0);
  for (uint64_t n = 0; n < 64; ++n) {
    Context context = {n, 0};
    isac_rt_thread_call(scoped, &context);
    assert(context.output == n * 7 + 3);
  }
}
