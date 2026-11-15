
#include "isac_runtime.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static DWORD caller;
static uint64_t worker(uint64_t value) {
  assert(GetCurrentThreadId() != caller);

  uint64_t *result = isac_rt_alloc(sizeof *result);
  *result = value * 3 + 1;
  return (uint64_t)(uintptr_t)result;
}
typedef struct {
  uint64_t input, output;
} Context;
static void scoped(void *opaque) {
  assert(GetCurrentThreadId() != caller);
  Context *context = opaque;
  context->output = context->input * 7 + 3;
}
static IsacStr str(const char *text) {
  IsacStr value = {(const uint8_t *)text, strlen(text)};
  return value;
}
static int equals(const IsacString *value, const char *text) {
  return value->len == strlen(text) && !memcmp(value->ptr, text, value->len);
}
int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "null-worker")) {
    isac_rt_thread_run_u64(NULL, 0);
    return 99;
  }
  isac_rt_env_init(argc, argv);
  assert(isac_rt_env_argument_count() == 2);
  IsacStr expected = str("Ol\xc3\xa1 \xe4\xb8\x96\xe7\x95\x8c \xf0\x9f\x8c\x8d");
  const IsacStr *argument = isac_rt_env_argument(1);
  assert(argument->len == expected.len && !memcmp(argument->ptr, expected.ptr, expected.len));
  IsacString text;
  isac_rt_string_new(&text);
  assert(SetEnvironmentVariableW(L"ISAC_WINDOWS_TEST", L"Ol\x00e1 \x4e16\x754c"));
  IsacStr name = str("ISAC_WINDOWS_TEST");
  assert(!isac_rt_string_env_var(&text, &name));
  assert(equals(&text, "Ol\xc3\xa1 \xe4\xb8\x96\xe7\x95\x8c"));
  assert(SetEnvironmentVariableW(L"ISAC_WINDOWS_TEST", L""));
  assert(!isac_rt_string_env_var(&text, &name) && text.len == 0);
  assert(SetEnvironmentVariableW(L"ISAC_WINDOWS_TEST", NULL));
  assert(isac_rt_string_env_var(&text, &name) == ENOENT);
  IsacStr path = str("arquivo Ol\xc3\xa1 \xe4\xb8\x96\xe7\x95\x8c.txt");
  assert(!isac_rt_fs_write_text(&path, &expected));
  assert(isac_rt_fs_exists(&path) == 1);
  assert(!isac_rt_string_read_file(&text, &path));
  assert(text.len == expected.len && !memcmp(text.ptr, expected.ptr, expected.len));
  assert(!isac_rt_fs_remove_file(&path));
  assert(isac_rt_fs_exists(&path) == 0);
  assert(isac_rt_string_read_file(&text, &path) == ENOENT);

  assert(text.len == expected.len && !memcmp(text.ptr, expected.ptr, expected.len));
  IsacStr bad = str("\xff"), embedded_nul = {(const uint8_t *)"a\0b", 3};
  assert(isac_rt_fs_exists(&bad) == -EILSEQ);
  assert(isac_rt_fs_exists(&embedded_nul) == -EINVAL);
  uint64_t before = isac_rt_monotonic_ns();
  assert(!isac_rt_sleep_ns(UINT64_C(2000000)));
  assert(isac_rt_monotonic_ns() >= before);
  assert(isac_rt_unix_ns() > UINT64_C(1700000000000000000));
  caller = GetCurrentThreadId();
  for (uint64_t n = 0; n < 32; ++n) {
    uint64_t *result = (uint64_t *)(uintptr_t)isac_rt_thread_run_u64(worker, n);
    assert(*result == n * 3 + 1);
    isac_rt_free(result);
    Context context = {n, 0};
    isac_rt_thread_call(scoped, &context);
    assert(context.output == n * 7 + 3);
  }
  IsacStr child = str("child helper.exe"), empty = str("");
  assert(isac_rt_process_run(&child, &empty, 0) == 17);

  IsacStr tricky = str("a b\"c\\\\ & exit 91");
  assert(isac_rt_process_run(&child, &tricky, 1) == 23);
  IsacStr trailing = str("a b\\\\");
  assert(isac_rt_process_run(&child, &trailing, 1) == 24);
  IsacStr missing = str("isac-no-such-child-9b4.exe");
  assert(isac_rt_process_run(&missing, &empty, 0) == -ENOENT);
  isac_rt_string_drop(&text);
  isac_rt_env_destroy();
  assert(isac_rt_test_allocations() == isac_rt_test_frees());
  puts("Windows runtime OK");
  return 0;
}
