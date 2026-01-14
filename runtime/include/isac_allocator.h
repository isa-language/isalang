#ifndef ISAC_ALLOCATOR_H
#define ISAC_ALLOCATOR_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

void *isac_allocator_allocate(size_t size, int zeroed);

void *isac_allocator_reallocate(void *ptr, size_t size);

void isac_allocator_deallocate(void *ptr);

#ifdef __cplusplus
}
#endif
#endif
