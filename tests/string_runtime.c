#include "isac_runtime.h"
#include <assert.h>
#include <string.h>
int main(void) {
  IsacString s;
  isac_rt_string_new(&s);
  assert(!s.ptr && !s.len && !s.capacity);
  IsacStr literal = {(const uint8_t *)"Ol\xc3\xa1", 4};
  isac_rt_string_push_str(&s, &literal);
  assert(s.len == 4 && s.capacity >= 4);
  IsacStr view;
  isac_rt_string_as_str(&view, &s);
  assert(view.ptr == s.ptr && view.len == 4);
  isac_rt_string_push_str(&s, &literal);
  assert(s.len == 8 && memcmp(s.ptr, "Ol\xc3\xa1Ol\xc3\xa1", 8) == 0);
  size_t capacity = s.capacity;
  isac_rt_string_clear(&s);
  assert(!s.len && s.capacity == capacity);
  isac_rt_string_drop(&s);
  assert(!s.ptr);
  assert(isac_rt_test_allocations() == 1 && isac_rt_test_frees() == 1);
  return 0;
}
