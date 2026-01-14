

#include "isac_allocator.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef union {
  max_align_t alignment;
  struct {
    uint64_t magic;
    size_t size;
  } value;
} Header;
static const uint64_t magic = UINT64_C(0x1aacbeefcafebabe);
static size_t live, allocations, reallocations, deallocations;
static int failing;

void test_allocator_fail(int fail) {
  failing = fail;
}
size_t test_allocator_live(void) {
  return live;
}
size_t test_allocator_allocations(void) {
  return allocations;
}
size_t test_allocator_reallocations(void) {
  return reallocations;
}
size_t test_allocator_deallocations(void) {
  return deallocations;
}
static Header *header(void *ptr) {
  Header *h = (Header *)ptr - 1;
  if (h->value.magic != magic)
    abort();
  return h;
}
void *isac_allocator_allocate(size_t size, int zeroed) {
  if (!size || failing || size > SIZE_MAX - sizeof(Header))
    return NULL;
  Header *h = malloc(sizeof(Header) + size);
  if (!h)
    return NULL;
  h->value.magic = magic;
  h->value.size = size;
  ++live;
  ++allocations;
  if (zeroed)
    memset(h + 1, 0, size);
  return h + 1;
}
void *isac_allocator_reallocate(void *ptr, size_t size) {
  Header *h = header(ptr);
  if (!size || failing || size > SIZE_MAX - sizeof(Header))
    return NULL;
  Header *grown = realloc(h, sizeof(Header) + size);
  if (!grown)
    return NULL;
  grown->value.size = size;
  ++reallocations;
  return grown + 1;
}
void isac_allocator_deallocate(void *ptr) {
  Header *h = header(ptr);
  h->value.magic = 0;
  --live;
  ++deallocations;
  free(h);
}
