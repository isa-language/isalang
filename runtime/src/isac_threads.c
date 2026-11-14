#include "isac_runtime.h"
#include <pthread.h>

typedef struct {
  IsacScalarWorker worker;
  uint64_t argument, result;
} IsacThreadCall;

static void *run_scalar(void *opaque) {
  IsacThreadCall *call = opaque;
  call->result = call->worker(call->argument);
  return NULL;
}

uint64_t isac_rt_thread_run_u64(IsacScalarWorker worker, uint64_t argument) {
  if (!worker)
    isac_rt_panic("null scalar thread worker");
  IsacThreadCall call = {worker, argument, 0};
  pthread_t thread;
  if (pthread_create(&thread, NULL, run_scalar, &call) != 0)
    isac_rt_panic("thread creation failed");

  if (pthread_join(thread, NULL) != 0)
    isac_rt_panic("thread join failed");
  return call.result;
}

typedef struct {
  IsacScopedWorker worker;
  void *context;
} IsacScopedCall;
static void *run_scoped(void *opaque) {
  IsacScopedCall *call = opaque;
  call->worker(call->context);
  return NULL;
}
void isac_rt_thread_call(IsacScopedWorker worker, void *context) {
  if (!worker || !context)
    isac_rt_panic("null scoped thread callback/context");
  IsacScopedCall call = {worker, context};
  pthread_t thread;
  if (pthread_create(&thread, NULL, run_scoped, &call) != 0)
    isac_rt_panic("thread creation failed");
  if (pthread_join(thread, NULL) != 0)
    isac_rt_panic("thread join failed");
}
