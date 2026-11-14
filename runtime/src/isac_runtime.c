#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "isac_runtime.h"
#include "isac_allocator.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

uint64_t isac_rt_monotonic_ns(void) {
  struct timespec value;
  if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
    isac_rt_panic("monotonic clock unavailable");
  if (value.tv_sec < 0 || (uint64_t)value.tv_sec > (UINT64_MAX - 999999999) / 1000000000)
    isac_rt_panic("monotonic clock exceeds nanosecond representation");
  return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}
#ifdef ISAC_RT_TESTING
#include <stdatomic.h>
static _Atomic size_t allocations, frees;
static atomic_flag reporting = ATOMIC_FLAG_INIT;
static void report(void) {
  fprintf(stderr, "ISAC_RT allocations=%zu frees=%zu\n",
          atomic_load_explicit(&allocations, memory_order_relaxed),
          atomic_load_explicit(&frees, memory_order_relaxed));
}
static void track_alloc(void) {
  if (!atomic_flag_test_and_set_explicit(&reporting, memory_order_relaxed)) {
    if (atexit(report))
      abort();
  }
  atomic_fetch_add_explicit(&allocations, 1, memory_order_relaxed);
}
size_t isac_rt_test_allocations(void) {
  return atomic_load_explicit(&allocations, memory_order_relaxed);
}
size_t isac_rt_test_frees(void) {
  return atomic_load_explicit(&frees, memory_order_relaxed);
}
#endif
_Noreturn void isac_rt_panic(const char *message) {
  fprintf(stderr, "isac runtime panic: %s\n", message);
  abort();
}
void *isac_rt_alloc(size_t size) {
  void *p = isac_allocator_allocate(size ? size : 1, 0);
  if (!p)
    isac_rt_panic("allocation failed");
#ifdef ISAC_RT_TESTING
  track_alloc();
#endif
  return p;
}
void *isac_rt_alloc_zeroed(size_t size) {
  void *p = isac_allocator_allocate(size ? size : 1, 1);
  if (!p)
    isac_rt_panic("allocation failed");
#ifdef ISAC_RT_TESTING
  track_alloc();
#endif
  return p;
}
void isac_rt_free(void *ptr) {
  if (!ptr)
    return;
#ifdef ISAC_RT_TESTING
  if (ptr)
    atomic_fetch_add_explicit(&frees, 1, memory_order_relaxed);
#endif
  isac_allocator_deallocate(ptr);
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
void isac_rt_print_bool(int32_t n) {
  isac_rt_print_i32(n != 0);
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

void isac_rt_print_i64(int64_t n) {
  printf("%lld\n", (long long)n);
}
void isac_rt_print_u64(uint64_t n) {
  printf("%llu\n", (unsigned long long)n);
}
void isac_rt_print_f64(double n) {
  printf("%.17g\n", n);
}
int64_t isac_rt_arith_i64(int64_t a, int64_t b, int32_t bits, int32_t rem) {
  int64_t min = bits == 64 ? INT64_MIN : -(INT64_C(1) << (bits - 1));
  if (!b || (a == min && b == -1))
    isac_rt_panic("invalid division");
  return rem ? a % b : a / b;
}
uint64_t isac_rt_arith_u64(uint64_t a, uint64_t b, int32_t bits, int32_t rem) {
  (void)bits;
  if (!b)
    isac_rt_panic("division by zero");
  return rem ? a % b : a / b;
}

void *isac_rt_realloc(void *ptr, size_t size) {
  if (!ptr)
    return isac_rt_alloc(size);
  if (!size) {
    isac_rt_free(ptr);
    return NULL;
  }
  void *result = isac_allocator_reallocate(ptr, size);
  if (!result)
    isac_rt_panic("allocation failed");
  return result;
}
void isac_rt_string_new(IsacString *out) {
  *out = (IsacString){NULL, 0, 0};
}
void isac_rt_string_from(IsacString *out, const IsacStr *text) {
  isac_rt_string_new(out);
  if (text->len) {
    out->ptr = isac_rt_alloc(text->len);
    isac_rt_memcpy(out->ptr, text->ptr, text->len);
    out->len = out->capacity = text->len;
  }
}
void isac_rt_index_check(size_t index, size_t length) {
  if (index >= length)
    isac_rt_panic("index out of bounds");
}
size_t isac_rt_string_len(const IsacString *text) {
  return text->len;
}

int32_t isac_rt_string_is_char_boundary(const IsacString *text, size_t index) {
  return index <= text->len && (index == text->len || (text->ptr[index] & 0xc0) != 0x80);
}
uint32_t isac_rt_string_char_at(const IsacString *text, size_t index) {
  if (index >= text->len || !isac_rt_string_is_char_boundary(text, index))
    isac_rt_panic("invalid UTF-8 character index");

  uint8_t first = text->ptr[index];
  if (first < 0x80)
    return first;
  unsigned length = first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
  uint32_t value = first & (length == 2 ? 0x1f : length == 3 ? 0x0f : 0x07);
  for (unsigned n = 1; n < length; ++n)
    value = (value << 6) | (text->ptr[index + n] & 0x3f);
  return value;
}
size_t isac_rt_string_capacity(const IsacString *text) {
  return text->capacity;
}
int32_t isac_rt_string_is_empty(const IsacString *text) {
  return text->len == 0;
}
void isac_rt_string_as_str(IsacStr *out, const IsacString *text) {
  *out = (IsacStr){text->ptr, text->len};
}
void isac_rt_string_push_str(IsacString *text, const IsacStr *other) {
  if (other->len > SIZE_MAX - text->len)
    isac_rt_panic("String capacity overflow");
  size_t required = text->len + other->len;
  if (required > text->capacity) {
    size_t grown = text->capacity > SIZE_MAX / 2 ? SIZE_MAX : text->capacity * 2;
    if (grown < 8)
      grown = 8;
    if (grown < required)
      grown = required;
    text->ptr = isac_rt_realloc(text->ptr, grown);
    text->capacity = grown;
  }
  if (other->len)
    isac_rt_memcpy(text->ptr + text->len, other->ptr, other->len);
  text->len = required;
}
void isac_rt_string_clear(IsacString *text) {
  text->len = 0;
}
void isac_rt_string_drop(IsacString *text) {
  if (text->ptr)
    isac_rt_free(text->ptr);
  *text = (IsacString){NULL, 0, 0};
}
void isac_rt_print_str(const uint8_t *ptr, size_t len) {
  if (len)
    fwrite(ptr, 1, len, stdout);
  fputc('\n', stdout);
}
void isac_rt_print_view(const IsacStr *text) {
  isac_rt_print_str(text->ptr, text->len);
}
void isac_rt_panic_str(const IsacStr *text) {
  fputs("isac runtime panic: ", stderr);
  if (text->len)
    fwrite(text->ptr, 1, text->len, stderr);
  fputc('\n', stderr);
  abort();
}
void isac_rt_string_print(const IsacString *text) {
  isac_rt_print_str(text->ptr, text->len);
}

static void append_bytes(IsacString *out, const char *ptr, size_t len) {
  IsacStr view = {(const uint8_t *)ptr, len};
  isac_rt_string_push_str(out, &view);
}
int32_t isac_rt_str_equal(const IsacStr *left, const IsacStr *right) {
  return left->len == right->len && (!left->len || memcmp(left->ptr, right->ptr, left->len) == 0);
}
void isac_rt_stdout_write(const IsacStr *view) {
  if (view->len && fwrite(view->ptr, 1, view->len, stdout) != view->len)
    isac_rt_panic("stdout write failed");
}
void isac_rt_stderr_write(const IsacStr *view) {
  if (view->len && fwrite(view->ptr, 1, view->len, stderr) != view->len)
    isac_rt_panic("stderr write failed");
}
void isac_rt_string_fmt_str(IsacString *out, const IsacStr *value) {
  isac_rt_string_push_str(out, value);
}
void isac_rt_string_fmt_bool(IsacString *out, int32_t value) {
  append_bytes(out, value ? "true" : "false", value ? 4 : 5);
}
void isac_rt_string_fmt_char(IsacString *out, uint32_t cp) {
  char bytes[4];
  size_t len;
  if (cp < 128) {
    bytes[0] = cp;
    len = 1;
  } else if (cp < 2048) {
    bytes[0] = 0xc0 | (cp >> 6);
    bytes[1] = 0x80 | (cp & 63);
    len = 2;
  } else if (cp < 65536) {
    bytes[0] = 0xe0 | (cp >> 12);
    bytes[1] = 0x80 | ((cp >> 6) & 63);
    bytes[2] = 0x80 | (cp & 63);
    len = 3;
  } else {
    bytes[0] = 0xf0 | (cp >> 18);
    bytes[1] = 0x80 | ((cp >> 12) & 63);
    bytes[2] = 0x80 | ((cp >> 6) & 63);
    bytes[3] = 0x80 | (cp & 63);
    len = 4;
  }
  append_bytes(out, bytes, len);
}
void isac_rt_string_fmt_i8(IsacString *out, int8_t value) {
  char bytes[128];
  int len = snprintf(bytes, sizeof(bytes), "%lld", (long long)value);
  if (len < 0 || (size_t)len >= sizeof(bytes))
    isac_rt_panic("scalar formatting failed");
  append_bytes(out, bytes, (size_t)len);
}
void isac_rt_string_fmt_i16(IsacString *out, int16_t value) {
  char bytes[128];
  int len = snprintf(bytes, sizeof(bytes), "%lld", (long long)value);
  if (len < 0 || (size_t)len >= sizeof(bytes))
    isac_rt_panic("scalar formatting failed");
  append_bytes(out, bytes, (size_t)len);
}
void isac_rt_string_fmt_i32(IsacString *out, int32_t value) {
  char bytes[128];
  int len = snprintf(bytes, sizeof(bytes), "%lld", (long long)value);
  if (len < 0 || (size_t)len >= sizeof(bytes))
    isac_rt_panic("scalar formatting failed");
  append_bytes(out, bytes, (size_t)len);
}
void isac_rt_string_fmt_i64(IsacString *out, int64_t value) {
  char bytes[128];
  int len = snprintf(bytes, sizeof(bytes), "%lld", (long long)value);
  if (len < 0 || (size_t)len >= sizeof(bytes))
    isac_rt_panic("scalar formatting failed");
  append_bytes(out, bytes, (size_t)len);
}
void isac_rt_string_fmt_isize(IsacString *out, ptrdiff_t value) {
  char bytes[128];
  int len = snprintf(bytes, sizeof(bytes), "%lld", (long long)value);
  if (len < 0 || (size_t)len >= sizeof(bytes))
    isac_rt_panic("scalar formatting failed");
  append_bytes(out, bytes, (size_t)len);
}
void isac_rt_string_fmt_u8(IsacString *out, uint8_t value) {
  char bytes[128];
  int len = snprintf(bytes, sizeof(bytes), "%llu", (unsigned long long)value);
  if (len < 0 || (size_t)len >= sizeof(bytes))
    isac_rt_panic("scalar formatting failed");
  append_bytes(out, bytes, (size_t)len);
}
void isac_rt_string_fmt_u16(IsacString *out, uint16_t value) {
  char bytes[128];
  int len = snprintf(bytes, sizeof(bytes), "%llu", (unsigned long long)value);
  if (len < 0 || (size_t)len >= sizeof(bytes))
    isac_rt_panic("scalar formatting failed");
  append_bytes(out, bytes, (size_t)len);
}
void isac_rt_string_fmt_u32(IsacString *out, uint32_t value) {
  char bytes[128];
  int len = snprintf(bytes, sizeof(bytes), "%llu", (unsigned long long)value);
  if (len < 0 || (size_t)len >= sizeof(bytes))
    isac_rt_panic("scalar formatting failed");
  append_bytes(out, bytes, (size_t)len);
}
void isac_rt_string_fmt_u64(IsacString *out, uint64_t value) {
  char bytes[128];
  int len = snprintf(bytes, sizeof(bytes), "%llu", (unsigned long long)value);
  if (len < 0 || (size_t)len >= sizeof(bytes))
    isac_rt_panic("scalar formatting failed");
  append_bytes(out, bytes, (size_t)len);
}
void isac_rt_string_fmt_usize(IsacString *out, size_t value) {
  char bytes[128];
  int len = snprintf(bytes, sizeof(bytes), "%llu", (unsigned long long)value);
  if (len < 0 || (size_t)len >= sizeof(bytes))
    isac_rt_panic("scalar formatting failed");
  append_bytes(out, bytes, (size_t)len);
}
void isac_rt_string_fmt_f32(IsacString *out, float value) {
  char bytes[128];
  int len = snprintf(bytes, sizeof(bytes), "%.9g", (double)value);
  if (len < 0 || (size_t)len >= sizeof(bytes))
    isac_rt_panic("scalar formatting failed");
  append_bytes(out, bytes, (size_t)len);
}
void isac_rt_string_fmt_f64(IsacString *out, double value) {
  char bytes[128];
  int len = snprintf(bytes, sizeof(bytes), "%.17g", (double)value);
  if (len < 0 || (size_t)len >= sizeof(bytes))
    isac_rt_panic("scalar formatting failed");
  append_bytes(out, bytes, (size_t)len);
}

static int input_utf8(const uint8_t *p, size_t len) {
  for (size_t i = 0; i < len;) {
    uint32_t cp = p[i++];
    unsigned n;
    uint32_t min;
    if (cp < 128)
      continue;
    if (cp >= 0xc2 && cp <= 0xdf) {
      n = 1;
      min = 128;
      cp &= 31;
    } else if (cp >= 0xe0 && cp <= 0xef) {
      n = 2;
      min = 2048;
      cp &= 15;
    } else if (cp >= 0xf0 && cp <= 0xf4) {
      n = 3;
      min = 65536;
      cp &= 7;
    } else
      return 0;
    if (n > len - i)
      return 0;
    while (n--) {
      if ((p[i] & 0xc0) != 0x80)
        return 0;
      cp = (cp << 6) | (p[i++] & 63);
    }
    if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
      return 0;
  }
  return 1;
}
void isac_rt_ffi_utf8(const uint8_t *ptr, size_t len) {
  if ((len && !ptr) || !input_utf8(ptr, len))
    isac_rt_panic("invalid UTF-8 at C boundary");
}

void isac_rt_input_read_String(IsacInputResult *out) {
  *out = (IsacInputResult){0};
  fflush(stdout);
  int c;
  while ((c = fgetc(stdin)) != EOF && c != '\n') {
    char byte = (char)c;
    append_bytes(&out->text, &byte, 1);
  }
  if (ferror(stdin))
    out->error = 2;
  else if (c == EOF && !out->text.len)
    out->error = 1;
  else {
    if (out->text.len && out->text.ptr[out->text.len - 1] == '\r')
      --out->text.len;
    if (!input_utf8(out->text.ptr, out->text.len))
      out->error = 3;
  }
  if (out->error)
    isac_rt_string_drop(&out->text);
}
void isac_rt_input_drop(IsacInputResult *out) {
  isac_rt_string_drop(&out->text);
}
int32_t isac_rt_input_result_ok(const IsacInputResult *out) {
  return out->error == 0;
}
int32_t isac_rt_input_result_error(const IsacInputResult *out) {
  return out->error;
}
static void input_require_ok(const IsacInputResult *out) {
  if (out->error)
    isac_rt_panic("attempt to read the value of an input error; check is_ok() or error()");
}
void isac_rt_input_result_str(IsacStr *out, const IsacInputResult *input) {
  input_require_ok(input);
  isac_rt_string_as_str(out, &input->text);
}
static char *input_cstr(const IsacString *source) {
  if (source->len == SIZE_MAX)
    isac_rt_panic("input size overflow");
  char *text = isac_rt_alloc(source->len + 1);
  if (source->len)
    isac_rt_memcpy(text, source->ptr, source->len);
  text[source->len] = 0;
  return text;
}
void isac_rt_input_parse_i32(IsacInputResult *out, const IsacString *source) {
  *out = (IsacInputResult){0};
  char *text = input_cstr(source);
  char *end = text;
  errno = 0;
  long long value = strtoll(text, &end, 10);
  out->value.signed_value = value;
  if (value < INT32_MIN || value > INT32_MAX)
    out->error = 4;
  if (errno == ERANGE || end == text || (size_t)(end - text) != source->len ||
      isspace((unsigned char)text[0]))
    out->error = 4;
  isac_rt_free(text);
}
void isac_rt_input_read_i32(IsacInputResult *out) {
  IsacInputResult line;
  isac_rt_input_read_String(&line);
  if (line.error) {
    *out = line;
    return;
  }
  isac_rt_input_parse_i32(out, &line.text);
  isac_rt_input_drop(&line);
}

int32_t isac_rt_input_result_i32(const IsacInputResult *out) {
  input_require_ok(out);
  return (int32_t)out->value.signed_value;
}
void isac_rt_input_parse_i64(IsacInputResult *out, const IsacString *source) {
  *out = (IsacInputResult){0};
  char *text = input_cstr(source);
  char *end = text;
  errno = 0;
  long long value = strtoll(text, &end, 10);
  out->value.signed_value = value;
  if (errno == ERANGE || end == text || (size_t)(end - text) != source->len ||
      isspace((unsigned char)text[0]))
    out->error = 4;
  isac_rt_free(text);
}
void isac_rt_input_read_i64(IsacInputResult *out) {
  IsacInputResult line;
  isac_rt_input_read_String(&line);
  if (line.error) {
    *out = line;
    return;
  }
  isac_rt_input_parse_i64(out, &line.text);
  isac_rt_input_drop(&line);
}

int64_t isac_rt_input_result_i64(const IsacInputResult *out) {
  input_require_ok(out);
  return (int64_t)out->value.signed_value;
}
void isac_rt_input_parse_u32(IsacInputResult *out, const IsacString *source) {
  *out = (IsacInputResult){0};
  char *text = input_cstr(source);
  char *end = text;
  errno = 0;
  unsigned long long value = strtoull(text, &end, 10);
  out->value.unsigned_value = value;
  if (text[0] == '-')
    out->error = 4;
  if (value > UINT32_MAX)
    out->error = 4;
  if (errno == ERANGE || end == text || (size_t)(end - text) != source->len ||
      isspace((unsigned char)text[0]))
    out->error = 4;
  isac_rt_free(text);
}
void isac_rt_input_read_u32(IsacInputResult *out) {
  IsacInputResult line;
  isac_rt_input_read_String(&line);
  if (line.error) {
    *out = line;
    return;
  }
  isac_rt_input_parse_u32(out, &line.text);
  isac_rt_input_drop(&line);
}

uint32_t isac_rt_input_result_u32(const IsacInputResult *out) {
  input_require_ok(out);
  return (uint32_t)out->value.unsigned_value;
}
void isac_rt_input_parse_u64(IsacInputResult *out, const IsacString *source) {
  *out = (IsacInputResult){0};
  char *text = input_cstr(source);
  char *end = text;
  errno = 0;
  unsigned long long value = strtoull(text, &end, 10);
  out->value.unsigned_value = value;
  if (text[0] == '-')
    out->error = 4;
  if (errno == ERANGE || end == text || (size_t)(end - text) != source->len ||
      isspace((unsigned char)text[0]))
    out->error = 4;
  isac_rt_free(text);
}
void isac_rt_input_read_u64(IsacInputResult *out) {
  IsacInputResult line;
  isac_rt_input_read_String(&line);
  if (line.error) {
    *out = line;
    return;
  }
  isac_rt_input_parse_u64(out, &line.text);
  isac_rt_input_drop(&line);
}

uint64_t isac_rt_input_result_u64(const IsacInputResult *out) {
  input_require_ok(out);
  return (uint64_t)out->value.unsigned_value;
}
void isac_rt_input_parse_f32(IsacInputResult *out, const IsacString *source) {
  *out = (IsacInputResult){0};
  char *text = input_cstr(source);
  char *end = text;
  errno = 0;
  float value = strtof(text, &end);
  out->value.float_value = value;
  if (!isfinite(value))
    out->error = 4;
  if (errno == ERANGE || end == text || (size_t)(end - text) != source->len ||
      isspace((unsigned char)text[0]))
    out->error = 4;
  isac_rt_free(text);
}
void isac_rt_input_read_f32(IsacInputResult *out) {
  IsacInputResult line;
  isac_rt_input_read_String(&line);
  if (line.error) {
    *out = line;
    return;
  }
  isac_rt_input_parse_f32(out, &line.text);
  isac_rt_input_drop(&line);
}

float isac_rt_input_result_f32(const IsacInputResult *out) {
  input_require_ok(out);
  return (float)out->value.float_value;
}
void isac_rt_input_parse_f64(IsacInputResult *out, const IsacString *source) {
  *out = (IsacInputResult){0};
  char *text = input_cstr(source);
  char *end = text;
  errno = 0;
  double value = strtod(text, &end);
  out->value.float_value = value;
  if (!isfinite(value))
    out->error = 4;
  if (errno == ERANGE || end == text || (size_t)(end - text) != source->len ||
      isspace((unsigned char)text[0]))
    out->error = 4;
  isac_rt_free(text);
}
void isac_rt_input_read_f64(IsacInputResult *out) {
  IsacInputResult line;
  isac_rt_input_read_String(&line);
  if (line.error) {
    *out = line;
    return;
  }
  isac_rt_input_parse_f64(out, &line.text);
  isac_rt_input_drop(&line);
}

double isac_rt_input_result_f64(const IsacInputResult *out) {
  input_require_ok(out);
  return (double)out->value.float_value;
}
void isac_rt_input_parse_bool(IsacInputResult *out, const IsacString *source) {
  *out = (IsacInputResult){0};
  char *text = input_cstr(source);
  char *end = text;
  errno = 0;
  if (source->len == 4 && memcmp(text, "true", 4) == 0) {
    out->value.signed_value = 1;
    end = text + 4;
  } else if (source->len == 5 && memcmp(text, "false", 5) == 0) {
    out->value.signed_value = 0;
    end = text + 5;
  }
  if (errno == ERANGE || end == text || (size_t)(end - text) != source->len ||
      isspace((unsigned char)text[0]))
    out->error = 4;
  isac_rt_free(text);
}
void isac_rt_input_read_bool(IsacInputResult *out) {
  IsacInputResult line;
  isac_rt_input_read_String(&line);
  if (line.error) {
    *out = line;
    return;
  }
  isac_rt_input_parse_bool(out, &line.text);
  isac_rt_input_drop(&line);
}

int32_t isac_rt_input_result_bool(const IsacInputResult *out) {
  input_require_ok(out);
  return (int32_t)out->value.signed_value;
}

static void fmt_debug_byte(IsacString *out, uint8_t c, char quote) {
  const char *escape = c == '\n'   ? "\\n"
                       : c == '\r' ? "\\r"
                       : c == '\t' ? "\\t"
                       : c == 0    ? "\\0"
                       : c == '\\' ? "\\\\"
                                   : NULL;
  if (escape)
    append_bytes(out, escape, 2);
  else if (c == (uint8_t)quote) {
    char bytes[2] = {'\\', (char)c};
    append_bytes(out, bytes, 2);
  } else {
    char byte = (char)c;
    append_bytes(out, &byte, 1);
  }
}
void isac_rt_string_fmt_debug_str(IsacString *out, const IsacStr *value) {
  append_bytes(out, "\"", 1);
  for (size_t i = 0; i < value->len; ++i)
    fmt_debug_byte(out, value->ptr[i], '"');
  append_bytes(out, "\"", 1);
}
void isac_rt_string_fmt_debug_char(IsacString *out, uint32_t value) {
  append_bytes(out, "'", 1);
  if (value < 128)
    fmt_debug_byte(out, (uint8_t)value, '\'');
  else
    isac_rt_string_fmt_char(out, value);
  append_bytes(out, "'", 1);
}

void isac_rt_input_parse_view_i32(IsacInputResult *out, const IsacStr *source) {
  IsacString view = {(uint8_t *)source->ptr, source->len, 0};
  isac_rt_input_parse_i32(out, &view);
}

void isac_rt_input_parse_view_i64(IsacInputResult *out, const IsacStr *source) {
  IsacString view = {(uint8_t *)source->ptr, source->len, 0};
  isac_rt_input_parse_i64(out, &view);
}

void isac_rt_input_parse_view_u32(IsacInputResult *out, const IsacStr *source) {
  IsacString view = {(uint8_t *)source->ptr, source->len, 0};
  isac_rt_input_parse_u32(out, &view);
}

void isac_rt_input_parse_view_u64(IsacInputResult *out, const IsacStr *source) {
  IsacString view = {(uint8_t *)source->ptr, source->len, 0};
  isac_rt_input_parse_u64(out, &view);
}

void isac_rt_input_parse_view_f32(IsacInputResult *out, const IsacStr *source) {
  IsacString view = {(uint8_t *)source->ptr, source->len, 0};
  isac_rt_input_parse_f32(out, &view);
}

void isac_rt_input_parse_view_f64(IsacInputResult *out, const IsacStr *source) {
  IsacString view = {(uint8_t *)source->ptr, source->len, 0};
  isac_rt_input_parse_f64(out, &view);
}

void isac_rt_input_parse_view_bool(IsacInputResult *out, const IsacStr *source) {
  IsacString view = {(uint8_t *)source->ptr, source->len, 0};
  isac_rt_input_parse_bool(out, &view);
}

static IsacStr *env_arguments;
static size_t env_argument_count;
static int env_initialized;
void isac_rt_env_init(int32_t argc, char **argv) {
  if (env_initialized || argc < 0 || (argc && !argv))
    isac_rt_panic("invalid environment initialization");
  if ((size_t)argc > SIZE_MAX / sizeof(IsacStr))
    isac_rt_panic("argument list is too large");
  for (int32_t i = 0; i < argc; ++i) {
    if (!argv[i] || !input_utf8((const uint8_t *)argv[i], strlen(argv[i])))
      isac_rt_panic("command-line argument is not valid UTF-8");
  }
  env_argument_count = (size_t)argc;
  if (argc)
    env_arguments = isac_rt_alloc_zeroed((size_t)argc * sizeof(IsacStr));
  for (int32_t i = 0; i < argc; ++i) {
    size_t len = strlen(argv[i]);
    uint8_t *copy = isac_rt_alloc(len);
    isac_rt_memcpy(copy, argv[i], len);
    env_arguments[i].ptr = copy;
    env_arguments[i].len = len;
  }
  env_initialized = 1;
}
void isac_rt_env_destroy(void) {
  for (size_t i = 0; i < env_argument_count; ++i)
    isac_rt_free((void *)env_arguments[i].ptr);
  isac_rt_free(env_arguments);
  env_arguments = NULL;
  env_argument_count = 0;
  env_initialized = 0;
}
size_t isac_rt_env_argument_count(void) {
  if (!env_initialized)
    isac_rt_panic("environment was not initialized");
  return env_argument_count;
}
const IsacStr *isac_rt_env_argument(size_t index) {
  if (!env_initialized || index >= env_argument_count)
    isac_rt_panic("command-line argument index out of bounds");
  return &env_arguments[index];
}

typedef struct {
  uint8_t *data;
  size_t len, capacity, element_size;
} IsacVecStorage;
static size_t vec_bytes(size_t element_size, size_t count) {
  if (!element_size || count > SIZE_MAX / element_size)
    isac_rt_panic("vector capacity overflow");
  return count * element_size;
}
void *isac_rt_vec_new(size_t element_size, size_t capacity) {
  size_t bytes = vec_bytes(element_size, capacity);
  IsacVecStorage *vec = isac_rt_alloc(sizeof(*vec));
  *vec = (IsacVecStorage){NULL, 0, capacity, element_size};
  if (bytes)
    vec->data = isac_rt_alloc(bytes);
  return vec;
}
size_t isac_rt_vec_len(const void *ptr) {
  return ((const IsacVecStorage *)ptr)->len;
}
size_t isac_rt_vec_capacity(const void *ptr) {
  return ((const IsacVecStorage *)ptr)->capacity;
}
void *isac_rt_vec_data(void *ptr) {
  return ((IsacVecStorage *)ptr)->data;
}
void *isac_rt_vec_at(void *ptr, size_t index) {
  IsacVecStorage *vec = ptr;
  isac_rt_index_check(index, vec->len);
  return vec->data + index * vec->element_size;
}
void *isac_rt_vec_push_slot(void *ptr) {
  IsacVecStorage *vec = ptr;
  if (vec->len == SIZE_MAX)
    isac_rt_panic("vector length overflow");
  if (vec->len == vec->capacity) {
    size_t grown = vec->capacity > SIZE_MAX / 2 ? SIZE_MAX : vec->capacity * 2;
    if (grown < 4)
      grown = 4;
    size_t bytes = vec_bytes(vec->element_size, grown);
    vec->data = isac_rt_realloc(vec->data, bytes);
    vec->capacity = grown;
  }
  return vec->data + vec->len++ * vec->element_size;
}
void *isac_rt_vec_pop_slot(void *ptr) {
  IsacVecStorage *vec = ptr;
  if (!vec->len)
    return NULL;
  return vec->data + --vec->len * vec->element_size;
}
void *isac_rt_vec_insert_slot(void *ptr, size_t index) {
  IsacVecStorage *vec = ptr;
  if (index > vec->len)
    isac_rt_panic("vector insertion index out of bounds");
  size_t old_len = vec->len;
  isac_rt_vec_push_slot(ptr);
  uint8_t *slot = vec->data + index * vec->element_size;
  memmove(slot + vec->element_size, slot, (old_len - index) * vec->element_size);
  return slot;
}
void isac_rt_vec_remove_slot(void *ptr, size_t index) {
  IsacVecStorage *vec = ptr;
  isac_rt_index_check(index, vec->len);
  uint8_t *slot = vec->data + index * vec->element_size;
  memmove(slot, slot + vec->element_size, (vec->len - index - 1) * vec->element_size);
  --vec->len;
}
void isac_rt_vec_swap(void *ptr, size_t left, size_t right) {
  IsacVecStorage *vec = ptr;
  isac_rt_index_check(left, vec->len);
  isac_rt_index_check(right, vec->len);
  uint8_t *a = vec->data + left * vec->element_size;
  uint8_t *b = vec->data + right * vec->element_size;
  for (size_t i = 0; i < vec->element_size; ++i) {
    uint8_t byte = a[i];
    a[i] = b[i];
    b[i] = byte;
  }
}
void isac_rt_vec_clear_storage(void *ptr) {
  ((IsacVecStorage *)ptr)->len = 0;
}
void isac_rt_vec_drop_storage(void *ptr) {
  IsacVecStorage *vec = ptr;
  isac_rt_free(vec->data);
  isac_rt_free(vec);
}

static int os_cstr(const IsacStr *view, char **out) {
  *out = NULL;
  if (view->len == SIZE_MAX || (view->len && memchr(view->ptr, 0, view->len)))
    return EINVAL;
  char *text = isac_rt_alloc(view->len + 1);
  if (view->len)
    memcpy(text, view->ptr, view->len);
  text[view->len] = 0;
  *out = text;
  return 0;
}
int32_t isac_rt_string_read_file(IsacString *out, const IsacStr *path) {
  char *name;
  int error = os_cstr(path, &name);
  if (error)
    return error;
  FILE *file = fopen(name, "rb");
  error = file ? 0 : (errno ? errno : EIO);
  isac_rt_free(name);
  if (error)
    return error;

  uint8_t *data = NULL;
  size_t len = 0, capacity = 0;
  uint8_t chunk[4096];
  for (;;) {
    size_t n = fread(chunk, 1, sizeof chunk, file);
    if (n) {
      if (len > SIZE_MAX - n) {
        error = EOVERFLOW;
        break;
      }
      size_t required = len + n;
      if (required > capacity) {
        size_t grown = capacity > SIZE_MAX / 2 ? SIZE_MAX : capacity * 2;
        if (grown < 4096)
          grown = 4096;
        capacity = required > grown ? required : grown;
        data = isac_rt_realloc(data, capacity);
      }
      memcpy(data + len, chunk, n);
      len += n;
    }
    if (n < sizeof chunk) {
      if (ferror(file))
        error = errno ? errno : EIO;
      break;
    }
  }
  if (fclose(file) != 0 && !error)
    error = errno ? errno : EIO;
  if (!error && !input_utf8(data, len))
    error = EILSEQ;
  if (error) {
    isac_rt_free(data);
    return error;
  }
  isac_rt_string_drop(out);
  *out = (IsacString){data, len, capacity};
  return 0;
}
int32_t isac_rt_string_env_var(IsacString *out, const IsacStr *name) {
  char *key;
  int error = os_cstr(name, &key);
  if (error)
    return error;
  if (!name->len || strchr(key, '=')) {
    isac_rt_free(key);
    return EINVAL;
  }
  const char *value = getenv(key);
  isac_rt_free(key);
  if (!value)
    return ENOENT;
  size_t len = strlen(value);
  if (!input_utf8((const uint8_t *)value, len))
    return EILSEQ;
  IsacStr view = {(const uint8_t *)value, len};
  IsacString temporary;
  isac_rt_string_from(&temporary, &view);
  isac_rt_string_drop(out);
  *out = temporary;
  return 0;
}
int32_t isac_rt_fs_write_text(const IsacStr *path, const IsacStr *text) {
  char *name;
  int error = os_cstr(path, &name);
  if (error)
    return error;
  FILE *file = fopen(name, "wb");
  error = file ? 0 : (errno ? errno : EIO);
  isac_rt_free(name);
  if (error)
    return error;
  size_t written = 0;
  while (written < text->len) {
    size_t n = fwrite(text->ptr + written, 1, text->len - written, file);
    if (!n) {
      error = errno ? errno : EIO;
      break;
    }
    written += n;
  }
  if (fclose(file) != 0 && !error)
    error = errno ? errno : EIO;
  return error;
}
int32_t isac_rt_fs_exists(const IsacStr *path) {
  char *name;
  int error = os_cstr(path, &name);
  if (error)
    return -error;
  struct stat info;
  int result = stat(name, &info);
  error = result == 0 ? 0 : errno;
  isac_rt_free(name);
  return !result ? 1 : error == ENOENT || error == ENOTDIR ? 0 : -error;
}
int32_t isac_rt_fs_remove_file(const IsacStr *path) {
  char *name;
  int error = os_cstr(path, &name);
  if (error)
    return error;
  if (unlink(name))
    error = errno;
  isac_rt_free(name);
  return error;
}
int32_t isac_rt_fs_create_dir(const IsacStr *path) {
  char *name;
  int error = os_cstr(path, &name);
  if (error)
    return error;
  if (mkdir(name, 0777))
    error = errno;
  isac_rt_free(name);
  return error;
}
int64_t isac_rt_process_run(const IsacStr *program, const IsacStr *argument, int32_t has_argument) {
  char *name, *arg = NULL;
  int error = os_cstr(program, &name);
  if (error)
    return -error;
  if (has_argument)
    error = os_cstr(argument, &arg);
  if (error) {
    isac_rt_free(name);
    return -error;
  }
  if (fflush(stdout) != 0 || fflush(stderr) != 0) {
    error = errno ? errno : EIO;
    isac_rt_free(name);
    isac_rt_free(arg);
    return -error;
  }
  char *argv[3] = {name, arg, NULL};
  extern char **environ;
  pid_t child;
  error = posix_spawnp(&child, name, NULL, NULL, argv, environ);
  isac_rt_free(name);
  isac_rt_free(arg);
  if (error)
    return -error;
  int status;
  while (waitpid(child, &status, 0) < 0) {
    if (errno != EINTR)
      return -errno;
  }
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  if (WIFSIGNALED(status))
    return 128 + WTERMSIG(status);
  return -EIO;
}
int32_t isac_rt_sleep_ns(uint64_t nanos) {
  uint64_t seconds = nanos / UINT64_C(1000000000);
  time_t sec = (time_t)seconds;
  if (sec < 0 || (uint64_t)sec != seconds)
    return EOVERFLOW;
  struct timespec request = {sec, (long)(nanos % UINT64_C(1000000000))};
  while (nanosleep(&request, &request)) {
    if (errno != EINTR)
      return errno;
  }
  return 0;
}
uint64_t isac_rt_unix_ns(void) {
  struct timespec value;
  if (clock_gettime(CLOCK_REALTIME, &value))
    isac_rt_panic("wall clock unavailable");
  if (value.tv_sec < 0 || (uint64_t)value.tv_sec > (UINT64_MAX - 999999999) / 1000000000)
    isac_rt_panic("wall clock exceeds nanosecond representation");
  return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}
