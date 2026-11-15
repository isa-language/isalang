#include "isac_runtime.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#endif

typedef struct {
  IsacScalarWorker worker;
  uint64_t argument, result;
} IsacThreadCall;

#ifdef _WIN32
static unsigned __stdcall run_scalar(void *opaque) {
#else
static void *run_scalar(void *opaque) {
#endif
  IsacThreadCall *call = opaque;
  call->result = call->worker(call->argument);
#ifdef _WIN32
  return 0;
#else
  return NULL;
#endif
}

uint64_t isac_rt_thread_run_u64(IsacScalarWorker worker, uint64_t argument) {
  if (!worker)
    isac_rt_panic("null scalar thread worker");
  IsacThreadCall call = {worker, argument, 0};
#ifdef _WIN32
  HANDLE thread = (HANDLE)_beginthreadex(NULL, 0, run_scalar, &call, 0, NULL);
  if (!thread)
    isac_rt_panic("thread creation failed");
  if (WaitForSingleObject(thread, INFINITE) != WAIT_OBJECT_0)
    isac_rt_panic("thread join failed");
  if (!CloseHandle(thread))
    isac_rt_panic("thread handle close failed");
#else
  pthread_t thread;
  if (pthread_create(&thread, NULL, run_scalar, &call) != 0)
    isac_rt_panic("thread creation failed");

  if (pthread_join(thread, NULL) != 0)
    isac_rt_panic("thread join failed");
#endif
  return call.result;
}

typedef struct {
  IsacScopedWorker worker;
  void *context;
} IsacScopedCall;
#ifdef _WIN32
static unsigned __stdcall run_scoped(void *opaque) {
#else
static void *run_scoped(void *opaque) {
#endif
  IsacScopedCall *call = opaque;
  call->worker(call->context);
#ifdef _WIN32
  return 0;
#else
  return NULL;
#endif
}
void isac_rt_thread_call(IsacScopedWorker worker, void *context) {
  if (!worker || !context)
    isac_rt_panic("null scoped thread callback/context");
  IsacScopedCall call = {worker, context};
#ifdef _WIN32
  HANDLE thread = (HANDLE)_beginthreadex(NULL, 0, run_scoped, &call, 0, NULL);
  if (!thread)
    isac_rt_panic("thread creation failed");

  if (WaitForSingleObject(thread, INFINITE) != WAIT_OBJECT_0)
    isac_rt_panic("thread join failed");
  if (!CloseHandle(thread))
    isac_rt_panic("thread handle close failed");
#else
  pthread_t thread;
  if (pthread_create(&thread, NULL, run_scoped, &call) != 0)
    isac_rt_panic("thread creation failed");
  if (pthread_join(thread, NULL) != 0)
    isac_rt_panic("thread join failed");
#endif
}
