#include "isac_allocator.h"
#include <stdlib.h>

void *isac_allocator_allocate(size_t size, int zeroed) {
  return zeroed ? calloc(1, size) : malloc(size);
}
void *isac_allocator_reallocate(void *ptr, size_t size) {
  return realloc(ptr, size);
}
void isac_allocator_deallocate(void *ptr) {
  free(ptr);
}
