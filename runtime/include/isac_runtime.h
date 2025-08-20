#ifndef ISAC_RUNTIME_H
#define ISAC_RUNTIME_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void *isac_rt_alloc(size_t size);
uint64_t isac_rt_monotonic_ns(void);
void *isac_rt_alloc_zeroed(size_t size);
void isac_rt_free(void *ptr);
void *isac_rt_memcpy(void *dst, const void *src, size_t size);
void *isac_rt_memmove(void *dst, const void *src, size_t size);
void isac_rt_print_i64(int64_t value);
void isac_rt_print_u64(uint64_t value);
void isac_rt_print_f64(double value);
int64_t isac_rt_arith_i64(int64_t a, int64_t b, int32_t bits, int32_t rem);
uint64_t isac_rt_arith_u64(uint64_t a, uint64_t b, int32_t bits, int32_t rem);
typedef struct {
  const uint8_t *ptr;
  size_t len;
} IsacStr;
typedef struct {
  uint8_t *ptr;
  size_t len, capacity;
} IsacString;

void isac_rt_env_init(int32_t argc, char **argv);
void isac_rt_env_destroy(void);
size_t isac_rt_env_argument_count(void);
const IsacStr *isac_rt_env_argument(size_t index);
void isac_rt_stdout_write(const IsacStr *);
void isac_rt_stderr_write(const IsacStr *);
void isac_rt_string_fmt_i8(IsacString *, int8_t);
void isac_rt_string_fmt_i16(IsacString *, int16_t);
void isac_rt_string_fmt_i32(IsacString *, int32_t);
void isac_rt_string_fmt_i64(IsacString *, int64_t);
void isac_rt_string_fmt_isize(IsacString *, ptrdiff_t);
void isac_rt_string_fmt_u8(IsacString *, uint8_t);
void isac_rt_string_fmt_u16(IsacString *, uint16_t);
void isac_rt_string_fmt_u32(IsacString *, uint32_t);
void isac_rt_string_fmt_u64(IsacString *, uint64_t);
void isac_rt_string_fmt_usize(IsacString *, size_t);
void isac_rt_string_fmt_f32(IsacString *, float);
void isac_rt_string_fmt_f64(IsacString *, double);
void isac_rt_string_fmt_bool(IsacString *, int32_t);
void isac_rt_string_fmt_char(IsacString *, uint32_t);
void isac_rt_string_fmt_debug_str(IsacString *, const IsacStr *);
void isac_rt_string_fmt_debug_char(IsacString *, uint32_t);
void isac_rt_string_fmt_str(IsacString *, const IsacStr *);
typedef struct {
  IsacString text;
  union {
    int64_t signed_value;
    uint64_t unsigned_value;
    double float_value;
  } value;
  int32_t error;
} IsacInputResult;

void isac_rt_input_parse_i32(IsacInputResult *, const IsacString *);
void isac_rt_input_parse_i64(IsacInputResult *, const IsacString *);
void isac_rt_input_parse_u32(IsacInputResult *, const IsacString *);
void isac_rt_input_parse_u64(IsacInputResult *, const IsacString *);
void isac_rt_input_parse_f32(IsacInputResult *, const IsacString *);
void isac_rt_input_parse_f64(IsacInputResult *, const IsacString *);
void isac_rt_input_parse_bool(IsacInputResult *, const IsacString *);
void isac_rt_input_parse_view_i32(IsacInputResult *, const IsacStr *);
void isac_rt_input_parse_view_i64(IsacInputResult *, const IsacStr *);
void isac_rt_input_parse_view_u32(IsacInputResult *, const IsacStr *);
void isac_rt_input_parse_view_u64(IsacInputResult *, const IsacStr *);
void isac_rt_input_parse_view_f32(IsacInputResult *, const IsacStr *);
void isac_rt_input_parse_view_f64(IsacInputResult *, const IsacStr *);
void isac_rt_input_parse_view_bool(IsacInputResult *, const IsacStr *);

void isac_rt_input_drop(IsacInputResult *);
void isac_rt_input_read_String(IsacInputResult *);
int32_t isac_rt_input_result_ok(const IsacInputResult *);
int32_t isac_rt_input_result_error(const IsacInputResult *);
void isac_rt_input_result_str(IsacStr *, const IsacInputResult *);
void isac_rt_input_read_i32(IsacInputResult *);
int32_t isac_rt_input_result_i32(const IsacInputResult *);
void isac_rt_input_read_i64(IsacInputResult *);
int64_t isac_rt_input_result_i64(const IsacInputResult *);
void isac_rt_input_read_u32(IsacInputResult *);
uint32_t isac_rt_input_result_u32(const IsacInputResult *);
void isac_rt_input_read_u64(IsacInputResult *);
uint64_t isac_rt_input_result_u64(const IsacInputResult *);
void isac_rt_input_read_f32(IsacInputResult *);
float isac_rt_input_result_f32(const IsacInputResult *);
void isac_rt_input_read_f64(IsacInputResult *);
double isac_rt_input_result_f64(const IsacInputResult *);
void isac_rt_input_read_bool(IsacInputResult *);
int32_t isac_rt_input_result_bool(const IsacInputResult *);
void *isac_rt_realloc(void *ptr, size_t size);
void isac_rt_index_check(size_t index, size_t length);
void isac_rt_string_new(IsacString *out);
void isac_rt_string_from(IsacString *out, const IsacStr *text);
size_t isac_rt_string_len(const IsacString *text);
int32_t isac_rt_string_is_char_boundary(const IsacString *text, size_t index);
uint32_t isac_rt_string_char_at(const IsacString *text, size_t index);
size_t isac_rt_string_capacity(const IsacString *text);
int32_t isac_rt_string_is_empty(const IsacString *text);
void isac_rt_string_as_str(IsacStr *out, const IsacString *text);
void isac_rt_string_push_str(IsacString *text, const IsacStr *other);
void isac_rt_string_clear(IsacString *text);
void isac_rt_string_drop(IsacString *text);
void isac_rt_print_str(const uint8_t *ptr, size_t len);
void isac_rt_print_view(const IsacStr *text);
void isac_rt_panic_str(const IsacStr *text);
void isac_rt_string_print(const IsacString *text);
#ifdef __cplusplus
[[noreturn]]
#else
_Noreturn
#endif
void isac_rt_panic(const char *message);
void isac_rt_print_i32(int32_t value);

void isac_rt_print_bool(int32_t value);
void isac_rt_print_usize(size_t value);
void isac_rt_println(const char *text);
int32_t isac_rt_div_i32(int32_t a, int32_t b);
size_t isac_rt_div_usize(size_t a, size_t b);
int32_t isac_rt_load_i32(const void *ptr);
void isac_rt_store_i32(void *ptr, int32_t value);

size_t isac_rt_buffer_size(size_t length);
void isac_rt_buffer_init(void *ptr, size_t length);
size_t isac_rt_buffer_len(const void *ptr);
int32_t isac_rt_buffer_read(const void *ptr, size_t index);
void isac_rt_buffer_write(void *ptr, size_t index, int32_t value);
#ifdef ISAC_RT_TESTING
size_t isac_rt_test_allocations(void);
size_t isac_rt_test_frees(void);
#endif
#ifdef __cplusplus
}
#endif
#endif
