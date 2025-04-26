#include <errno.h>
#include <math.h>
#include <ctype.h>
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
  void *result = realloc(ptr, size);
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
size_t isac_rt_string_len(const IsacString *text) {
  return text->len;
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
void isac_rt_string_print(const IsacString *text) {
  isac_rt_print_str(text->ptr, text->len);
}

static void append_bytes(IsacString *out, const char *ptr, size_t len) {
  IsacStr view = {(const uint8_t *)ptr, len};
  isac_rt_string_push_str(out, &view);
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
static char *input_cstr(IsacInputResult *out) {
  isac_rt_input_read_String(out);
  if (out->error)
    return NULL;
  if (out->text.len == SIZE_MAX)
    isac_rt_panic("input size overflow");
  char *text = isac_rt_alloc(out->text.len + 1);
  if (out->text.len)
    isac_rt_memcpy(text, out->text.ptr, out->text.len);
  text[out->text.len] = 0;
  return text;
}
void isac_rt_input_read_i32(IsacInputResult *out) {
  char *text = input_cstr(out);
  if (!text)
    return;
  char *end = text;
  errno = 0;
  long long value = strtoll(text, &end, 10);
  out->value.signed_value = value;
  if (value < INT32_MIN || value > INT32_MAX)
    out->error = 4;
  if (errno == ERANGE || end == text || (size_t)(end - text) != out->text.len ||
      isspace((unsigned char)text[0]))
    out->error = 4;
  isac_rt_free(text);
  isac_rt_string_drop(&out->text);
}
int32_t isac_rt_input_result_i32(const IsacInputResult *out) {
  input_require_ok(out);
  return (int32_t)out->value.signed_value;
}
void isac_rt_input_read_i64(IsacInputResult *out) {
  char *text = input_cstr(out);
  if (!text)
    return;
  char *end = text;
  errno = 0;
  long long value = strtoll(text, &end, 10);
  out->value.signed_value = value;
  if (errno == ERANGE || end == text || (size_t)(end - text) != out->text.len ||
      isspace((unsigned char)text[0]))
    out->error = 4;
  isac_rt_free(text);
  isac_rt_string_drop(&out->text);
}
int64_t isac_rt_input_result_i64(const IsacInputResult *out) {
  input_require_ok(out);
  return (int64_t)out->value.signed_value;
}
void isac_rt_input_read_u32(IsacInputResult *out) {
  char *text = input_cstr(out);
  if (!text)
    return;
  char *end = text;
  errno = 0;
  unsigned long long value = strtoull(text, &end, 10);
  out->value.unsigned_value = value;
  if (text[0] == '-')
    out->error = 4;
  if (value > UINT32_MAX)
    out->error = 4;
  if (errno == ERANGE || end == text || (size_t)(end - text) != out->text.len ||
      isspace((unsigned char)text[0]))
    out->error = 4;
  isac_rt_free(text);
  isac_rt_string_drop(&out->text);
}
uint32_t isac_rt_input_result_u32(const IsacInputResult *out) {
  input_require_ok(out);
  return (uint32_t)out->value.unsigned_value;
}
void isac_rt_input_read_u64(IsacInputResult *out) {
  char *text = input_cstr(out);
  if (!text)
    return;
  char *end = text;
  errno = 0;
  unsigned long long value = strtoull(text, &end, 10);
  out->value.unsigned_value = value;
  if (text[0] == '-')
    out->error = 4;
  if (errno == ERANGE || end == text || (size_t)(end - text) != out->text.len ||
      isspace((unsigned char)text[0]))
    out->error = 4;
  isac_rt_free(text);
  isac_rt_string_drop(&out->text);
}
uint64_t isac_rt_input_result_u64(const IsacInputResult *out) {
  input_require_ok(out);
  return (uint64_t)out->value.unsigned_value;
}
void isac_rt_input_read_f32(IsacInputResult *out) {
  char *text = input_cstr(out);
  if (!text)
    return;
  char *end = text;
  errno = 0;
  float value = strtof(text, &end);
  out->value.float_value = value;
  if (!isfinite(value))
    out->error = 4;
  if (errno == ERANGE || end == text || (size_t)(end - text) != out->text.len ||
      isspace((unsigned char)text[0]))
    out->error = 4;
  isac_rt_free(text);
  isac_rt_string_drop(&out->text);
}
float isac_rt_input_result_f32(const IsacInputResult *out) {
  input_require_ok(out);
  return (float)out->value.float_value;
}
void isac_rt_input_read_f64(IsacInputResult *out) {
  char *text = input_cstr(out);
  if (!text)
    return;
  char *end = text;
  errno = 0;
  double value = strtod(text, &end);
  out->value.float_value = value;
  if (!isfinite(value))
    out->error = 4;
  if (errno == ERANGE || end == text || (size_t)(end - text) != out->text.len ||
      isspace((unsigned char)text[0]))
    out->error = 4;
  isac_rt_free(text);
  isac_rt_string_drop(&out->text);
}
double isac_rt_input_result_f64(const IsacInputResult *out) {
  input_require_ok(out);
  return (double)out->value.float_value;
}
void isac_rt_input_read_bool(IsacInputResult *out) {
  char *text = input_cstr(out);
  if (!text)
    return;
  char *end = text;
  errno = 0;
  if (out->text.len == 4 && memcmp(text, "true", 4) == 0) {
    out->value.signed_value = 1;
    end = text + 4;
  } else if (out->text.len == 5 && memcmp(text, "false", 5) == 0) {
    out->value.signed_value = 0;
    end = text + 5;
  }
  if (errno == ERANGE || end == text || (size_t)(end - text) != out->text.len ||
      isspace((unsigned char)text[0]))
    out->error = 4;
  isac_rt_free(text);
  isac_rt_string_drop(&out->text);
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
