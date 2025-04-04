#ifndef ISAC_RUNTIME_H
#define ISAC_RUNTIME_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void *isac_rt_alloc(size_t size);
void *isac_rt_alloc_zeroed(size_t size);
void isac_rt_free(void *ptr);
void *isac_rt_memcpy(void *dst, const void *src, size_t size);
void *isac_rt_memmove(void *dst, const void *src, size_t size);
#ifdef __cplusplus
[[noreturn]]
#else
_Noreturn
#endif
void isac_rt_panic(const char *message);
void isac_rt_print_i32(int32_t value);
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
